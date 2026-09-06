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

SPI clock at bring-up is 4 MHz. There is no software-controlled panel power pin
on this board. See `main/board.h`.

**Register reads are not possible on this panel.** The 7.5inch e-Paper V2
specification says it twice, once for each serial mode: "Under serial mode,
only write operations are allowed." The controller never drives SDA back, so
`epd_probe()` reports `reads=bad` and its verdict rests on the BUSY line
instead (see the log contract below). The probe still attempts the reads —
they cost 11 bytes and they are the evidence for that statement.

## Quick start

```sh
source tools/env.sh          # activate ESP-IDF v5.0.8
idf.py set-target esp32      # once, generates sdkconfig from sdkconfig.defaults
idf.py build
tools/flash.sh --capture     # build, flash and verify the UART output
```

The default serial port is `/dev/cu.usbserial-110`; override it with
`PORT=/dev/cu.xxx tools/flash.sh`.

### Data path and the bit-bang fallback

Commands and image data go out over the SPI peripheral as a plain full-duplex,
write-only device. The half-duplex + 3-wire configuration that an earlier
revision used (so that reads could share the device) let short commands
through but silently dropped the 4000-byte DMA image chunks, and the panel came
up as noise; reads never worked either way, so nothing was lost by dropping it.

`CONFIG_EPD_DATA_BITBANG=y` replaces the whole write path with the hand-clocked
GPIO transport of `components/epd_uc8179/epd_bitbang.c` (the same one the probe
always uses). It is a diagnostic: about 1 s per 48000-byte plane instead of
96 ms, but with no peripheral and no DMA in the picture. Turn it on without
menuconfig with

```sh
sed -i '' 's|^# CONFIG_EPD_DATA_BITBANG is not set$|CONFIG_EPD_DATA_BITBANG=y|' sdkconfig
idf.py build
```

and check the resulting `[EPD-TEST] datapath=` line in the capture.

## Repository layout

```
CMakeLists.txt         project definition
sdkconfig.defaults     target, flash and console settings
main/                  application: app_main.c, board.h, test_log.h
components/gfx/        1-bpp framebuffer and drawing primitives
                       (vendored Waveshare GUI_Paint + STM fonts, gfx_* helpers)
components/epd_uc8179/ UC8179 panel driver: epd_bus.c (write path, plain
                       full-duplex SPI), epd_bitbang.c (GPIO transport for the
                       probe, and owner of RST/DC/BUSY), epd_uc8179.c (probe,
                       init / display / sleep sequences). Kconfig: panel
                       variant, BUSY timeout, SPI clock, EPD_DATA_BITBANG
tools/env.sh           sourced: activates the ESP-IDF toolchain
tools/flash.sh         build + flash (+ optional capture)
tools/capture.py       non-interactive UART capture for automated checks
```

The firmware performs **exactly one panel refresh per boot** and puts the panel
back into deep sleep afterwards, on every exit path (Waveshare rule: a panel
left powered is damaged by the sustained high voltage).

## Log contract

The firmware prints machine-readable lines prefixed with `[EPD-TEST] `
(see `main/test_log.h`). `tools/capture.py` parses them:

- `[EPD-TEST] boot chip=... mac=... idf=... reset=... heap=...` — boot banner
- `[EPD-TEST] probe verdict=PRESENT|ABSENT|UNCERTAIN reads=ok|bad
  chip_rev=0x.. prod=.. lut=.. flg=0x.. temp=..C pbc=..
  busy_after_reset=. busy_low_ms=.. busy_release_ms=..` — controller probe
  (one line). `busy_low_ms` / `busy_release_ms` are the low-then-high BUSY
  signature after power-on: both ≥ 0 means the controller answered a command
  on a pin the ESP32 pulls down, which is the presence test that works here.
  `reads=` says whether the register reads carried real data (`CHIP_REV`
  == `0x0C`); on this panel it is always `bad`, and `chip_rev`, `prod`, `lut`,
  `flg`, `temp` and `pbc` are then garbage from an undriven line — the panel
  prints `n/a` for them rather than a plausible-looking number. The verdict is
  informational only; the refresh is attempted whatever it says.
- `[EPD-TEST] init ok` — panel initialisation sequence completed
- `[EPD-TEST] datapath=spi|bitbang` — which transport carried the image
  (`CONFIG_EPD_DATA_BITBANG`)
- `[EPD-TEST] refresh_ms=N` — wall-clock duration of the one full refresh
  (≈ 15000–25000 ms on the tri-colour panel, ≈ 4000 ms would mean a b/w LUT)
- `[EPD-TEST] sleep ok` — panel back in deep sleep
- `[EPD-TEST] DONE` — success marker, capture stops here (exit 0)
- `[EPD-TEST] ERROR stage=bus|init|display|sleep err=ESP_ERR_...` — failure
  marker (exit 1); `stage=display err=ESP_ERR_TIMEOUT` means BUSY never
  released
- `[EPD-TEST] alive uptime=Ns heap=N` — heartbeat every 10 s

Only `app_main` prints `[EPD-TEST]` lines; the driver logs through `ESP_LOG*`
under the `epd` tag (power-on, refresh and power-off BUSY durations), so the
two streams never interleave inside one line.

Capture exit codes: 0 ok, 1 error marker, 2 timeout, 3 pyserial missing,
4 a `--expect` substring never appeared.
