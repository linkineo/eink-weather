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
 *     epd_power_init(&pins); epd_power(true);  // panel rail up (implicit in
 *                                              // every entry point below)
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

/* Pin map. pwr / pwr_aux are the panel power enable lines (active high),
 * -1 when a board revision does not have one -- see epd_power(). */
typedef struct {
    int sck, mosi, cs, dc, rst, busy;
    int pwr, pwr_aux;
    int spi_hz;
} epd_pins_t;

/*----------------------------------------------------------- panel power ----
 * On this board revision the panel's 3.3 V rail hangs off an LDO that a plain
 * GPIO switches (board.h: GPIO2 -> R35 -> Q32 -> Q31 -> LDO). Nothing below
 * works before that rail is up: with the pin low the controller runs on the
 * leakage current of the signal lines, answers commands on BUSY, and ignores
 * D/C and every data byte -- which is exactly the failure this driver chased
 * for three waves.
 *
 * Every entry point that configures the control pins (epd_probe(),
 * epd_dc_diag(), epd_rst_diag(), epd_dcscan(), epd_bus_init()) powers the
 * panel first, so callers do not have to. These two exist so that the
 * application can do it explicitly at boot and switch the rail off again for
 * deep sleep.
 *---------------------------------------------------------------------------*/

/* Remember the pin map and configure the power pins as outputs. Switches
 * nothing on its own. Safe to call more than once; a pin already driven keeps
 * its level. pins must not be NULL. */
esp_err_t epd_power_init(const epd_pins_t *pins);

/* Switch the panel rail. The first switch-on also waits for the LDO to
 * settle. Returns ESP_ERR_INVALID_STATE before epd_power_init(); a board with
 * no power pin (pwr and pwr_aux both -1) succeeds and does nothing. */
esp_err_t epd_power(bool on);

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

/*------------------------------------------------ 3-wire / 4-wire diagnostic ----
 * The innocent explanation for DC_NOT_SEEN that no D/C experiment can reach:
 * the controller may be strapped into 3-wire serial mode, where the D/C PIN is
 * not a bus signal at all (spec 3.3-2-3: "The pin DC can be connected to an
 * external ground") and each frame is nine bits, "DC bit, D7 to D0 bit". A
 * controller in that mode, whose shift register clears on the falling edge of
 * CSB and latches on the rising one, reads every 8-bit frame this firmware has
 * ever sent as [D/C = 0][byte] -- a COMMAND, whatever the D/C pin does. That is
 * precisely what five hardware runs observed, with no broken line anywhere.
 *
 * The five oracles send 9-bit frames instead and watch the same BUSY power-on
 * signature; see components/epd_uc8179/epd_diag.c for what each one asks.
 *-------------------------------------------------------------------------------*/

/* The five oracles, in the order epd_wire_diag() runs them. */
typedef enum {
    EPD_WIRE_F1 = 0,      /* control: plain 8-bit 0x04, D/C pin low -- expected yes */
    EPD_WIRE_F2,          /* 9-bit [0][0x04], D/C first -- yes only in 3-wire/D/C-first */
    EPD_WIRE_F3,          /* D/C-first DSLP + 0xA5 as DATA, then 8-bit PON -- no only if the data frame landed */
    EPD_WIRE_F4,          /* 9-bit [0x04][0], D/C last -- yes in 3-wire/D/C-last AND in 4-wire */
    EPD_WIRE_F5,          /* D/C-last DSLP + 0xA5 as DATA, then 8-bit PON -- no only if the data frame landed */
    EPD_WIRE_ORACLES      /* count, not an oracle */
} epd_wire_oracle_t;

typedef struct {
    epd_pon_watch_t watch[EPD_WIRE_ORACLES];   /* the raw BUSY_N observation */
    bool            powered[EPD_WIRE_ORACLES]; /* ...and whether it was a power-ON */
    char verdict[24];     /* "THREE_WIRE_DC_FIRST" | "THREE_WIRE_DC_LAST" | "FOUR_WIRE" | "INCONCLUSIVE" */
} epd_wire_diag_result_t;
/* Why the two flags are not the same thing, and why the verdict is built on
 * the second: watch[].pon is only the low-then-high BUSY_N signature, and on
 * this controller POF produces one too -- a ~41 ms pulse, with the rails up or
 * down alike (rsttest, six runs). Since two of the five frames below decode to
 * POF under the hypotheses they are meant to refute, the signature alone
 * cannot tell "the panel powered on" from "some command ran". The release time
 * can, and by a factor of three: PON releases BUSY at ~131 ms, POF at ~41 ms.
 * powered[] is watch[].pon qualified by that duration, and it is what each
 * oracle was actually asking about. */

