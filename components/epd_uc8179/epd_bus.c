#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

#include "epd_bitbang.h"
#include "epd_bus.h"

static const char *TAG = "epd";

#define EPD_SPI_HOST      SPI2_HOST
#define EPD_MAX_TRANSFER  4096
#define EPD_DC_COMMAND    0
#define EPD_DC_DATA       1
#define EPD_BUSY_POLL_MS  5

#if CONFIG_EPD_DATA_BITBANG
#define EPD_DATAPATH_BITBANG 1
#else
#define EPD_DATAPATH_BITBANG 0
#endif

typedef struct {
#if !EPD_DATAPATH_BITBANG
    spi_device_handle_t dev;
#endif
    bool inited;
} epd_bus_ctx_t;

static epd_bus_ctx_t s_bus;

#if !EPD_DATAPATH_BITBANG
/* Staging buffer for fills and inversions. It lives in .bss (internal RAM) and
 * is word aligned so the SPI driver can hand it straight to DMA. */
static WORD_ALIGNED_ATTR uint8_t s_chunk[EPD_CHUNK_BYTES];

/* Called by the SPI driver just before a transaction starts, i.e. before CS
 * is asserted, so the DC line is already stable when the panel latches the
 * first bit. trans->user carries 0 for a command and 1 for data. Every
 * transfer here goes through spi_device_polling_transmit(), so this always
 * runs in task context and may call ordinary flash-resident code. */
static void epd_bus_pre_cb(spi_transaction_t *trans)
{
    epd_bb_set_dc((int)(intptr_t)trans->user);
}
#endif

const char *epd_datapath_name(void)
{
    return EPD_DATAPATH_BITBANG ? "bitbang" : "spi";
}

void epd_bus_delay_ms(uint32_t ms)
{
    if (ms == 0) {
        return;
    }
    /* Do not rely on a particular CONFIG_FREERTOS_HZ: short waits are busy
     * loops on the ROM microsecond delay, longer ones yield to the scheduler
     * with one extra tick so the delay is never cut short. */
    if (ms >= 10) {
        vTaskDelay(pdMS_TO_TICKS(ms) + 1);
    } else {
        esp_rom_delay_us(ms * 1000U);
    }
}

bool epd_bus_ready(void)
{
    return s_bus.inited;
}

