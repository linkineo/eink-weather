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
 * Two further diagnostics live at the bottom of this file, both built on the
 * same BUSY-only observability:
 *
 *   epd_rst_diag()  Does the reset pulse reach the controller? RST is the one
 *                   remaining unproven line: three hardware runs never once
 *                   observed an effect that only a reset could explain (deep
 *                   sleep, the obvious candidate, is armed by a DATA byte and
 *                   data bytes do not land). This matters because D/C (FPC pin
 *                   11) and RST (pin 10) are adjacent contacts: two dead
 *                   adjacent lines mean a mechanical fault, one dead line does
 *                   not. Two observables are tried, and the file records which
 *                   one earned the verdict:
 *                     - the length of the BUSY_N low pulse that POF produces,
 *                       expected ~44 ms with the rails up and nothing at all
 *                       on a controller a reset has just cleared. This one is
 *                       only meaningful if the pulse really does depend on the
 *                       power state, so a second POF on an already-off
 *                       controller is measured as its control (pof_off_ms).
 *                     - BUSY_N sampled while RST_N is held low. A controller
 *                       that restarts holds BUSY_N low, and nothing else on
 *                       the board can pull that line down, so a low here is
 *                       proof. One-sided: no low proves nothing.
 *
 *   epd_dcscan()    Is D/C simply on a different GPIO on this board revision?
 *                   Each candidate pin is driven as a second D/C line and put
 *                   through oracle A. A candidate that really reaches the
 *                   controller's D/C input keeps the CDI parameter a
 *                   parameter, so the panel stays off: pon=no identifies it.
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

/* --- RST oracle (epd_rst_diag) ------------------------------------------ */
/* RST_N low pulse. The reference drivers use 2-5 ms and the spec gives no
 * minimum, so 20 ms is a deliberate overkill: this test must never fail
 * because the pulse was too short to be noticed. */
#define EPD_DIAG_RST_LOW_MS      20
/* Settling time after RST_N goes high again, before the panel is questioned:
 * long enough for the controller's own power-on sequence, short enough that
 * a still-powered controller cannot have changed state on its own. */
#define EPD_DIAG_RST_SETTLE_MS   50
/* How long POF is given to pull BUSY_N low at all. Every observed POF started
 * within a millisecond or two; 100 ms of patience is the "BUSY never went
 * low" threshold the verdict below is written against. */
#define EPD_DIAG_POF_FALL_MS     100
/* Upper bound on any single BUSY-low measurement (the diagnostic must never
 * hang, and no legitimate POF pulse is anywhere near this long). */
#define EPD_DIAG_BUSY_HOLD_MS    2000
/* Boundary between "POF really switched something off" and "POF had nothing
 * to do": a real power-off pulse measures 41-44 ms here, so anything under
 * 10 ms is the second case. Measured on this hardware (2026-09-06): POF
 * pulses BUSY_N for the same 41 ms with the rails already off, which is what
 * pof_off_ms exists to detect and why the verdict cannot rest on this alone. */
#define EPD_DIAG_RST_SHORT_MS    10

/* --- D/C GPIO scan (epd_dcscan) ------------------------------------------ */
/* Per-candidate BUSY watch. A PON signature has never taken longer than
 * 131 ms; 300 ms keeps the 13-candidate sweep short while leaving margin. */
#define EPD_DCSCAN_WATCH_MS      300

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

/* Sample BUSY_N every millisecond for watch_ms and record the low-then-high
 * power-on signature. w->pon is true only when both edges were seen, which is
 * what every oracle below is asking about. */
static void epd_diag_watch_for(const char *what, int watch_ms, epd_pon_watch_t *w)
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
        if (ms >= watch_ms) {
            break;
        }
        epd_diag_sleep_1ms(i);
    }

    w->pon = (w->low_ms >= 0 && w->high_ms >= 0);
    ESP_LOGI(TAG, "%s: BUSY low at %d ms, high at %d ms -> pon=%s",
             what, w->low_ms, w->high_ms, w->pon ? "yes" : "no");
}

