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
  Serial port on this Mac: **`/dev/cu.usbserial-110`** or `-10` (depends on the USB port used). MAC `3c:61:05:11:8f:4c`.
  Auto-reset via DTR/RTS works: flashing needs no button.
- Panel: **Waveshare 7.5inch e-Paper (B) V3** (sticker "V3" on the panel; Waveshare: fully compatible
  with the V2 demo/protocol) — 800×480, **black/white/red**, controller **UC8179C**. Label on the panel: `DEPG0750RWU790F30HP` (DKE; `RW` = red/white tri-color,
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

  Panel power: the schematic has a MOSFET switch (GPIO2 → R35 → Q32 → Q31) in front of the
  `EPD_3.3V` LDO and the Waveshare Loader drives GPIO2 and GPIO33 high; the firmware does the same
  (`BOARD_EPD_PWR`, `BOARD_EPD_PWR_AUX`), but on this unit it changed nothing measurable.
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

- `main/` — application: `app_main.c` (banner → probe → Hello World → sleep → heartbeat, or the
  no-refresh D/C diagnostic when `CONFIG_APP_DIAG_ONLY=y`), `board.h` (pin map), `test_log.h`
  (log macro), `Kconfig.projbuild` (bring-up options).
- `components/epd_uc8179/` — panel driver: `epd_bitbang.c` (GPIO transport transcribed from
  Waveshare `DEV_Config.cpp`; owns RST/DC/BUSY; used by the probe and by the diagnostic data path),
  `epd_bus.c` (spi_master **full-duplex write-only** on SPI2_HOST, DC via `pre_cb`, DMA chunks of
  4000 bytes), `epd_uc8179.c` (init/clear/display/sleep transcribed from Waveshare `EPD_7in5b_V2.c`
  V2.0 2024-08-07; probe = BUSY low→high signature after power-on, run in bit-bang mode before the
  SPI bus exists), `epd_diag.c` (D/C oracle: data bytes that trigger a PON signature prove the
  controller does not see D/C). Kconfig: panel variant (`EPD_PANEL_7IN5B_V2` default, `EPD_PANEL_7IN5_V2` b/w),
  `EPD_BUSY_TIMEOUT_MS`, `EPD_SPI_HZ`, `EPD_DATA_BITBANG` (diagnostic: send everything by GPIO).
- `components/gfx/` — vendored Waveshare `GUI_Paint` + STMicro fonts (8/12/16/20/24 px) plus
  `gfx_draw_text_scaled`, `gfx_text_width`, `gfx_draw_checkerboard`. Framebuffers are 1 bpp,
  48000 bytes per plane, **bit 1 = white / no ink**; black plane and red plane are two images.
- `tools/` — `env.sh`, `flash.sh`, `capture.py`.

## Controller facts worth remembering (UC8179 command table, panel spec)

- Data planes: `0x10` old/black data, `0x13` new/red data. Tri-color: `0x10` ← black plane as-is,
  `0x13` ← red plane **inverted**. B/W variant: `0x10` ← image, `0x13` ← `~image`.
- PSR `0x00`: `0x0F` tri-color (BWROTP), `0x1F` b/w (BWOTP). CDI `0x50`: `11 07` tri-color,
  `10 07` b/w. TRES `0x61`: `03 20 01 E0`. Sleep: `0x50 F7`, `0x02`, `0x07 A5`.
- **Register reads do not work on this module** (verified 2026-09-06 with the SPI peripheral in
  3-wire half-duplex mode and with bit-banged GPIO: every byte read equals the last bit the ESP32
  drove). The panel spec says serial mode allows write operations only. Presence detection relies
  on the BUSY power-on signature (PON → BUSY low → high after ~130 ms), not on REV/FLG/TSC.
- Upstream references: github.com/waveshareteam/e-Paper → `RaspberryPi_JetsonNano/c/lib/`
  (`e-Paper/EPD_7in5b_V2.c`, `GUI/GUI_Paint.c`, `Fonts/`), Waveshare wiki pages
  "E-Paper ESP32 Driver Board" and "7.5inch e-Paper HAT (B) Manual".

## Bring-up journal (2026-09-06)

1. **Run 1** (half-duplex 3-wire SPI device, DMA chunks): commands accepted (PON → BUSY signature,
   DRF → genuine 17.8 s tri-color refresh, POF ok), but the panel showed uniform random pixel
   noise and register reads returned `7f ff ff…` (floating line).
2. **Run 2** (plain full-duplex SPI for writes, bit-bang probe): planes clocked out in exactly the
   wire time (96 ms per 48000 bytes at 4 MHz), refresh 17.8 s, **still uniform noise**. Bit-bang
   reads return the last driven bit → reads are impossible on this module (spec: write-only).
3. **Run 3** (no-refresh D/C oracle, `CONFIG_APP_DIAG_ONLY=y`, bit-bang): control PON → BUSY
   signature at 131 ms; a data byte 0x04 sent after CDI, the 0xA5 check byte of DSLP, and a lone
   data byte 0x04 all trigger the very same PON signature → **verdict `DC_NOT_SEEN`**, identical on
   two runs. The controller executes every byte as a command: the D/C line never reaches it high.
   ESP32 pads read back correctly (no short on the board side), so the fault is an open somewhere
   between GPIO27 and the panel's FPC pin 11 (panel FPC → adapter board → FFC → board connector),
   or a wrong bus-select strap.
4. **Run 4** (panel FPC plugged directly into the board, no adapter/FFC): identical `DC_NOT_SEEN`.
   RST oracle inconclusive (POF pulses BUSY 41 ms whatever the power state, so it cannot tell a
   reset controller from a powered one; BUSY never moves during the RST pulse). D/C GPIO scan over
   2,4,5,16,17,18,19,21,22,23,32,33: none honoured → D/C is not routed to another GPIO.
5. **Run 5** (GPIO2 and GPIO33 driven high like the Waveshare Loader's `EPD_initSPI()`; the
   schematic shows GPIO2 → R35 → Q32 → Q31 → LDO `EPD_3.3V`): no change at all, same timings to
   the millisecond → the panel rail was already on; GPIO2 gates nothing measurable on this unit.
   The Loader's 7.5 (B) V2 init/show/load sequence is identical to ours (PSR 0x0F, CDI 11 07,
   black plane as-is in 0x10, red plane inverted in 0x13), so no driver/panel mismatch either.
   Everything software-side is exhausted: the controller decodes 8-bit command frames (4-wire
   mode) but its D/C input reads low → open on FPC pin 11 (board connector / trace / panel flex).
   Physical checks pending: continuity IO27 ↔ connector pin 11, inspection of contacts 10–11.
