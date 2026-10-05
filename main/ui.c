// Weather screen, layout "B" (French), from the design canvas:
//
//   +----------------------------------------------+----------------+
//   | Météo Hésingue                               |#  INTÉRIEUR   #|
//   | EXTÉRIEUR                                    |#              #|
//   | 12,4°  (compass)                             |#   21,6°      #|
//   |        SO · 5 kn                             |#              #|
//   +----------------------------------------------+----------------+
//   | (sun) [####////////] 486 W/m²        (PLUIE|SEC) (drop) today  |
//   +------------------------------------------------------------------+
//   (wifi) SSID                       mardi 6 octobre 2026 · mis à jour 14:45
//
// Coordinates are panel pixels; text is placed by its baseline.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "ui.h"
#include "ui_assets.h"

#define MARGIN_L 18
#define MARGIN_R (UI_W - 18)

// Indoor block (inverted).
#define IN_X0 534
#define IN_Y0 12
#define IN_Y1 184
#define IN_RADIUS 14

// Middle band with the solar gauge and rain state.
#define BAND_Y0 192
#define BAND_Y1 241
#define BAND_CY 217
#define BAR_X0 58
#define BAR_W 258
#define BAR_Y0 207
#define BAR_H 16
#define SOLAR_MAX_WM2 1000.0f

#define FOOT_BASE 263
#define KMH_PER_KNOT 1.852f

static ui_px_fn s_px;

static void px(int x, int y, int black)
{
    if (x >= 0 && x < UI_W && y >= 0 && y < UI_H) {
        s_px(x, y, black);
    }
}

static void fill_rect(int x0, int y0, int w, int h, int black)
{
    for (int y = y0; y < y0 + h; y++) {
        for (int x = x0; x < x0 + w; x++) {
            px(x, y, black);
        }
    }
}

static void fill_round_rect(int x0, int y0, int w, int h, int r, int black)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int cx = x < r ? r : (x >= w - r ? w - r - 1 : x);
            int cy = y < r ? r : (y >= h - r ? h - r - 1 : y);
            int dx = x - cx, dy = y - cy;
            if (dx * dx + dy * dy <= r * r) {
                px(x0 + x, y0 + y, black);
            }
        }
    }
}

static void stroke_round_rect(int x0, int y0, int w, int h, int r, int t)
{
    fill_round_rect(x0, y0, w, h, r, 1);
    fill_round_rect(x0 + t, y0 + t, w - 2 * t, h - 2 * t, r - t, 0);
}

static void ring(int cx, int cy, float r_in, float r_out)
{
    int ro = (int)ceilf(r_out);
    for (int y = -ro; y <= ro; y++) {
        for (int x = -ro; x <= ro; x++) {
            float d = sqrtf((float)(x * x + y * y));
            if (d >= r_in && d <= r_out) {
                px(cx + x, cy + y, 1);
            }
        }
    }
}

static float edge(float ax, float ay, float bx, float by, float x, float y)
{
    return (bx - ax) * (y - ay) - (by - ay) * (x - ax);
}

static void fill_triangle(float ax, float ay, float bx, float by, float cx, float cy)
{
    int x0 = (int)floorf(fminf(ax, fminf(bx, cx))), x1 = (int)ceilf(fmaxf(ax, fmaxf(bx, cx)));
    int y0 = (int)floorf(fminf(ay, fminf(by, cy))), y1 = (int)ceilf(fmaxf(ay, fmaxf(by, cy)));
    float area = edge(ax, ay, bx, by, cx, cy);
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float fx = x + 0.5f, fy = y + 0.5f;
            float e0 = edge(bx, by, cx, cy, fx, fy);
            float e1 = edge(cx, cy, ax, ay, fx, fy);
            float e2 = edge(ax, ay, bx, by, fx, fy);
            if ((area > 0 && e0 >= 0 && e1 >= 0 && e2 >= 0) ||
                (area < 0 && e0 <= 0 && e1 <= 0 && e2 <= 0)) {
                px(x, y, 1);
            }
        }
    }
}