/* The D/C oracles' watch: the standard window, logged under their own name. */
static void epd_diag_watch(const char *name, epd_pon_watch_t *w)
{
    char what[32];
    snprintf(what, sizeof(what), "dctest oracle %s", name);
    epd_diag_watch_for(what, EPD_DIAG_WATCH_MS, w);
}

/* Time one BUSY_N low pulse: wait up to EPD_DIAG_POF_FALL_MS for the falling
 * edge, then measure how long the line stays low (bounded by
 * EPD_DIAG_BUSY_HOLD_MS, so a controller that never releases cannot hang the
 * diagnostic). Returns the low duration in ms, or -1 if BUSY never fell --
 * which is itself an answer, not an error. */
static int epd_diag_busy_pulse(const char *what)
{
    const int64_t t0 = esp_timer_get_time();
    int64_t t_fall = -1;

    for (unsigned i = 0;; i++) {
        if (epd_bb_busy_level() == 0) {
            t_fall = esp_timer_get_time();
            break;
        }
        if ((esp_timer_get_time() - t0) / 1000 >= EPD_DIAG_POF_FALL_MS) {
            break;
        }
        epd_diag_sleep_1ms(i);
    }

    if (t_fall < 0) {
        ESP_LOGI(TAG, "%s: BUSY never went low within %d ms -> low_ms=-1",
                 what, EPD_DIAG_POF_FALL_MS);
        return -1;
    }

    const int fall_ms = (int)((t_fall - t0) / 1000);
    int low_ms = 0;
    for (unsigned i = 0;; i++) {
        low_ms = (int)((esp_timer_get_time() - t_fall) / 1000);
        if (epd_bb_busy_level() == 1) {
            break;
        }
        if (low_ms >= EPD_DIAG_BUSY_HOLD_MS) {
            ESP_LOGW(TAG, "%s: BUSY still low after %d ms, giving up", what, low_ms);
            break;
        }
        epd_diag_sleep_1ms(i);
    }

    ESP_LOGI(TAG, "%s: BUSY fell after %d ms, stayed low %d ms", what, fall_ms, low_ms);
    return low_ms;
}

/* Close an oracle: power the analogue rails down and reset the controller, so
 * the next oracle starts from the same known state and the panel is never
 * left powered (a panel held at high voltage is damaged irreversibly). The
 * reset also wakes the controller if the oracle managed to put it to sleep. */
static void epd_diag_power_off(const char *what)
{
    epd_bb_cmd(UC8179_POF);

    int ms = 0;
    if (epd_bus_wait_busy_high(EPD_DIAG_POF_TIMEOUT_MS, &ms) == ESP_OK) {
        ESP_LOGI(TAG, "%s: power-off, BUSY released after %d ms", what, ms);
    } else {
        ESP_LOGW(TAG, "%s: power-off wait gave up after %d ms", what, ms);
    }
}

static void epd_diag_power_down_for(const char *what)
{
    epd_diag_power_off(what);
    epd_bb_reset_pulse();
}

