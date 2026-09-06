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
 * epd_probe() reads registers, which the SPI peripheral cannot do reliably on
 * this 3-wire bus, so it drives the pins by hand and releases them again on
 * the way out. Calling it after epd_bus_init() would fight the SPI driver for
 * the same pins.
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
    bool    reads_ok;           /* register reads returned real data (rev[6] == 0x0C) */
    int     busy_after_reset;   /* BUSY level right after the reset pulse (expected 1 = idle) */
    int     busy_low_ms;        /* ms after PON until BUSY went low, -1 if it never did */
    int     busy_release_ms;    /* ms after PON until BUSY returned high, -1 if timeout */
    uint8_t rev[7];             /* REV 0x70: PROD_REV[23:0], LUT_REV[23:0], CHIP_REV (expected 0x0C) */
    uint8_t flg;                /* FLG 0x71: b6 PTL, b5 I2C_ERR, b4 I2C_BUSYN, b3 DATA_FLAG, b2 PON, b1 POF, b0 BUSY_N */
    int8_t  temp_c;             /* TSC 0x40 byte 0: internal temperature sensor, °C */
    int     pbc_pass;           /* PBC 0x44 bit0 PSTA (1 = panel glass check pass), -1 if not read */
} epd_probe_result_t;
/* rev, flg, temp_c and pbc_pass are only meaningful when reads_ok is true.
 * When it is false the read path came back with an undriven line (0x7F 0xFF
 * ... or all zeroes) and those four fields carry that garbage verbatim --
 * do not print them as if they were panel data. busy_after_reset,
 * busy_low_ms and busy_release_ms are always valid: they are pin
 * observations, not register reads. */

/* Probe the controller over bit-banged GPIO: reset, BUSY level, PWR/BTST/PON,
 * the BUSY low-then-high power-on signature, the REV/FLG/TSC/PBC registers,
 * then POF. Configures and releases the pins itself, so it must run BEFORE
 * epd_bus_init(). pins must not be NULL. */
esp_err_t epd_probe(const epd_pins_t *pins, epd_probe_result_t *out);

/* One-line summary of a probe result, for the [EPD-TEST] log. */
void      epd_probe_format(const epd_probe_result_t *r, char *buf, size_t len);

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