static void bitmap(const ui_bitmap_t *b, int x0, int y0, int black)
{
    int stride = (b->w + 7) / 8;
    for (int y = 0; y < b->h; y++) {
        for (int x = 0; x < b->w; x++) {
            if (b->bits[y * stride + x / 8] & (0x80 >> (x % 8))) {
                px(x0 + x, y0 + y, black);
            }
        }
    }
}

// ---- text -------------------------------------------------------------------

static uint32_t utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t cp;
    int n;
    if (p[0] < 0x80) {
        cp = p[0];
        n = 1;
    } else if ((p[0] & 0xE0) == 0xC0 && p[1]) {
        cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
        n = 2;
    } else if ((p[0] & 0xF0) == 0xE0 && p[1] && p[2]) {
        cp = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        n = 3;
    } else {
        cp = '?';
        n = 1;
        while (p[n] && (p[n] & 0xC0) == 0x80) {
            n++;
        }
    }
    *s += n;
    return cp;
}

static const ui_glyph_t *glyph(const ui_font_t *f, uint32_t cp)
{
    for (int i = 0; i < f->count; i++) {
        if (f->glyphs[i].cp == cp) {
            return &f->glyphs[i];
        }
    }
    for (int i = 0; i < f->count; i++) {   // fallback for missing characters
        if (f->glyphs[i].cp == '?') {
            return &f->glyphs[i];
        }
    }
    return NULL;
}

static int text_width(const ui_font_t *f, const char *s, int tracking)
{
    int w = 0, n = 0;
    while (*s) {
        const ui_glyph_t *g = glyph(f, utf8_next(&s));
        if (g) {
            w += g->adv + tracking;
            n++;
        }
    }
    return n ? w - tracking : 0;
}

// Draw `s` with its baseline at `base`; returns the x after the last glyph.
static int text(const ui_font_t *f, int x, int base, const char *s, int tracking, int black)
{
    int top = base - f->ascent;
    while (*s) {
        const ui_glyph_t *g = glyph(f, utf8_next(&s));
        if (!g) {
            continue;
        }
        int stride = (g->w + 7) / 8;
        const uint8_t *bits = f->bits + g->off;
        for (int y = 0; y < f->h; y++) {
            for (int gx = 0; gx < g->w; gx++) {
                if (bits[y * stride + gx / 8] & (0x80 >> (gx % 8))) {
                    px(x + g->x0 + gx, top + y, black);
                }
            }
        }
        x += g->adv + tracking;
    }
    return x - tracking;
}

static void text_right(const ui_font_t *f, int x_right, int base, const char *s, int tracking)
{
    text(f, x_right - text_width(f, s, tracking), base, s, tracking, 1);
}

// "12,4°" (French decimal comma); "--" when the value is missing.
static void fmt_temp(char *buf, size_t n, bool ok, float c)
{
    if (!ok) {
        snprintf(buf, n, "--");
        return;
    }
    if (fabsf(c) < 0.05f) {
        c = 0.0f;
    }
    snprintf(buf, n, "%.1f°", c);
    char *dot = strchr(buf, '.');
    if (dot) {
        *dot = ',';
    }
}

// ---- widgets ------------------------------------------------------------------

// Wind compass: ring with N/E/S/W ticks and an arrow pointing where the wind blows.
static void compass(int cx, int cy, int from_deg)
{
    ring(cx, cy, 23.0f, 25.0f);
    fill_rect(cx - 1, cy - 26, 2, 6, 1);
    fill_rect(cx - 1, cy + 20, 2, 6, 1);
    fill_rect(cx - 26, cy - 1, 6, 2, 1);
    fill_rect(cx + 20, cy - 1, 6, 2, 1);

    float a = (float)((from_deg + 180) % 360) * (float)M_PI / 180.0f;
    float ca = cosf(a), sa = sinf(a);
    // Arrow pointing up (screen -y) before rotation: tip, right, notch, left.
    const float pts[4][2] = {{0, -18}, {7, 4}, {0, -1}, {-7, 4}};
    float r[4][2];
    for (int i = 0; i < 4; i++) {
        r[i][0] = cx + pts[i][0] * ca - pts[i][1] * sa;
        r[i][1] = cy + pts[i][0] * sa + pts[i][1] * ca;
    }
    fill_triangle(r[0][0], r[0][1], r[1][0], r[1][1], r[2][0], r[2][1]);
    fill_triangle(r[0][0], r[0][1], r[2][0], r[2][1], r[3][0], r[3][1]);
}