static void epd_diag_power_down(const char *name)
{
    char what[32];
    snprintf(what, sizeof(what), "dctest oracle %s", name);
    epd_diag_power_down_for(what);
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

/*============================================================== RST oracle ==*/

/*
 * The short reset pulse this diagnostic uses: RST_N low 20 ms, high, then
 * 50 ms to settle. Deliberately not epd_bb_reset_pulse() (200/5/200 ms): what
 * is under test is the line, not the reference timing, and the short pulse
 * keeps the three sub-experiments inside their runtime budget. 20 ms is still
 * four times the reference low time.
 *
 * BUSY_N is sampled throughout, and the answer is returned: a controller that
 * is being reset holds BUSY_N low while it restarts, so a low seen here is
 * DIRECT proof that the pulse arrived. The test is one-sided -- no low does
 * not prove the opposite, because nothing guarantees this controller signals
 * its reset on BUSY at all -- but a positive is conclusive on its own, which
 * no other observable in this file can offer for RST.
 */
static bool epd_rst_diag_pulse(const epd_pins_t *pins)
{
    bool low_seen = false;

    gpio_set_level((gpio_num_t)pins->rst, 0);
    for (unsigned i = 0; i < EPD_DIAG_RST_LOW_MS; i++) {
        if (epd_bb_busy_level() == 0) {
            low_seen = true;
        }
        epd_diag_sleep_1ms(i);
    }

    gpio_set_level((gpio_num_t)pins->rst, 1);
    for (unsigned i = 0; i < EPD_DIAG_RST_SETTLE_MS; i++) {
        if (epd_bb_busy_level() == 0) {
            low_seen = true;
        }
        epd_diag_sleep_1ms(i);
    }

    return low_seen;
}

/* Opening of each sub-experiment: full reset pulse, PON, and the power-on
 * signature that says the rails really came up. If this watch says no, the
 * question that follows has no meaning. */
static void epd_rst_diag_power_up(const char *what, epd_pon_watch_t *w)
{
    epd_bb_reset_pulse();
    epd_bb_cmd(UC8179_PON);
    epd_diag_watch_for(what, EPD_DIAG_WATCH_MS, w);
}

esp_err_t epd_rst_diag(const epd_pins_t *pins, epd_rst_diag_result_t *out)
{
    ESP_RETURN_ON_FALSE(pins != NULL, ESP_ERR_INVALID_ARG, TAG, "pins is NULL");
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
    ESP_RETURN_ON_FALSE(!epd_bus_ready(), ESP_ERR_INVALID_STATE, TAG,
                        "epd_rst_diag() must run before epd_bus_init()");

    memset(out, 0, sizeof(*out));
    out->pof_ref_ms       = -1;
    out->pof_off_ms       = -1;
    out->pof_after_rst_ms = -1;
    out->busy_after_rst   = -1;

    ESP_RETURN_ON_ERROR(epd_bb_init(pins), TAG, "bit-bang init failed");

    /* ------------------------------------------------------- reference POF
     * PON, wait for the signature, then POF with nothing in between. This is
     * the yardstick: the BUSY_N low pulse of a genuine power-off on a
     * controller whose rails are up (~44 ms in every run so far). */
    epd_rst_diag_power_up("rsttest ref PON", &out->pon_ref);
    epd_bb_cmd(UC8179_POF);
    out->pof_ref_ms = epd_diag_busy_pulse("rsttest ref POF");
    (void)epd_bus_wait_busy_high(EPD_DIAG_POF_TIMEOUT_MS, NULL);

    /* ----------------------------------------------- control for the oracle
     * A second POF, on the controller the first one just switched off. The
     * whole RST oracle rests on the assumption that the length of the POF
     * pulse says something about the power state -- and that assumption is
     * exactly what this measures. If an already-off controller still produces
     * the full pulse, POF simply always takes that long, the oracle cannot
     * discriminate, and "pof_after_rst_ms == pof_ref_ms" means nothing. */
    epd_bb_cmd(UC8179_POF);
    out->pof_off_ms = epd_diag_busy_pulse("rsttest off POF");
    (void)epd_bus_wait_busy_high(EPD_DIAG_POF_TIMEOUT_MS, NULL);

    /* --------------------------------------------------------- POF after RST
     * The same, with a reset pulse between the power-on and the power-off. If
     * the pulse reached the controller, its rails and registers are already
     * cleared and POF has nothing left to switch off: no BUSY pulse, or a
     * couple of milliseconds. If the pulse went nowhere, the panel is still
     * powered and POF behaves exactly like the reference. */
    epd_rst_diag_power_up("rsttest rst PON", &out->pon_rst);
    out->busy_low_during_rst = epd_rst_diag_pulse(pins);
    /* Sampled before POF so a BUSY line that is still low from the reset
     * itself cannot be mistaken for the power-off pulse below. */
    out->busy_after_rst = epd_bb_busy_level();
    ESP_LOGI(TAG, "rsttest: BUSY went low during the RST pulse: %s; BUSY is %d "
                  "%d ms after RST_N was released",
             out->busy_low_during_rst ? "yes" : "no",
             out->busy_after_rst, EPD_DIAG_RST_SETTLE_MS);
    epd_bb_cmd(UC8179_POF);
    out->pof_after_rst_ms = epd_diag_busy_pulse("rsttest rst POF");
    (void)epd_bus_wait_busy_high(EPD_DIAG_POF_TIMEOUT_MS, NULL);

    /* --------------------------------------------------------- PON after RST
     * The reverse reading of the same event: a controller that really was
     * reset powers up again from scratch and produces the full signature; an
     * already-powered controller asked to power on again has little or
     * nothing left to do. */
    epd_pon_watch_t opening;
    epd_rst_diag_power_up("rsttest rev PON", &opening);
    if (epd_rst_diag_pulse(pins)) {
        out->busy_low_during_rst = true;
    }
    epd_bb_cmd(UC8179_PON);
    epd_diag_watch_for("rsttest rev PON#2", EPD_DIAG_WATCH_MS, &out->pon_after_rst);

    /* Close: rails off, controller reset. */
    epd_diag_power_down_for("rsttest");

    /* --------------------------------------------------------------- verdict
     * Two independent ways to earn a "yes", and only one of them is sound on
     * every hardware:
     *
     *  - BUSY_N went low while RST_N was low. Nothing but the controller can
     *    pull that line down, so the pulse reached it. Conclusive.
     *  - The POF pulse disappeared after the reset, on a panel that was
     *    powered and whose POF pulse demonstrably depends on being powered
     *    (pof_off_ms short). Conclusive only when that last condition holds.
     */
    const bool ref_ok   = out->pon_ref.pon && out->pof_ref_ms >= EPD_DIAG_RST_SHORT_MS;
    const bool pof_says = out->pof_off_ms >= 0 && out->pof_off_ms < EPD_DIAG_RST_SHORT_MS;
    const bool lost     = (out->pof_after_rst_ms < 0) ||
                          (out->pof_after_rst_ms < EPD_DIAG_RST_SHORT_MS);

    out->pof_oracle_usable = ref_ok && pof_says;
    out->rst_effective     = out->busy_low_during_rst ||
                             (out->pof_oracle_usable && out->pon_rst.pon && lost);

    if (!ref_ok) {
        ESP_LOGW(TAG, "rsttest: control is not sound (ref PON pon=%s, ref POF low=%d ms) "
                      "-- rst_effective=no means UNDECIDED here, not 'RST works'",
                 out->pon_ref.pon ? "yes" : "no", out->pof_ref_ms);
    } else if (!pof_says) {
        ESP_LOGW(TAG, "rsttest: the POF oracle cannot discriminate on this controller: "
                      "POF pulls BUSY low for %d ms even with the rails already off "
                      "(reference %d ms). Only busy_during_rst can prove RST here, and "
                      "it is one-sided: rst_effective=no means UNDECIDED, not 'RST is dead'",
                 out->pof_off_ms, out->pof_ref_ms);
    }
    ESP_LOGI(TAG, "rsttest: pof_ref=%d ms pof_off=%d ms pof_after_rst=%d ms "
                  "busy_during_rst=%s pon_after_rst=%s (low %d ms, high %d ms) "
                  "-> pof_oracle_usable=%s rst_effective=%s",
             out->pof_ref_ms, out->pof_off_ms, out->pof_after_rst_ms,
             out->busy_low_during_rst ? "yes" : "no",
             out->pon_after_rst.pon ? "yes" : "no",
             out->pon_after_rst.low_ms, out->pon_after_rst.high_ms,
             out->pof_oracle_usable ? "yes" : "no",
             out->rst_effective ? "yes" : "no");

    return ESP_OK;
}

void epd_rst_diag_format(const epd_rst_diag_result_t *r, char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }
    if (r == NULL) {
        buf[0] = '\0';
        return;
    }
    snprintf(buf, len,
             "rsttest pof_ref_ms=%d pof_after_rst_ms=%d pon_after_rst=%s(%d) "
             "pof_off_ms=%d pof_oracle=%s busy_during_rst=%s busy_after_rst=%d "
             "rst_effective=%s",
             r->pof_ref_ms, r->pof_after_rst_ms,
             r->pon_after_rst.pon ? "yes" : "no", r->pon_after_rst.high_ms,
             r->pof_off_ms, r->pof_oracle_usable ? "usable" : "degenerate",
             r->busy_low_during_rst ? "low" : "high",
             r->busy_after_rst,
             r->rst_effective ? "yes" : "no");
}

