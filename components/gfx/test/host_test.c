/*****************************************************************************
 * | File        :   host_test.c
 * | Function    :   Host-side unit test for the gfx component.
 * |
 * | Compiles the vendored GUI_Paint.c, the fonts and gfx_text.c with the host
 * | compiler (see run_host_test.sh) and checks the framebuffer contents byte by
 * | byte. No hardware, no ESP-IDF -- esp_log.h comes from test/stub_include.
 * |
 * | Exit status 0 = all checks passed, 1 = at least one failed.
 *
 * SPDX-License-Identifier: MIT
 * Part of the eink-weather project.
 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"

#define W GFX_PANEL_WIDTH  /* 800 */
#define H GFX_PANEL_HEIGHT /* 480 */
#define STRIDE (W / 8)     /* 100 bytes per row */

/* Bytes of 0xA5 appended after each plane to catch out-of-bounds writes. */
#define CANARY_BYTES 64
#define CANARY_FILL 0xA5

static int g_checks;
static int g_failures;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            printf("  FAIL (%s:%d) ", __FILE__, __LINE__);                     \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

static void section(const char *name)
{
    printf("\n== %s\n", name);
}

/*---------------------------------------------------------------------------
 * Framebuffer helpers -- deliberately independent of GUI_Paint, so a bug in
 * Paint_SetPixel cannot hide behind a matching bug in the reader.
 *-------------------------------------------------------------------------*/

/** Non-zero when pixel (x, y) carries ink (bit == 0). */
static int px_black(const uint8_t *img, int x, int y)
{
    return (img[y * STRIDE + x / 8] & (0x80 >> (x % 8))) == 0;
}

static uint8_t *plane_alloc(void)
{
    uint8_t *p = malloc(GFX_PLANE_BYTES + CANARY_BYTES);
    if (p == NULL) {
        printf("out of memory\n");
        exit(2);
    }
    memset(p, 0x5A, GFX_PLANE_BYTES); /* junk, so Paint_Clear must do work */
    memset(p + GFX_PLANE_BYTES, CANARY_FILL, CANARY_BYTES);
    return p;
}

static int canary_intact(const uint8_t *p)
{
    for (int i = 0; i < CANARY_BYTES; i++) {
        if (p[GFX_PLANE_BYTES + i] != CANARY_FILL) {
            return 0;
        }
    }
    return 1;
}

/** Select @p p as the current image and clear it to white. */
static void plane_select_white(uint8_t *p)
{
    Paint_NewImage(p, W, H, ROTATE_0, WHITE);
    Paint_Clear(WHITE);
}

static int count_black(const uint8_t *img)
{
    int n = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            n += px_black(img, x, y);
        }
    }
    return n;
}

/*---------------------------------------------------------------------------
 * 1. Image setup / clear
 *-------------------------------------------------------------------------*/
static void test_fresh_image(uint8_t *a)
{
    section("fresh image is all white (0xFF)");

    plane_select_white(a);

    CHECK(GFX_PLANE_BYTES == 48000, "GFX_PLANE_BYTES is %d, expected 48000",
          (int)GFX_PLANE_BYTES);
    CHECK(Paint.Width == W && Paint.Height == H, "Paint geometry %ux%u",
          Paint.Width, Paint.Height);
    CHECK(Paint.WidthByte == STRIDE, "WidthByte is %u, expected %d",
          Paint.WidthByte, STRIDE);
    CHECK(Paint.Scale == 2, "Paint.Scale is %u, expected 2 (1 bpp)", Paint.Scale);

    int bad = -1;
    for (int i = 0; i < GFX_PLANE_BYTES; i++) {
        if (a[i] != 0xFF) {
            bad = i;
            break;
        }
    }
    CHECK(bad < 0, "byte %d is 0x%02X, expected 0xFF", bad, bad < 0 ? 0 : a[bad]);
    CHECK(count_black(a) == 0, "cleared image has %d black pixels", count_black(a));
    CHECK(canary_intact(a), "Paint_Clear wrote past the end of the plane");
}