/* Run the 3-wire/4-wire oracles over bit-banged GPIO. Same constraints as
 * epd_dc_diag(): before epd_bus_init(), never refreshes, and every oracle is
 * closed with a power-off that is sent in all three framings (so the panel ends
 * up unpowered whichever mode turns out to be real) plus a reset pulse. */
esp_err_t epd_wire_diag(const epd_pins_t *pins, epd_wire_diag_result_t *out);

/* One-line summary of a wire diagnostic result, for the [EPD-TEST] log. */
void      epd_wire_diag_format(const epd_wire_diag_result_t *r, char *buf, size_t len);

/*--------------------------------------------------- RST line diagnostic ----
 * Does the hardware reset pulse reach the controller? Nothing has ever proved
 * it: commands, BUSY and power are proven, but every reset so far could have
 * been a no-op without changing a single observation. The oracle is the length
 * of the BUSY_N low pulse that POF produces -- long on a still-powered
 * controller, absent or very short on one whose rails a reset has just cleared.
 * See components/epd_uc8179/epd_diag.c.
 *---------------------------------------------------------------------------*/

/* BUSY-low duration reported as -1 when BUSY never went low at all. */
typedef struct {
    epd_pon_watch_t pon_ref;        /* control power-on, before the reference POF */
    int  pof_ref_ms;                /* POF BUSY-low duration with no reset in between */
    int  pof_off_ms;                /* POF BUSY-low duration with the rails already off */
    epd_pon_watch_t pon_rst;        /* power-on of the RST variant, before the pulse */
    bool busy_low_during_rst;       /* BUSY_N went low while RST_N was held low */
    int  busy_after_rst;            /* BUSY level once the RST pulse has settled */
    int  pof_after_rst_ms;          /* POF BUSY-low duration after the RST pulse */
    epd_pon_watch_t pon_after_rst;  /* PON repeated right after an RST pulse */
    bool pof_oracle_usable;         /* the POF pulse length does depend on the power state */
    bool rst_effective;             /* the reset pulse demonstrably reached the controller */
} epd_rst_diag_result_t;
/* rst_effective is one-sided: "yes" is proof, "no" only means no proof was
 * obtained -- and when pof_oracle_usable is false it means nothing at all,
 * because POF then pulses BUSY whatever the power state. */

/* Run the RST oracle over bit-banged GPIO; same constraints as epd_dc_diag()
 * (before epd_bus_init(), no refresh, ends powered off and reset). */
esp_err_t epd_rst_diag(const epd_pins_t *pins, epd_rst_diag_result_t *out);

/* One-line summary of an RST diagnostic result, for the [EPD-TEST] log. */
void      epd_rst_diag_format(const epd_rst_diag_result_t *r, char *buf, size_t len);

/*------------------------------------------------------- D/C GPIO scan -----
 * Excludes the last innocent explanation for DC_NOT_SEEN: a board revision
 * that routes D/C to some other GPIO. Each candidate is driven as a second
 * D/C line alongside GPIO27 and asked the question of oracle A -- a candidate
 * that is really wired to the controller's D/C input makes the CDI parameter
 * stay a parameter, so the panel does NOT power on.
 *---------------------------------------------------------------------------*/

#define EPD_DCSCAN_MAX 16   /* capacity of the candidate table below */

typedef struct {
    int             n;                     /* candidates actually tested */
    int             gpio[EPD_DCSCAN_MAX];  /* the candidate pins, in test order */
    epd_pon_watch_t watch[EPD_DCSCAN_MAX]; /* pon=no means the candidate acted as D/C */
    int             found;                 /* first candidate honoured as D/C, -1 = none */
} epd_dcscan_result_t;

/* Scan the candidate GPIOs for a D/C line. Same constraints as epd_dc_diag().
 * Every candidate is returned to its default state (input, pull-up) on the
 * way out; the real D/C pin stays a driven output. */
esp_err_t epd_dcscan(const epd_pins_t *pins, epd_dcscan_result_t *out);

/* One-line summary of a scan result, for the [EPD-TEST] log. */
void      epd_dcscan_format(const epd_dcscan_result_t *r, char *buf, size_t len);

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
