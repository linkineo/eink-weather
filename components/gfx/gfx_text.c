/*****************************************************************************
 * | File        :   gfx_text.c
 * | Function    :   Drawing helpers layered on top of the vendored GUI_Paint.
 * |
 * | Everything here goes through Paint_SetPixel() on the currently selected
 * | Paint image, so it honours the image's rotation, mirroring and bit layout.
 *
 * SPDX-License-Identifier: MIT
 * Part of the eink-weather project.
 *****************************************************************************/

#include "gfx.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* First and last glyph present in the STM ASCII font tables. */
#define GFX_FIRST_CHAR ' '  /* 32 */
#define GFX_LAST_CHAR  '~'  /* 126 */
#define GFX_FALLBACK_CHAR '?'

/**
 * Clip-safe pixel write.
 *
 * Paint_SetPixel() does bounds-check its arguments, but with '>' instead of
 * '>=' -- so x == Paint.Width slips through and writes into the first byte of
 * the next row, and at the bottom-right corner it writes one byte past the end
 * of the framebuffer. Coordinates are taken as uint32_t so callers can add an
 * offset without worrying about UWORD wrap-around.
 */
static void gfx_pixel(uint32_t x, uint32_t y, UWORD color)
{
    if (x >= (uint32_t)Paint.Width || y >= (uint32_t)Paint.Height) {
        return;
    }
    Paint_SetPixel((UWORD)x, (UWORD)y, color);
}

/** Fill an axis-aligned block of scale x scale pixels with one colour. */
static void gfx_fill_block(uint32_t x, uint32_t y, UBYTE scale, UWORD color)
{
    for (UBYTE dy = 0; dy < scale; dy++) {
        for (UBYTE dx = 0; dx < scale; dx++) {
            gfx_pixel(x + dx, y + dy, color);
        }
    }
}

void gfx_draw_text_scaled(UWORD x, UWORD y, const char *s, sFONT *font,
                          UBYTE scale, UWORD fg, UWORD bg)
{
    if (s == NULL || font == NULL || font->table == NULL || scale == 0) {
        return;
    }

    /*
     * Glyph table layout, mirroring Paint_DrawChar():
     *   row_bytes            = (Width + 7) / 8      (bytes per glyph row)
     *   glyph base           = (c - ' ') * Height * row_bytes
     *   pixel (col) in a row = row[col / 8] & (0x80 >> (col % 8))   -- MSB first
     */
    const uint32_t row_bytes = ((uint32_t)font->Width + 7u) / 8u;
    const uint32_t glyph_bytes = (uint32_t)font->Height * row_bytes;

    uint32_t pen_x = x;

    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < (unsigned char)GFX_FIRST_CHAR || c > (unsigned char)GFX_LAST_CHAR) {
            c = (unsigned char)GFX_FALLBACK_CHAR;
        }

        const uint8_t *glyph =
            &font->table[(uint32_t)(c - (unsigned char)GFX_FIRST_CHAR) * glyph_bytes];

        for (uint32_t row = 0; row < (uint32_t)font->Height; row++) {
            const uint8_t *row_ptr = glyph + row * row_bytes;
            const uint32_t py = (uint32_t)y + row * scale;

            for (uint32_t col = 0; col < (uint32_t)font->Width; col++) {
                const int on = row_ptr[col / 8u] & (0x80 >> (col % 8u));
                gfx_fill_block(pen_x + col * scale, py, scale, on ? fg : bg);
            }
        }

        pen_x += (uint32_t)font->Width * scale;
    }
}

UWORD gfx_text_width(const char *s, sFONT *font, UBYTE scale)
{
    if (s == NULL || font == NULL || scale == 0) {
        return 0;
    }

    const uint32_t width = (uint32_t)strlen(s) * font->Width * scale;

    return (width > UINT16_MAX) ? (UWORD)UINT16_MAX : (UWORD)width;
}

void gfx_draw_checkerboard(UWORD x, UWORD y, UWORD w, UWORD h, UWORD cell)
{
    if (w == 0 || h == 0 || cell == 0) {
        return;
    }

    /*
     * Clamp the extent to the image up front. The checkerboard phase is derived
     * from the offset inside the rectangle, so clipping the loop bounds cannot
     * shift the pattern, and it keeps x + dx from wrapping a UWORD.
     */
    const uint32_t avail_w = (x < Paint.Width) ? (uint32_t)(Paint.Width - x) : 0u;
    const uint32_t avail_h = (y < Paint.Height) ? (uint32_t)(Paint.Height - y) : 0u;
    const uint32_t span_w = ((uint32_t)w < avail_w) ? (uint32_t)w : avail_w;
    const uint32_t span_h = ((uint32_t)h < avail_h) ? (uint32_t)h : avail_h;

    for (uint32_t dy = 0; dy < span_h; dy++) {
        const uint32_t cy = dy / cell;

        for (uint32_t dx = 0; dx < span_w; dx++) {
            const uint32_t cx = dx / cell;

            gfx_pixel((uint32_t)x + dx, (uint32_t)y + dy,
                      ((cx + cy) & 1u) ? WHITE : BLACK);
        }
    }
}
