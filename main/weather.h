// Shared data types for the Ecowitt e-paper weather display.
#ifndef _WEATHER_H_
#define _WEATHER_H_

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "esp_err.h"

// One observation from the Ecowitt cloud (metric units). Each group has its own
// `has_*` flag because a station may lack a sensor or the API may omit a block.
typedef struct {
    float indoor_c;
    float outdoor_c;
    float rain_rate_mm_h;   // current rain rate
    float rain_today_mm;    // accumulated since local midnight
    float solar_wm2;
    float wind_speed_kmh;
    int wind_dir_deg;       // 0..359, direction the wind comes from
    bool has_indoor;
    bool has_outdoor;
    bool has_rain;
    bool has_solar;
    bool has_wind;
    time_t obs_time;        // station observation time (UTC epoch)
} weather_t;

static inline bool weather_raining_now(const weather_t *w)
{
    return w->has_rain && w->rain_rate_mm_h > 0.0f;
}

static inline bool weather_rained_today(const weather_t *w)
{
    return w->has_rain && w->rain_today_mm > 0.0f;
}

// Connection / freshness state shown alongside the weather.
typedef struct {
    char ssid[33];
    int8_t rssi;            // dBm, valid when wifi_ok
    bool wifi_ok;
    bool have_data;         // at least one successful fetch since boot
    bool stale;             // last fetch failed; showing older data
    time_t last_update;     // clock time of the last panel refresh with fresh data
    esp_err_t last_err;     // ESP_OK or the error of the last failed fetch
} status_t;

#endif // _WEATHER_H_
