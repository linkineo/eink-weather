/*
 * UC8179 command sequences for the Waveshare 7.5inch e-Paper (B) V2 and
 * 7.5inch e-Paper V2 panels.
 *
 * Every sequence below is transcribed from the Waveshare Raspberry Pi
 * reference driver and nothing else:
 *   - tri-colour: EPD_7in5b_V2.c (V1.0, 2024-08-07)
 *   - black/white: EPD_7in5_V2.c
 * Register codes and bit positions are cross-checked against the
 * "7.5inch e-Paper V2 Specification" text (Revision 2.0).
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

#include "epd_bitbang.h"
#include "epd_bus.h"
#include "epd_uc8179.h"

static const char *TAG = "epd";

/* UC8179 command codes (spec command table, lines 700-800 of the spec text). */
#define UC8179_PSR   0x00   /* Panel setting */
#define UC8179_PWR   0x01   /* Power setting */
#define UC8179_POF   0x02   /* Power off */
#define UC8179_PON   0x04   /* Power on */
#define UC8179_BTST  0x06   /* Booster soft start */
#define UC8179_DSLP  0x07   /* Deep sleep */
#define UC8179_DTM1  0x10   /* Data start transmission 1 */
#define UC8179_DRF   0x12   /* Display refresh */
#define UC8179_DTM2  0x13   /* Data start transmission 2 */
#define UC8179_DUSPI 0x15   /* Dual SPI mode */
#define UC8179_TSC   0x40   /* Temperature sensor calibration (read) */
#define UC8179_PBC   0x44   /* Panel break / glass check (read) */
#define UC8179_CDI   0x50   /* VCOM and data interval setting */
#define UC8179_TCON  0x60   /* TCON setting */
#define UC8179_TRES  0x61   /* Resolution setting */
#define UC8179_REV   0x70   /* Revision (read) */
#define UC8179_FLG   0x71   /* Get status (read) */

/* Delay after DRF before the BUSY line may be trusted. The tri-colour
 * reference uses 10 ms and the black/white one 100 ms; the spec only demands
 * 200 us, so the longer of the two is used for both panels. */
#define EPD_DRF_SETTLE_MS      100
/* Delay after PON before waiting on BUSY (reference: DEV_Delay_ms(100)). */
#define EPD_PON_SETTLE_MS      100
/* Probe: how long to watch BUSY for the low-then-high power-on signature. */
#define EPD_PROBE_WINDOW_MS    2000
/* Probe: short timeout for the closing power-off. */
#define EPD_PROBE_POF_TIMEOUT_MS 5000
/* Settle time between the power-on signature and the register reads. */
#define EPD_PROBE_READ_SETTLE_MS 10

#if CONFIG_EPD_PANEL_7IN5_V2
#define EPD_PANEL_IS_BW 1
#else
#define EPD_PANEL_IS_BW 0
#endif

/* ------------------------------------------------------------------ helpers */

static esp_err_t epd_cmd_data(uint8_t cmd, const uint8_t *data, size_t n)
{
    ESP_RETURN_ON_ERROR(epd_bus_cmd(cmd), TAG, "cmd 0x%02X failed", cmd);
    if (n > 0) {
        ESP_RETURN_ON_ERROR(epd_bus_data(data, n), TAG, "data for 0x%02X failed", cmd);
    }
    return ESP_OK;
}

static esp_err_t epd_cmd_byte(uint8_t cmd, uint8_t value)
{
    return epd_cmd_data(cmd, &value, 1);
}

/* Same, over the bit-bang transport used by the probe (no error path: the
 * GPIO writes cannot fail once the pins are configured). */
static void epd_bb_cmd_data(uint8_t cmd, const uint8_t *data, size_t n)
{
    epd_bb_cmd(cmd);
    if (n > 0) {
        epd_bb_data(data, n);
    }
}

/* Send one full plane and log how long the transfer itself took, which is the
 * number that separates a working data path from a silently discarded one
 * (and tells the two Kconfig data paths apart: ~0.1 s over SPI at 4 MHz,
 * ~1 s bit-banged). */
static esp_err_t epd_log_plane(int plane, esp_err_t err, int64_t t0)
{
    const int ms = (int)((esp_timer_get_time() - t0) / 1000);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "plane %d: %d bytes in %d ms", plane, (int)EPD_PLANE_BYTES, ms);
    }
    return err;
}

/* PWR 0x01: VGH=20V, VGL=-20V, VDH=15V, VDL=-15V.
 * EPD_7in5b_V2.c:108-112 / EPD_7in5_V2.c:113-117. */