/*---------------------------------------------------------------------------
 * 2. Rectangle
 *
 * Vendored quirk asserted on purpose: Paint_DrawPoint() with DOT_PIXEL_1X1 and
 * DOT_FILL_AROUND runs its inner loop once with XDir_Num == 0 and then calls
 * Paint_SetPixel(Xpoint + XDir_Num - Dot_Pixel, ...), i.e. it writes at
 * (x - 1, y - 1). Every Paint_DrawLine / Paint_DrawRectangle / Paint_DrawCircle
 * outline therefore lands one pixel up and to the left of the coordinates asked
 * for. Pinning it here means a future vendored update that changes it shows up
 * as a test failure instead of a mysteriously shifted layout.
 *-------------------------------------------------------------------------*/
#define VENDOR_DOT_BIAS 1

static void test_rectangle(uint8_t *a)
{
    section("Paint_DrawRectangle: border black, interior white");

    plane_select_white(a);

    const int x0 = 10, y0 = 20, x1 = 110, y1 = 70;
    Paint_DrawRectangle((UWORD)x0, (UWORD)y0, (UWORD)x1, (UWORD)y1, BLACK,
                        DOT_PIXEL_1X1, DRAW_FILL_EMPTY);

    const int bx0 = x0 - VENDOR_DOT_BIAS, by0 = y0 - VENDOR_DOT_BIAS;
    const int bx1 = x1 - VENDOR_DOT_BIAS, by1 = y1 - VENDOR_DOT_BIAS;
    printf("  border drawn at x %d..%d, y %d..%d (requested %d..%d / %d..%d)\n",
           bx0, bx1, by0, by1, x0, x1, y0, y1);

    /* Whole top and bottom edges. */
    for (int x = bx0; x <= bx1; x++) {
        if (!px_black(a, x, by0) || !px_black(a, x, by1)) {
            CHECK(0, "horizontal border gap at x=%d", x);
            break;
        }
    }
    /* Whole left and right edges. */
    for (int y = by0; y <= by1; y++) {
        if (!px_black(a, bx0, y) || !px_black(a, bx1, y)) {
            CHECK(0, "vertical border gap at y=%d", y);
            break;
        }
    }
    CHECK(1, "border scan completed"); /* record the two scans above as a pass */

    /* Corners. */
    CHECK(px_black(a, bx0, by0), "top-left corner not black");
    CHECK(px_black(a, bx1, by0), "top-right corner not black");
    CHECK(px_black(a, bx0, by1), "bottom-left corner not black");
    CHECK(px_black(a, bx1, by1), "bottom-right corner not black");

    /* Interior is untouched. */
    CHECK(!px_black(a, (bx0 + bx1) / 2, (by0 + by1) / 2), "interior centre is black");
    CHECK(!px_black(a, bx0 + 1, by0 + 1), "interior corner is black");
    CHECK(!px_black(a, bx1 - 1, by1 - 1), "interior corner is black");

    /* Just outside stays white. */
    CHECK(!px_black(a, bx0 - 1, by0), "pixel left of the border is black");
    CHECK(!px_black(a, bx1 + 1, by1), "pixel right of the border is black");
    CHECK(!px_black(a, bx0, by0 - 1), "pixel above the border is black");
    CHECK(!px_black(a, bx0, by1 + 1), "pixel below the border is black");

    /* Exact black-pixel count: perimeter of a (bx1-bx0+1) x (by1-by0+1) box. */
    const int w = bx1 - bx0 + 1, h = by1 - by0 + 1;
    const int expect = 2 * w + 2 * h - 4;
    CHECK(count_black(a) == expect, "border has %d black pixels, expected %d",
          count_black(a), expect);
    CHECK(canary_intact(a), "Paint_DrawRectangle wrote past the end of the plane");
}

/*---------------------------------------------------------------------------
 * 2b. Full-bleed border
 *
 * Because of the -1 bias above, a border on the outermost pixel row/column has
 * to be asked for as (1, 1) .. (800, 480), not (0, 0) .. (799, 479). With
 * (0, 0) the vendored code computes 0 - 1 in unsigned arithmetic (this is the
 * -Wtype-limits warning suppressed in CMakeLists.txt), lands on 65535, and
 * Paint_SetPixel drops it -- so the top row and left column silently vanish.
 * Pinned here because the app layer needs the working form.
 *-------------------------------------------------------------------------*/
