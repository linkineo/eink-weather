/*
 * epd_diag.c - electrical diagnostic for the D/C (data/command) line.
 *
 * The problem this exists to settle: on this board single-byte COMMANDS reach
 * the controller (PON pulls BUSY_N low and releases it ~131 ms later, DRF
 * runs a genuine 17.8 s tri-colour refresh, POF releases BUSY again) while
 * DATA bytes apparently never do -- 48000 bytes per plane are clocked out in
 * 96 ms and the glass still comes up as uniform random noise, i.e. the
 * controller's RAM was never written. Both the SPI peripheral and the
 * bit-banged transport behave the same way, which points away from the
 * transport and at the D/C line itself: if the controller sees D/C stuck low
 * (open contact on FPC pin 11, broken trace, dead pad), then every byte is a
 * command. PON/DRF/POF still work, parameters and image bytes are executed as
 * mostly-invalid commands, and RAM keeps whatever it powered up with.
 *
 * Register reads would answer this in one line, but the 7.5inch e-Paper V2
 * specification says twice that "under serial mode, only write operations are
 * allowed" and three hardware runs confirmed it: reads return the last bit the
 * ESP32 drove. So the diagnostic needs an oracle that is observable on the
 * BUSY pin alone.
 *
 * That oracle is the power-on signature. PON (0x04) is the one command whose
 * effect is visible without touching the glass: BUSY_N goes low while the
 * booster and the regulators start, then returns high (spec, Power ON: "When
 * all voltages are ready, the BUSY_N signal will return to high"). So send the
 * byte 0x04 under different D/C conditions and watch BUSY:
 *
 *   D (control)  0x04 with DC=0, after a reset. A normal PON. Must give the
 *                signature, otherwise the detector itself is broken and every
 *                other answer is meaningless.
 *   A            CDI (0x50) with DC=0, then 0x04 with DC=1. If D/C is honoured
 *                the 0x04 is just CDI's parameter and nothing powers on. If it
 *                is not, the 0x04 executes as PON.
 *   B            DSLP (0x07) with DC=0, then 0xA5 with DC=1, then PON. Deep
 *                sleep only arms when the 0xA5 check byte arrives as DATA. If
 *                the data byte landed, the panel is asleep and ignores PON; if
 *                it did not, PON runs.
 *   E (polarity) 0x04 with DC=1 and no command before it. Distinguishes "D/C
 *                not seen" from "D/C inverted": an inverted line makes E fire
 *                and D silent, a dead line makes both fire.
 *
 * Nothing here refreshes the panel: no DRF, no DTM1/DTM2, no image data. Every
 * oracle that may have powered the analogue rails is followed by POF and a
 * hardware reset, so the panel ends idle with the booster off.
 *
 * Runs on bit-banged GPIO (epd_bitbang.c) and must therefore run before
 * epd_bus_init(), like epd_probe().
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

#include "epd_bitbang.h"
#include "epd_bus.h"
#include "epd_uc8179.h"

static const char *TAG = "epd";

/* The four command codes this file needs (UC8179 spec command table). Kept
 * local: epd_diag.c is a self-contained diagnostic and deliberately does not
 * share the panel sequences in epd_uc8179.c. */
#define UC8179_POF   0x02   /* Power off */
#define UC8179_PON   0x04   /* Power on */
#define UC8179_DSLP  0x07   /* Deep sleep (needs the 0xA5 check byte as DATA) */
#define UC8179_CDI   0x50   /* VCOM and data interval setting */

/* Length of one PON signature watch. Power-on took 34-131 ms in every
 * hardware run so far, so 400 ms of 1 ms samples is a wide margin while
 * keeping the whole diagnostic under three seconds. */
#define EPD_DIAG_WATCH_MS        400
/* Closing power-off: short, because the diagnostic must never hang. */
#define EPD_DIAG_POF_TIMEOUT_MS  2000
/* Time given to DSLP to arm before oracle B tries to power on. */
#define EPD_DIAG_DSLP_SETTLE_MS  50
/* Settle time between driving a pad and sampling it back. Orders of magnitude
 * more than an ESP32 pad plus an FFC needs, and still invisible in the log. */
