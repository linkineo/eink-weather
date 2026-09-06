#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

#include "epd_bus.h"

static const char *TAG = "epd";

#define EPD_SPI_HOST      SPI2_HOST
#define EPD_MAX_TRANSFER  4096
#define EPD_READ_MAX      8       /* REV (0x70) is the longest read: 7 bytes */
#define EPD_DC_COMMAND    0
#define EPD_DC_DATA       1
#define EPD_BUSY_POLL_MS  5

/* Reset pulse timing, transcribed from the tri-colour Raspberry Pi reference
 * driver EPD_7in5b_V2.c:40-48 (EPD_Reset): DEV_Delay_ms(200) / 5 / 200.
 * The Waveshare references disagree with each other -- the black/white driver
 * EPD_7in5_V2.c:38-46 uses 20 ms / 2 ms / 20 ms and the tri-colour ESP32 port
 * EPD_7in5b_V2.cpp:37-45 uses 200 ms / 2 ms / 200 ms -- and the spec gives no
 * minimum RST_N pulse width. The panel actually fitted here is the tri-colour
 * one, so wave 2 follows its own reference: the longest, most conservative
 * timing. The cost is 400 ms per reset, paid twice per boot (probe + init),
 * which is negligible next to a 15-25 s refresh. */
#define EPD_RST_HIGH_MS   200
#define EPD_RST_LOW_MS    5

typedef struct {
    spi_device_handle_t dev;
    int  rst;
    int  dc;
    int  busy;
    bool inited;
} epd_bus_ctx_t;

static epd_bus_ctx_t s_bus;

/* Staging buffer for fills and inversions, and receive buffer for register
 * reads. Both live in .bss (internal RAM) and are word aligned so the SPI
 * driver can hand them straight to DMA. The receive buffer is deliberately
 * larger than EPD_READ_MAX-rounded-up: DMA writes whole words, so the buffer
 * must be a multiple of 4 bytes. */
static WORD_ALIGNED_ATTR uint8_t s_chunk[EPD_CHUNK_BYTES];
static WORD_ALIGNED_ATTR uint8_t s_rx[16];

/* Called by the SPI driver just before a transaction starts, i.e. before CS
 * is asserted, so the DC line is already stable when the panel latches the
 * first bit. trans->user carries 0 for a command and 1 for data. */
static IRAM_ATTR void epd_bus_pre_cb(spi_transaction_t *trans)
{
    int dc = (int)(intptr_t)trans->user;
    gpio_set_level(s_bus.dc, dc);
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

    int hz = pins->spi_hz > 0 ? pins->spi_hz : CONFIG_EPD_SPI_HZ;

    /* RST and DC are plain outputs; CS is driven by the SPI peripheral.
     * RST idles high (the reset is active low, spec note 1.5-3). */
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << pins->rst) | (1ULL << pins->dc),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out_cfg), TAG, "gpio_config(rst/dc) failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(pins->rst, 1), TAG, "rst high failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(pins->dc, 0), TAG, "dc low failed");

    /* BUSY_N is an input with the internal pull-down enabled: with no panel
     * attached the pin then reads 0 deterministically instead of floating.
     * The controller's push-pull output easily overrides the ~45 kOhm
     * pull-down when a panel is present. */
    gpio_config_t busy_cfg = {
        .pin_bit_mask = (1ULL << pins->busy),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&busy_cfg), TAG, "gpio_config(busy) failed");

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

    /* SPI_DEVICE_3WIRE makes the peripheral use the MOSI pin (spid) for both
     * directions, SPI_DEVICE_HALFDUPLEX splits a transaction into a send
     * phase and a receive phase instead of running them at once. input_delay_ns
     * covers the GPIO-matrix round trip; at 4 MHz it still resolves to zero
     * compensation dummy bits, which is what the UC8179 expects (it starts
     * driving SDA immediately after the command byte). */
    spi_device_interface_config_t dev_cfg = {
        .mode           = 0,
        .clock_speed_hz = hz,
        .spics_io_num   = pins->cs,
        .queue_size     = 4,
        .flags          = SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_3WIRE,
        .input_delay_ns = 50,
        .pre_cb         = epd_bus_pre_cb,
    };

    s_bus.rst  = pins->rst;
    s_bus.dc   = pins->dc;
    s_bus.busy = pins->busy;

    esp_err_t err = spi_bus_add_device(EPD_SPI_HOST, &dev_cfg, &s_bus.dev);
    if (err != ESP_OK) {
        spi_bus_free(EPD_SPI_HOST);
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        return err;
    }

    s_bus.inited = true;
    ESP_LOGI(TAG, "bus ready: sck=%d mosi=%d cs=%d dc=%d rst=%d busy=%d %d Hz",
             pins->sck, pins->mosi, pins->cs, pins->dc, pins->rst, pins->busy, hz);
    return ESP_OK;
}

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

