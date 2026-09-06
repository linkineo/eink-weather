/*
 * Private SPI + GPIO layer for the UC8179 e-paper controller.
 *
 * The panel is wired as a 3-wire (SDA/SCL/CSB + DC) bus: there is no MISO,
 * the controller drives the same SDA line the host uses for MOSI when a
 * register is read. The ESP32 SPI peripheral is therefore configured
 * half-duplex + 3-wire so that it samples the MOSI pin during a receive-only
 * transaction.
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
 * used as the size of the driver's internal fill/invert staging buffer. */
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

/* Read n bytes back from a register: command byte with DC low and CS held,
 * then a receive-only phase with DC high. n must be <= 8. */
esp_err_t epd_bus_read(uint8_t cmd, uint8_t *out, size_t n);

/* Hardware reset pulse: RST high 20 ms, low 2 ms, high 20 ms. */
void epd_bus_reset_pulse(void);

/* Current level of the BUSY_N line: 0 = busy, 1 = idle. */
int epd_bus_busy_level(void);

/* Poll BUSY_N until it reads high, at most timeout_ms. Returns
 * ESP_ERR_TIMEOUT on expiry. elapsed_ms may be NULL; when given it always
 * receives the measured wall-clock duration, timeout or not. */
esp_err_t epd_bus_wait_busy_high(int timeout_ms, int *elapsed_ms);

/* Delay helper that does not depend on the configured tick rate. */
void epd_bus_delay_ms(uint32_t ms);