/*=========================================================== D/C GPIO scan ==*/

/*
 * Candidate pins, in the order they are tried. What is missing matters as
 * much as what is here: 0/2/5/12/15 are strapping pins (2 and 5 are in the
 * list because they are only read at reset, 12 is not because a wrong level
 * there would change the flash voltage), 1 and 3 are the console UART, 6-11
 * are the SPI flash, 34-39 are input-only, and 13/14/15/25/26/27 already
 * belong to the panel. The real D/C pin is prepended at runtime as the
 * sanity check.
 *
 * GPIO2 and GPIO33 stay in the table but are skipped at runtime whenever the
 * board maps them to the panel power rail (epd_dcscan_reserved() below): they
 * are still legitimate candidates on a board revision that has no such rail.
 */
static const int k_dcscan_candidates[] = { 2, 4, 5, 16, 17, 18, 19, 21, 22, 23, 32, 33 };

/* Pins this diagnostic may not touch: they carry the bus and the control
 * lines it needs while the scan runs, or they switch the panel rail. A power
 * pin driven low as a mirrored D/C would cut the panel's supply mid-sweep and
 * come back as a false "found": the panel would not power on, which is
 * exactly the signature this scan reads as "this pin is the real D/C". */
static bool epd_dcscan_reserved(const epd_pins_t *pins, int g)
{
    return g == pins->sck || g == pins->mosi || g == pins->cs ||
           g == pins->rst || g == pins->busy ||
           g == pins->pwr || g == pins->pwr_aux;
}

