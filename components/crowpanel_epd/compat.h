// Minimal Arduino-compatibility shim so the Elecrow CrowPanel vendor driver
// (spi.c / EPD_Init.c / EPD.c, originally Arduino .cpp) builds unmodified under
// ESP-IDF. Only the handful of Arduino primitives the driver actually uses are
// provided, mapped onto the ESP-IDF GPIO / FreeRTOS APIs.
#ifndef _COMPAT_H_
#define _COMPAT_H_

#include <stdint.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_rom_sys.h"   // esp_rom_delay_us

#ifndef HIGH
#define HIGH 1
#endif
#ifndef LOW
#define LOW 0
#endif
#ifndef OUTPUT
#define OUTPUT 1
#endif
#ifndef INPUT
#define INPUT 0
#endif

static inline void pinMode(int pin, int mode)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << pin),
        .mode = (mode == OUTPUT) ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

static inline void digitalWrite(int pin, int level)
{
    gpio_set_level((gpio_num_t)pin, level ? 1 : 0);
}

static inline int digitalRead(int pin)
{
    return gpio_get_level((gpio_num_t)pin);
}

static inline void delay(uint32_t ms)
{
    // Round up to at least one tick so sub-tick delays still yield.
    vTaskDelay((ms == 0) ? 1 : pdMS_TO_TICKS(ms));
}

static inline void delayMicroseconds(uint32_t us)
{
    esp_rom_delay_us(us);
}

#endif // _COMPAT_H_
