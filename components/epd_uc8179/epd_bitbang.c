/*
 * Bit-banged GPIO transport for the UC8179, transcribed from the Waveshare
 * e-Paper ESP32 Driver Board demo (DEV_Config.cpp, V1.0 2020-02-19):
 * GPIO_Config() and DEV_SPI_WriteByte().
 *
 * The only deliberate difference is an explicit 1 us delay after each clock
 * edge. Arduino's digitalWrite() is slow enough that the reference needs no
 * delay; gpio_set_level() is not, so without one the clock would run far
 * above the controller's limits. 1 us per edge puts SCL at roughly 300-500
 * kHz, comfortably inside the UC8179's write cycle (tscycw >= 100 ns).
 *
 * Write only, deliberately: the panel spec allows nothing else in serial mode
 * (see epd_bitbang.h), so the transcription of DEV_SPI_ReadByte() that used to
 * live here is gone.
 *
 * This file also owns the panel power rail (epd_power_init() / epd_power()),
 * because it is the module every entry point goes through and the rail has to
 * be up before any pin below is touched. Waveshare's "Loader" firmware for
 * this board does the same thing in EPD_initSPI(), where PIN_SPI_CS_S (GPIO2)
 * and PIN_SPI_PWR (GPIO33) go high before any other pin is even configured.
 */
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

#include "epd_bitbang.h"
#include "epd_bus.h"

static const char *TAG = "epd";

/* Half period of the bit-banged clock, in microseconds. */
#define EPD_BB_EDGE_US 1

/* Reset pulse timing, transcribed from the tri-colour Raspberry Pi reference
 * driver EPD_7in5b_V2.c:40-48 (EPD_Reset): DEV_Delay_ms(200) / 5 / 200.
 * The Waveshare references disagree with each other -- the black/white driver
 * EPD_7in5_V2.c:38-46 uses 20 ms / 2 ms / 20 ms and the tri-colour ESP32 port
 * EPD_7in5b_V2.cpp:37-45 uses 200 ms / 2 ms / 200 ms -- and the spec gives no
 * minimum RST_N pulse width. The panel actually fitted here is the tri-colour
 * one, so this follows its own reference: the longest, most conservative
 * timing. The cost is 400 ms per reset, paid twice per boot (probe + init),
 * which is negligible next to a 15-25 s refresh. */
#define EPD_RST_HIGH_MS 200
#define EPD_RST_LOW_MS  5

/* Time given to the panel rail after the enable pin first goes high. The
 * RT9193 LDO itself is up in well under a millisecond; what this waits for is
 * the controller behind it, whose power-on reset must complete before the
 * first byte is clocked in. 200 ms is the same order as the reset pulse above
 * and is paid exactly once per boot. */
#define EPD_PWR_SETTLE_MS 200

typedef struct {
    gpio_num_t sck, mosi, cs, dc, rst, busy;
    bool ctrl_ready;   /* rst / dc / busy configured */
    bool data_ready;   /* sck / mosi / cs owned by this module */
} epd_bb_ctx_t;

static epd_bb_ctx_t s_bb;

/* Panel power rail. Separate from s_bb because it outlives every transport:
 * the pins are configured once and never handed to a peripheral. */
typedef struct {
    int  pwr;          /* enable pin, -1 = this board has none */
    int  pwr_aux;      /* second pin the reference firmware drives, -1 = none */
    bool configured;   /* epd_power_init() has run */
    bool ever_on;      /* a power pin has already been taken high once */
} epd_pwr_ctx_t;

static epd_pwr_ctx_t s_pwr = { .pwr = -1, .pwr_aux = -1 };

/* Diagnostic only: a second pin driven alongside DC, -1 = none. See
 * epd_bb_set_dc_mirror(). */
static int s_dc_mirror = -1;

/* One power pin as a plain GPIO output. The output data register is left
 * alone, so a pin that is already driving high keeps doing so across a
 * repeated epd_power_init(); on the very first call it holds 0, which is the
 * rail's off state and the state the panel comes out of reset in anyway. */
static esp_err_t epd_pwr_config_pin(int gpio)
{
    if (gpio < 0) {
        return ESP_OK;
    }
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << gpio),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    return gpio_config(&cfg);
}