static void test_full_bleed_border(uint8_t *a)
{
    section("full-bleed border needs (1,1)..(800,480), not (0,0)..(799,479)");

    /* The naive form loses the top row and the left column. */
    plane_select_white(a);
    Paint_DrawRectangle(0, 0, W - 1, H - 1, BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
    CHECK(!px_black(a, 0, 0), "(0,0)..(799,479) unexpectedly reaches the corner");
    printf("  (0,0)..(799,479): corner (0,0) black? %s -- top/left row lost\n",
           px_black(a, 0, 0) ? "yes" : "no");

    /* The corrected form covers the whole perimeter. */
    plane_select_white(a);
    Paint_DrawRectangle(1, 1, W, H, BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);

    int gaps = 0;
    for (int x = 0; x < W; x++) {
        gaps += !px_black(a, x, 0) + !px_black(a, x, H - 1);
    }
    for (int y = 0; y < H; y++) {
        gaps += !px_black(a, 0, y) + !px_black(a, W - 1, y);
    }
    CHECK(gaps == 0, "%d gaps in the full-bleed perimeter", gaps);
    CHECK(count_black(a) == 2 * W + 2 * H - 4, "perimeter ink is %d px, expected %d",
          count_black(a), 2 * W + 2 * H - 4);
    CHECK(!px_black(a, 1, 1), "perimeter is thicker than one pixel");
    CHECK(canary_intact(a), "full-bleed border wrote past the end of the plane");
    printf("  (1,1)..(800,480): full %dx%d perimeter, %d px, no overflow\n", W, H,
           count_black(a));
}

/*---------------------------------------------------------------------------
 * 3. Vendored text
 *-------------------------------------------------------------------------*/
static void test_vendor_text(uint8_t *a)
{
    section("Paint_DrawString_EN(0, 0, \"H\", &Font24, BLACK, WHITE)");

    plane_select_white(a);
    Paint_DrawString_EN(0, 0, "H", &Font24, BLACK, WHITE);

    const int n = count_black(a);
    printf("  glyph 'H' at scale 1 covers %d black pixels\n", n);
    CHECK(n > 0, "no black pixel produced");
    CHECK(!px_black(a, W - 1, H - 1), "pixel (799, 479) is not white");

    /* Ink must stay inside the glyph box: 17 x 24 at the origin. */
    int outside = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (px_black(a, x, y) && (x >= Font24.Width || y >= Font24.Height)) {
                outside++;
            }
        }
    }
    CHECK(outside == 0, "%d black pixels outside the %ux%u glyph box", outside,
          Font24.Width, Font24.Height);
    CHECK(canary_intact(a), "Paint_DrawString_EN wrote past the end of the plane");
}

/*---------------------------------------------------------------------------
 * 4. gfx_draw_text_scaled at scale 1 == Paint_DrawString_EN
 *-------------------------------------------------------------------------*/
static int first_diff(const uint8_t *a, const uint8_t *b)
{
    for (int i = 0; i < GFX_PLANE_BYTES; i++) {
        if (a[i] != b[i]) {
            return i;
        }
    }
    return -1;
}

static void compare_scale1(uint8_t *a, uint8_t *b, const char *font_name,
                           sFONT *font, const char *s, UWORD x, UWORD y,
                           UWORD fg, UWORD bg)
{
    plane_select_white(a);
    Paint_DrawString_EN(x, y, s, font, fg, bg);

    plane_select_white(b);
    gfx_draw_text_scaled(x, y, s, font, 1, fg, bg);

    const int d = first_diff(a, b);
    CHECK(d < 0,
          "%s \"%s\" at (%u,%u) fg=%02X: buffers differ at byte %d "
          "(vendor 0x%02X, gfx 0x%02X)",
          font_name, s, x, y, fg, d, d < 0 ? 0 : a[d], d < 0 ? 0 : b[d]);

    if (d < 0) {
        printf("  %-7s \"%s\" @(%3u,%3u) fg=0x%02X bg=0x%02X -> identical "
               "(%d black px)\n",
               font_name, s, x, y, fg, bg, count_black(b));
    }
}

