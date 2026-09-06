# CLAUDE.md — eink-weather developer notes

Internal notes to pick this project up without prior context. User-facing overview: [README.md](README.md).

## What this is

ESP32 firmware for an e-ink weather display. Target product: show the current readings of an
Ecowitt weather station (Ecowitt API v3, same data as the macOS app Caelio in `~/devl/ecowitt`,
whose `Claude.md` documents the API fields and unit ids). Current stage (2026-09-06): **hardware
bring-up** — boot banner on UART, panel probe, "Hello, World!" on the panel. No WiFi yet.

## Hardware (verified 2026-09-06)

- Board: **Waveshare e-Paper ESP32 Driver Board** — ESP32-D0WDQ6 rev v1.0, dual core 240 MHz,
  40 MHz crystal, **4 MB flash (DIO)**, USB-UART **CP2102N** (Apple native driver).
  Serial port on this Mac: **`/dev/cu.usbserial-110`**. MAC `3c:61:05:11:8f:4c`.
  Auto-reset via DTR/RTS works: flashing needs no button.
- Panel: **Waveshare 7.5inch e-Paper (B) V2** — 800×480, **black/white/red**, controller
  **UC8179C**. Label on the panel: `DEPG0750RWU790F30HP` (DKE; `RW` = red/white tri-color,
  `BN` would be black/white). Full refresh ≈ 15–25 s (a b/w panel does ≈ 4 s).
- E-paper wiring on this board (from Waveshare `DEV_Config.h` and the schematic):

  | Signal | GPIO | Note |
  |---|---|---|
  | SCK / CLK | 13 | |
  | MOSI / DIN (controller SDA, bidirectional) | 14 | reads happen on this line |
  | CS | 15 | |
  | DC | 27 | |
  | RST | 26 | active low |
  | BUSY | 25 | input, **low = busy** |

  No panel power pin: the panel's 3.3 V comes from an always-on LDO.
- Switches: **SW1 "Display Config" must be on A (0.47 Ω)** for 7.5" panels (B = 3 Ω is for the
  small 1.54/2.13/2.9 b/w panels). **SW2 (USB-UART power) must stay ON** or flashing stops working.
- Panel safety rules (Waveshare): put the panel to **deep sleep after every refresh** (long
  high-voltage exposure damages it irreversibly); refresh interval **≥ 180 s**; tri-color panels
  should be refreshed at least once every 24 h.

## Toolchain — rules that must not be broken

- ESP-IDF **v5.0.8-452-gd9f9b7d8ed** lives in `/Users/2pat/devl/esp-idf`. **Never change that
  clone's commit/branch** (`git checkout/pull/submodule update/rebase` are forbidden there; the
  user needs this exact commit for another project). Activate with `source tools/env.sh`
  (which sources `export.sh`).
- Missing tools are installed with `python $IDF_PATH/tools/idf_tools.py install <tool>` into
  `~/.espressif/tools` — never through the clone. cmake 3.30.2 and ninja 1.12.1 were installed
  that way on 2026-09-06. Homebrew's cmake 4.x is not used once the IDF env is exported.
- The IDF Python venv (`~/.espressif/python_env/idf5.0_py3.13_env`) provides `esptool` and
  `pyserial`; use its `python` (first on PATH after export) for `tools/capture.py`.

## Build, flash, verify

```bash
source tools/env.sh && idf.py set-target esp32 && idf.py build
tools/flash.sh --capture --timeout 90          # build + flash + UART capture until [EPD-TEST] DONE
python tools/capture.py --expect "verdict=PRESENT"   # capture only (resets the board first)
```

Log contract for automation: every machine-readable line starts with `[EPD-TEST]` (see
`main/test_log.h`): `boot ...`, `probe ...`, `init ok`, `refresh_ms=...`, `sleep ok`, `DONE`,
`alive uptime=..s`, `ERROR stage=... err=...`. `tools/capture.py` exits 0 on `DONE`, 1 on
`ERROR`, 2 on timeout, 4 when an `--expect` substring is missing.

## Repository layout

- `main/` — application: `app_main.c` (banner → probe → Hello World → sleep → heartbeat),
  `board.h` (pin map), `test_log.h` (log macro).
- `components/epd_uc8179/` — panel driver: `epd_bus.c` (spi_master half-duplex **3-wire** on
  SPI2_HOST, DC via `pre_cb`, register reads = command transaction with `SPI_TRANS_CS_KEEP_ACTIVE`
  + receive-only transaction), `epd_uc8179.c` (init/clear/display/sleep transcribed from Waveshare
  `EPD_7in5b_V2.c` V2.0 2024-08-07, probe via REV 0x70 / FLG 0x71 / TSC 0x40 / PBC 0x44 and the
  BUSY low→high signature after power-on). Kconfig: panel variant (`EPD_PANEL_7IN5B_V2` default,
  `EPD_PANEL_7IN5_V2` b/w), `EPD_BUSY_TIMEOUT_MS`, `EPD_SPI_HZ` (≤ 5 MHz while reads share the
  device).
- `components/gfx/` — vendored Waveshare `GUI_Paint` + STMicro fonts (8/12/16/20/24 px) plus
  `gfx_draw_text_scaled`, `gfx_text_width`, `gfx_draw_checkerboard`. Framebuffers are 1 bpp,
  48000 bytes per plane, **bit 1 = white / no ink**; black plane and red plane are two images.
- `tools/` — `env.sh`, `flash.sh`, `capture.py`.

## Controller facts worth remembering (UC8179 command table, panel spec)

- Data planes: `0x10` old/black data, `0x13` new/red data. Tri-color: `0x10` ← black plane as-is,
  `0x13` ← red plane **inverted**. B/W variant: `0x10` ← image, `0x13` ← `~image`.
- PSR `0x00`: `0x0F` tri-color (BWROTP), `0x1F` b/w (BWOTP). CDI `0x50`: `11 07` tri-color,
  `10 07` b/w. TRES `0x61`: `03 20 01 E0`. Sleep: `0x50 F7`, `0x02`, `0x07 A5`.
- Reads: SDA is bidirectional; read clock ≤ 5 MHz (tscycr ≥ 200 ns), writes ≤ 10 MHz.
  REV `0x70` → 7 bytes, last = CHIP_REV fixed `0x0C`. FLG `0x71` → b6 PTL, b5 I2C_ERR,
  b4 I2C_BUSYN, b3 DATA_FLAG, b2 PON, b1 POF, b0 BUSY_N. TSC `0x40` → byte 0 = °C (int8).
  PBC `0x44` → bit 0 = panel glass check pass.
- Upstream references: github.com/waveshareteam/e-Paper → `RaspberryPi_JetsonNano/c/lib/`
  (`e-Paper/EPD_7in5b_V2.c`, `GUI/GUI_Paint.c`, `Fonts/`), Waveshare wiki pages
  "E-Paper ESP32 Driver Board" and "7.5inch e-Paper HAT (B) Manual".

## Workflow

Fable (architecture, specs, review, hardware verification) orchestrates Opus agents
(`Agent`, `model: "opus"`) that write the code in waves on disjoint file sets. Code, comments
and docs in English; conversation with the user in French. Always finish a change with
build → flash → `capture.py` → visual check on the panel, then update this file and README.
