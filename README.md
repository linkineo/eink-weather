# eink-weather

ESP32 firmware for an e-ink weather display. Weather data will later be pulled
from an Ecowitt station; the current stage is **hardware bring-up** — boot,
identify the chip and drive the panel.

## Hardware

- **Board:** Waveshare *e-Paper ESP32 Driver Board* (ESP32-D0WDQ6 rev 1.0,
  dual core, 4 MB flash DIO, CP2102N USB-UART).
- **Panel:** Waveshare 7.5inch e-Paper (B) V2, 800x480, black/white/red
  (DKE DEPG0750RWU790F30HP, UC8179C controller).
- **Switches:** SW1 "Display Config" must be on **A (0.47R)** for 7.5" panels,
  SW2 (USB-UART power) must be **ON**.

### Pin map

| Signal | GPIO |
| ------ | ---- |
| SCK    | 13   |
| MOSI / DIN | 14 |
| CS     | 15   |
| DC     | 27   |
| RST    | 26   |
| BUSY   | 25   |

SPI clock at bring-up is 4 MHz (UC8179 reads must stay at or below 5 MHz).
There is no software-controlled panel power pin on this board.
See `main/board.h`.

## Quick start

```sh
source tools/env.sh          # activate ESP-IDF v5.0.8
idf.py set-target esp32      # once, generates sdkconfig from sdkconfig.defaults
idf.py build
tools/flash.sh --capture     # build, flash and verify the UART output
```

The default serial port is `/dev/cu.usbserial-110`; override it with
`PORT=/dev/cu.xxx tools/flash.sh`.

## Repository layout

```
CMakeLists.txt         project definition (wave 1 restricts COMPONENTS to main)
sdkconfig.defaults     target, flash and console settings
main/                  application: app_main.c, board.h, test_log.h
components/gfx/        framebuffer and drawing primitives (to be added)
components/epd_uc8179/ UC8179 panel driver (to be added)
tools/env.sh           sourced: activates the ESP-IDF toolchain
tools/flash.sh         build + flash (+ optional capture)
tools/capture.py       non-interactive UART capture for automated checks
```

## Log contract

The firmware prints machine-readable lines prefixed with `[EPD-TEST] `
(see `main/test_log.h`). `tools/capture.py` parses them:

- `[EPD-TEST] boot chip=... mac=... idf=... reset=... heap=...` — boot banner
- `[EPD-TEST] DONE` — success marker, capture stops here (exit 0)
- `[EPD-TEST] ERROR ...` — failure marker (exit 1)
- `[EPD-TEST] alive uptime=Ns heap=N` — heartbeat every 10 s

Capture exit codes: 0 ok, 1 error marker, 2 timeout, 3 pyserial missing,
4 a `--expect` substring never appeared.