static void test_scale1_matches_vendor(uint8_t *a, uint8_t *b)
{
    section("gfx_draw_text_scaled(scale=1) is byte-identical to "
            "Paint_DrawString_EN");

    /* One case per glyph row stride: 1 byte (Font8/12), 2 (Font16/20), 3 (Font24). */
    const char *msg = "Hello, World! 0123 #@?";

    compare_scale1(a, b, "Font8", &Font8, msg, 10, 40, BLACK, WHITE);
    compare_scale1(a, b, "Font12", &Font12, msg, 10, 40, BLACK, WHITE);
    compare_scale1(a, b, "Font16", &Font16, msg, 10, 40, BLACK, WHITE);
    compare_scale1(a, b, "Font20", &Font20, msg, 10, 40, BLACK, WHITE);
    compare_scale1(a, b, "Font24", &Font24, msg, 10, 40, BLACK, WHITE);

    /* Origin, and an odd x that straddles byte boundaries. */
    compare_scale1(a, b, "Font24", &Font24, "H", 0, 0, BLACK, WHITE);
    compare_scale1(a, b, "Font24", &Font24, msg, 7, 1, BLACK, WHITE);

    /* Inverted colours: here Paint_DrawChar paints the background too, so the
       comparison also covers the background pixels of the glyph box. */
    compare_scale1(a, b, "Font24", &Font24, msg, 10, 100, WHITE, BLACK);
    compare_scale1(a, b, "Font16", &Font16, msg, 3, 200, WHITE, BLACK);

    /*
     * Full printable ASCII sweep for every font, so every glyph offset in every
     * table is compared. Chunked at 32 characters because Paint_DrawString_EN
     * wraps to the next line once Xpoint + Font->Width exceeds Paint.Width and
     * gfx_draw_text_scaled truncates instead (see test_wrap_vs_truncate);
     * 32 * 17 == 544 px keeps even Font24 on one line.
     */
    static const int CHUNK = 32;
    sFONT *fonts[] = { &Font8, &Font12, &Font16, &Font20, &Font24 };
    const char *names[] = { "Font8", "Font12", "Font16", "Font20", "Font24" };

    for (size_t f = 0; f < sizeof(fonts) / sizeof(fonts[0]); f++) {
        for (int base = 0; base < 95; base += CHUNK) {
            char chunk[33];
            int n = 0;
            for (; n < CHUNK && base + n < 95; n++) {
                chunk[n] = (char)(' ' + base + n);
            }
            chunk[n] = '\0';
            compare_scale1(a, b, names[f], fonts[f], chunk, 0, 300, BLACK, WHITE);
        }
    }
}

/*---------------------------------------------------------------------------
 * 4b. Documented divergence from the vendored code
 *-------------------------------------------------------------------------*/
static void test_wrap_vs_truncate(uint8_t *a, uint8_t *b)
{
    section("documented divergence: vendor wraps, gfx truncates");

    /* 95 Font24 glyphs need 1615 px; the image is 800 px wide. */
    char ascii[95 + 1];
    for (int i = 0; i < 95; i++) {
        ascii[i] = (char)(' ' + i);
    }
    ascii[95] = '\0';

    plane_select_white(a);
    Paint_DrawString_EN(0, 0, ascii, &Font24, BLACK, WHITE);

    plane_select_white(b);
    gfx_draw_text_scaled(0, 0, ascii, &Font24, 1, BLACK, WHITE);

    CHECK(first_diff(a, b) >= 0,
          "vendor and gfx agree on an over-long string -- wrapping behaviour "
          "changed, update gfx.h");

    /* The vendor spills onto following rows; gfx keeps everything in row 0. */
    int vendor_below = 0, gfx_below = 0;
    for (int y = Font24.Height; y < H; y++) {
        for (int x = 0; x < W; x++) {
            vendor_below += px_black(a, x, y);
            gfx_below += px_black(b, x, y);
        }
    }
    printf("  ink below the first row: Paint_DrawString_EN %d px, "
           "gfx_draw_text_scaled %d px\n",
           vendor_below, gfx_below);
    CHECK(vendor_below > 0, "Paint_DrawString_EN did not wrap as expected");
    CHECK(gfx_below == 0, "gfx_draw_text_scaled wrapped instead of truncating");
    CHECK(canary_intact(a) && canary_intact(b), "plane overflow");
}

/*---------------------------------------------------------------------------
 * 5. Integer scaling
 *-------------------------------------------------------------------------*/
