/*
 * Private bit-banged GPIO transport for the UC8179 e-paper controller.
 *
 * Why this exists next to the SPI peripheral driver in epd_bus.c:
 *
 *  - Byte-exact control of CS, SCK, MOSI and D/C, which the diagnostics need:
 *    epd_probe() and epd_dc_diag() place individual bytes with a chosen D/C
 *    level and time the BUSY response, rather than running transactions. The
 *    bit-level behaviour below is a transcription of DEV_SPI_WriteByte() in
 *    Waveshare's own ESP32 port (DEV_Config.cpp), which never uses the SPI
 *    peripheral at all, so it follows a path known to work on this board.
 *
 *  - Those diagnostics therefore run *before* the SPI bus exists: they call
 *    epd_bb_init(), talk to the controller, then epd_bb_release() hands
 *    SCK/MOSI/CS back so epd_bus_init() can give them to the peripheral.
 *
 *  - CONFIG_EPD_DATA_BITBANG additionally routes commands and image data
 *    through this module, as a diagnostic fallback (slow: ~1 s per plane).
 *
 * There is no read side, and there will not be one: the 7.5inch e-Paper V2
 * specification says for both serial modes that "under serial mode, only
 * write operations are allowed". Wave 2 tried reads through the SPI
 * peripheral (SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_3WIRE) and wave 3 through a
 * transcription of DEV_SPI_ReadByte(); both got back the last bit the ESP32
 * had driven on the shared SDA line, never controller data.
 *
 * This module also owns the control pins (RST, DC, BUSY) and the panel power
 * rail for both transports, so the reset pulse, the BUSY sampling and the
 * power-up exist in exactly one place.
 *
 * None of these functions is re-entrant; a single task must own the panel.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "epd_uc8179.h"

/* Drive both panel power pins (epd_pins_t::pwr / ::pwr_aux), active high; a
 * pin given as -1 is skipped and a call before epd_power_init() does nothing.
 * The public epd_power() wraps this and adds the one-off settling delay --
 * prefer it. This raw form exists for callers that already hold the rail up. */
void epd_bb_set_power(int on);

/* Configure the control pins: the panel power rail first (epd_power_init()
 * plus epd_power(true), so nothing below talks to an unpowered controller),
 * then RST and DC as outputs (RST high because reset is active low, DC low)
 * and BUSY as an input with the internal pull-down so a missing panel reads 0
 * deterministically instead of floating. Leaves SCK/MOSI/CS untouched, which
 * is what the SPI data path wants. */
esp_err_t epd_bb_init_ctrl(const epd_pins_t *pins);

/* epd_bb_init_ctrl() plus SCK/MOSI/CS as plain GPIO outputs, idling at
 * CS high / SCK low (DEV_Config.cpp GPIO_Config()). */
esp_err_t epd_bb_init(const epd_pins_t *pins);

/* Return SCK/MOSI/CS to their reset state so spi_bus_initialize() can route
 * them to the SPI peripheral. RST/DC/BUSY stay configured and usable. */
void epd_bb_release(void);

/* True once the control pins have been configured (by either init). */
bool epd_bb_ctrl_ready(void);

/* True once SCK/MOSI/CS are owned by this module (epd_bb_init() called and
 * epd_bb_release() not called since). */
bool epd_bb_data_ready(void);

/* Drive the DC line directly: 0 = command, 1 = data. */
void epd_bb_set_dc(int level);

/* Diagnostic hook: mirror every D/C level change onto a second GPIO, so that
 * epd_bb_cmd() / epd_bb_data() drive a candidate pin exactly like the real
 * D/C pin. Used by the D/C GPIO scan (epd_diag.c) to ask "would the controller
 * honour D/C if it were wired to GPIO n?" without duplicating the transport.
 * The caller owns the pin and must have configured it as an output before
 * enabling the mirror; -1 disables it. Both inits clear it. */
void epd_bb_set_dc_mirror(int gpio);

/* One byte out, MSB first, CS asserted for the byte
 * (DEV_Config.cpp DEV_SPI_WriteByte()). DC is *not* touched. */
void epd_bb_write_byte(uint8_t b);

/* One command byte: DC low, then the byte. */
void epd_bb_cmd(uint8_t cmd);

/* n data bytes: DC high, then the bytes. */
void epd_bb_data(const uint8_t *p, size_t n);

/* Hardware reset pulse: RST high 200 ms, low 5 ms, high 200 ms -- the timing
 * of the tri-colour reference driver EPD_7in5b_V2.c:40-48. */
void epd_bb_reset_pulse(void);

/* Current level of the BUSY_N line: 0 = busy, 1 = idle, -1 if not configured. */
int epd_bb_busy_level(void);
