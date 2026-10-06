// One wake cycle, ending in deep sleep (the e-paper keeps its image unpowered).
//
// Refresh slots fall on multiples of CONFIG_WEATHER_POLL_MINUTES past local
// midnight (15 -> :00 :15 :30 :45). For each slot T the RTC timer wakes the chip
// at T - CONFIG_WEATHER_WAKE_LEAD_SECONDS; it reconnects Wi-Fi, re-syncs the
// clock, fetches at T - CONFIG_WEATHER_FETCH_LEAD_SECONDS and redraws at once,
// so the panel shows slot T's data before T. Power-on/reset fetches right away.
//
// Deep sleep loses RAM, so the last good observation and the schedule live in
// RTC slow memory. The RTC slow clock drifts; every wake measures the drift
// against SNTP and scales the next sleep to compensate.
#include <math.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "diag.h"
#include "display.h"
#include "ecowitt.h"
#include "poller.h"
#include "timesync.h"
#include "wifi.h"

static const char *TAG = "poller";

#define SLOT_SEC ((time_t)CONFIG_WEATHER_POLL_MINUTES * 60)
#define FETCH_LEAD_SEC ((time_t)CONFIG_WEATHER_FETCH_LEAD_SECONDS)
#define WAKE_LEAD_SEC ((time_t)CONFIG_WEATHER_WAKE_LEAD_SECONDS)
#define WIFI_TIMEOUT_MS 20000
#define SNTP_TIMEOUT_MS 15000
#define MIN_SLEEP_SEC 5
_Static_assert(CONFIG_WEATHER_WAKE_LEAD_SECONDS > CONFIG_WEATHER_FETCH_LEAD_SECONDS,
               "wake lead must exceed fetch lead");

#define RTC_MAGIC 0x57454131u   // "WEA1"
#define CLK_RATIO_MIN 0.8
#define CLK_RATIO_MAX 1.2

typedef struct {
    uint32_t magic;
    weather_t weather;          // last good observation
    bool have_data;
    time_t last_update;         // slot (or power-on time) of the last good data
    time_t next_slot;           // slot the pending timer wake is for; 0 = none
    double sleep_start;         // wall clock when we went to sleep (s)
    double clk_ratio;           // true seconds per RTC second
} rtc_state_t;

static RTC_DATA_ATTR rtc_state_t s_rtc;