static void test_scaling(uint8_t *a, uint8_t *b)
{
    section("gfx_draw_text_scaled: nearest-neighbour block expansion");

    const UBYTE scale = 3;
    const char *s = "Hi";

    plane_select_white(a);
    gfx_draw_text_scaled(0, 0, s, &Font24, 1, BLACK, WHITE);

    plane_select_white(b);
    gfx_draw_text_scaled(0, 0, s, &Font24, scale, BLACK, WHITE);

    /* Every source pixel must appear as a scale x scale block. */
    const int sw = (int)strlen(s) * Font24.Width;
    const int sh = Font24.Height;
    int mismatches = 0;
    int first_x = -1, first_y = -1;

    for (int y = 0; y < sh && mismatches == 0; y++) {
        for (int x = 0; x < sw && mismatches == 0; x++) {
            const int want = px_black(a, x, y);
            for (int j = 0; j < scale; j++) {
                for (int i = 0; i < scale; i++) {
                    if (px_black(b, x * scale + i, y * scale + j) != want) {
                        mismatches++;
                        first_x = x * scale + i;
                        first_y = y * scale + j;
                    }
                }
            }
        }
    }
    CHECK(mismatches == 0, "scaled pixel (%d,%d) does not match its source block",
          first_x, first_y);
    printf("  %dx%d source pixels verified as %ux%u blocks\n", sw, sh, scale, scale);

    /* Explicit spot checks: find the first black source pixel and prove its
       whole block is black while the pixel just past it follows the source. */
    int sx = -1, sy = -1;
    for (int y = 0; y < sh && sx < 0; y++) {
        for (int x = 0; x < sw; x++) {
            if (px_black(a, x, y)) {
                sx = x;
                sy = y;
                break;
            }
        }
    }
    CHECK(sx >= 0, "no black pixel in the scale-1 reference");
    if (sx >= 0) {
        printf("  first ink at source (%d,%d) -> block (%d..%d, %d..%d)\n", sx, sy,
               sx * scale, sx * scale + scale - 1, sy * scale, sy * scale + scale - 1);
        CHECK(px_black(b, sx * 3 + 0, sy * 3 + 0), "block corner (0,0) not black");
        CHECK(px_black(b, sx * 3 + 1, sy * 3 + 1), "block centre not black");
        CHECK(px_black(b, sx * 3 + 2, sy * 3 + 2), "block corner (2,2) not black");
    }

    /* Total ink must be exactly scale^2 times the unscaled ink. */
    CHECK(count_black(b) == count_black(a) * scale * scale,
          "scaled ink is %d px, expected %d", count_black(b),
          count_black(a) * scale * scale);

    /* Extents. */
    const int total_w = (int)strlen(s) * Font24.Width * scale;
    const int total_h = Font24.Height * scale;
    CHECK(!px_black(b, total_w, 0), "ink at x == text width (%d)", total_w);
    CHECK(!px_black(b, 0, total_h), "ink at y == text height (%d)", total_h);
    CHECK(canary_intact(b), "gfx_draw_text_scaled wrote past the end of the plane");

    section("gfx_text_width");

    printf("  gfx_text_width(\"Hi\", &Font24, 3) = %u\n",
           gfx_text_width("Hi", &Font24, 3));
    CHECK(gfx_text_width("Hi", &Font24, 3) == 2 * 17 * 3,
          "expected %d, got %u", 2 * 17 * 3, gfx_text_width("Hi", &Font24, 3));
    CHECK(gfx_text_width("Hi", &Font24, 1) == 2 * 17, "scale 1 width wrong");
    CHECK(gfx_text_width("", &Font24, 3) == 0, "empty string width is not 0");
    CHECK(gfx_text_width(NULL, &Font24, 3) == 0, "NULL string width is not 0");
    CHECK(gfx_text_width("Hi", NULL, 3) == 0, "NULL font width is not 0");
    CHECK(gfx_text_width("Hi", &Font24, 0) == 0, "scale 0 width is not 0");
    CHECK(gfx_text_width("Hi", &Font8, 2) == 2 * 5 * 2, "Font8 width wrong");
    CHECK(gfx_text_width("ABCDE", &Font12, 4) == 5 * 7 * 4, "Font12 width wrong");

    /* The measured width must equal the actual pen advance. */
    plane_select_white(a);
    gfx_draw_text_scaled(0, 0, "Hi", &Font24, 3, BLACK, WHITE);
    plane_select_white(b);
    gfx_draw_text_scaled(gfx_text_width("Hi", &Font24, 3), 0, "Hi", &Font24, 3,
                         BLACK, WHITE);
    int contiguous = 1;
    for (int y = 0; y < Font24.Height * 3; y++) {
        for (int x = 0; x < 2 * 17 * 3; x++) {
            if (px_black(a, x, y) != px_black(b, x + 2 * 17 * 3, y)) {
                contiguous = 0;
            }
        }
    }
    CHECK(contiguous, "gfx_text_width does not match the pen advance");
}

