/*
 * Bit-banged GPIO transport for the UC8179, transcribed from the Waveshare
 * e-Paper ESP32 Driver Board demo (DEV_Config.cpp, V1.0 2020-02-19):
 * GPIO_Config() and DEV_SPI_WriteByte().
 *
 * The deliberate differences from the reference are all delays, and they exist
 * for the same reason: Arduino's digitalWrite() costs 300-400 ns per call,
 * gpio_set_level() 50-80 ns, so a literal transcription runs roughly five
 * times faster than the code that is known to drive this panel.
 *
 *  - EPD_BB_EDGE_US after each clock edge. Without it the clock would run far
 *    above the controller's limits; 1 us per edge puts SCL at roughly 300-500
 *    kHz, comfortably inside the UC8179's write cycle (tscycw >= 100 ns).
 *
 *  - EPD_BB_DC_SETUP_US after the D/C line changes level, and
 *    EPD_BB_CS_SETUP_US / EPD_BB_CS_HOLD_US around the CS window.
 *
 *    Those three came out of the wave-8 hypothesis, which they then refuted;
 *    they are kept because they are correct and free, not because they fixed
 *    anything. The hypothesis: Waveshare's own unmodified Arduino demo
 *    (epd7in5b_V2-demo -- same pins, same command sequence, same bit-banged
 *    transport) clears this exact board and panel to a clean WHITE screen, so
 *    its DATA bytes reach the controller's RAM, while this firmware, with an
 *    identical command sequence, always produced random noise and every D/C
 *    oracle in epd_diag.c reported that the controller had seen D/C LOW for
 *    the data bytes. The one difference was timing: this transport raised D/C
 *    and pulled CS low roughly 100 ns later, where digitalWrite() would have
 *    taken ~400 ns per call. If the controller sampled D/C with less margin
 *    than that, it would latch the OLD level -- 0, command -- for every data
 *    byte, and all seven earlier runs would follow.
 *
 *    It does not. With the values below the diagnostic still returned
 *    DC_NOT_SEEN, and so did a run at 50 us of D/C setup and 10 us of CS
 *    setup/hold -- 2500 times the spec's tcds of 20 ns, and 100 times its
 *    tcss/tcsh of 100 ns. In that run a LONE 0x04 clocked out with D/C high
 *    and nothing before it still executed as PON (BUSY low 0 ms, high 131 ms:
 *    a full power-on, not the 41 ms POF pulse this controller also emits). So
 *    D/C setup time is not what separates this firmware from the Arduino demo,
 *    and whatever does is still unidentified.
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

/* Setup time granted to the D/C line after it changes level, before the CS
 * window that carries the byte it qualifies opens. 250 times the spec's tcds
 * of 20 ns, and paid once per command and once per plane rather than once per
 * byte, so its cost is unmeasurable. */
#define EPD_BB_DC_SETUP_US 5

/* Margins around the CS window: time between CS falling and the first rising
 * clock edge (spec tcss >= 100 ns), and between the last falling clock edge
 * and CS rising (spec tcsh >= 100 ns). Paid per byte, which is why they are
 * smaller than the D/C setup -- 4 us on top of a ~20 us byte, i.e. about 200 ms
 * on a 48000-byte plane, against a 15-25 s refresh. */
#define EPD_BB_CS_SETUP_US 2
#define EPD_BB_CS_HOLD_US  2

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
    int  dc_level;     /* last level driven on D/C, -1 = unknown */
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
    /* "Unknown" rather than 0: the very next epd_bb_set_dc() then pays the
     * setup delay whatever it asks for, instead of trusting a level this
     * function drove before the pin was even in its final configuration. */
    s_bb.dc_level = -1;

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

    const int want = (level != 0) ? 1 : 0;
    const bool changed = (s_bb.dc_level != want);

    gpio_set_level(s_bb.dc, want);
    if (s_dc_mirror >= 0) {
        /* The mirror pin is configured by whoever enabled it; this only
         * follows the D/C level, on the same side of the byte. */
        gpio_set_level((gpio_num_t)s_dc_mirror, want);
    }
    s_bb.dc_level = want;

    /* Setup time, and only when the line really moved: a plane is 48000 calls
     * to epd_bb_data()'s inner loop with D/C already high, and paying 5 us on
     * each of those would add four minutes to a refresh for nothing. */
    if (changed) {
        esp_rom_delay_us(EPD_BB_DC_SETUP_US);
    }
}

void epd_bb_set_dc_mirror(int gpio)
{
    s_dc_mirror = gpio;
}

/* Open the CS window: CS low, then tcss of setup before any clock edge.
 * Factored out with its closing counterpart so the 8-bit and the 9-bit writer
 * below stay bit-for-bit identical in timing. */
static inline void epd_bb_cs_assert(void)
{
    gpio_set_level(s_bb.cs, 0);
    esp_rom_delay_us(EPD_BB_CS_SETUP_US);
}

/* Close it: tcsh of hold after the last falling clock edge, then CS high. */
static inline void epd_bb_cs_release(void)
{
    esp_rom_delay_us(EPD_BB_CS_HOLD_US);
    gpio_set_level(s_bb.cs, 1);
}

/* One bit out on MOSI, clocked in by the controller on the rising edge of SCL
 * (spec 3.3-2-2, Table 3-2). The two delays are a deliberate difference from
 * DEV_SPI_WriteByte(); see the file header. */
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
    epd_bb_cs_assert();
    for (int i = 0; i < 8; i++) {
        epd_bb_clock_bit(b & 0x80u);
        b = (uint8_t)(b << 1);
    }
    epd_bb_cs_release();
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
    epd_bb_cs_assert();
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
    epd_bb_cs_release();
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
