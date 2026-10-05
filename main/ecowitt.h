#ifndef _ECOWITT_H_
#define _ECOWITT_H_

#include "esp_err.h"
#include "weather.h"

// Fetch the current observation from the Ecowitt cloud (API v3,
// /device/real_time) and parse it into `out`. The station MAC is taken from
// CONFIG_ECOWITT_MAC if set, otherwise resolved once via /device/list and
// cached in NVS. Requires Wi-Fi; returns ESP_OK only if the API answered
// code 0 and at least one value was parsed.
esp_err_t ecowitt_fetch(weather_t *out);

#endif // _ECOWITT_H_
