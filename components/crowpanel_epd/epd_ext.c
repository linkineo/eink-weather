// Extension helpers on top of the Elecrow CrowPanel vendor e-paper driver.
//
// EPD_ShowStringScaled() renders text with the built-in 24x48 font magnified by
// an integer factor, which the stock EPD_ShowChar() cannot do. This lets a short
// string span the full 792 px panel width (the vendor's largest font is only
// 24 px wide per glyph).
//
// The font table `ascii_4824` is defined (external linkage) in EPDfont.h, which
// is already compiled into EPD.c. We reference it here via `extern` instead of
// re-including the header, to avoid a duplicate-definition link error.
#include "EPD.h"

// ascii_4824[glyph][144]: one glyph is 24 columns wide x 48 rows tall, stored as
// 6 vertical byte-strips (8 rows each). Byte index i -> column (i % 24),
// strip (i / 24); bit m within the byte (LSB first) -> row = strip*8 + m.
extern const unsigned char ascii_4824[][144];

#define FONT4824_W 24
#define FONT4824_H 48
#define FONT4824_BYTES 144

void EPD_ShowStringScaled(uint16_t x, uint16_t y, const char *str,
                          uint16_t scale, uint16_t color)
{
    if (scale == 0) {
        scale = 1;
    }

    for (; *str != '\0'; str++) {
        // Printable ASCII only; the table starts at ' ' (0x20).
        if (*str < ' ') {
            x += FONT4824_W * scale;
            continue;
        }
        uint16_t glyph = (uint16_t)(*str - ' ');

        for (uint16_t i = 0; i < FONT4824_BYTES; i++) {
            unsigned char bits = ascii_4824[glyph][i];
            uint16_t col = i % FONT4824_W;
            uint16_t strip = i / FONT4824_W;
            for (uint16_t m = 0; m < 8; m++) {
                if (bits & 0x01) {
                    uint16_t dx = col;
                    uint16_t dy = (uint16_t)(strip * 8 + m);
                    uint16_t px = (uint16_t)(x + dx * scale);
                    uint16_t py = (uint16_t)(y + dy * scale);
                    for (uint16_t sy = 0; sy < scale; sy++) {
                        for (uint16_t sx = 0; sx < scale; sx++) {
                            Paint_SetPixel(px + sx, py + sy, color);
                        }
                    }
                }
                bits >>= 1;
            }
        }
        x += FONT4824_W * scale;
    }
}
