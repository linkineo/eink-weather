// Host preview of main/ui.c: renders sample states to PGM files.
//   tools/preview/ui_preview.sh  -> build/preview/*.png
#include <stdio.h>
#include <string.h>
#include "ui.h"

static unsigned char fb[UI_H][UI_W];

static void plot(int x, int y, int black)
{
    fb[y][x] = black ? 0 : 255;
}

static void render(const char *path, const weather_t *w, const status_t *st, time_t now)
{
    memset(fb, 255, sizeof(fb));
    ui_render(w, st, now, plot);
    FILE *f = fopen(path, "wb");
    fprintf(f, "P5\n%d %d\n255\n", UI_W, UI_H);
    fwrite(fb, 1, sizeof(fb), f);
    fclose(f);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    char path[256];
    time_t now = 1791238500;   // 2026-10-06 ~14:15 local
    weather_t w = {
        .indoor_c = 21.6f, .outdoor_c = 12.4f, .rain_rate_mm_h = 0.0f, .rain_today_mm = 3.2f,
        .solar_wm2 = 486.0f, .wind_speed_kmh = 9.0f, .wind_dir_deg = 225,
        .has_indoor = true, .has_outdoor = true, .has_rain = true, .has_solar = true, .has_wind = true,
    };
    status_t st = {.ssid = "HomeNet", .wifi_ok = true, .have_data = true, .last_update = now};

    snprintf(path, sizeof(path), "%s/1_dry_rained.pgm", dir);
    render(path, &w, &st, now);

    w.rain_rate_mm_h = 1.4f; w.outdoor_c = -3.0f; w.solar_wm2 = 1080.0f; w.wind_dir_deg = 10;
    snprintf(path, sizeof(path), "%s/2_raining.pgm", dir);
    render(path, &w, &st, now);

    w.rain_rate_mm_h = 0; w.rain_today_mm = 0; w.outdoor_c = 28.7f; w.solar_wm2 = 0; w.wind_dir_deg = 90;
    st.stale = true;
    snprintf(path, sizeof(path), "%s/3_stale_no_rain.pgm", dir);
    render(path, &w, &st, now);

    status_t none = {.ssid = "HomeNet"};
    snprintf(path, sizeof(path), "%s/4_no_data.pgm", dir);
    render(path, &w, &none, now);
    return 0;
}
