/*
 * Private bit-banged GPIO transport for the UC8179 e-paper controller.
 *
 * Why this exists next to the SPI peripheral driver in epd_bus.c:
 *
 *  - Register reads. The controller shares one SDA line for both directions.
 *    Wave 2 tried to do that with the ESP32 SPI peripheral configured
 *    SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_3WIRE and every read came back as a
 *    floating line (0x7F 0xFF ...). Waveshare's own ESP32 port never uses the
 *    peripheral at all: DEV_SPI_WriteByte()/DEV_SPI_ReadByte() in
 *    DEV_Config.cpp toggle the pins by hand. The bit-level behaviour below is
 *    a transcription of those two functions, so reads follow a path that is
 *    known to work on exactly this board.
 *
 *  - The probe therefore runs *before* the SPI bus exists: it calls
 *    epd_bb_init(), talks to the controller, then epd_bb_release() hands
 *    SCK/MOSI/CS back so epd_bus_init() can give them to the peripheral.
 *
 *  - CONFIG_EPD_DATA_BITBANG additionally routes commands and image data
 *    through this module, as a diagnostic fallback (slow: ~1 s per plane).
 *
 * This module also owns the control pins (RST, DC, BUSY) for both transports,
 * so the reset pulse and the BUSY sampling exist in exactly one place.
 *
 * None of these functions is re-entrant; a single task must own the panel.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "epd_uc8179.h"

/* Configure the control pins only: RST and DC as outputs (RST high because
 * reset is active low, DC low), BUSY as an input with the internal pull-down
 * so a missing panel reads 0 deterministically instead of floating. Leaves
 * SCK/MOSI/CS untouched, which is what the SPI data path wants. */
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

/* One byte out, MSB first, CS asserted for the byte
 * (DEV_Config.cpp DEV_SPI_WriteByte()). DC is *not* touched. */
void epd_bb_write_byte(uint8_t b);

/* One command byte: DC low, then the byte. */
void epd_bb_cmd(uint8_t cmd);

/* n data bytes: DC high, then the bytes. */
void epd_bb_data(const uint8_t *p, size_t n);

/* One byte in from the shared SDA line (DEV_Config.cpp DEV_SPI_ReadByte()):
 * MOSI switched to input, CS asserted, each bit sampled *before* its clock
 * pulse, then MOSI switched back to output. DC is not touched. */
uint8_t epd_bb_read_byte(void);

/* Register read: command byte with DC low, then n bytes back with DC high.
 * CS is toggled per byte, exactly like the Waveshare reference. */
void epd_bb_read(uint8_t cmd, uint8_t *out, size_t n);

/* Hardware reset pulse: RST high 200 ms, low 5 ms, high 200 ms -- the timing
 * of the tri-colour reference driver EPD_7in5b_V2.c:40-48. */
void epd_bb_reset_pulse(void);

/* Current level of the BUSY_N line: 0 = busy, 1 = idle, -1 if not configured. */
int epd_bb_busy_level(void);
