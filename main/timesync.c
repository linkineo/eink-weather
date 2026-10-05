// Wall clock: SNTP in poll mode plus the local timezone from Kconfig, so
// localtime() yields the date and "last update" time shown on the panel.
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "timesync.h"

static const char *TAG = "time";

// Anything before 2024-01-01 means the RTC was never set.
#define VALID_EPOCH 1704067200

void timesync_start(void)
{
    setenv("TZ", CONFIG_WEATHER_TZ, 1);
    tzset();
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();
    ESP_LOGI(TAG, "SNTP started, TZ=%s", CONFIG_WEATHER_TZ);
}

bool timesync_is_valid(void)
{
    return time(NULL) > VALID_EPOCH;
}

bool timesync_wait(uint32_t timeout_ms)
{
    for (uint32_t waited = 0; waited < timeout_ms; waited += 250) {
        if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED || timesync_is_valid()) {
            time_t now = time(NULL);
            struct tm lt;
            char buf[32];
            localtime_r(&now, &lt);
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &lt);
            ESP_LOGI(TAG, "Clock set: %s", buf);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    ESP_LOGW(TAG, "SNTP not synced after %lu ms", (unsigned long)timeout_ms);
    return false;
}

bool timesync_wait_sntp(uint32_t timeout_ms)
{
    for (uint32_t waited = 0; waited < timeout_ms; waited += 100) {
        if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    ESP_LOGW(TAG, "SNTP not synced after %lu ms, using RTC time", (unsigned long)timeout_ms);
    return false;
}
