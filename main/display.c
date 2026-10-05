// CrowPanel 5.79" (792x272, dual SSD1683) rendering.
//
// The layout itself lives in ui.c (portable, previewable on the host); this file
// only maps it onto the vendor driver's frame buffer and runs the refresh.
//
// Refresh flow per update (the vendor sequence proven on this board): clear the
// controller RAM with a full refresh to avoid ghosting, then draw with a fast
// refresh, then deep-sleep the controllers. The image persists without power.
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "EPD.h"
#include "display.h"
#include "ui.h"

static const char *TAG = "display";

// EPD_W x EPD_H = 800 x 272; 1 bit/pixel. (800 px memory skips the 8 px seam
// between the two controllers; the visible area is 792 px.)
#define EPD_PWR_PIN 7
#define DISPLAY_ROTATION 0
static uint8_t ImageBW[EPD_W * EPD_H / 8];

void display_init(void)
{
    gpio_hold_dis(EPD_PWR_PIN);   // released from the deep-sleep hold
    gpio_config_t pwr = {
        .pin_bit_mask = (1ULL << EPD_PWR_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&pwr);
    gpio_set_level(EPD_PWR_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(10));   // let panel power rail settle
    EPD_GPIOInit();
}

void display_power_off(void)
{
    // Cut the panel rail and keep it low through deep sleep (image persists).
    gpio_set_level(EPD_PWR_PIN, 0);
    gpio_hold_en(EPD_PWR_PIN);
    gpio_deep_sleep_hold_en();
}

static void plot(int x, int y, int black)
{
    Paint_SetPixel((uint16_t)x, (uint16_t)y, black ? BLACK : WHITE);
}

void display_render(const weather_t *w, const status_t *st)
{
    Paint_NewImage(ImageBW, EPD_W, EPD_H, DISPLAY_ROTATION, WHITE);
    Paint_Clear(WHITE);
    ui_render(w, st, time(NULL), plot);

    ESP_LOGI(TAG, "Refreshing panel");
    EPD_FastMode1Init();
    EPD_Display_Clear();
    EPD_Update();
    EPD_FastMode1Init();
    EPD_Display(ImageBW);
    EPD_FastUpdate();
    EPD_DeepSleep();
    ESP_LOGI(TAG, "Panel updated");
}
