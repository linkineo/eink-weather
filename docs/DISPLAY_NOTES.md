# Display notes and lessons learned

What it takes to drive the CrowPanel 5.79" e-paper reliably, and the other
pitfalls hit while building this project. Verified on hardware on 2026-10-06
unless stated otherwise.

## 1. Panel and driver

- **Two SSD1683 controllers in cascade.** The 792 × 272 panel is split into two
  396-column halves: a master and a slave. The master uses the normal SSD1683
  commands (`0x24` write RAM, `0x44/0x45` RAM window, `0x4E/0x4F` RAM address,
  `0x11` data entry mode). The slave is addressed by the same commands with bit 7
  set (`0xA4`, `0xC4/0xC5`, `0xCE/0xCF`, `0x91`) — see
  `components/crowpanel_epd/EPD_Init.c`.
- **800-px frame buffer with an 8-px seam.** The driver keeps an 800 × 272 1-bit
  buffer (27 200 bytes). `Paint_SetPixel()` adds 8 to x for x ≥ 396 so the two
  halves line up; the visible area is still 792 px. Always draw through
  `Paint_SetPixel()` (logical 0..791) and let it handle the seam.
- **Panel power is switched by GPIO7** (high = on). Drive it high and wait ~10 ms
  before talking to the controllers. Before deep sleep we drive it low and hold it
  (`gpio_hold_en` + `gpio_deep_sleep_hold_en`); release the hold
  (`gpio_hold_dis`) on wake before reconfiguring. The image stays without power.
- **SPI is bit-banged** by the vendor code (`spi.c`, CS toggled per byte, D/C set
  before each byte). It is slow but proven; we kept it unmodified. The Arduino
  primitives it uses (`pinMode`, `digitalWrite`, `delay`, …) are mapped to
  ESP-IDF in `compat.h`, so the vendor files compile almost verbatim.
- **Rotation:** `Paint_NewImage(..., Rotate, ...)`. The vendor header defines
  `Rotation 180`; we pass `DISPLAY_ROTATION 0` (the panel's native orientation) from `display.c` instead of editing the
  header.
- **Refresh sequence that works** (per update, ~5 s):
  ```c
  EPD_FastMode1Init();      // SW reset + temperature load
  EPD_Display_Clear();      // write white to both RAMs of both controllers
  EPD_Update();             // full refresh: removes ghosting
  EPD_FastMode1Init();
  EPD_Display(ImageBW);     // write the 800x272 buffer
  EPD_FastUpdate();         // fast refresh to show it
  EPD_DeepSleep();          // controllers to deep sleep (0x10)
  ```
  Then cut GPIO7. Partial refresh is available in the driver but not used.
- **1-bit only.** `WHITE = 0xFF`, `BLACK = 0x00` in the buffer. Design with pure
  black/white; use hatching instead of grey.

## 2. Fonts and graphics

- The vendor font is ASCII only (no `°`, no accents) and its largest size is
  24 × 48. We generate our own 1-bit fonts from **Barlow / Barlow Condensed**
  (OFL) with `tools/gen_assets.py`: Pillow rasterises each glyph, threshold 110/255
  keeps strokes solid. Text strings in C are UTF-8; `ui.c` decodes them.
- Icons are drawn with Pillow at 4× and downsampled, then thresholded — much
  cleaner than drawing small icons with primitives.
- `ui.c` has no ESP-IDF dependency, so `tools/preview/ui_preview.sh` renders the
  exact firmware output to PNG on the host. Use it before flashing.

## 3. Timing, sleep and the clock

- **Start SNTP only after Wi-Fi has an IP.** lwIP's SNTP waits a random 0–5 s
  before the first request; if that request is sent before the link is up it is
  lost and the next try comes only after the 15 s receive timeout. Starting SNTP
  in parallel with Wi-Fi made the sync miss an 8 s window.
- **`sntp_get_sync_status()` vs "time is valid".** After deep sleep the RTC still
  holds a plausible time, so "time > 2024" is not proof of a fresh sync. Use
  `timesync_wait_sntp()` (waits for `SNTP_SYNC_STATUS_COMPLETED`) when you need
  real NTP time, e.g. to measure drift.
- **The RTC slow clock drifts.** Measured +5.5 s over a 598 s sleep (ratio 0.9954)
  on the internal RC oscillator. `poller.c` compares RTC-kept time with SNTP on
  every timer wake and scales the next sleep. A 60 s wake lead covers boot (~0.6 s),
  Wi-Fi (~2–3 s), SNTP (≤ 5 s) and the drift; the panel was ready 24 s before the slot.
- **Deep sleep loses RAM.** The last good observation, the pending slot and the
  drift ratio live in `RTC_DATA_ATTR` memory. A reset/flash clears it (power-on path).
- Reflashing works while the board is in deep sleep: the USB-serial bridge's
  auto-reset (RTS/DTR → EN/IO0) wakes it into the bootloader.

## 4. Ecowitt API

- Values are **strings** at `data.<group>.<field>.value`; times are epoch strings.
- This station reports rain under **`rainfall_piezo`** (piezo gauge), not
  `rainfall`; the parser tries piezo first and falls back.
- Units are chosen by query parameters (`temp_unitid=1` °C, `rainfall_unitid=12`
  mm, `wind_speed_unitid=7` km/h, `solar_irradiance_unitid=16` W/m²,
  `pressure_unitid=3` hPa). Knots are computed on the device (km/h ÷ 1.852).
- HTTPS works with the ESP-IDF CA bundle (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE`).
- Never log request URLs: they contain the keys.

## 5. Build environment

- The previous Mac had ESP-IDF at `~/devl/esp-idf`; the tools in `./.espressif`
  were copied over but the Python venv pointed at a Python 3.13 that did not exist
  on the new machine. Recreate it with `idf_tools.py install-python-env` (Python 3.9
  works for IDF 5.0).
- New Kconfig symbols sometimes need `idf.py reconfigure` before the build sees
  them.
- Wi-Fi + TLS + cJSON + fonts do not fit the default 1 MB app partition:
  `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y` (1.5 MB).

## 6. History: the abandoned Waveshare 7.5" attempt

Before the CrowPanel, this repository targeted a **Waveshare e-Paper ESP32 Driver
Board** (ESP32-D0WDQ6) with a **Waveshare 7.5" (B) V2** panel (UC8179C). Bring-up
failed: diagnostics showed the controller treating every byte as a command (the
D/C line never seen high). The final entry (commit `ae68073`) records that even
Waveshare's unmodified demos failed, pointing to a hardware fault on the D/C
line. The full journal is in the git history (`git show ae68073:CLAUDE.md`).
Lessons worth keeping from it:

- When a controller ignores data, check **D/C and CS setup/hold timing** first,
  then rule out hardware by running the vendor's unmodified demo.
- Board switches matter (the Waveshare board needed SW1 on the 0.47 Ω position for
  7.5" panels).
