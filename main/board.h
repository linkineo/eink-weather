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
 *  - This board revision DOES have a software-controlled panel power rail,
 *    on a pin the Waveshare demo code never mentions as such. See
 *    BOARD_EPD_PWR below: until wave 6 the firmware left it low, the panel
 *    ran on the leakage current of the signal lines, and every run behaved as
 *    if the controller ignored D/C.
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
 * Panel power enable, active high.
 *
 * The board netlist routes GPIO2 through R35 to the base of Q32, which drives
 * the gate of the P-MOSFET Q31 sitting in front of the RT9193 LDO that
 * produces the panel rail EPD_3.3V. So GPIO2 high = panel powered, GPIO2 low
 * (the reset state) = panel rail off.
 *
 * Waveshare's own "Loader" firmware for this board calls the pin
 * PIN_SPI_CS_S and drives it high in EPD_initSPI(), before it touches any
 * other pin -- which is why that firmware drives this panel and earlier waves
 * of this one did not.
 */
#define BOARD_EPD_PWR      2

/*
 * Waveshare Loader PIN_SPI_PWR, driven high in the same place. On this board
 * revision GPIO33 only reaches the expansion header, so it powers nothing
 * here; it is driven high anyway, for parity with the firmware that is known
 * to work on this hardware.
 */
#define BOARD_EPD_PWR_AUX  33

/*
 * SPI clock used at bring-up for both writes and reads.
 * The UC8179 controller specifies a maximum read clock of 5 MHz, so keep this
 * at or below 4 MHz while any register/RAM read-back is in use.
 */
#define BOARD_EPD_SPI_HZ 4000000
