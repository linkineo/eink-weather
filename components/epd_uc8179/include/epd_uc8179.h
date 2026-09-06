/*
 * Driver for the Waveshare 7.5inch e-Paper (B) V2 / 7.5inch e-Paper V2 panel
 * (800x480, UC8179C controller) on the Waveshare e-Paper ESP32 Driver Board.
 *
 * The command sequences are transcribed from the Waveshare Raspberry Pi
 * reference driver (EPD_7in5b_V2.c V1.0 2024-08-07, EPD_7in5_V2.c) and the
 * register codes verified against the 7.5inch e-Paper V2 specification
 * (Revision 2.0).
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
    uint8_t rev[7];             /* REV 0x70: PROD_REV[23:0], LUT_REV[23:0], CHIP_REV (expected 0x0C) */
    uint8_t flg;                /* FLG 0x71: b6 PTL, b5 I2C_ERR, b4 I2C_BUSYN, b3 DATA_FLAG, b2 PON, b1 POF, b0 BUSY_N */
    int8_t  temp_c;             /* TSC 0x40 byte 0: internal temperature sensor, °C */
    int     pbc_pass;           /* PBC 0x44 bit0 PSTA (1 = panel glass check pass), -1 if not read */
} epd_probe_result_t;

esp_err_t epd_bus_init(const epd_pins_t *pins);                     /* configure GPIOs + SPI; call once */
esp_err_t epd_probe(epd_probe_result_t *out);                       /* reset → PWR/BTST/PON → BUSY signature → reads → POF */
void      epd_probe_format(const epd_probe_result_t *r, char *buf, size_t len); /* one-line summary, see below */
esp_err_t epd_init(void);                                           /* full Waveshare init for the Kconfig panel */
esp_err_t epd_clear(void);                                          /* all white */
esp_err_t epd_display(const uint8_t *black, const uint8_t *red);    /* red may be NULL; blocks until refresh done */
esp_err_t epd_sleep(void);                                          /* 0x50 F7, 0x02 + busy, 0x07 A5 */
const char *epd_panel_name(void);                                   /* e.g. "7.5in (B) V2 800x480 BWR" */
