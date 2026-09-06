/*
 * board.h - pin map for the Waveshare "e-Paper ESP32 Driver Board".
 *
 * Source: official Waveshare e-Paper ESP32 Driver Board `DEV_Config.h`
 * (Waveshare e-Paper ESP32 demo) cross-checked against the board schematic.
 *
 * Hardware notes for bring-up:
 *  - SW1 "Display Config" must be set to A (0.47R) for the 7.5" panels.
 *    Position B (3R) is meant for the small panels and will not drive a 7.5".
 *  - SW2 (USB-UART power switch) must be ON, otherwise the board is not
 *    powered from USB and neither flashing nor the panel will work.
 *  - This board has no software-controlled panel power rail; the panel is
 *    always powered while the board is.
 */

#pragma once

/* SPI bus to the e-paper panel (HSPI pins on this board). */
#define BOARD_EPD_SCK   13
#define BOARD_EPD_MOSI  14
#define BOARD_EPD_CS    15

/* Panel control lines. */
#define BOARD_EPD_DC    27
#define BOARD_EPD_RST   26
#define BOARD_EPD_BUSY  25

/*
 * SPI clock used at bring-up for both writes and reads.
 * The UC8179 controller specifies a maximum read clock of 5 MHz, so keep this
 * at or below 4 MHz while any register/RAM read-back is in use.
 */
#define BOARD_EPD_SPI_HZ 4000000