static const uint8_t k_pwr[4]  = { 0x07, 0x07, 0x3F, 0x3F };
/* BTST 0x06: enhanced display drive.
 * EPD_7in5b_V2.c:115-119 / EPD_7in5_V2.c:120-124. */
static const uint8_t k_btst[4] = { 0x17, 0x17, 0x28, 0x17 };
/* TRES 0x61: HRES 800 (0x0320), VRES 480 (0x01E0).
 * EPD_7in5b_V2.c:128-132 / EPD_7in5_V2.c:133-137. */
static const uint8_t k_tres[4] = { 0x03, 0x20, 0x01, 0xE0 };
/* CDI 0x50: EPD_7in5b_V2.c:137-139 (0x11 0x07) vs EPD_7in5_V2.c:145-147
 * (0x10 0x07). PSR 0x00: 0x0F = BWR OTP LUT, 0x1F = BW OTP LUT. */
#if EPD_PANEL_IS_BW
static const uint8_t k_psr     = 0x1F;
static const uint8_t k_cdi[2]  = { 0x10, 0x07 };
#else
static const uint8_t k_psr     = 0x0F;
static const uint8_t k_cdi[2]  = { 0x11, 0x07 };
#endif

/* Power on, then wait for BUSY to be released. Reference:
 * EPD_7in5b_V2.c:121-123. */
static esp_err_t epd_power_on(void)
{
    ESP_RETURN_ON_ERROR(epd_bus_cmd(UC8179_PON), TAG, "PON failed");
    epd_bus_delay_ms(EPD_PON_SETTLE_MS);

    int ms = 0;
    esp_err_t err = epd_bus_wait_busy_high(CONFIG_EPD_BUSY_TIMEOUT_MS, &ms);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "power-on wait failed after %d ms: %s", ms, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "power-on: BUSY released after %d ms", ms);
    return ESP_OK;
}

/* DRF, settle, then wait for the refresh to complete. Reference:
 * EPD_7in5b_V2.c:93-98 (TurnOnDisplay). */
static esp_err_t epd_turn_on_display(void)
{
    ESP_RETURN_ON_ERROR(epd_bus_cmd(UC8179_DRF), TAG, "DRF failed");
    epd_bus_delay_ms(EPD_DRF_SETTLE_MS);

    int ms = 0;
    esp_err_t err = epd_bus_wait_busy_high(CONFIG_EPD_BUSY_TIMEOUT_MS, &ms);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "refresh wait failed after %d ms: %s", ms, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "refresh: BUSY released after %d ms", ms);
    return ESP_OK;
}

/* --------------------------------------------------------------- public API */

const char *epd_panel_name(void)
{
#if EPD_PANEL_IS_BW
    return "7.5in V2 800x480 BW";
#else
    return "7.5in (B) V2 800x480 BWR";
#endif
}

esp_err_t epd_init(void)
{
    ESP_RETURN_ON_FALSE(epd_bus_ready(), ESP_ERR_INVALID_STATE, TAG, "call epd_bus_init() first");

    ESP_LOGI(TAG, "init %s", epd_panel_name());
    epd_bus_reset_pulse();

    ESP_RETURN_ON_ERROR(epd_cmd_data(UC8179_PWR, k_pwr, sizeof(k_pwr)), TAG, "PWR failed");
    ESP_RETURN_ON_ERROR(epd_cmd_data(UC8179_BTST, k_btst, sizeof(k_btst)), TAG, "BTST failed");
    ESP_RETURN_ON_ERROR(epd_power_on(), TAG, "power on failed");
    ESP_RETURN_ON_ERROR(epd_cmd_byte(UC8179_PSR, k_psr), TAG, "PSR failed");
    ESP_RETURN_ON_ERROR(epd_cmd_data(UC8179_TRES, k_tres, sizeof(k_tres)), TAG, "TRES failed");
    ESP_RETURN_ON_ERROR(epd_cmd_byte(UC8179_DUSPI, 0x00), TAG, "DUSPI failed");
    ESP_RETURN_ON_ERROR(epd_cmd_data(UC8179_CDI, k_cdi, sizeof(k_cdi)), TAG, "CDI failed");
    ESP_RETURN_ON_ERROR(epd_cmd_byte(UC8179_TCON, 0x22), TAG, "TCON failed");

    return ESP_OK;
}

