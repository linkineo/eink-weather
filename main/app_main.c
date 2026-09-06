/*
 * app_main.c - wave 1 "hello world" / hardware bring-up firmware.
 *
 * Prints one banner line describing the running chip, then "DONE", then a
 * heartbeat every 10 s. Wave 2 inserts the display work before the DONE line.
 * All output follows the log contract in test_log.h.
 */

#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "test_log.h"

#define HEARTBEAT_PERIOD_MS 10000

static const char *chip_model_str(esp_chip_model_t model)
{
    switch (model) {
    case CHIP_ESP32:   return "ESP32";
    case CHIP_ESP32S2: return "ESP32-S2";
    case CHIP_ESP32S3: return "ESP32-S3";
    case CHIP_ESP32C3: return "ESP32-C3";
    case CHIP_ESP32H2: return "ESP32-H2";
    case CHIP_ESP32C2: return "ESP32-C2";
    default:           return "UNKNOWN";
    }
}

static const char *reset_reason_str(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_UNKNOWN:   /* fall through */
    default:                return "UNKNOWN";
    }
}

/* Build a "WiFi/BT/BLE"-style feature list into buf. */
static void chip_features_str(uint32_t features, char *buf, size_t len)
{
    static const struct {
        uint32_t bit;
        const char *name;
    } table[] = {
        { CHIP_FEATURE_WIFI_BGN, "WiFi" },
        { CHIP_FEATURE_BT,       "BT" },
        { CHIP_FEATURE_BLE,      "BLE" },
        { CHIP_FEATURE_EMB_FLASH, "EmbFlash" },
        { CHIP_FEATURE_EMB_PSRAM, "EmbPSRAM" },
        { CHIP_FEATURE_IEEE802154, "802.15.4" },
    };

    buf[0] = '\0';
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (!(features & table[i].bit)) {
            continue;
        }
        if (buf[0] != '\0') {
            strlcat(buf, "/", len);
        }
        strlcat(buf, table[i].name, len);
    }
    if (buf[0] == '\0') {
        strlcat(buf, "none", len);
    }
}

static void print_boot_banner(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    char features[64];
    chip_features_str(chip.features, features, sizeof(features));

    uint32_t flash_bytes = 0;
    if (esp_flash_get_size(NULL, &flash_bytes) != ESP_OK) {
        flash_bytes = 0;
    }

    uint8_t mac[6] = { 0 };
    esp_efuse_mac_get_default(mac);

    EPD_TEST_LOG("boot chip=%s rev=%u.%u cores=%u features=%s flash=%" PRIu32
                 "MB mac=%02x:%02x:%02x:%02x:%02x:%02x idf=%s reset=%s heap=%" PRIu32,
                 chip_model_str(chip.model),
                 chip.revision / 100u, chip.revision % 100u,
                 chip.cores,
                 features,
                 flash_bytes / (1024u * 1024u),
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                 esp_get_idf_version(),
                 reset_reason_str(esp_reset_reason()),
                 esp_get_free_heap_size());
}

void app_main(void)
{
    print_boot_banner();

    /* wave 2: display bring-up goes here, before DONE. */

    EPD_TEST_LOG("DONE");

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_PERIOD_MS));
        EPD_TEST_LOG("alive uptime=%llds heap=%" PRIu32,
                     esp_timer_get_time() / 1000000LL,
                     esp_get_free_heap_size());
    }
}