esp_err_t epd_bus_init(const epd_pins_t *pins)
{
    ESP_RETURN_ON_FALSE(pins != NULL, ESP_ERR_INVALID_ARG, TAG, "pins is NULL");
    ESP_RETURN_ON_FALSE(!s_bus.inited, ESP_ERR_INVALID_STATE, TAG, "bus already initialised");

#if EPD_DATAPATH_BITBANG
    /* Diagnostic data path: no SPI peripheral at all, every byte is clocked
     * out by epd_bitbang.c. epd_probe() has already released the pins. */
    ESP_RETURN_ON_ERROR(epd_bb_init(pins), TAG, "bit-bang init failed");
    s_bus.inited = true;
    ESP_LOGI(TAG, "bus ready (bit-bang): sck=%d mosi=%d cs=%d dc=%d rst=%d busy=%d",
             pins->sck, pins->mosi, pins->cs, pins->dc, pins->rst, pins->busy);
    return ESP_OK;
#else
    int hz = pins->spi_hz > 0 ? pins->spi_hz : CONFIG_EPD_SPI_HZ;

    /* RST, DC and BUSY belong to epd_bitbang.c whichever data path is in use,
     * so the reset pulse and the BUSY sampling exist in exactly one place.
     * CS/SCK/MOSI are claimed by the SPI peripheral below; the probe already
     * released them with epd_bb_release(). */
    ESP_RETURN_ON_ERROR(epd_bb_init_ctrl(pins), TAG, "control pins failed");

    spi_bus_config_t bus_cfg = {
        .sclk_io_num     = pins->sck,
        .mosi_io_num     = pins->mosi,
        .miso_io_num     = -1,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = EPD_MAX_TRANSFER,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(EPD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
                        TAG, "spi_bus_initialize failed");

    /* Plain full-duplex, write-only: flags = 0, no MISO pin. This is what
     * every working e-paper driver does (GxEPD2, the IDF spi_master LCD
     * example) and it is what wave 2 got wrong: with
     * SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_3WIRE the short command transactions
     * still reached the panel, but the 4000-byte DMA image transfers silently
     * did not, leaving the controller's RAM uninitialised (random noise on the
     * glass). Reads no longer share this device -- they are bit-banged in
     * epd_bitbang.c before the bus exists -- so nothing here has to care about
     * the bidirectional SDA line any more. */
    spi_device_interface_config_t dev_cfg = {
        .mode           = 0,
        .clock_speed_hz = hz,
        .spics_io_num   = pins->cs,
        .queue_size     = 4,
        .flags          = 0,
        .pre_cb         = epd_bus_pre_cb,
    };

    esp_err_t err = spi_bus_add_device(EPD_SPI_HOST, &dev_cfg, &s_bus.dev);
    if (err != ESP_OK) {
        spi_bus_free(EPD_SPI_HOST);
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        return err;
    }

    s_bus.inited = true;
    ESP_LOGI(TAG, "bus ready (spi): sck=%d mosi=%d cs=%d dc=%d rst=%d busy=%d %d Hz",
             pins->sck, pins->mosi, pins->cs, pins->dc, pins->rst, pins->busy, hz);
    return ESP_OK;
#endif
}

#if !EPD_DATAPATH_BITBANG
/* One transfer of up to EPD_CHUNK_BYTES data bytes with DC high. */
static esp_err_t epd_bus_write_chunk(const uint8_t *p, size_t n)
{
    spi_transaction_t t = {
        .length    = 8 * n,
        .tx_buffer = p,
        .user      = (void *)(intptr_t)EPD_DC_DATA,
    };
    return spi_device_polling_transmit(s_bus.dev, &t);
}
#endif

esp_err_t epd_bus_cmd(uint8_t cmd)
{
    ESP_RETURN_ON_FALSE(s_bus.inited, ESP_ERR_INVALID_STATE, TAG, "bus not initialised");

#if EPD_DATAPATH_BITBANG
    epd_bb_cmd(cmd);
    return ESP_OK;
#else
    spi_transaction_t t = {
        .flags  = SPI_TRANS_USE_TXDATA,
        .length = 8,
        .user   = (void *)(intptr_t)EPD_DC_COMMAND,
    };
    t.tx_data[0] = cmd;
    return spi_device_polling_transmit(s_bus.dev, &t);
#endif
}

esp_err_t epd_bus_data(const uint8_t *p, size_t n)
{
    ESP_RETURN_ON_FALSE(s_bus.inited, ESP_ERR_INVALID_STATE, TAG, "bus not initialised");
    ESP_RETURN_ON_FALSE(p != NULL || n == 0, ESP_ERR_INVALID_ARG, TAG, "data is NULL");

    if (n == 0) {
        return ESP_OK;
    }

#if EPD_DATAPATH_BITBANG
    /* A whole plane takes about a second of busy-looping, so the chunk loop
     * hands the CPU back between chunks: the idle task must still run or the
     * task watchdog fires. CS is deasserted after every byte anyway (that is
     * how the Waveshare reference clocks bytes out), so a pause between two
     * bytes is invisible to the controller. */
    size_t off = 0;
    while (off < n) {
        size_t len = n - off;
        if (len > EPD_CHUNK_BYTES) {
            len = EPD_CHUNK_BYTES;
        }
        epd_bb_data(p + off, len);
        off += len;
        vTaskDelay(1);
    }
    return ESP_OK;
#else
    /* Up to four bytes fit in the transaction descriptor itself, which avoids
     * any DMA-capability question for the small init payloads. */
    if (n <= 4) {
        spi_transaction_t t = {
            .flags  = SPI_TRANS_USE_TXDATA,
            .length = 8 * n,
            .user   = (void *)(intptr_t)EPD_DC_DATA,
        };
        memcpy(t.tx_data, p, n);
        return spi_device_polling_transmit(s_bus.dev, &t);
    }

    /* Larger payloads go out in chunks. CS deasserts between chunks; the
     * UC8179 does not care (the Waveshare reference drivers toggle CS around
     * every single byte). */
    size_t off = 0;
    while (off < n) {
        size_t len = n - off;
        if (len > EPD_CHUNK_BYTES) {
            len = EPD_CHUNK_BYTES;
        }
        ESP_RETURN_ON_ERROR(epd_bus_write_chunk(p + off, len), TAG, "data chunk failed");
        off += len;
    }
    return ESP_OK;
#endif
}

esp_err_t epd_bus_data_fill(uint8_t value, size_t n)
{
    ESP_RETURN_ON_FALSE(s_bus.inited, ESP_ERR_INVALID_STATE, TAG, "bus not initialised");

#if EPD_DATAPATH_BITBANG
    epd_bb_set_dc(1);
    for (size_t i = 0; i < n; i++) {
        epd_bb_write_byte(value);
        if ((i % EPD_CHUNK_BYTES) == (EPD_CHUNK_BYTES - 1)) {
            vTaskDelay(1);
            epd_bb_set_dc(1);
        }
    }
    return ESP_OK;
#else
    size_t off = 0;
    size_t filled = 0;
    while (off < n) {
        size_t len = n - off;
        if (len > EPD_CHUNK_BYTES) {
            len = EPD_CHUNK_BYTES;
        }
        if (filled < len) {
            memset(s_chunk + filled, value, len - filled);
            filled = len;
        }
        ESP_RETURN_ON_ERROR(epd_bus_write_chunk(s_chunk, len), TAG, "fill chunk failed");
        off += len;
    }
    return ESP_OK;
#endif
}

esp_err_t epd_bus_data_inv(const uint8_t *src, size_t n)
{
    ESP_RETURN_ON_FALSE(s_bus.inited, ESP_ERR_INVALID_STATE, TAG, "bus not initialised");
    ESP_RETURN_ON_FALSE(src != NULL || n == 0, ESP_ERR_INVALID_ARG, TAG, "src is NULL");

#if EPD_DATAPATH_BITBANG
    epd_bb_set_dc(1);
    for (size_t i = 0; i < n; i++) {
        epd_bb_write_byte((uint8_t)~src[i]);
        if ((i % EPD_CHUNK_BYTES) == (EPD_CHUNK_BYTES - 1)) {
            vTaskDelay(1);
            epd_bb_set_dc(1);
        }
    }
    return ESP_OK;
#else
    size_t off = 0;
    while (off < n) {
        size_t len = n - off;
        if (len > EPD_CHUNK_BYTES) {
            len = EPD_CHUNK_BYTES;
        }
        for (size_t i = 0; i < len; i++) {
            s_chunk[i] = (uint8_t)~src[off + i];
        }
        ESP_RETURN_ON_ERROR(epd_bus_write_chunk(s_chunk, len), TAG, "invert chunk failed");
        off += len;
    }
    return ESP_OK;
#endif
}

void epd_bus_reset_pulse(void)
{
    epd_bb_reset_pulse();
}

int epd_bus_busy_level(void)
{
    return epd_bb_busy_level();
}

esp_err_t epd_bus_wait_busy_high(int timeout_ms, int *elapsed_ms)
{
    /* Only the control pins have to be configured: this is also used by the
     * probe, which runs before epd_bus_init(). */
    ESP_RETURN_ON_FALSE(epd_bb_ctrl_ready(), ESP_ERR_INVALID_STATE, TAG,
                        "control pins not configured");

    /* BUSY_N low means the controller is working and must not be interrupted
     * (spec note 1.5-4). No status command is sent while waiting; only the
     * pin is sampled. */
    const int64_t t0 = esp_timer_get_time();
    const TickType_t poll = pdMS_TO_TICKS(EPD_BUSY_POLL_MS) > 0
                            ? pdMS_TO_TICKS(EPD_BUSY_POLL_MS) : 1;
    esp_err_t err = ESP_ERR_TIMEOUT;

    for (;;) {
        if (epd_bb_busy_level() == 1) {
            err = ESP_OK;
            break;
        }
        if ((esp_timer_get_time() - t0) / 1000 >= (int64_t)timeout_ms) {
            break;
        }
        vTaskDelay(poll);
    }

    int ms = (int)((esp_timer_get_time() - t0) / 1000);
    if (elapsed_ms != NULL) {
        *elapsed_ms = ms;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BUSY still low after %d ms (timeout %d ms)", ms, timeout_ms);
    }
    return err;
}