esp_err_t epd_power_init(const epd_pins_t *pins)
{
    ESP_RETURN_ON_FALSE(pins != NULL, ESP_ERR_INVALID_ARG, TAG, "pins is NULL");

    s_pwr.pwr     = pins->pwr;
    s_pwr.pwr_aux = pins->pwr_aux;

    ESP_RETURN_ON_ERROR(epd_pwr_config_pin(s_pwr.pwr), TAG,
                        "gpio_config(pwr=%d) failed", s_pwr.pwr);
    ESP_RETURN_ON_ERROR(epd_pwr_config_pin(s_pwr.pwr_aux), TAG,
                        "gpio_config(pwr_aux=%d) failed", s_pwr.pwr_aux);

    s_pwr.configured = true;
    return ESP_OK;
}

void epd_bb_set_power(int on)
{
    if (!s_pwr.configured) {
        return;
    }
    if (s_pwr.pwr >= 0) {
        gpio_set_level((gpio_num_t)s_pwr.pwr, on != 0);
    }
    if (s_pwr.pwr_aux >= 0) {
        gpio_set_level((gpio_num_t)s_pwr.pwr_aux, on != 0);
    }
}

esp_err_t epd_power(bool on)
{
    ESP_RETURN_ON_FALSE(s_pwr.configured, ESP_ERR_INVALID_STATE, TAG,
                        "call epd_power_init() first");

    epd_bb_set_power(on ? 1 : 0);

    /* The settling delay is only owed on the first switch-on: the rail is
     * never taken down again inside one boot, so the second and later
     * epd_bb_init_ctrl() calls of the normal flow must not each add 200 ms. */
    if (on && !s_pwr.ever_on && (s_pwr.pwr >= 0 || s_pwr.pwr_aux >= 0)) {
        s_pwr.ever_on = true;
        ESP_LOGI(TAG, "panel power: gpio%d=1 gpio%d=1", s_pwr.pwr, s_pwr.pwr_aux);
        epd_bus_delay_ms(EPD_PWR_SETTLE_MS);
    }
    return ESP_OK;
}

esp_err_t epd_bb_init_ctrl(const epd_pins_t *pins)
{
    ESP_RETURN_ON_FALSE(pins != NULL, ESP_ERR_INVALID_ARG, TAG, "pins is NULL");

    /* Power BEFORE anything else. Nothing below means anything on an unpowered
     * panel, and with the rail off the controller still answers commands on
     * BUSY through the leakage current of the signal lines -- which is what
     * made three waves of diagnostics read "the controller does not see D/C". */
    ESP_RETURN_ON_ERROR(epd_power_init(pins), TAG, "power pins failed");
    ESP_RETURN_ON_ERROR(epd_power(true), TAG, "panel power-on failed");

    /* A mirror belongs to one diagnostic run; a fresh init never inherits it. */
    s_dc_mirror = -1;

    s_bb.sck  = (gpio_num_t)pins->sck;
    s_bb.mosi = (gpio_num_t)pins->mosi;
    s_bb.cs   = (gpio_num_t)pins->cs;
    s_bb.dc   = (gpio_num_t)pins->dc;
    s_bb.rst  = (gpio_num_t)pins->rst;
    s_bb.busy = (gpio_num_t)pins->busy;

    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << pins->rst) | (1ULL << pins->dc),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&out_cfg), TAG, "gpio_config(rst/dc) failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(s_bb.rst, 1), TAG, "rst high failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(s_bb.dc, 0), TAG, "dc low failed");

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

    s_bb.ctrl_ready = true;
    return ESP_OK;
}

esp_err_t epd_bb_init(const epd_pins_t *pins)
{
    ESP_RETURN_ON_ERROR(epd_bb_init_ctrl(pins), TAG, "control pins failed");

    /* GPIO_Config(): CS/SCK/MOSI outputs, CS high, SCK low. */
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << pins->sck) | (1ULL << pins->mosi) | (1ULL << pins->cs),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io_cfg), TAG, "gpio_config(sck/mosi/cs) failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(s_bb.cs, 1), TAG, "cs high failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(s_bb.sck, 0), TAG, "sck low failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(s_bb.mosi, 0), TAG, "mosi low failed");

    s_bb.data_ready = true;
    return ESP_OK;
}

void epd_bb_release(void)
{
    if (!s_bb.data_ready) {
        return;
    }
    /* gpio_reset_pin() detaches the pin from any peripheral signal and leaves
     * it a pulled-up input, which is the state spi_bus_initialize() expects to
     * find. RST/DC/BUSY are deliberately left alone: both transports use the
     * helpers below for them. */
    gpio_reset_pin(s_bb.sck);
    gpio_reset_pin(s_bb.mosi);
    gpio_reset_pin(s_bb.cs);
    s_bb.data_ready = false;
}

