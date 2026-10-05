// Ecowitt cloud client (API v3, https://api.ecowitt.net/api/v3).
//
// Response shape of /device/real_time (verified against the live station):
//   { "code": 0, "msg": "success", "time": "<epoch>",
//     "data": { "outdoor": { "temperature": { "time": "<epoch>", "unit": "℃", "value": "12.9" }, ... },
//               "indoor":  { "temperature": {...}, ... },
//               "solar_and_uvi": { "solar": { "unit": "W/m²", "value": "0.0" }, ... },
//               "rainfall_piezo": { "rain_rate": {...mm/hr}, "daily": {...mm}, ... },
//               "wind": { "wind_speed": {...km/h}, "wind_direction": { "unit": "º", "value": "162" }, ... } } }
// Values are strings. Stations with a tipping-bucket gauge report "rainfall"
// instead of "rainfall_piezo"; both are accepted.
//
// Request URLs carry the API keys, so they are never logged.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "nvs.h"
#include "ecowitt.h"
#include "secrets.h"

static const char *TAG = "ecowitt";

#define API_BASE "https://api.ecowitt.net/api/v3"
// Metric units: degC, hPa, km/h, mm, W/m2.
#define UNIT_PARAMS "temp_unitid=1&pressure_unitid=3&wind_speed_unitid=7" \
                    "&rainfall_unitid=12&solar_irradiance_unitid=16"
#define RESP_MAX 8192
#define URL_MAX 384
#define NVS_NS "ecowitt"

static char s_resp[RESP_MAX];
static int s_resp_len;
static char s_mac[24];

static esp_err_t on_http_event(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        int room = RESP_MAX - 1 - s_resp_len;
        int n = (evt->data_len < room) ? evt->data_len : room;
        memcpy(s_resp + s_resp_len, evt->data, n);
        s_resp_len += n;
        s_resp[s_resp_len] = '\0';
    }
    return ESP_OK;
}

// GET `url` into s_resp and return the parsed JSON (caller frees), or NULL.
static cJSON *http_get_json(const char *url)
{
    s_resp_len = 0;
    s_resp[0] = '\0';
    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = on_http_event,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "HTTP request failed: %s, status %d", esp_err_to_name(err), status);
        return NULL;
    }
    cJSON *root = cJSON_Parse(s_resp);
    if (root == NULL) {
        ESP_LOGE(TAG, "Invalid JSON (%d bytes)", s_resp_len);
        return NULL;
    }
    const cJSON *code = cJSON_GetObjectItem(root, "code");
    if (!cJSON_IsNumber(code) || code->valueint != 0) {
        const cJSON *msg = cJSON_GetObjectItem(root, "msg");
        ESP_LOGE(TAG, "API error code %d: %s", cJSON_IsNumber(code) ? code->valueint : -1,
                 cJSON_IsString(msg) ? msg->valuestring : "?");
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

// Number from a JSON string ("12.9") or number; false if absent/unparsable.
static bool json_num(const cJSON *item, double *out)
{
    if (cJSON_IsNumber(item)) {
        *out = item->valuedouble;
        return true;
    }
    if (cJSON_IsString(item) && item->valuestring[0] != '\0') {
        char *end;
        *out = strtod(item->valuestring, &end);
        return end != item->valuestring;
    }
    return false;
}

// data.<group>.<field>.value as float.
static bool get_value(const cJSON *data, const char *group, const char *field, float *out)
{
    const cJSON *g = cJSON_GetObjectItem(data, group);
    const cJSON *f = cJSON_GetObjectItem(g, field);
    double v;
    if (!json_num(cJSON_GetObjectItem(f, "value"), &v)) {
        return false;
    }
    *out = (float)v;
    return true;
}

static esp_err_t resolve_mac(void)
{
    if (s_mac[0] != '\0') {
        return ESP_OK;
    }
    if (CONFIG_ECOWITT_MAC[0] != '\0') {
        strlcpy(s_mac, CONFIG_ECOWITT_MAC, sizeof(s_mac));
        ESP_LOGI(TAG, "Using station MAC from Kconfig");
        return ESP_OK;
    }

    nvs_handle_t nvs;
    size_t len = sizeof(s_mac);
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) == ESP_OK) {
        esp_err_t err = nvs_get_str(nvs, "mac", s_mac, &len);
        nvs_close(nvs);
        if (err == ESP_OK && s_mac[0] != '\0') {
            ESP_LOGI(TAG, "Using station MAC cached in NVS");
            return ESP_OK;
        }
    }

    char url[URL_MAX];
    snprintf(url, sizeof(url), API_BASE "/device/list?application_key=%s&api_key=%s",
             ECOWITT_APP_KEY, ECOWITT_API_KEY);
    cJSON *root = http_get_json(url);
    if (root == NULL) {
        return ESP_FAIL;
    }
    const cJSON *list = cJSON_GetObjectItem(cJSON_GetObjectItem(root, "data"), "list");
    const cJSON *mac = cJSON_GetObjectItem(cJSON_GetArrayItem(list, 0), "mac");
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    if (cJSON_IsString(mac)) {
        strlcpy(s_mac, mac->valuestring, sizeof(s_mac));
        ESP_LOGI(TAG, "Resolved station MAC via /device/list (%d device(s))",
                 cJSON_GetArraySize(list));
        if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) == ESP_OK) {
            nvs_set_str(nvs, "mac", s_mac);
            nvs_commit(nvs);
            nvs_close(nvs);
        }
        ret = ESP_OK;
    } else {
        ESP_LOGE(TAG, "No device found for these API keys");
    }
    cJSON_Delete(root);
    return ret;
}

