/*****************************************************************************
 * | File        :   gfx.h
 * | Function    :   Public umbrella header of the eink-weather gfx component.
 * |
 * | Wraps the vendored Waveshare GUI_Paint drawing library plus the STM ASCII
 * | fonts, and adds a few helpers the weather layout needs (integer-scaled text,
 * | text measurement, checkerboard fill).
 * |
 * | The component is pure software: it composes 1-bpp framebuffers in RAM and
 * | never touches SPI, GPIO or the panel. Pushing the buffers to the display is
 * | the job of the epd_uc8179 component.
 *
 * SPDX-License-Identifier: MIT
 * Part of the eink-weather project.
 *****************************************************************************/
#ifndef GFX_H_
#define GFX_H_

#include <stdint.h>

#include "GUI_Paint.h" /* PAINT, Paint_*(), WHITE/BLACK, ROTATE_*, DOT_PIXEL... */
#include "fonts.h"     /* sFONT, Font8/12/16/20/24                             */

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================
 * Framebuffer convention
 *===========================================================================
 *
 * Panel: Waveshare 7.5inch e-Paper (B) V2, 800 x 480, controller UC8179C.
 * It is a three-colour panel driven by TWO separate 1-bpp planes of
 * GFX_PLANE_BYTES (= 800 * 480 / 8 = 48000) bytes each:
 *
 *     plane 0 -> black ink
 *     plane 1 -> red ink
 *
 * Bit encoding inside a plane (this is the GUI_Paint "Scale == 2" layout):
 *
 *     bit == 1  ->  white, i.e. NO ink from this plane
 *     bit == 0  ->  ink    (black in the black plane, red in the red plane)
 *
 * Memory layout: row-major, 100 bytes per row (800 / 8), MSB first, so pixel
 * (x, y) lives in bit (0x80 >> (x % 8)) of byte [y * 100 + x / 8].
 *
 * GUI_Paint keeps ONE globally selected image (the `Paint` struct), so a plane
 * is drawn like this:
 *
 *     static uint8_t black[GFX_PLANE_BYTES];
 *     static uint8_t red[GFX_PLANE_BYTES];
 *
 *     Paint_NewImage(black, GFX_PANEL_WIDTH, GFX_PANEL_HEIGHT, ROTATE_0, WHITE);
 *     Paint_Clear(WHITE);
 *     Paint_DrawString_EN(10, 10, "Hello", &Font24, BLACK, WHITE);
 *
 *     Paint_NewImage(red, GFX_PANEL_WIDTH, GFX_PANEL_HEIGHT, ROTATE_0, WHITE);
 *     Paint_Clear(WHITE);
 *     gfx_draw_text_scaled(10, 60, "!", &Font24, 3, BLACK, WHITE);
 *
 * Note the asymmetry that trips people up: BOTH planes are drawn with the
 * colour constant BLACK wherever ink is wanted. BLACK/WHITE here name bit
 * values (0x00 / 0xFF), not the physical ink colour -- which plane the buffer
 * ends up in is what decides black versus red. GUI_Paint.h even defines
 * RED == BLACK for exactly this reason.
 *
 * Paint_NewImage() only records geometry; it does NOT clear the buffer. Call
 * Paint_Clear(WHITE) (or memset(buf, 0xFF, GFX_PLANE_BYTES)) after selecting a
 * freshly allocated plane.
 *
 * A pixel set in both planes at once is undefined on the panel (the two inks
 * fight); keep the planes disjoint.
 *
 * The two 48000-byte planes are far too big for the stack -- make them static,
 * or heap-allocate them (heap_caps_malloc / malloc). No alignment requirement
 * beyond the natural byte alignment; the SPI driver in epd_uc8179 may prefer
 * DMA-capable memory, which static .bss on the ESP32 internal RAM satisfies.
 */