#define EPD_DIAG_PAD_SETTLE_US   20

/* ------------------------------------------------------------------ helpers */

/* One millisecond of delay that does not depend on the tick rate (the same
 * helper the probe uses: one tick is at most 1 ms only when the tick rate is
 * at least 1 kHz, and the expression folds to a constant at compile time). */
static void epd_diag_sleep_1ms(unsigned iteration)
{
    if (pdMS_TO_TICKS(1) >= 1) {
        vTaskDelay(1);
    } else {
        esp_rom_delay_us(1000);
        /* Yield now and then so the idle task is not starved for a whole
         * watch window when the tick is coarser than 1 ms. */
        if ((iteration % 50U) == 49U) {
            vTaskDelay(1);
        }
    }
}

/* Sample BUSY_N every millisecond for EPD_DIAG_WATCH_MS and record the
 * low-then-high power-on signature. w->pon is true only when both edges were
 * seen, which is what every oracle below is asking about. */
static void epd_diag_watch(const char *name, epd_pon_watch_t *w)
{
    w->low_ms  = -1;
    w->high_ms = -1;
    w->pon     = false;

    const int64_t t0 = esp_timer_get_time();
    bool seen_low = false;

    for (unsigned i = 0;; i++) {
        const int level = epd_bb_busy_level();
        const int ms = (int)((esp_timer_get_time() - t0) / 1000);

        if (!seen_low) {
            if (level == 0) {
                seen_low = true;
                w->low_ms = ms;
            }
        } else if (level == 1) {
            w->high_ms = ms;
            break;
        }
        if (ms >= EPD_DIAG_WATCH_MS) {
            break;
        }
        epd_diag_sleep_1ms(i);
    }

    w->pon = (w->low_ms >= 0 && w->high_ms >= 0);
    ESP_LOGI(TAG, "dctest oracle %s: BUSY low at %d ms, high at %d ms -> pon=%s",
             name, w->low_ms, w->high_ms, w->pon ? "yes" : "no");
}

/* Close an oracle: power the analogue rails down and reset the controller, so
 * the next oracle starts from the same known state and the panel is never
 * left powered (a panel held at high voltage is damaged irreversibly). The
 * reset also wakes the controller if the oracle managed to put it to sleep. */
static void epd_diag_power_down(const char *name)
{
    epd_bb_cmd(UC8179_POF);

    int ms = 0;
    if (epd_bus_wait_busy_high(EPD_DIAG_POF_TIMEOUT_MS, &ms) == ESP_OK) {
        ESP_LOGI(TAG, "dctest oracle %s: power-off, BUSY released after %d ms", name, ms);
    } else {
        ESP_LOGW(TAG, "dctest oracle %s: power-off wait gave up after %d ms", name, ms);
    }

    epd_bb_reset_pulse();
}

/* Drive one pad high and low with its input buffer enabled and read it back.
 * A pad that follows the drive gives 1/0; anything else means the line is
 * shorted to a rail or to a neighbour, or the pad is dead. Afterwards the pin
 * goes back to the plain output that the bit-bang transport expects, at
 * idle_level (CS idles high, everything else low -- and RST is deliberately
 * restored low here so the panel stays in reset for the whole pad check). */