static const char *compass_point(int deg)
{
    static const char *pts[] = {"N", "NE", "E", "SE", "S", "SO", "O", "NO"};
    return pts[((deg % 360 + 360) % 360 * 2 + 45) / 90 % 8];
}

// Horizontal gauge: hatched track, solid fill up to the value, quarter ticks.
static void solar_bar(float wm2)
{
    float ratio = wm2 / SOLAR_MAX_WM2;
    ratio = ratio < 0 ? 0 : (ratio > 1 ? 1 : ratio);
    int fill_w = (int)lroundf(ratio * BAR_W);
    for (int y = BAR_Y0; y < BAR_Y0 + BAR_H; y++) {
        for (int x = BAR_X0; x < BAR_X0 + BAR_W; x++) {
            bool frame = y < BAR_Y0 + 2 || y >= BAR_Y0 + BAR_H - 2 ||
                         x < BAR_X0 + 2 || x >= BAR_X0 + BAR_W - 2;
            bool ink = frame || x < BAR_X0 + fill_w || (x + y) % 5 == 0;
            px(x, y, ink);
        }
    }
    for (int i = 0; i <= 4; i++) {
        int x = BAR_X0 + i * (BAR_W - 2) / 4;
        fill_rect(x, BAR_Y0 + BAR_H + 3, 2, (i % 2) ? 4 : 6, 1);
    }
}

// ---- screen ---------------------------------------------------------------------

static void draw_footer(const status_t *st, time_t now)
{
    static const char *days[] = {"dimanche", "lundi", "mardi", "mercredi",
                                 "jeudi", "vendredi", "samedi"};
    static const char *months[] = {"janvier", "février", "mars", "avril", "mai", "juin",
                                   "juillet", "août", "septembre", "octobre",
                                   "novembre", "décembre"};
    bitmap(&icon_wifi, MARGIN_L, FOOT_BASE - 13, 1);
    text(&font_small, MARGIN_L + 20, FOOT_BASE, st->ssid[0] ? st->ssid : "--", 0, 1);

    char buf[96] = "";
    struct tm lt;
    localtime_r(&now, &lt);
    if (lt.tm_year + 1900 >= 2024) {   // clock is set
        snprintf(buf, sizeof(buf), "%s %d %s %d", days[lt.tm_wday], lt.tm_mday,
                 months[lt.tm_mon], lt.tm_year + 1900);
    }
    if (st->have_data) {
        struct tm ut;
        localtime_r(&st->last_update, &ut);
        size_t len = strlen(buf);
        snprintf(buf + len, sizeof(buf) - len, "%s%s %02d:%02d", len ? " · " : "",
                 st->stale ? "hors ligne, données de" : "mis à jour", ut.tm_hour, ut.tm_min);
    } else {
        size_t len = strlen(buf);
        snprintf(buf + len, sizeof(buf) - len, "%sen attente de données", len ? " · " : "");
    }
    text_right(&font_small, MARGIN_R, FOOT_BASE, buf, 0);
}