esp_err_t ecowitt_fetch(weather_t *out)
{
    esp_err_t err = resolve_mac();
    if (err != ESP_OK) {
        return err;
    }

    char url[URL_MAX];
    snprintf(url, sizeof(url),
             API_BASE "/device/real_time?application_key=%s&api_key=%s&mac=%s&call_back=all&" UNIT_PARAMS,
             ECOWITT_APP_KEY, ECOWITT_API_KEY, s_mac);
    cJSON *root = http_get_json(url);
    if (root == NULL) {
        return ESP_FAIL;
    }

    const cJSON *data = cJSON_GetObjectItem(root, "data");
    weather_t w = { 0 };
    float dir = 0;
    w.has_outdoor = get_value(data, "outdoor", "temperature", &w.outdoor_c);
    w.has_indoor = get_value(data, "indoor", "temperature", &w.indoor_c);
    w.has_solar = get_value(data, "solar_and_uvi", "solar", &w.solar_wm2);
    const char *rain = cJSON_HasObjectItem(data, "rainfall_piezo") ? "rainfall_piezo" : "rainfall";
    w.has_rain = get_value(data, rain, "rain_rate", &w.rain_rate_mm_h) &&
                 get_value(data, rain, "daily", &w.rain_today_mm);
    w.has_wind = get_value(data, "wind", "wind_direction", &dir);
    get_value(data, "wind", "wind_speed", &w.wind_speed_kmh);
    w.wind_dir_deg = (int)(dir + 0.5f) % 360;

    double t;
    const cJSON *ot = cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(data, "outdoor"),
                                                              "temperature"), "time");
    if (json_num(ot, &t) || json_num(cJSON_GetObjectItem(root, "time"), &t)) {
        w.obs_time = (time_t)t;
    }
    cJSON_Delete(root);

    if (!(w.has_outdoor || w.has_indoor || w.has_rain || w.has_solar || w.has_wind)) {
        ESP_LOGE(TAG, "Response had no usable fields");
        return ESP_ERR_INVALID_RESPONSE;
    }
    ESP_LOGI(TAG, "in %.1fC out %.1fC | rain %.1f mm/h, today %.1f mm (%s) | solar %.0f W/m2 | "
             "wind %d deg %.1f km/h",
             w.indoor_c, w.outdoor_c, w.rain_rate_mm_h, w.rain_today_mm, rain,
             w.solar_wm2, w.wind_dir_deg, w.wind_speed_kmh);
    *out = w;
    return ESP_OK;
}