static void epd_diag_pad_check(gpio_num_t pin, int idle_level, epd_pad_check_t *out)
{
    const gpio_config_t probe_cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode         = GPIO_MODE_INPUT_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    const gpio_config_t restore_cfg = {
        .pin_bit_mask = (1ULL << pin),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    out->drive1 = -1;
    out->drive0 = -1;

    if (gpio_config(&probe_cfg) != ESP_OK) {
        return;
    }

    gpio_set_level(pin, 1);
    esp_rom_delay_us(EPD_DIAG_PAD_SETTLE_US);
    out->drive1 = gpio_get_level(pin);

    gpio_set_level(pin, 0);
    esp_rom_delay_us(EPD_DIAG_PAD_SETTLE_US);
    out->drive0 = gpio_get_level(pin);

    /* Restore before returning, whatever the read-back said. */
    (void)gpio_config(&restore_cfg);
    gpio_set_level(pin, idle_level != 0);
}

static void epd_diag_set_verdict(epd_dc_diag_result_t *r, const char *verdict)
{
    snprintf(r->verdict, sizeof(r->verdict), "%s", verdict);
}

/* --------------------------------------------------------------- public API */

esp_err_t epd_dc_diag(const epd_pins_t *pins, epd_dc_diag_result_t *out)
{
    ESP_RETURN_ON_FALSE(pins != NULL, ESP_ERR_INVALID_ARG, TAG, "pins is NULL");
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
    ESP_RETURN_ON_FALSE(!epd_bus_ready(), ESP_ERR_INVALID_STATE, TAG,
                        "epd_dc_diag() must run before epd_bus_init()");

    memset(out, 0, sizeof(*out));
    epd_diag_set_verdict(out, "INCONCLUSIVE");

    /* Everything runs on bit-banged GPIO: this diagnostic has to place single
     * bytes with a chosen D/C level and nothing else, which the SPI peripheral
     * cannot express as directly. epd_bb_init() leaves CS high, SCK low,
     * MOSI low, DC low and RST high. */
    ESP_RETURN_ON_ERROR(epd_bb_init(pins), TAG, "bit-bang init failed");

    /* ----------------------------------------------------------- pad check
     * First, and with RST held low the whole time: the pins toggled below
     * include SCK, MOSI and CS, and a controller out of reset would read
     * those toggles as bus traffic. */
    gpio_set_level((gpio_num_t)pins->rst, 0);
    epd_bus_delay_ms(1);

    epd_diag_pad_check((gpio_num_t)pins->dc,   0, &out->pad_dc);
    epd_diag_pad_check((gpio_num_t)pins->rst,  0, &out->pad_rst);
    epd_diag_pad_check((gpio_num_t)pins->cs,   1, &out->pad_cs);
    epd_diag_pad_check((gpio_num_t)pins->sck,  0, &out->pad_sck);
    epd_diag_pad_check((gpio_num_t)pins->mosi, 0, &out->pad_mosi);

    ESP_LOGI(TAG, "dctest pads: dc=%d/%d rst=%d/%d cs=%d/%d sck=%d/%d mosi=%d/%d"
                  " (driven 1 / driven 0, 1/0 = pad follows)",
             out->pad_dc.drive1,   out->pad_dc.drive0,
             out->pad_rst.drive1,  out->pad_rst.drive0,
             out->pad_cs.drive1,   out->pad_cs.drive0,
             out->pad_sck.drive1,  out->pad_sck.drive0,
             out->pad_mosi.drive1, out->pad_mosi.drive0);

    /* ------------------------------------------------------------ oracle D
     * Release RST with the normal reset pulse (RST high 200 ms, low 5 ms, high
     * 200 ms). That pulse is also oracle D's opening reset -- it ends with the
     * controller idle and out of reset, which is exactly what D wants, so
     * there is no point paying 405 ms twice.
     *
     * Control experiment: 0x04 as a plain COMMAND. Expected pon=yes; a "no"
     * here means the detector is broken (or D/C is inverted, which oracle E
     * then confirms) and no other line of this result may be trusted. */
    epd_bb_reset_pulse();
    epd_bb_cmd(UC8179_PON);
    epd_diag_watch("D", &out->oracle_d);
    epd_diag_power_down("D");

    /* ------------------------------------------------------------ oracle A
     * Is a data byte misread as a command? CDI (0x50) expects a parameter, so
     * with D/C honoured the following 0x04 is consumed as that parameter and
     * nothing powers on (pon=no). With D/C not seen, the 0x04 arrives as a
     * command and executes PON (pon=yes). */
    static const uint8_t k_pon_as_data = UC8179_PON;
    epd_bb_cmd(UC8179_CDI);
    epd_bb_data(&k_pon_as_data, 1);
    epd_diag_watch("A", &out->oracle_a);
    epd_diag_power_down("A");

    /* ------------------------------------------------------------ oracle B
     * Is a data byte required for an effect? DSLP (0x07) only arms deep sleep
     * when its 0xA5 check byte arrives as DATA. If the data byte landed, the
     * controller is asleep and ignores the PON that follows (pon=no). If it
     * did not, PON runs (pon=yes). Either way the power-down's reset pulse
     * wakes the controller again. */
    static const uint8_t k_dslp_check = 0xA5;
    epd_bb_cmd(UC8179_DSLP);
    epd_bb_data(&k_dslp_check, 1);
    epd_bus_delay_ms(EPD_DIAG_DSLP_SETTLE_MS);
    epd_bb_cmd(UC8179_PON);
    epd_diag_watch("B", &out->oracle_b);
    epd_diag_power_down("B");

    /* ------------------------------------------------------------ oracle E
     * Polarity. A lone 0x04 with DC=1 and no command before it: with D/C
     * honoured it is a parameter to nothing and does nothing (pon=no), with
     * D/C not seen -- or inverted -- it executes PON (pon=yes). Read together
     * with oracle D: D=yes and E=yes means the line is ignored, D=no and
     * E=yes means it is inverted. */
    epd_bb_data(&k_pon_as_data, 1);
    epd_diag_watch("E", &out->oracle_e);
    epd_diag_power_down("E");

    /* ------------------------------------------------------------- verdict */
    const bool d = out->oracle_d.pon;
    const bool a = out->oracle_a.pon;
    const bool b = out->oracle_b.pon;
    const bool e = out->oracle_e.pon;

    if (d && !a && !b && !e) {
        /* The control fires and no data byte ever acts as a command: D/C is
         * wired, seen and the right way round. The noise then has another
         * cause. */
        epd_diag_set_verdict(out, "DC_OK");
    } else if (d && a && b && e) {
        /* Every byte is a command whatever D/C does: the controller does not
         * see the line. Suspect the FPC/FFC contact for D/C before anything
         * in the firmware. */
        epd_diag_set_verdict(out, "DC_NOT_SEEN");
    } else if (!d && e) {
        /* The control is silent and the lone data byte fires: the controller
         * sees D/C, with the opposite sense. */
        epd_diag_set_verdict(out, "DC_INVERTED");
    } else {
        epd_diag_set_verdict(out, "INCONCLUSIVE");
    }

    ESP_LOGI(TAG, "dctest verdict=%s (D=%s A=%s B=%s E=%s)", out->verdict,
             d ? "yes" : "no", a ? "yes" : "no", b ? "yes" : "no", e ? "yes" : "no");

    /* The panel is off and reset: the last epd_diag_power_down() ended with
     * POF plus a reset pulse, so the booster is off and the controller idles.
     * SCK/MOSI/CS stay owned by epd_bitbang.c -- this mode never builds an
     * SPI bus, so there is nothing to hand them to. */
    return ESP_OK;
}

void epd_dc_diag_format(const epd_dc_diag_result_t *r, char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }
    if (r == NULL) {
        buf[0] = '\0';
        return;
    }
    snprintf(buf, len,
             "dctest pads=dc:%d/%d,rst:%d/%d,cs:%d/%d,sck:%d/%d,mosi:%d/%d "
             "oracleD_pon=%s oracleA_pon=%s oracleB_pon=%s oracleE_pon=%s verdict=%s",
             r->pad_dc.drive1,   r->pad_dc.drive0,
             r->pad_rst.drive1,  r->pad_rst.drive0,
             r->pad_cs.drive1,   r->pad_cs.drive0,
             r->pad_sck.drive1,  r->pad_sck.drive0,
             r->pad_mosi.drive1, r->pad_mosi.drive0,
             r->oracle_d.pon ? "yes" : "no",
             r->oracle_a.pon ? "yes" : "no",
             r->oracle_b.pon ? "yes" : "no",
             r->oracle_e.pon ? "yes" : "no",
             r->verdict);
}