/*===========================================================================
 * Vendored GUI_Paint quirks worth knowing
 *===========================================================================
 *
 * These are upstream Waveshare behaviours, left unmodified on purpose and
 * pinned by test/host_test.c so a future vendored update cannot change them
 * silently. They affect the Paint_* functions only -- the gfx_* helpers below
 * do their own clipping and have no coordinate bias.
 *
 * 1. Outlines are drawn one pixel up and to the left of the coordinates given.
 *    Paint_DrawPoint() ends up calling
 *        Paint_SetPixel(Xpoint + XDir_Num - Dot_Pixel, ...)
 *    which for DOT_PIXEL_1X1 evaluates to (x - 1, y - 1). Every
 *    Paint_DrawLine / Paint_DrawRectangle / Paint_DrawCircle outline inherits
 *    the bias.
 *
 *    Consequence: a full-bleed border must be asked for as
 *        Paint_DrawRectangle(1, 1, 800, 480, BLACK, DOT_PIXEL_1X1, ...)
 *    With (0, 0, 799, 479) the top row and left column are lost -- 0 - 1 is
 *    computed in unsigned arithmetic, wraps to 65535, and Paint_SetPixel drops
 *    it.
 *
 * 2. DOT_PIXEL_NxN is really (2N-1) x (2N-1) pixels with DOT_FILL_AROUND (the
 *    default), so DOT_PIXEL_2X2 gives a 3x3 dot, not 2x2.
 *
 * 3. Paint_SetPixel()'s bounds check uses '>' where it means '>=', so
 *    x == Paint.Width slips through and writes into the first byte of the next
 *    row; at the bottom-right corner that is one byte past the plane. Keep
 *    Paint_* coordinates strictly below 800 / 480 (see quirk 1 for the one
 *    place where 800 / 480 is nonetheless the right argument, since the -1 bias
 *    cancels it out).
 *
 * 4. Paint_DrawString_EN() wraps to a new line when a glyph would cross the
 *    right edge, and back to the top when it would cross the bottom.
 *    gfx_draw_text_scaled() truncates instead.
 *
 * 5. Paint_DrawNum(), Paint_DrawNumDecimals() and Paint_DrawTime() forward the
 *    colours in swapped order (Color_Background is passed as the foreground
 *    argument), so they render inverted: passing BLACK, WHITE gives white
 *    digits on a black box. Paint_DrawNum() and Paint_DrawNumDecimals() also
 *    put two 255-byte arrays on the stack (~510 bytes), and Paint_DrawNum()
 *    mishandles negative input: its digit loop uses C truncated modulo, so -42
 *    comes out as ",." rather than "-42". Prefer snprintf() plus
 *    Paint_DrawString_EN() or gfx_draw_text_scaled() -- relevant for the
 *    weather layout, where sub-zero temperatures are the normal case.
 */

#define GFX_PANEL_WIDTH  800
#define GFX_PANEL_HEIGHT 480

/** Bytes in one 1-bpp plane for the 800x480 panel: 48000. */
#define GFX_PLANE_BYTES  ((GFX_PANEL_WIDTH * GFX_PANEL_HEIGHT) / 8)

/*===========================================================================
 * Helpers
 *===========================================================================*/

/**
 * @brief Draw ASCII text scaled by an integer factor (nearest-neighbour).
 *
 * Renders onto the currently selected Paint image (see Paint_NewImage /
 * Paint_SelectImage). Glyphs are read from font->table exactly the way
 * Paint_DrawChar() reads them: ASCII offset 32, row stride
 * (font->Width + 7) / 8 bytes, MSB first, font->Height rows per glyph. At
 * scale == 1 the output is therefore identical to Paint_DrawString_EN() for the
 * same string, position and font.
 *
 * Each source pixel becomes a scale x scale block, so a character advances the
 * pen by font->Width * scale and occupies font->Height * scale rows.
 *
 * Characters outside the printable range 32..126 (including any byte >= 0x80,
 * i.e. non-ASCII/UTF-8 input) are rendered as '?'.
 *
 * Colours follow GUI_Paint: WHITE (0xFF) clears the bit, BLACK (0x00) sets ink.
 * Unlike Paint_DrawChar(), background pixels are ALWAYS painted with @p bg --
 * Paint_DrawChar() skips them when bg == WHITE, which lets whatever was already
 * in the buffer show through the glyph box. Drawing over existing content with
 * bg == WHITE therefore erases the glyph box here.
 *
 * Anything falling outside the current image is clipped per pixel; a string
 * running off the right edge is truncated, NOT wrapped (Paint_DrawString_EN
 * wraps).
 *
 * @param x      Left edge of the first glyph, in pixels.
 * @param y      Top edge of the text, in pixels.
 * @param s      NUL-terminated ASCII string; NULL is a no-op.
 * @param font   Font to render with, e.g. &Font24; NULL is a no-op.
 * @param scale  Integer magnification, >= 1; 0 is a no-op.
 * @param fg     Foreground colour (BLACK for ink).
 * @param bg     Background colour (usually WHITE).
 */
void gfx_draw_text_scaled(UWORD x, UWORD y, const char *s, sFONT *font,
                          UBYTE scale, UWORD fg, UWORD bg);

/**
 * @brief Width in pixels of @p s rendered with @p font at @p scale.
 *
 * Equals strlen(s) * font->Width * scale, i.e. the pen advance -- useful for
 * centering: x = (GFX_PANEL_WIDTH - gfx_text_width(s, f, n)) / 2.
 * Does not account for clipping, and returns 0 for NULL/empty input or
 * scale == 0. Saturates at UINT16_MAX rather than wrapping.
 */
UWORD gfx_text_width(const char *s, sFONT *font, UBYTE scale);

/**
 * @brief Fill a rectangle with a checkerboard of cell x cell squares.
 *
 * Covers [x, x + w) x [y, y + h) on the currently selected Paint image,
 * alternating BLACK and WHITE. The phase is anchored at (x, y): the square
 * containing (x, y) is BLACK, and a square is BLACK when
 * ((px - x) / cell + (py - y) / cell) is even.
 *
 * Squares along the right/bottom edge are truncated if w or h is not a multiple
 * of cell. Everything outside the current image is clipped. w == 0, h == 0 or
 * cell == 0 is a no-op.
 *
 * Useful as a bring-up test pattern and as a cheap 50% grey fill (cell == 1).
 */
void gfx_draw_checkerboard(UWORD x, UWORD y, UWORD w, UWORD h, UWORD cell);

#ifdef __cplusplus
}
#endif

#endif /* GFX_H_ */