/*---------------------------------------------------------------------------
 * 6. Non-printable characters fall back to '?'
 *-------------------------------------------------------------------------*/
static void test_fallback_char(uint8_t *a, uint8_t *b)
{
    section("characters outside 32..126 render as '?'");

    plane_select_white(a);
    gfx_draw_text_scaled(20, 20, "?A?", &Font24, 2, BLACK, WHITE);

    plane_select_white(b);
    gfx_draw_text_scaled(20, 20, "\x01" "A\xE9", &Font24, 2, BLACK, WHITE);

    const int d = first_diff(a, b);
    CHECK(d < 0, "fallback rendering differs at byte %d", d);

    /* Tab and DEL too. */
    plane_select_white(b);
    gfx_draw_text_scaled(20, 20, "\tA\x7F", &Font24, 2, BLACK, WHITE);
    CHECK(first_diff(a, b) < 0, "tab/DEL fallback differs");
}

/*---------------------------------------------------------------------------
 * 7. Checkerboard
 *-------------------------------------------------------------------------*/
static void test_checkerboard(uint8_t *a)
{
    section("gfx_draw_checkerboard");

    const int cell = 8;
    plane_select_white(a);
    gfx_draw_checkerboard(0, 0, 32, 32, (UWORD)cell);

    /* Phase: square (0,0) is black, neighbours alternate. */
    CHECK(px_black(a, 0, 0), "(0,0) not black");
    CHECK(px_black(a, 7, 7), "(7,7) not black (same square as the origin)");
    CHECK(!px_black(a, 8, 0), "(8,0) not white");
    CHECK(!px_black(a, 0, 8), "(0,8) not white");
    CHECK(px_black(a, 8, 8), "(8,8) not black");
    CHECK(px_black(a, 16, 0), "(16,0) not black");
    CHECK(!px_black(a, 24, 0), "(24,0) not white");
    CHECK(px_black(a, 31, 31), "(31,31) not black");

    /* Full-field check of the alternation rule. */
    int wrong = 0;
    for (int y = 0; y < 32; y++) {
        for (int x = 0; x < 32; x++) {
            const int want = (((x / cell) + (y / cell)) & 1) == 0;
            if (px_black(a, x, y) != want) {
                wrong++;
            }
        }
    }
    CHECK(wrong == 0, "%d of 1024 checkerboard pixels have the wrong colour", wrong);

    /* Exactly half the area is black for an even number of whole cells. */
    CHECK(count_black(a) == 32 * 32 / 2, "checkerboard ink is %d px, expected %d",
          count_black(a), 32 * 32 / 2);

    /* Nothing painted outside the rectangle. */
    CHECK(!px_black(a, 32, 0), "ink at x == w");
    CHECK(!px_black(a, 0, 32), "ink at y == h");

    /* Offset origin re-anchors the phase at (x, y). */
    plane_select_white(a);
    gfx_draw_checkerboard(100, 50, 24, 24, 4);
    CHECK(px_black(a, 100, 50), "offset checkerboard origin not black");
    CHECK(!px_black(a, 104, 50), "offset checkerboard phase wrong");
    CHECK(px_black(a, 104, 54), "offset checkerboard phase wrong");
    CHECK(!px_black(a, 99, 50), "ink one pixel left of the rectangle");
    CHECK(!px_black(a, 100, 49), "ink one pixel above the rectangle");

    /* Degenerate arguments are no-ops, not crashes or divisions by zero. */
    plane_select_white(a);
    gfx_draw_checkerboard(10, 10, 0, 10, 4);
    gfx_draw_checkerboard(10, 10, 10, 0, 4);
    gfx_draw_checkerboard(10, 10, 10, 10, 0);
    CHECK(count_black(a) == 0, "degenerate checkerboard painted %d px",
          count_black(a));
    CHECK(canary_intact(a), "gfx_draw_checkerboard wrote past the end of the plane");
}

/*---------------------------------------------------------------------------
 * 8. Clipping at the edges of the image
 *
 * Paint_SetPixel's own guard is off by one (x > Paint.Width instead of >=), so
 * a helper that trusted it would wrap into the next row and, in the bottom-right
 * corner, write one byte past the framebuffer. The gfx_* helpers clip
 * themselves; the canary proves it.
 *-------------------------------------------------------------------------*/
