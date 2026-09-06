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
| PWR (panel rail enable) | 2 |
| PWR aux (header only)   | 33 |

SPI clock at bring-up is 4 MHz. See `main/board.h`.

**The panel has a software-controlled power rail, and it must be switched on
first.** GPIO2 runs through R35 to the base of Q32, which drives the P-MOSFET
Q31 in front of the RT9193 LDO that produces the panel rail `EPD_3.3V`: GPIO2
high = panel powered, and the reset state (low) leaves the panel dark.
Waveshare's own "Loader" firmware for this board calls the pin `PIN_SPI_CS_S`
and drives it high in `EPD_initSPI()` before it configures any other pin;
`PIN_SPI_PWR` (GPIO33) goes high in the same place and only reaches the
expansion header here, but is driven high too for parity. The driver raises
both at the top of `epd_bb_init_ctrl()`, so `epd_probe()`, `epd_dc_diag()`,
`epd_rst_diag()`, `epd_dcscan()` and `epd_bus_init()` all get power before
they touch a pin; `epd_power(bool)` switches the rail explicitly, and waits
200 ms the first time it comes up. Before wave 6 this pin was left low: the
controller ran on the leakage current of the signal lines, which is enough to
answer commands on BUSY (PON, DRF, POF all worked) but not to latch D/C or a
single data byte — so every diagnostic read `verdict=DC_NOT_SEEN` and every
refresh produced noise.

**The controller cannot be read back, and the firmware no longer tries.**
The 7.5inch e-Paper V2 specification says it twice, once for each serial mode:
"Under serial mode, only write operations are allowed." Two hardware runs
confirmed it — through the SPI peripheral in half-duplex 3-wire mode and
through hand-clocked GPIO — and both times a read returned the last bit the
ESP32 had driven on the shared SDA line, never controller data. So there is no
chip revision, no status flag word, no panel temperature and no glass-check
result to display anywhere: `epd_probe()` detects the panel from the **BUSY
power-on signature** alone (BUSY_N goes low while PON brings the rails up, then
returns high), and that is the only thing this controller ever tells us about
itself.

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

### Diagnostic mode: does the controller see D/C?

`CONFIG_APP_DIAG_ONLY=y` replaces the whole display path with
`epd_dc_diag()` (`components/epd_uc8179/epd_diag.c`). It answers one
electrical question — does the controller see the D/C (data/command) line? —
and **never refreshes the panel**, so unlike the display build it can be run
as often as needed.

The question is worth asking because commands demonstrably work here (PON
pulls BUSY low and releases it, DRF runs a real 17.8 s tri-colour refresh)
while data bytes apparently never do: both planes are clocked out in full and
the glass still comes up as random noise, i.e. the controller's RAM was never
written. A D/C line stuck low at the controller — an open contact on the FPC
or FFC, a broken trace — produces exactly that: every byte becomes a command,
so PON/DRF/POF still work and image bytes are executed as invalid commands.

With reads impossible, the only observable is the BUSY power-on signature, so
the diagnostic sends the byte `0x04` (PON) under four different D/C conditions
and watches BUSY for 400 ms each time:

| Oracle | What it sends | `pon=yes` means |
| ------ | ------------- | --------------- |
| D (control) | `0x04` with DC=0 | the detector works (expected) |
| A | CDI `0x50`, then `0x04` with DC=1 | a data byte executed as a command |
| B | DSLP `0x07` + `0xA5` with DC=1, then PON | the `0xA5` never armed deep sleep |
| E (polarity) | a lone `0x04` with DC=1 | D/C ignored, or inverted |

`verdict=DC_OK` (D yes, the rest no) means the line is fine and the noise has
another cause; `DC_NOT_SEEN` (all four yes) points at the connector — or, as
it turned out here, at the panel power rail: a controller running on the
leakage current of its signal lines produces exactly this result, so check
that `[EPD-TEST] pwr gpio2=1` is in the capture before suspecting the
hardware. `DC_INVERTED` (D no, E yes) means the sense is reversed. A pad check
on GPIO 27/26/15/13/14 runs first, with the panel held in reset, and reports
the read-back of each pin driven high and low (`1/0` = healthy).

```sh
sed -i '' 's|^# CONFIG_APP_DIAG_ONLY is not set$|CONFIG_APP_DIAG_ONLY=y|' sdkconfig
idf.py build
tools/flash.sh --capture --timeout 40 --expect "dctest"
```