static double now_s(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

// First slot whose wake time (slot - WAKE_LEAD_SEC) is at least MIN_SLEEP_SEC away.
static time_t next_slot(time_t now)
{
    struct tm lt;
    localtime_r(&now, &lt);
    time_t since_midnight = lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec;
    time_t slot = now - since_midnight + (since_midnight / SLOT_SEC + 1) * SLOT_SEC;
    while (slot - WAKE_LEAD_SEC < now + MIN_SLEEP_SEC) {
        slot += SLOT_SEC;
    }
    return slot;
}

static void sleep_until(time_t target)
{
    for (time_t left = target - time(NULL); left > 0; left = target - time(NULL)) {
        vTaskDelay(pdMS_TO_TICKS((left > 60 ? 60 : left) * 1000));
    }
}

static void log_time(const char *what, time_t t)
{
    struct tm lt;
    char buf[24];
    localtime_r(&t, &lt);
    strftime(buf, sizeof(buf), "%H:%M:%S", &lt);
    ESP_LOGI(TAG, "%s %s", what, buf);
}

// Compare RTC-kept time with SNTP after a timer wake and update clk_ratio.
static void calibrate(double rtc_now, int64_t mono_at_rtc_now)
{
    double slept_rtc = rtc_now - s_rtc.sleep_start;
    if (s_rtc.sleep_start <= 0 || slept_rtc < 60) {
        return;
    }
    double true_now = now_s() - (esp_timer_get_time() - mono_at_rtc_now) / 1e6;
    double slept_true = true_now - s_rtc.sleep_start;
    double r = slept_true / slept_rtc;
    if (r < CLK_RATIO_MIN || r > CLK_RATIO_MAX) {
        ESP_LOGW(TAG, "Ignoring implausible clock ratio %.4f", r);
        return;
    }
    s_rtc.clk_ratio = 0.5 * s_rtc.clk_ratio + 0.5 * r;
    ESP_LOGI(TAG, "RTC drift %+.1f s over %.0f s; clock ratio %.4f",
             slept_rtc - slept_true, slept_true, s_rtc.clk_ratio);
}

static void deep_sleep(void)
{
    time_t now = time(NULL);
    time_t wake;
    if (timesync_is_valid()) {
        s_rtc.next_slot = next_slot(now);
        wake = s_rtc.next_slot - WAKE_LEAD_SEC;
        log_time("Next slot", s_rtc.next_slot);
    } else {
        // No wall clock (no network at power-on): retry after one period.
        s_rtc.next_slot = 0;
        wake = now + SLOT_SEC;
    }
    s_rtc.sleep_start = now_s();
    diag_sleeping(now, wake);
    double sleep_s = (wake - s_rtc.sleep_start) / s_rtc.clk_ratio;
    if (sleep_s < MIN_SLEEP_SEC) {
        sleep_s = MIN_SLEEP_SEC;
    }
    ESP_LOGI(TAG, "Deep sleep for %.0f s", sleep_s);
    esp_wifi_stop();
    esp_sleep_enable_timer_wakeup((uint64_t)(sleep_s * 1e6));
    esp_deep_sleep_start();
}

void poller_run(void)
{
    bool timer_wake = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER &&
                      s_rtc.magic == RTC_MAGIC;
    if (s_rtc.magic != RTC_MAGIC) {
        memset(&s_rtc, 0, sizeof(s_rtc));
        s_rtc.magic = RTC_MAGIC;
        s_rtc.clk_ratio = 1.0;
    }
    // Wall clock as kept by the RTC through deep sleep, before SNTP corrects it.
    double rtc_now = now_s();
    int64_t mono_at_rtc_now = esp_timer_get_time();
    ESP_LOGI(TAG, "%s", timer_wake ? "Timer wake" : "Power-on: fetching now");

    diag_stage(DIAG_WIFI);
    status_t st = {0};
    strlcpy(st.ssid, wifi_ssid(), sizeof(st.ssid));
    st.wifi_ok = wifi_wait_connected(WIFI_TIMEOUT_MS);
    st.rssi = st.wifi_ok ? wifi_rssi() : 0;
    // SNTP starts only once the link is up: a first request sent too early is
    // lost and lwIP waits 15 s before retrying.
    diag_stage(DIAG_SNTP);
    timesync_start();
    if (st.wifi_ok && timesync_wait_sntp(SNTP_TIMEOUT_MS) && timer_wake) {
        calibrate(rtc_now, mono_at_rtc_now);
    }
    if (timesync_is_valid()) {
        diag_check_missed(time(NULL));
    }

    // Scheduled wake that is on time: fetch at slot - FETCH_LEAD and label with
    // the slot. Otherwise (power-on, late wake, no clock): fetch now.
    time_t slot = 0;
    if (timer_wake && s_rtc.next_slot && timesync_is_valid() && time(NULL) < s_rtc.next_slot) {
        slot = s_rtc.next_slot;
        sleep_until(slot - FETCH_LEAD_SEC);
    }

    diag_stage(DIAG_FETCH);
    esp_err_t err = ESP_ERR_WIFI_NOT_CONNECT;
    if (st.wifi_ok) {
        weather_t w;
        err = ecowitt_fetch(&w);
        if (err == ESP_OK) {
            s_rtc.weather = w;
            s_rtc.have_data = true;
            s_rtc.last_update = slot ? slot : time(NULL);
        }
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Fetch failed (%s)%s", esp_err_to_name(err),
                 s_rtc.have_data ? ", showing last data" : "");
    }
    st.last_err = err;
    st.stale = (err != ESP_OK);
    st.have_data = s_rtc.have_data;
    st.last_update = s_rtc.last_update;

    diag_stage(DIAG_PANEL);
    display_init();
    display_render(&s_rtc.weather, &st);
    display_power_off();
    if (slot) {
        ESP_LOGI(TAG, "Slot ready %ld s early", (long)(slot - time(NULL)));
    }
    deep_sleep();
}
