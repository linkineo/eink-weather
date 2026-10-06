// Post-mortem diagnostics: see diag.h.
#include <stdint.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "diag.h"

static const char *TAG = "diag";

#define CYCLE_WATCHDOG_S 120
#define MISSED_WAKE_MARGIN_S 120
#define NVS_NS "diag"

static RTC_DATA_ATTR uint8_t s_stage;   // diag_stage_t of the running cycle
static RTC_DATA_ATTR uint32_t s_restarts;
static esp_timer_handle_t s_watchdog;

static const char *stage_name(uint8_t s)
{
    static const char *names[] = {"none", "boot", "wifi", "sntp", "fetch", "panel", "sleep"};
    return s < sizeof(names) / sizeof(names[0]) ? names[s] : "?";
}

static const char *reset_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external pin";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep wake";
    case ESP_RST_BROWNOUT: return "brownout";
    default: return "unknown";
    }
}

static void watchdog_cb(void *arg)
{
    ESP_LOGE(TAG, "Wake cycle exceeded %d s in stage '%s': restarting",
             CYCLE_WATCHDOG_S, stage_name(s_stage));
    s_restarts++;
    esp_restart();   // RTC memory (last data, stage) survives a software reset
}

void diag_boot(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    if (r == ESP_RST_DEEPSLEEP) {
        if (s_stage != DIAG_SLEEP && s_stage != 0) {
            ESP_LOGW(TAG, "Previous cycle did not reach sleep (stage '%s')", stage_name(s_stage));
        }
    } else if (r == ESP_RST_POWERON || r == ESP_RST_EXT) {
        s_restarts = 0;
        ESP_LOGI(TAG, "Boot: %s", reset_name(r));
    } else {
        ESP_LOGW(TAG, "Boot after %s reset while previous cycle was in stage '%s' (%lu restarts)",
                 reset_name(r), stage_name(s_stage), (unsigned long)s_restarts);
    }

    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) == ESP_OK) {
        uint32_t cycles = 0;
        if (nvs_get_u32(nvs, "cycles", &cycles) == ESP_OK) {
            ESP_LOGI(TAG, "%lu cycles completed so far", (unsigned long)cycles);
        }
        nvs_close(nvs);
    }

    s_stage = DIAG_BOOT;
    const esp_timer_create_args_t args = {.callback = watchdog_cb, .name = "cycle_wdt"};
    if (esp_timer_create(&args, &s_watchdog) == ESP_OK) {
        esp_timer_start_once(s_watchdog, (uint64_t)CYCLE_WATCHDOG_S * 1000000);
    }
}

void diag_stage(diag_stage_t stage)
{
    s_stage = (uint8_t)stage;
}

void diag_check_missed(time_t now)
{
    nvs_handle_t nvs;
    int64_t slept_at = 0, wake = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) != ESP_OK) {
        return;
    }
    nvs_get_i64(nvs, "slept_at", &slept_at);
    nvs_get_i64(nvs, "wake", &wake);
    nvs_close(nvs);
    if (wake > 0 && now > wake + MISSED_WAKE_MARGIN_S) {
        struct tm a, b;
        char sa[20], sb[20];
        time_t ta = (time_t)slept_at, tb = (time_t)wake;
        localtime_r(&ta, &a);
        localtime_r(&tb, &b);
        strftime(sa, sizeof(sa), "%d.%m %H:%M:%S", &a);
        strftime(sb, sizeof(sb), "%d.%m %H:%M:%S", &b);
        ESP_LOGW(TAG, "Missed wake: last sleep %s, expected wake %s (%lld min ago). "
                      "Power was lost or the timer wake never happened.",
                 sa, sb, (long long)((now - wake) / 60));
    }
}

void diag_sleeping(time_t now, time_t wake)
{
    s_stage = DIAG_SLEEP;
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) {
        return;
    }
    uint32_t cycles = 0;
    nvs_get_u32(nvs, "cycles", &cycles);
    nvs_set_u32(nvs, "cycles", cycles + 1);
    nvs_set_i64(nvs, "slept_at", (int64_t)now);
    nvs_set_i64(nvs, "wake", (int64_t)wake);
    nvs_commit(nvs);
    nvs_close(nvs);
}