/* Take a candidate over as a plain output at the command level (D/C low). */
static esp_err_t epd_dcscan_claim(gpio_num_t g)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << g),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };

    /* From the default state, so a candidate left over from an earlier boot
     * mode cannot keep a peripheral signal routed to it. */
    gpio_reset_pin(g);
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "gpio_config(GPIO%d) failed", (int)g);
    return gpio_set_level(g, 0);
}

/* Give a candidate back. The real D/C pin is the exception: it has to stay a
 * driven output for the rest of the diagnostic, so it is restored instead of
 * released to the default input-with-pull-up. */
static void epd_dcscan_release(const epd_pins_t *pins, gpio_num_t g)
{
    if ((int)g == pins->dc) {
        (void)epd_dcscan_claim(g);
    } else {
        gpio_reset_pin(g);
    }
}

esp_err_t epd_dcscan(const epd_pins_t *pins, epd_dcscan_result_t *out)
{
    ESP_RETURN_ON_FALSE(pins != NULL, ESP_ERR_INVALID_ARG, TAG, "pins is NULL");
    ESP_RETURN_ON_FALSE(out != NULL, ESP_ERR_INVALID_ARG, TAG, "out is NULL");
    ESP_RETURN_ON_FALSE(!epd_bus_ready(), ESP_ERR_INVALID_STATE, TAG,
                        "epd_dcscan() must run before epd_bus_init()");

    memset(out, 0, sizeof(*out));
    out->found = -1;

    ESP_RETURN_ON_ERROR(epd_bb_init(pins), TAG, "bit-bang init failed");

    /* Build the list: the wired D/C pin first as a sanity check (it must come
     * out pon=yes, i.e. NOT found -- anything else contradicts dctest and
     * invalidates the sweep), then the candidates. */
    out->gpio[out->n++] = pins->dc;
    for (size_t i = 0; i < sizeof(k_dcscan_candidates) / sizeof(k_dcscan_candidates[0]); i++) {
        const int g = k_dcscan_candidates[i];
        if (out->n >= EPD_DCSCAN_MAX || g == pins->dc || epd_dcscan_reserved(pins, g)) {
            continue;
        }
        out->gpio[out->n++] = g;
    }

    /* Oracle A's byte: 0x04 offered as CDI's parameter. If the controller
     * honours the candidate as D/C it stays a parameter and nothing happens;
     * if not, it executes as PON and the panel powers up. */
    static const uint8_t k_cdi_param = UC8179_PON;

    for (int i = 0; i < out->n; i++) {
        const gpio_num_t g = (gpio_num_t)out->gpio[i];
        char what[24];
        snprintf(what, sizeof(what), "dcscan GPIO%d", out->gpio[i]);

        if (epd_dcscan_claim(g) != ESP_OK) {
            ESP_LOGW(TAG, "%s: cannot be driven, skipped", what);
            out->watch[i].pon     = true;   /* not a candidate: never "found" */
            out->watch[i].low_ms  = -1;
            out->watch[i].high_ms = -1;
            continue;
        }
        epd_bb_set_dc_mirror(out->gpio[i]);

        /* From here every epd_bb_cmd() drives GPIO27 and the candidate low,
         * and epd_bb_data() drives both high. */
        epd_bb_reset_pulse();
        epd_bb_cmd(UC8179_CDI);
        epd_bb_data(&k_cdi_param, 1);
        epd_diag_watch_for(what, EPD_DCSCAN_WATCH_MS, &out->watch[i]);

        if (!out->watch[i].pon && out->found < 0) {
            out->found = out->gpio[i];
        }

        /* POF only: the next candidate opens with its own reset pulse, and
         * the sweep closes with one. */
        epd_diag_power_off(what);
        epd_bb_set_dc_mirror(-1);
        epd_dcscan_release(pins, g);
    }

    /* Close: rails off, controller reset, every candidate back to default. */
    epd_diag_power_down_for("dcscan");

    if (out->found >= 0) {
        ESP_LOGW(TAG, "dcscan: GPIO%d is honoured as D/C -- the board does not "
                      "wire D/C to GPIO%d", out->found, pins->dc);
    } else {
        ESP_LOGI(TAG, "dcscan: no candidate acted as D/C (%d tested)", out->n);
    }

    return ESP_OK;
}

void epd_dcscan_format(const epd_dcscan_result_t *r, char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return;
    }
    if (r == NULL) {
        buf[0] = '\0';
        return;
    }

    /* "27,2,4,..." -- three digits plus a comma per entry is the worst case. */
    char cands[EPD_DCSCAN_MAX * 5 + 1];
    size_t off = 0;
    cands[0] = '\0';
    for (int i = 0; i < r->n && i < EPD_DCSCAN_MAX; i++) {
        const int w = snprintf(cands + off, sizeof(cands) - off, "%s%d",
                               (i == 0) ? "" : ",", r->gpio[i]);
        if (w < 0 || (size_t)w >= sizeof(cands) - off) {
            break;
        }
        off += (size_t)w;
    }

    if (r->found >= 0) {
        snprintf(buf, len, "dcscan candidates=%s found=GPIO%d", cands, r->found);
    } else {
        snprintf(buf, len, "dcscan candidates=%s found=none", cands);
    }
}
