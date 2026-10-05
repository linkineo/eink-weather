// Bitmap font / icon formats produced by tools/gen_assets.py.
#ifndef _UI_TYPES_H_
#define _UI_TYPES_H_

#include <stdint.h>

// Bitmaps are packed rows, MSB first, (w + 7) / 8 bytes per row, 1 = ink.
typedef struct {
    uint16_t cp;        // Unicode code point
    uint8_t w;          // bitmap width
    uint8_t adv;        // advance width
    int8_t x0;          // bitmap x offset from the pen position (<= 0)
    uint32_t off;       // byte offset into the font's bit data
} ui_glyph_t;

typedef struct {
    uint8_t h;          // glyph box height (all glyphs)
    uint8_t ascent;     // baseline, from the box top
    uint8_t cap_top;    // top of capitals / digits, from the box top
    uint8_t count;
    const ui_glyph_t *glyphs;
    const uint8_t *bits;
} ui_font_t;

typedef struct {
    uint8_t w;
    uint8_t h;
    const uint8_t *bits;
} ui_bitmap_t;

#endif // _UI_TYPES_H_