esp_err_t epd_bus_cmd(uint8_t cmd)
{
    ESP_RETURN_ON_FALSE(s_bus.inited, ESP_ERR_INVALID_STATE, TAG, "bus not initialised");

    spi_transaction_t t = {
        .flags  = SPI_TRANS_USE_TXDATA,
        .length = 8,
        .user   = (void *)(intptr_t)EPD_DC_COMMAND,
    };
    t.tx_data[0] = cmd;
    return spi_device_polling_transmit(s_bus.dev, &t);
}

esp_err_t epd_bus_data(const uint8_t *p, size_t n)
{
    ESP_RETURN_ON_FALSE(s_bus.inited, ESP_ERR_INVALID_STATE, TAG, "bus not initialised");
    ESP_RETURN_ON_FALSE(p != NULL || n == 0, ESP_ERR_INVALID_ARG, TAG, "data is NULL");

    if (n == 0) {
        return ESP_OK;
    }

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
}

esp_err_t epd_bus_data_fill(uint8_t value, size_t n)
{
    ESP_RETURN_ON_FALSE(s_bus.inited, ESP_ERR_INVALID_STATE, TAG, "bus not initialised");

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
}

esp_err_t epd_bus_data_inv(const uint8_t *src, size_t n)
{
    ESP_RETURN_ON_FALSE(s_bus.inited, ESP_ERR_INVALID_STATE, TAG, "bus not initialised");
    ESP_RETURN_ON_FALSE(src != NULL || n == 0, ESP_ERR_INVALID_ARG, TAG, "src is NULL");

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
}

esp_err_t epd_bus_read(uint8_t cmd, uint8_t *out, size_t n)
{
    ESP_RETURN_ON_FALSE(s_bus.inited, ESP_ERR_INVALID_STATE, TAG, "bus not initialised");
    ESP_RETURN_ON_FALSE(out != NULL && n > 0 && n <= EPD_READ_MAX, ESP_ERR_INVALID_ARG,
                        TAG, "bad read length %u", (unsigned)n);

    /* SPI_TRANS_CS_KEEP_ACTIVE is only accepted while the bus is acquired
     * (spi_master.c rejects it otherwise), and IDF 5.0 refuses a single
     * half-duplex transaction that has both a send and a receive phase, so
     * the read is two transactions under one bus lock. */
    ESP_RETURN_ON_ERROR(spi_device_acquire_bus(s_bus.dev, portMAX_DELAY),
                        TAG, "acquire_bus failed");

    spi_transaction_t tcmd = {
        .flags  = SPI_TRANS_USE_TXDATA | SPI_TRANS_CS_KEEP_ACTIVE,
        .length = 8,
        .user   = (void *)(intptr_t)EPD_DC_COMMAND,
    };
    tcmd.tx_data[0] = cmd;

    esp_err_t err = spi_device_polling_transmit(s_bus.dev, &tcmd);
    if (err == ESP_OK) {
        memset(s_rx, 0, sizeof(s_rx));
        /* Receive-only: no tx_buffer and no USE_TXDATA, so the send phase is
         * skipped and the peripheral releases the shared SDA line while the
         * controller drives it. rxlength must be non-zero for the receive
         * phase to run at all in half-duplex mode. */
        spi_transaction_t tdat = {
            .length    = 8 * n,
            .rxlength  = 8 * n,
            .tx_buffer = NULL,
            .rx_buffer = s_rx,
            .user      = (void *)(intptr_t)EPD_DC_DATA,
        };
        err = spi_device_polling_transmit(s_bus.dev, &tdat);
        if (err == ESP_OK) {
            memcpy(out, s_rx, n);
        }
    }

    spi_device_release_bus(s_bus.dev);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read 0x%02X failed: %s", cmd, esp_err_to_name(err));
    }
    return err;
}

void epd_bus_reset_pulse(void)
{
    if (!s_bus.inited) {
        return;
    }
    gpio_set_level(s_bus.rst, 1);
    epd_bus_delay_ms(EPD_RST_HIGH_MS);
    gpio_set_level(s_bus.rst, 0);
    epd_bus_delay_ms(EPD_RST_LOW_MS);
    gpio_set_level(s_bus.rst, 1);
    epd_bus_delay_ms(EPD_RST_HIGH_MS);
}

int epd_bus_busy_level(void)
{
    if (!s_bus.inited) {
        return -1;
    }
    return gpio_get_level(s_bus.busy);
}

esp_err_t epd_bus_wait_busy_high(int timeout_ms, int *elapsed_ms)
{
    ESP_RETURN_ON_FALSE(s_bus.inited, ESP_ERR_INVALID_STATE, TAG, "bus not initialised");

    /* BUSY_N low means the controller is working and must not be interrupted
     * (spec note 1.5-4). No status command is sent while waiting; only the
     * pin is sampled. */
    const int64_t t0 = esp_timer_get_time();
    const TickType_t poll = pdMS_TO_TICKS(EPD_BUSY_POLL_MS) > 0
                            ? pdMS_TO_TICKS(EPD_BUSY_POLL_MS) : 1;
    esp_err_t err = ESP_ERR_TIMEOUT;

    for (;;) {
        if (gpio_get_level(s_bus.busy) == 1) {
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