static void test_clipping(uint8_t *a)
{
    section("gfx_* helpers clip at the image edges");

    plane_select_white(a);

    gfx_draw_text_scaled((UWORD)(W - 10), (UWORD)(H - 10), "Overflow", &Font24, 4,
                         BLACK, WHITE);
    CHECK(canary_intact(a), "scaled text past the right/bottom edge overflowed");

    gfx_draw_checkerboard((UWORD)(W - 10), (UWORD)(H - 10), 200, 200, 8);
    CHECK(canary_intact(a), "checkerboard past the right/bottom edge overflowed");

    gfx_draw_text_scaled((UWORD)(W + 100), (UWORD)(H + 100), "Way off", &Font24, 2,
                         BLACK, WHITE);
    gfx_draw_checkerboard((UWORD)(W + 100), (UWORD)(H + 100), 50, 50, 4);
    CHECK(canary_intact(a), "fully off-image drawing overflowed");

    /* Wrap-around guard: x + w overflows a UWORD. */
    gfx_draw_checkerboard(65500, 470, 1000, 1000, 8);
    CHECK(canary_intact(a), "UWORD wrap-around in gfx_draw_checkerboard");

    /* Text that runs off the right edge is truncated, not wrapped onto the
       next line -- the last row of the image must stay untouched. */
    plane_select_white(a);
    gfx_draw_text_scaled((UWORD)(W - 20), 0, "TRUNCATED", &Font24, 1, BLACK, WHITE);
    int wrapped = 0;
    for (int y = Font24.Height; y < H; y++) {
        for (int x = 0; x < W; x++) {
            wrapped += px_black(a, x, y);
        }
    }
    CHECK(wrapped == 0, "%d px wrapped below the first text row", wrapped);
    CHECK(canary_intact(a), "truncated text overflowed");

    /* NULL / zero-scale arguments are no-ops. */
    plane_select_white(a);
    gfx_draw_text_scaled(10, 10, NULL, &Font24, 2, BLACK, WHITE);
    gfx_draw_text_scaled(10, 10, "x", NULL, 2, BLACK, WHITE);
    gfx_draw_text_scaled(10, 10, "x", &Font24, 0, BLACK, WHITE);
    gfx_draw_text_scaled(10, 10, "", &Font24, 2, BLACK, WHITE);
    CHECK(count_black(a) == 0, "no-op text call painted %d px", count_black(a));
}

/*---------------------------------------------------------------------------
 * 9. Two independent planes (black + red), as the app will use them
 *-------------------------------------------------------------------------*/
static void test_two_planes(uint8_t *black, uint8_t *red)
{
    section("black and red planes stay independent");

    plane_select_white(black);
    Paint_DrawString_EN(10, 10, "BLACK", &Font24, BLACK, WHITE);
    const int black_ink = count_black(black);

    plane_select_white(red);
    gfx_draw_text_scaled(10, 50, "RED", &Font24, 2, BLACK, WHITE);
    const int red_ink = count_black(red);

    CHECK(black_ink > 0, "black plane is empty");
    CHECK(red_ink > 0, "red plane is empty");
    CHECK(count_black(black) == black_ink,
          "drawing into the red plane changed the black plane");
    printf("  black plane %d px, red plane %d px\n", black_ink, red_ink);

    /* Re-selecting an existing plane must not clear it. */
    Paint_SelectImage(black);
    CHECK(count_black(black) == black_ink, "Paint_SelectImage cleared the plane");
    CHECK(canary_intact(black) && canary_intact(red), "plane overflow");
}

int main(void)
{
    printf("gfx host test -- 800x480 1bpp, %d bytes per plane\n", GFX_PLANE_BYTES);

    uint8_t *a = plane_alloc();
    uint8_t *b = plane_alloc();

    test_fresh_image(a);
    test_rectangle(a);
    test_full_bleed_border(a);
    test_vendor_text(a);
    test_scale1_matches_vendor(a, b);
    test_wrap_vs_truncate(a, b);
    test_scaling(a, b);
    test_fallback_char(a, b);
    test_checkerboard(a);
    test_clipping(a);
    test_two_planes(a, b);

    free(a);
    free(b);

    printf("\n%d checks, %d failures -- %s\n", g_checks, g_failures,
           g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