esp_err_t epd_clear(void)
{
    ESP_RETURN_ON_FALSE(epd_bus_ready(), ESP_ERR_INVALID_STATE, TAG, "call epd_bus_init() first");

    /* EPD_7in5b_V2.c:204-222 (Clear): DTM1 all 0xFF, DTM2 all 0x00.
     * EPD_7in5_V2.c:252-279 uses the same two values. */
    int64_t t0 = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(epd_bus_cmd(UC8179_DTM1), TAG, "DTM1 failed");
    ESP_RETURN_ON_ERROR(epd_log_plane(1, epd_bus_data_fill(0xFF, EPD_PLANE_BYTES), t0),
                        TAG, "DTM1 fill failed");

    t0 = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(epd_bus_cmd(UC8179_DTM2), TAG, "DTM2 failed");
    ESP_RETURN_ON_ERROR(epd_log_plane(2, epd_bus_data_fill(0x00, EPD_PLANE_BYTES), t0),
                        TAG, "DTM2 fill failed");

    return epd_turn_on_display();
}

esp_err_t epd_display(const uint8_t *black, const uint8_t *red)
{
    ESP_RETURN_ON_FALSE(epd_bus_ready(), ESP_ERR_INVALID_STATE, TAG, "call epd_bus_init() first");
    ESP_RETURN_ON_FALSE(black != NULL, ESP_ERR_INVALID_ARG, TAG, "black plane is NULL");

    /* DTM1 always carries the black plane verbatim: bit 1 = white, 0 = black. */
    int64_t t0 = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(epd_bus_cmd(UC8179_DTM1), TAG, "DTM1 failed");
    ESP_RETURN_ON_ERROR(epd_log_plane(1, epd_bus_data(black, EPD_PLANE_BYTES), t0),
                        TAG, "DTM1 data failed");

    t0 = esp_timer_get_time();
    ESP_RETURN_ON_ERROR(epd_bus_cmd(UC8179_DTM2), TAG, "DTM2 failed");

#if EPD_PANEL_IS_BW
    /* EPD_7in5_V2.c:314-330: the black/white panel wants the complement of
     * the image in DTM2. The reference inverts the caller's buffer in place;
     * here the inversion is staged in the driver's chunk buffer instead. */
    (void)red;
    ESP_RETURN_ON_ERROR(epd_log_plane(2, epd_bus_data_inv(black, EPD_PLANE_BYTES), t0),
                        TAG, "DTM2 data failed");
#else
    /* EPD_7in5b_V2.c:283-288: the tri-colour panel wants the red plane
     * inverted (~ryimage[i]). The application's red buffer uses 0xFF for
     * "no red", so an absent buffer is equivalent to sending 0x00. */
    if (red != NULL) {
        ESP_RETURN_ON_ERROR(epd_log_plane(2, epd_bus_data_inv(red, EPD_PLANE_BYTES), t0),
                            TAG, "DTM2 data failed");
    } else {
        ESP_RETURN_ON_ERROR(epd_log_plane(2, epd_bus_data_fill(0x00, EPD_PLANE_BYTES), t0),
                            TAG, "DTM2 fill failed");
    }
#endif

    return epd_turn_on_display();
}

esp_err_t epd_sleep(void)
{
    ESP_RETURN_ON_FALSE(epd_bus_ready(), ESP_ERR_INVALID_STATE, TAG, "call epd_bus_init() first");

    /* EPD_7in5b_V2.c:364-373 (Sleep): CDI 0xF7, POF + wait, DSLP 0xA5. */
    ESP_RETURN_ON_ERROR(epd_cmd_byte(UC8179_CDI, 0xF7), TAG, "CDI failed");
    ESP_RETURN_ON_ERROR(epd_bus_cmd(UC8179_POF), TAG, "POF failed");

    int ms = 0;
    esp_err_t err = epd_bus_wait_busy_high(CONFIG_EPD_BUSY_TIMEOUT_MS, &ms);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "power-off wait failed after %d ms: %s", ms, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "power-off: BUSY released after %d ms", ms);

    ESP_RETURN_ON_ERROR(epd_cmd_byte(UC8179_DSLP, 0xA5), TAG, "DSLP failed");
    return ESP_OK;
}

/* -------------------------------------------------------------------- probe */

static void epd_probe_sleep_1ms(unsigned iteration)
{
    /* One tick is at most 1 ms only when the tick rate is >= 1 kHz. The
     * expression folds to a constant at compile time. */
    if (pdMS_TO_TICKS(1) >= 1) {
        vTaskDelay(1);
    } else {
        esp_rom_delay_us(1000);
        /* Yield now and then so the idle task is not starved for the whole
         * probe window when the tick is coarser than 1 ms. */
        if ((iteration % 50U) == 49U) {
            vTaskDelay(1);
        }
    }
}

