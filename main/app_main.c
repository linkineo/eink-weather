// Ecowitt weather station on the CrowPanel ESP32-S3 5.79" e-paper display.
//
// Every boot (power-on or deep-sleep timer wake): NVS -> Wi-Fi -> one
// poller cycle (fetch, redraw), then deep sleep until just before the next
// refresh slot (see poller.c).
#include "esp_log.h"
#include "nvs_flash.h"
#include "poller.h"
#include "wifi.h"

static const char *TAG = "app";

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(wifi_start());
    ESP_LOGI(TAG, "Refresh every %d min", CONFIG_WEATHER_POLL_MINUTES);
    poller_run();
}
