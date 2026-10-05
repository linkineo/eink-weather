# Météo Hésingue — product spec

A wall/shelf e-paper display that shows the live readings of a personal Ecowitt
weather station, refreshed every 15 minutes, running on a CrowPanel ESP32-S3
5.79" e-paper board.

## 1. Chosen design (layout "B", French)

![Design B mockup](img/design-b-mockup.png)

*Design mockup (from the design canvas, 792×272, 1-bit). Firmware renders of the
same layout, produced by `tools/preview/ui_preview.sh` from the real C code:*

| Dry, rained earlier today | Raining now |
|---|---|
| ![Firmware render, dry](img/firmware-render-dry.png) | ![Firmware render, raining](img/firmware-render-raining.png) |

Other proposals (A "Columns", C "Cards") were considered and rejected; B was
chosen for the strongest hierarchy on the wide 792×272 panel.

## 2. Content and hierarchy

| Priority | Element | Source field (Ecowitt API v3 `real_time`) | Rendering |
|---|---|---|---|
| Title | "Météo Hésingue" | — | Barlow SemiBold 22 px, top left |
| 1 | Outdoor temperature | `outdoor.temperature` (°C) | Barlow Condensed Bold 136 px, label "EXTÉRIEUR" |
| 1 | Indoor temperature | `indoor.temperature` (°C) | Condensed Bold 96 px, white on a black rounded block, label "INTÉRIEUR" |
| 2 | Raining now | `rainfall_piezo.rain_rate` > 0 (fallback `rainfall.rain_rate`) | Black pill "PLUIE" with cloud icon, else outlined pill "SEC" |
| 2 | Rained today | `rainfall_piezo.daily` > 0 (fallback `rainfall.daily`) | Filled drop + "Aujourd’hui 3,2 mm", else empty drop + "Pas de pluie aujourd’hui" |
| 2 | Solar irradiance | `solar_and_uvi.solar` (W/m²) | Horizontal gauge 0–1000 W/m² (hatched track, solid fill, quarter ticks) + value |
| 3 | Wind direction + speed | `wind.wind_direction` (°, direction the wind comes from), `wind.wind_speed` (km/h) | Compass ring next to the outdoor temperature; arrow points where the wind blows to; below it "SO · 5 kn": 8-point French label (N NE E SE S SO O NO) and speed in knots (km/h ÷ 1.852, rounded) |
| 4 (discreet) | Wi-Fi SSID | from build-time credentials | 13 px footer, left, with Wi-Fi icon |
| 4 (discreet) | Today's date + last update | device clock (SNTP) | 13 px footer, right: "mardi 6 octobre 2026 · mis à jour 14:45" |

Rules: pure black/white (no greys — e-paper is 1-bit here), large fonts, no
clutter. Decimal comma (French). Language: French labels on the device.

### States

- **Normal:** as above, footer says "mis à jour HH:MM" with the slot time.
- **Fetch failed:** last good values stay on screen, footer says
  "hors ligne, données de HH:MM".
- **No data since power-on:** temperatures "--", footer "en attente de données".
- **Sensor block missing** in the API response: that value shows "--".

## 3. Refresh schedule and power

- Refresh slots are aligned to the wall clock: **:00, :15, :30, :45** (multiples of
  `CONFIG_WEATHER_POLL_MINUTES` past local midnight).
- Between refreshes the ESP32-S3 is in **deep sleep** and the panel supply
  (GPIO7) is switched off and held low; the e-paper keeps its image unpowered.
- The RTC timer wakes the chip **60 s before** each slot
  (`CONFIG_WEATHER_WAKE_LEAD_SECONDS`), it connects to Wi-Fi, re-syncs the clock with
  SNTP, **fetches 30 s before the slot** (`CONFIG_WEATHER_FETCH_LEAD_SECONDS`) and
  redraws immediately, so the update is **on screen before the slot time**
  (measured: ready ~24 s early).
- The RTC slow clock drifts (~0.5 % measured). Each timer wake measures the drift
  against SNTP and scales the next sleep (ratio kept in RTC memory).
- **Power-on / reset:** fetch and draw immediately (~14 s from power to image),
  then sleep until the next slot.
- If no clock is available (no network at power-on), retry after one period.

## 4. Data source

Ecowitt cloud API v3 (https://doc.ecowitt.net/web/#/apiv3cn?page_id=2):

- `GET https://api.ecowitt.net/api/v3/device/list?application_key=…&api_key=…`
  → `data.list[0].mac` (used once, cached in NVS; override with `CONFIG_ECOWITT_MAC`).
- `GET https://api.ecowitt.net/api/v3/device/real_time?application_key=…&api_key=…&mac=…&call_back=all&temp_unitid=1&pressure_unitid=3&wind_speed_unitid=7&rainfall_unitid=12&solar_irradiance_unitid=16`
  → every value is a string at `data.<group>.<field>.value` (units: °C, hPa, km/h, mm, W/m²).

## 5. Configuration and secrets

- `wifi_creds` (`SSID=…`, `PASSWORD=…`) and `ecowitt_creds` (`api_key=…`,
  `app_key=…`) in the project root, **git-ignored**. CMake turns them into
  `build/…/secrets.h` at build time. Secrets are never logged.
- Kconfig (`idf.py menuconfig` → "Weather display"): poll minutes, fetch lead,
  wake lead, POSIX timezone (default `CET-1CEST,M3.5.0,M10.5.0/3`), optional MAC.
- Note: the keys are stored in the firmware image in plain form; anyone with
  physical access to the board can read them from flash.

## 6. Out of scope (for now)

Battery operation tuning, partial refresh, buttons/rotary switch, humidity,
pressure, UV, multiple languages on the device, OTA updates.
