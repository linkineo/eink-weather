/*****************************************************************************
 * | File        :   DEV_Config.h
 * | Function    :   Minimal shim for the Waveshare RaspberryPi GUI library.
 * |
 * | This is NOT the vendored Waveshare DEV_Config.h. The original pulls in the
 * | whole Raspberry Pi hardware abstraction (bcm2835 / wiringPi / lgpio, SPI and
 * | GPIO prototypes) which has no meaning on an ESP32 and would collide with the
 * | panel driver component. GUI_Paint.{h,c} only needs the three integer
 * | typedefs from it, so that is all this shim provides.
 * |
 * | Panel / SPI access lives in the separate epd_uc8179 component; nothing in
 * | the gfx component touches hardware.
 *
 * SPDX-License-Identifier: MIT
 * Part of the eink-weather project.
 *****************************************************************************/
#ifndef GFX_DEV_CONFIG_SHIM_H_
#define GFX_DEV_CONFIG_SHIM_H_

#include <stdint.h>

/*
 * Kept as object-like macros exactly like the upstream Waveshare header, rather
 * than as C typedefs. Vendored e-paper sources habitually define the same three
 * names; an identical macro redefinition is legal C, whereas a typedef that
 * meets a macro of the same name is a hard error. The #ifndef guards make this
 * header safe to include alongside another vendored DEV_Config.h.
 */
#ifndef UBYTE
#define UBYTE   uint8_t
#endif

#ifndef UWORD
#define UWORD   uint16_t
#endif

#ifndef UDOUBLE
#define UDOUBLE uint32_t
#endif

#endif /* GFX_DEV_CONFIG_SHIM_H_ */
