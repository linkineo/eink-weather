/*
 * Private write-only transport for the UC8179 e-paper controller.
 *
 * The panel is wired as a 3-wire bus (SDA/SCL/CSB + DC). The controller can
 * drive the shared SDA line to answer a register read, but this layer never
 * reads: the ESP32 SPI peripheral is configured as a plain full-duplex,
 * write-only device (flags = 0, no MISO pin) because that is the only
 * configuration in which the long DMA image transfers actually reach the
 * panel. Wave 2 used SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_3WIRE so it could
 * read as well; short commands went through but the 4000-byte image chunks
 * silently did not, and the glass came up as random noise.
 *
 * Register reads therefore live in epd_bitbang.c and run before this bus
 * exists (see epd_probe()). CONFIG_EPD_DATA_BITBANG additionally routes the
 * writes below through that same bit-bang path, as a diagnostic fallback.
 *
 * RST, DC and BUSY are owned by epd_bitbang.c in both modes; the three
 * helpers at the bottom of this header forward to it.
 *
 * None of these functions is re-entrant: the data helpers share one static
 * chunk buffer, so a single task must own the panel.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "epd_uc8179.h"

/* Largest single SPI transfer. Kept below the bus max_transfer_sz (4096) and
 * used as the size of the driver's internal fill/invert staging buffer, and
 * as the yield interval of the bit-bang data path. */
#define EPD_CHUNK_BYTES 4000

/* True once epd_bus_init() has completed successfully. */
bool epd_bus_ready(void);

/* Write one command byte with DC low. */
esp_err_t epd_bus_cmd(uint8_t cmd);

/* Write n data bytes with DC high, straight from the caller's buffer.
 * The buffer is not modified. Buffers that are not DMA-capable still work
 * (the SPI driver stages them through a temporary allocation), but internal
 * RAM is preferred. */
esp_err_t epd_bus_data(const uint8_t *p, size_t n);

/* Write the same byte n times with DC high. */
esp_err_t epd_bus_data_fill(uint8_t value, size_t n);

/* Write n data bytes with DC high, each one bitwise inverted. The source
 * buffer is never modified; the inversion is staged in the driver's own
 * chunk buffer. */
esp_err_t epd_bus_data_inv(const uint8_t *src, size_t n);

/* Hardware reset pulse: RST high 200 ms, low 5 ms, high 200 ms -- the timing
 * of the tri-colour reference driver EPD_7in5b_V2.c:40-48. Forwards to
 * epd_bitbang.c, which owns RST. */
void epd_bus_reset_pulse(void);

/* Current level of the BUSY_N line: 0 = busy, 1 = idle, -1 if the control
 * pins are not configured yet. */
int epd_bus_busy_level(void);

/* Poll BUSY_N until it reads high, at most timeout_ms. Returns
 * ESP_ERR_TIMEOUT on expiry. elapsed_ms may be NULL; when given it always
 * receives the measured wall-clock duration, timeout or not. Needs only the
 * control pins, so the probe can use it before epd_bus_init(). */
esp_err_t epd_bus_wait_busy_high(int timeout_ms, int *elapsed_ms);

/* Delay helper that does not depend on the configured tick rate. */
void epd_bus_delay_ms(uint32_t ms);