6. **Run 6** (3-wire hypothesis, 9-bit frames, `wiretest`): `[0][0x04]` as a 9-bit frame gives the
   40 ms POF signature (the controller latched the first 8 bits = 0x02 and dropped the 9th),
   9-bit DSLP+0xA5 frames never arm deep sleep in either bit order, plain 8-bit PON always works →
   **FOUR_WIRE**, positively proven. Flipping the FPC in the connector kills all communication
   (BUSY stuck low, RST read back high through a cross-connection), so the original orientation is
   the right one and the board-side RST path is electrically intact. Software is exhausted:
   Waveshare's Loader (same pins, same bit-bang, same init) is the last arbiter to re-run.
   The board now enumerates as `/dev/cu.usbserial-10` (USB port changed).
7. **Run 7 — arbitration with Waveshare's own code** (`arduino-cli` 1.5.1 + esp32 core 3.3.11,
   unmodified `esp32-waveshare-epd` library, example `epd7in5b_V2-demo`, sources kept in the
   session scratchpad `arduino_ref/`): `EPD_7IN5B_V2_Clear()` gave a clean **white** screen → data
   bytes DO reach the RAM on this board+panel with Arduino-speed bit-banging. The demo's next step
   (`Init_Fast` + demo image) produced noise and BUSY never released (> 76 s) — the fast mode is
   not usable on this panel batch; we only use the normal init. Root cause of our failures:
   **D/C (and CS) setup time**. Our transports raised D/C and started clocking ~100–200 ns later
   (`gpio_set_level` ≈ 60 ns, `cs_ena_pretrans` = 0), Arduino's `digitalWrite` gives ≈ 0.4 µs per
   call; the controller latched the previous D/C level, so every data byte was a command. Fix:
   explicit setup/hold delays in both transports (run 8).
8. **Run 8** — setup/hold margins added (D/C 5 µs then 50 µs, CS 2/2 then 10/10 µs, SPI
   `cs_ena_pretrans/posttrans` = 2): `DC_NOT_SEEN` unchanged, a lone data byte 0x04 still powers the
   panel on. Timing is NOT the difference. Re-reading run 7: Waveshare's Clear stream is 0xFF (no
   such command) and 0x00 (PSR without parameter) — harmless as commands — so the white screen was
   the panel RAM's power-on content after the user had unplugged the board, not written data; and
   `Init_Fast` contains 0xE0 **0x02** which, read as a command, powers the panel off → the hang.
   Confirmed by running the vendor demo with only the normal-mode drawing block: the refresh
   started at +20.6 s and BUSY never released in 150 s. **Vendor code fails identically → the D/C
   line is open on this board/panel pair today**, whatever worked in the past.

Panel-care rule during bring-up: one refresh per flash cycle, ≥ 60 s between refreshes, sleep
after every refresh; use `--after no_reset` when flashing so a capture reset does not cause a
second, interrupted refresh.

## Workflow

Fable (architecture, specs, review, hardware verification) orchestrates Opus agents
(`Agent`, `model: "opus"`) that write the code in waves on disjoint file sets. Code, comments
and docs in English; conversation with the user in French. Always finish a change with
build → flash → `capture.py` → visual check on the panel, then update this file and README.
