#ifndef _WIFI_H_
#define _WIFI_H_

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Start the station interface and connect to the SSID from secrets.h.
// Reconnects automatically (with backoff) whenever the link drops.
esp_err_t wifi_start(void);

// Block until an IP address is obtained or `timeout_ms` elapses.
bool wifi_wait_connected(uint32_t timeout_ms);

bool wifi_is_connected(void);
const char *wifi_ssid(void);
// RSSI of the current AP in dBm, or 0 when not connected.
int8_t wifi_rssi(void);

#endif // _WIFI_H_
