/*
 * Driver for the Waveshare 7.5inch e-Paper (B) V2/V3 / 7.5inch e-Paper V2
 * panel (800x480, UC8179C controller) on the Waveshare e-Paper ESP32 Driver
 * Board.
 *
 * The command sequences are transcribed from the Waveshare Raspberry Pi
 * reference driver (EPD_7in5b_V2.c V1.0 2024-08-07, EPD_7in5_V2.c) and the
 * register codes verified against the 7.5inch e-Paper V2 specification
 * (Revision 2.0).
 *
 * Call order, and it matters:
 *
 *     epd_probe(&pins, &result);   // bit-banged GPIO, must come FIRST
 *     epd_bus_init(&pins);         // hands SCK/MOSI/CS to the SPI peripheral
 *     epd_init(); ... epd_display(...); epd_sleep();
 *
 * epd_probe() drives the pins by hand (epd_bitbang.c) and releases them again
 * on the way out; calling it after epd_bus_init() would fight the SPI driver
 * for the same pins. epd_dc_diag() has the same constraint and is an
 * alternative to the whole sequence, not a step in it: it runs instead of the
 * display path (CONFIG_APP_DIAG_ONLY) and never refreshes the panel.
 *
 * The controller cannot be read back. The 7.5inch e-Paper V2 specification
 * states for both serial modes that "under serial mode, only write operations
 * are allowed", and three hardware runs confirmed it -- a read returns the last
 * bit the ESP32 drove on the shared SDA line, over the SPI peripheral and over
 * bit-banged GPIO alike. Presence and behaviour are therefore established
 * entirely from the BUSY_N pin.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#define EPD_WIDTH  800
#define EPD_HEIGHT 480
#define EPD_PLANE_BYTES (EPD_WIDTH * EPD_HEIGHT / 8)   /* 48000 */

typedef struct { int sck, mosi, cs, dc, rst, busy; int spi_hz; } epd_pins_t;

typedef struct {
    bool    present;            /* verdict == PRESENT */
    char    verdict[12];        /* "PRESENT" | "ABSENT" | "UNCERTAIN" */
    int     busy_after_reset;   /* BUSY level right after the reset pulse (expected 1 = idle) */
    int     busy_low_ms;        /* ms after PON until BUSY went low, -1 if it never did */
    int     busy_release_ms;    /* ms after PON until BUSY returned high, -1 if timeout */
} epd_probe_result_t;
/* All three numbers are pin observations, so they are always valid. The
 * verdict rests on the low-then-high BUSY signature that PON produces:
 * PRESENT when it completed, ABSENT when BUSY never went low at all,
 * UNCERTAIN when it went low and never came back. */

/* Probe the controller over bit-banged GPIO: reset, BUSY level, PWR/BTST/PON,
 * the BUSY low-then-high power-on signature, then POF. Configures and
 * releases the pins itself, so it must run BEFORE epd_bus_init(). pins must
 * not be NULL. */
esp_err_t epd_probe(const epd_pins_t *pins, epd_probe_result_t *out);

/* One-line summary of a probe result, for the [EPD-TEST] log. */
void      epd_probe_format(const epd_probe_result_t *r, char *buf, size_t len);

/*--------------------------------------------------- D/C line diagnostic ----
 * Decides whether the controller sees the D/C (data/command) line, using the
 * BUSY power-on signature as the only observable -- no refresh, no image data.
 * See components/epd_uc8179/epd_diag.c for what each oracle asks and why.
 *---------------------------------------------------------------------------*/

/* One BUSY power-on signature watch: BUSY_N sampled every millisecond for
 * 400 ms, looking for the low-then-high edge pair that PON produces on a live
 * controller. */
typedef struct {
    bool pon;       /* the complete low-then-high signature was seen */
    int  low_ms;    /* ms until BUSY went low, -1 if it never did */
    int  high_ms;   /* ms until BUSY came back high, -1 if it never did */
} epd_pon_watch_t;

/* Read-back of one pad driven by the ESP32 with its input buffer enabled.
 * A healthy pad gives drive1 == 1 and drive0 == 0; anything else means the
 * line is shorted or stuck (-1 = the pad could not be configured). */
typedef struct {
    int drive1;
    int drive0;
} epd_pad_check_t;

typedef struct {
    /* Pad read-backs, taken with the panel held in reset. */
    epd_pad_check_t pad_dc, pad_rst, pad_cs, pad_sck, pad_mosi;
    /* The four oracles. pon=yes means the panel powered on. */
    epd_pon_watch_t oracle_d;   /* control: 0x04 as a COMMAND -- expected yes */
    epd_pon_watch_t oracle_a;   /* CDI then 0x04 as DATA -- yes only if D/C is ignored */
    epd_pon_watch_t oracle_b;   /* DSLP + 0xA5 as DATA, then PON -- no only if the data byte landed */
    epd_pon_watch_t oracle_e;   /* a lone 0x04 as DATA -- yes if D/C is ignored or inverted */
    char verdict[16];           /* "DC_OK" | "DC_NOT_SEEN" | "DC_INVERTED" | "INCONCLUSIVE" */
} epd_dc_diag_result_t;

/* Run the D/C oracles over bit-banged GPIO. Configures the pins itself and
 * must run BEFORE epd_bus_init(); leaves the panel powered off (POF) and
 * reset, so it is safe to repeat. Never refreshes the panel. */
esp_err_t epd_dc_diag(const epd_pins_t *pins, epd_dc_diag_result_t *out);

/* One-line summary of a diagnostic result, for the [EPD-TEST] log. */
void      epd_dc_diag_format(const epd_dc_diag_result_t *r, char *buf, size_t len);

/* Configure the write path to the panel; call once, after epd_probe(). */
esp_err_t epd_bus_init(const epd_pins_t *pins);

/* "spi" or "bitbang" -- which transport epd_bus_init() built
 * (CONFIG_EPD_DATA_BITBANG). */
const char *epd_datapath_name(void);

esp_err_t epd_init(void);                                           /* full Waveshare init for the Kconfig panel */
esp_err_t epd_clear(void);                                          /* all white */
esp_err_t epd_display(const uint8_t *black, const uint8_t *red);    /* red may be NULL; blocks until refresh done */
esp_err_t epd_sleep(void);                                          /* 0x50 F7, 0x02 + busy, 0x07 A5 */
const char *epd_panel_name(void);                                   /* e.g. "7.5in (B) V2 800x480 BWR" */