static void draw_rain(const weather_t *w)
{
    if (!w->has_rain) {
        text_right(&font_body, MARGIN_R, BAND_CY + 5, "Pluie : --", 0);
        return;
    }
    char today[48];
    if (weather_rained_today(w)) {
        snprintf(today, sizeof(today), "Aujourd’hui %.1f mm", w->rain_today_mm);
        char *dot = strchr(today, '.');
        if (dot) {
            *dot = ',';
        }
    } else {
        snprintf(today, sizeof(today), "Pas de pluie aujourd’hui");
    }
    int tx = MARGIN_R - text_width(&font_body, today, 0);
    text(&font_body, tx, BAND_CY + 5, today, 0, 1);
    const ui_bitmap_t *drop = weather_rained_today(w) ? &icon_drop : &icon_drop_empty;
    int dx = tx - 6 - drop->w;
    bitmap(drop, dx, BAND_CY - drop->h / 2, 1);

    if (weather_raining_now(w)) {
        const char *s = "PLUIE";
        int pw = 12 + icon_cloud_rain.w + 6 + text_width(&font_label, s, 1) + 12;
        int px0 = dx - 16 - pw;
        fill_round_rect(px0, BAND_CY - 13, pw, 26, 13, 1);
        bitmap(&icon_cloud_rain, px0 + 12, BAND_CY - icon_cloud_rain.h / 2, 0);
        text(&font_label, px0 + 12 + icon_cloud_rain.w + 6, BAND_CY + 5, s, 1, 0);
    } else {
        const char *s = "SEC";
        int pw = 12 + text_width(&font_label, s, 1) + 12;
        int px0 = dx - 16 - pw;
        stroke_round_rect(px0, BAND_CY - 12, pw, 24, 12, 2);
        text(&font_label, px0 + 12, BAND_CY + 5, s, 1, 1);
    }
}

void ui_render(const weather_t *w, const status_t *st, time_t now, ui_px_fn plot)
{
    s_px = plot;
    char buf[32];

    // Outdoor: title, label, big temperature, wind compass.
    text(&font_title, MARGIN_L, 38, "Météo Hésingue", 0, 1);
    text(&font_label, MARGIN_L, 60, "EXTÉRIEUR", 3, 1);
    fmt_temp(buf, sizeof(buf), st->have_data && w->has_outdoor, w->outdoor_c);
    int x = text(&font_big, MARGIN_L - 2, 168, buf, -4, 1);
    if (st->have_data && w->has_wind) {
        int cx = x + 16 + 28;
        compass(cx, 102, w->wind_dir_deg);
        // Direction and speed in knots (API delivers km/h).
        char wind[24];
        snprintf(wind, sizeof(wind), "%s · %d kn", compass_point(w->wind_dir_deg),
                 (int)lroundf(w->wind_speed_kmh / KMH_PER_KNOT));
        text(&font_label, cx - text_width(&font_label, wind, 0) / 2, 148, wind, 0, 1);
    }

    // Indoor: inverted rounded block.
    fill_round_rect(IN_X0, IN_Y0, MARGIN_R - IN_X0, IN_Y1 - IN_Y0, IN_RADIUS, 1);
    bitmap(&icon_house, IN_X0 + 20, 30, 0);
    text(&font_label, IN_X0 + 20 + icon_house.w + 8, 44, "INTÉRIEUR", 3, 0);
    fmt_temp(buf, sizeof(buf), st->have_data && w->has_indoor, w->indoor_c);
    text(&font_mid, IN_X0 + 18, 166, buf, -3, 0);

    // Band: solar gauge + rain state.
    fill_rect(MARGIN_L, BAND_Y0, MARGIN_R - MARGIN_L, 2, 1);
    fill_rect(MARGIN_L, BAND_Y1 - 1, MARGIN_R - MARGIN_L, 2, 1);
    bitmap(&icon_sun, MARGIN_L, BAND_CY - icon_sun.h / 2, 1);
    bool solar_ok = st->have_data && w->has_solar;
    solar_bar(solar_ok ? w->solar_wm2 : 0.0f);
    if (solar_ok) {
        snprintf(buf, sizeof(buf), "%d", (int)lroundf(w->solar_wm2));
    } else {
        snprintf(buf, sizeof(buf), "--");
    }
    x = text(&font_val, BAR_X0 + BAR_W + 16, BAND_CY + 11, buf, 0, 1);
    text(&font_unit, x + 5, BAND_CY + 11, "W/m²", 0, 1);
    if (st->have_data) {
        draw_rain(w);
    }

    draw_footer(st, now);
}