static void epd_probe_set_verdict(epd_probe_result_t *r, const char *verdict)
{
    snprintf(r->verdict, sizeof(r->verdict), "%s", verdict);
    r->present = (strcmp(verdict, "PRESENT") == 0);
}

/* A read that came back from an undriven SDA line rather than from the
 * controller: every byte 0xFF, or the 0x7F-then-0xFF pattern wave 2 saw (the
 * accumulator starts at 0xFF and its top bit is shifted out before the first
 * sample, so a line that idles high yields 0x7F for the first byte and 0xFF
 * for the rest). */
static bool epd_read_is_float_high(const uint8_t *p, size_t n)
{
    if (n == 0 || (p[0] != 0xFF && p[0] != 0x7F)) {
        return false;
    }
    for (size_t i = 1; i < n; i++) {
        if (p[i] != 0xFF) {
            return false;
        }
    }
    return true;
}

/* A read that came back as a line held low. */
static bool epd_read_is_zero(const uint8_t *p, size_t n)
{
    if (n == 0) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (p[i] != 0x00) {
            return false;
        }
    }
    return true;
}

esp_err_t epd_probe(const epd_pins_t *pins, epd_probe_result_t *out)
{
    ESP_RETURN_ON_FALSE(pins != NULL, ESP_ERR_INVALID_ARG, TAG, "pins is NULL");
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
    ESP_RETURN_ON_FALSE(!epd_bus_ready(), ESP_ERR_INVALID_STATE, TAG,
                        "epd_probe() must run before epd_bus_init()");

    memset(out, 0, sizeof(*out));
    out->busy_low_ms     = -1;
    out->busy_release_ms = -1;
    out->pbc_pass        = -1;
    epd_probe_set_verdict(out, "UNCERTAIN");

    /* The whole probe runs on bit-banged GPIO. The SPI peripheral cannot read
     * this 3-wire bus (wave 2: every register came back as a floating line),
     * while Waveshare's own ESP32 port drives the pins by hand and does read
     * it, so the probe follows that path and hands the pins over afterwards. */
    ESP_RETURN_ON_ERROR(epd_bb_init(pins), TAG, "bit-bang init failed");

    /* 1. Reset, then look at BUSY. A healthy controller idles high. */
    epd_bb_reset_pulse();
    out->busy_after_reset = epd_bb_busy_level();

    /* 2. Bring the analogue rails up and watch BUSY. On a present panel PON
     *    pulls BUSY_N low while the booster and regulators start and the
     *    internal temperature sensor takes its one-time reading, then
     *    releases it (spec, Power ON section: "When all voltages are ready,
     *    the BUSY_N signal will return to high"). A pin held down by the
     *    internal pull-down cannot produce that low-then-high edge on
     *    command, which makes it the primary presence signature. */
    epd_bb_cmd_data(UC8179_PWR, k_pwr, sizeof(k_pwr));
    epd_bb_cmd_data(UC8179_BTST, k_btst, sizeof(k_btst));
    epd_bb_cmd(UC8179_PON);

    const int64_t t0 = esp_timer_get_time();
    bool seen_low = false;
    for (unsigned i = 0;; i++) {
        int level = epd_bb_busy_level();
        int ms = (int)((esp_timer_get_time() - t0) / 1000);

        if (!seen_low) {
            if (level == 0) {
                seen_low = true;
                out->busy_low_ms = ms;
            }
        } else if (level == 1) {
            out->busy_release_ms = ms;
            break;
        }
        if (ms >= EPD_PROBE_WINDOW_MS) {
            break;
        }
        epd_probe_sleep_1ms(i);
    }
    ESP_LOGI(TAG, "probe: busy_after_reset=%d busy_low_ms=%d busy_release_ms=%d",
             out->busy_after_reset, out->busy_low_ms, out->busy_release_ms);

    /* 3. Read the identification and status registers. The spec gives no
     *    separate sensing time for TSC (0x40) with the internal sensor: PON
     *    already performed the one-time sensing, so a short settle is enough. */
    epd_bus_delay_ms(EPD_PROBE_READ_SETTLE_MS);
    if (epd_bb_busy_level() != 1) {
        ESP_LOGW(TAG, "probe: BUSY still low, register reads may be unreliable");
    }

    /* REV (0x70): PROD_REV[23:0], LUT_REV[23:0], CHIP_REV. CHIP_REV is fixed
     * at 00001100b = 0x0C (spec, Revision section), which is what makes it
     * the reads_ok witness. */
    epd_bb_read(UC8179_REV, out->rev, sizeof(out->rev));

    /* FLG (0x71): b6 PTL_FLAG, b5 I2C_ERR, b4 I2C_BUSYN, b3 DATA_FLAG,
     * b2 PON, b1 POF, b0 BUSY_N (spec, Get Status section; reset value
     * 0x13). PON should read 1 and BUSY_N 1 while idle and powered. */
    uint8_t flg = 0;
    epd_bb_read(UC8179_FLG, &flg, 1);
    out->flg = flg;

    /* TSC (0x40): byte 0 is TS[7:0], the internal sensor reading in degrees
     * Celsius, two's complement (spec table: 1110_0111 = -25, 0000_0000 = 0,
     * 0001_1001 = +25). Byte 1 only carries D[2:0] for an external sensor. */
    uint8_t tsc[2] = { 0, 0 };
    epd_bb_read(UC8179_TSC, tsc, sizeof(tsc));
    out->temp_c = (int8_t)tsc[0];

    /* PBC (0x44): bit 0 is PSTA, 1 = panel glass check pass. */
    uint8_t pbc = 0;
    epd_bb_read(UC8179_PBC, &pbc, 1);
    out->pbc_pass = pbc & 0x01;

    out->reads_ok = (out->rev[6] == 0x0C);
    ESP_LOGI(TAG, "probe: reads=%s rev=%02x %02x %02x %02x %02x %02x %02x "
                  "flg=0x%02X tsc=%02x %02x pbc=0x%02X",
             out->reads_ok ? "ok" : "bad",
             out->rev[0], out->rev[1], out->rev[2], out->rev[3],
             out->rev[4], out->rev[5], out->rev[6],
             flg, tsc[0], tsc[1], pbc);

    /* 4. Verdict. Two independent witnesses: a CHIP_REV of 0x0C means the
     *    read path really carried controller data, and a complete BUSY
     *    low-then-high signature means something answered a command on a pin
     *    the ESP32 pulls down. Either one on its own is conclusive. */
    const bool sig_complete = (out->busy_low_ms >= 0 && out->busy_release_ms >= 0);
    const bool all_float = epd_read_is_float_high(out->rev, sizeof(out->rev))
                           && epd_read_is_float_high(&flg, 1)
                           && epd_read_is_float_high(tsc, sizeof(tsc))
                           && epd_read_is_float_high(&pbc, 1);
    const bool all_zero = epd_read_is_zero(out->rev, sizeof(out->rev))
                          && epd_read_is_zero(&flg, 1)
                          && epd_read_is_zero(tsc, sizeof(tsc))
                          && epd_read_is_zero(&pbc, 1);

    if (out->reads_ok || sig_complete) {
        epd_probe_set_verdict(out, "PRESENT");
    } else if (all_zero || all_float) {
        epd_probe_set_verdict(out, "ABSENT");
    } else {
        epd_probe_set_verdict(out, "UNCERTAIN");
    }

    /* 5. Power the panel back down so epd_init() starts from a known state.
     *    A short timeout: the probe must never hang. */
    epd_bb_cmd(UC8179_POF);
    int ms = 0;
    if (epd_bus_wait_busy_high(EPD_PROBE_POF_TIMEOUT_MS, &ms) == ESP_OK) {
        ESP_LOGI(TAG, "probe: power-off: BUSY released after %d ms", ms);
    } else {
        ESP_LOGW(TAG, "probe: power-off wait gave up after %d ms", ms);
    }

    /* 6. Hand SCK/MOSI/CS back so epd_bus_init() can claim them. RST, DC and
     *    BUSY stay configured -- epd_bitbang.c owns them for both transports. */
    epd_bb_release();
    return ESP_OK;
}

void epd_probe_format(const epd_probe_result_t *r, char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }
    if (r == NULL) {
        buf[0] = '\0';
        return;
    }
    /* reads=bad marks chip_rev/prod/lut/flg/temp/pbc as garbage from an
     * undriven line: the BUSY fields are still trustworthy. */
    snprintf(buf, len,
             "probe verdict=%s reads=%s chip_rev=0x%02X prod=%02x.%02x.%02x lut=%02x.%02x.%02x "
             "flg=0x%02X temp=%dC pbc=%d busy_after_reset=%d busy_low_ms=%d busy_release_ms=%d",
             r->verdict, r->reads_ok ? "ok" : "bad", r->rev[6],
             r->rev[0], r->rev[1], r->rev[2],
             r->rev[3], r->rev[4], r->rev[5],
             r->flg, (int)r->temp_c, r->pbc_pass,
             r->busy_after_reset, r->busy_low_ms, r->busy_release_ms);
}