bool epd_bb_ctrl_ready(void)
{
    return s_bb.ctrl_ready;
}

bool epd_bb_data_ready(void)
{
    return s_bb.data_ready;
}

void epd_bb_set_dc(int level)
{
    if (!s_bb.ctrl_ready) {
        return;
    }
    gpio_set_level(s_bb.dc, level != 0);
    if (s_dc_mirror >= 0) {
        /* The mirror pin is configured by whoever enabled it; this only
         * follows the D/C level, on the same side of the byte. */
        gpio_set_level((gpio_num_t)s_dc_mirror, level != 0);
    }
}

void epd_bb_set_dc_mirror(int gpio)
{
    s_dc_mirror = gpio;
}

/* One bit out on MOSI, clocked in by the controller on the rising edge of SCL
 * (spec 3.3-2-2, Table 3-2). The two delays are the only deliberate difference
 * from DEV_SPI_WriteByte(); factoring them out here is what keeps the 8-bit
 * and the 9-bit writer below bit-for-bit identical in timing. */
static inline void epd_bb_clock_bit(int bit)
{
    gpio_set_level(s_bb.mosi, bit ? 1 : 0);
    gpio_set_level(s_bb.sck, 1);
    esp_rom_delay_us(EPD_BB_EDGE_US);
    gpio_set_level(s_bb.sck, 0);
    esp_rom_delay_us(EPD_BB_EDGE_US);
}

/* DEV_SPI_WriteByte(): CS low, then for each bit MSB first set MOSI and pulse
 * SCK high/low, then CS high. */
void epd_bb_write_byte(uint8_t b)
{
    if (!s_bb.data_ready) {
        return;
    }
    gpio_set_level(s_bb.cs, 0);
    for (int i = 0; i < 8; i++) {
        epd_bb_clock_bit(b & 0x80u);
        b = (uint8_t)(b << 1);
    }
    gpio_set_level(s_bb.cs, 1);
}

/* One 9-bit frame: the byte plus its D/C bit, inside a single CS window.
 *
 * The spec's 3-wire order (3.3-2-3) is "DC bit, D7 to D0 bit", i.e.
 * dc_first = true; dc_first = false clocks the byte out first and the D/C bit
 * last, which is the other convention a controller could plausibly use and
 * which epd_wire_diag() has to rule out rather than assume.
 *
 * The physical D/C pin is deliberately NOT touched: in 3-wire mode it is not a
 * bus signal at all ("The pin DC can be connected to an external ground"), and
 * leaving it low is exactly the tie-low the spec's Table 7-3 asks for. Callers
 * that care set it low once before the first frame. */
void epd_bb_write_frame9(int dc_bit, uint8_t b, bool dc_first)
{
    if (!s_bb.data_ready) {
        return;
    }
    gpio_set_level(s_bb.cs, 0);
    if (dc_first) {
        epd_bb_clock_bit(dc_bit != 0);
    }
    for (int i = 0; i < 8; i++) {
        epd_bb_clock_bit(b & 0x80u);
        b = (uint8_t)(b << 1);
    }
    if (!dc_first) {
        epd_bb_clock_bit(dc_bit != 0);
    }
    gpio_set_level(s_bb.cs, 1);
}

void epd_bb_cmd(uint8_t cmd)
{
    epd_bb_set_dc(0);
    epd_bb_write_byte(cmd);
}

void epd_bb_data(const uint8_t *p, size_t n)
{
    if (p == NULL) {
        return;
    }
    epd_bb_set_dc(1);
    for (size_t i = 0; i < n; i++) {
        epd_bb_write_byte(p[i]);
    }
}

void epd_bb_reset_pulse(void)
{
    if (!s_bb.ctrl_ready) {
        return;
    }
    gpio_set_level(s_bb.rst, 1);
    epd_bus_delay_ms(EPD_RST_HIGH_MS);
    gpio_set_level(s_bb.rst, 0);
    epd_bus_delay_ms(EPD_RST_LOW_MS);
    gpio_set_level(s_bb.rst, 1);
    epd_bus_delay_ms(EPD_RST_HIGH_MS);
}

int epd_bb_busy_level(void)
{
    if (!s_bb.ctrl_ready) {
        return -1;
    }
    return gpio_get_level(s_bb.busy);
}