Switch back with the reverse `sed` (`CONFIG_APP_DIAG_ONLY=y` →
`# CONFIG_APP_DIAG_ONLY is not set`) — or `idf.py menuconfig`, menu
"eink-weather bring-up".

## Repository layout

```
CMakeLists.txt         project definition
sdkconfig.defaults     target, flash and console settings
main/                  application: app_main.c, board.h, test_log.h,
                       Kconfig.projbuild (APP_DIAG_ONLY)
components/gfx/        1-bpp framebuffer and drawing primitives
                       (vendored Waveshare GUI_Paint + STM fonts, gfx_* helpers)
components/epd_uc8179/ UC8179 panel driver: epd_bus.c (write path, plain
                       full-duplex SPI), epd_bitbang.c (write-only GPIO
                       transport for the diagnostics, and owner of
                       RST/DC/BUSY and the panel power rail),
                       epd_uc8179.c (probe, init / display /
                       sleep sequences), epd_diag.c (D/C oracles, no refresh).
                       Kconfig: panel variant, BUSY timeout, SPI clock,
                       EPD_DATA_BITBANG
tools/env.sh           sourced: activates the ESP-IDF toolchain
tools/flash.sh         build + flash (+ optional capture)
tools/capture.py       non-interactive UART capture for automated checks
```

The firmware performs **exactly one panel refresh per boot** and puts the panel
back into deep sleep afterwards, on every exit path (Waveshare rule: a panel
left powered is damaged by the sustained high voltage). The `APP_DIAG_ONLY`
build performs **none**, and leaves the panel powered off and reset.

## Log contract

The firmware prints machine-readable lines prefixed with `[EPD-TEST] `
(see `main/test_log.h`). `tools/capture.py` parses them:

- `[EPD-TEST] boot chip=... mac=... idf=... reset=... heap=...` — boot banner
- `[EPD-TEST] pwr gpio2=1 gpio33=1` — the panel power rail is up and settled.
  Printed by both builds, immediately after the banner and before anything
  else touches a panel pin (see the hardware section above)
- `[EPD-TEST] probe verdict=PRESENT|ABSENT|UNCERTAIN busy_after_reset=.
  busy_low_ms=.. busy_release_ms=..` — controller probe (one line).
  `busy_low_ms` / `busy_release_ms` are the low-then-high BUSY signature after
  power-on, and they are the whole verdict: `PRESENT` when the signature
  completed (something answered a command on a pin the ESP32 pulls down, which
  nothing absent can fake), `ABSENT` when BUSY never went low, `UNCERTAIN` when
  it went low and never came back. The verdict is informational; the refresh is
  attempted whatever it says.
- `[EPD-TEST] dctest pads=dc:1/0,rst:1/0,cs:1/0,sck:1/0,mosi:1/0
  oracleD_pon=yes oracleA_pon=no oracleB_pon=no oracleE_pon=no
  verdict=DC_OK|DC_NOT_SEEN|DC_INVERTED|INCONCLUSIVE` — the D/C diagnostic
  (`CONFIG_APP_DIAG_ONLY`, one line, see above). This is the *only* panel line
  that build prints: no `init`, no `refresh_ms`, no `sleep`
- `[EPD-TEST] init ok` — panel initialisation sequence completed
- `[EPD-TEST] datapath=spi|bitbang` — which transport carried the image
  (`CONFIG_EPD_DATA_BITBANG`)
- `[EPD-TEST] refresh_ms=N` — wall-clock duration of the one full refresh
  (≈ 15000–25000 ms on the tri-colour panel, ≈ 4000 ms would mean a b/w LUT)
- `[EPD-TEST] sleep ok` — panel back in deep sleep
- `[EPD-TEST] DONE` — success marker, capture stops here (exit 0)
- `[EPD-TEST] ERROR stage=power|bus|init|display|sleep|dcdiag err=ESP_ERR_...` —
  failure marker (exit 1); `stage=display err=ESP_ERR_TIMEOUT` means BUSY never
  released
- `[EPD-TEST] alive uptime=Ns heap=N` — heartbeat every 10 s

Only `app_main` prints `[EPD-TEST]` lines; the driver logs through `ESP_LOG*`
under the `epd` tag (power-on, refresh and power-off BUSY durations), so the
two streams never interleave inside one line.

Capture exit codes: 0 ok, 1 error marker, 2 timeout, 3 pyserial missing,
4 a `--expect` substring never appeared.
