/*
 * app_main.c - hardware bring-up firmware.
 *
 * Boot banner on UART, then the wave-2 display bring-up: SPI bus, controller
 * probe, panel init, one composed "Hello, World!" image, panel back to deep
 * sleep. Exactly ONE panel refresh per boot (Waveshare rule: never leave the
 * panel powered, keep refreshes rare), then a heartbeat every 10 s forever.
 *
 * All machine-readable output follows the log contract in test_log.h, and
 * every [EPD-TEST] line is printed from this task only - the driver logs
 * through ESP_LOG* so the two streams cannot interleave inside a line.
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "board.h"
#include "epd_uc8179.h"
#include "gfx.h"
#include "test_log.h"

#define HEARTBEAT_PERIOD_MS 10000

static const char *TAG = "app";

/* The two 1-bpp planes. 48000 bytes each is far too much for any task stack,
 * and .bss on internal RAM is exactly what the SPI driver prefers (DMA
 * capable), so they are static. */
_Static_assert(GFX_PLANE_BYTES == EPD_PLANE_BYTES,
               "gfx and epd_uc8179 disagree about the plane size");
static uint8_t s_black[GFX_PLANE_BYTES];
static uint8_t s_red[GFX_PLANE_BYTES];

/*===========================================================================
 * Layout of the bring-up image (800 x 480)
 *
 * All Paint_* coordinates stay strictly below 800 / 480 (gfx.h quirk 3); the
 * single exception is the full-bleed border, where the -1 outline bias of
 * quirk 1 cancels the +1 (see compose_black()).
 *===========================================================================*/
#define LAYOUT_BORDER_PX    3     /* border thickness, nested 1-px outlines   */
#define LAYOUT_CORNER_INSET 12    /* corner markers, inside the border        */
#define LAYOUT_TITLE_Y      60
#define LAYOUT_TITLE_SCALE  3     /* Font24 x 3 -> 51 x 72 px per glyph       */
#define LAYOUT_INFO_X       40
#define LAYOUT_INFO_Y       200
#define LAYOUT_INFO_STEP    40    /* Font20 is 20 px tall -> 20 px of air     */
#define LAYOUT_CHECKER_X    40
#define LAYOUT_CHECKER_Y    400
#define LAYOUT_CHECKER_W    720
#define LAYOUT_CHECKER_H    40
#define LAYOUT_CHECKER_CELL 10
#define LAYOUT_RED_BAR_Y0   150
#define LAYOUT_RED_BAR_Y1   162
#define LAYOUT_DISC_CX      700
#define LAYOUT_DISC_CY      300
#define LAYOUT_DISC_R       40

static const char k_title[] = "Hello, World!";

/*===========================================================================
 * Chip / reset helpers, shared by the UART banner and the panel image
 *===========================================================================*/

static const char *chip_model_str(esp_chip_model_t model)
{
    switch (model) {
    case CHIP_ESP32:   return "ESP32";
    case CHIP_ESP32S2: return "ESP32-S2";
    case CHIP_ESP32S3: return "ESP32-S3";
    case CHIP_ESP32C3: return "ESP32-C3";
    case CHIP_ESP32H2: return "ESP32-H2";
    case CHIP_ESP32C2: return "ESP32-C2";
    default:           return "UNKNOWN";
    }
}

static const char *reset_reason_str(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_UNKNOWN:   /* fall through */
    default:                return "UNKNOWN";
    }
}

/* Build a "WiFi/BT/BLE"-style feature list into buf. */
static void chip_features_str(uint32_t features, char *buf, size_t len)
{
    static const struct {
        uint32_t bit;
        const char *name;
    } table[] = {
        { CHIP_FEATURE_WIFI_BGN, "WiFi" },
        { CHIP_FEATURE_BT,       "BT" },
        { CHIP_FEATURE_BLE,      "BLE" },
        { CHIP_FEATURE_EMB_FLASH, "EmbFlash" },
        { CHIP_FEATURE_EMB_PSRAM, "EmbPSRAM" },
        { CHIP_FEATURE_IEEE802154, "802.15.4" },
    };

    buf[0] = '\0';
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (!(features & table[i].bit)) {
            continue;
        }
        if (buf[0] != '\0') {
            strlcat(buf, "/", len);
        }
        strlcat(buf, table[i].name, len);
    }
    if (buf[0] == '\0') {
        strlcat(buf, "none", len);
    }
}

static void print_boot_banner(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    char features[64];
    chip_features_str(chip.features, features, sizeof(features));

    uint32_t flash_bytes = 0;
    if (esp_flash_get_size(NULL, &flash_bytes) != ESP_OK) {
        flash_bytes = 0;
    }

    uint8_t mac[6] = { 0 };
    esp_efuse_mac_get_default(mac);

    EPD_TEST_LOG("boot chip=%s rev=%u.%u cores=%u features=%s flash=%" PRIu32
                 "MB mac=%02x:%02x:%02x:%02x:%02x:%02x idf=%s reset=%s heap=%" PRIu32,
                 chip_model_str(chip.model),
                 chip.revision / 100u, chip.revision % 100u,
                 chip.cores,
                 features,
                 flash_bytes / (1024u * 1024u),
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                 esp_get_idf_version(),
                 reset_reason_str(esp_reset_reason()),
                 esp_get_free_heap_size());
}

/*===========================================================================
 * Image composition
 *===========================================================================*/

/*
 * One line of text at scale 1. gfx_draw_text_scaled() renders identically to
 * Paint_DrawString_EN() at that scale, but truncates at the right edge instead
 * of wrapping onto the line below (gfx.h quirk 4): an unexpectedly long runtime
 * string can then never overwrite the next info line.
 */
static void draw_line(UWORD x, UWORD y, const char *s, sFONT *font)
{
    gfx_draw_text_scaled(x, y, s, font, 1, BLACK, WHITE);
}

/* x of the horizontally centred title; shared with the red plane's bar. */
static UWORD title_x(UWORD width)
{
    return (UWORD)((GFX_PANEL_WIDTH - width) / 2);
}

static void compose_black(const epd_probe_result_t *probe)
{
    char buf[128];

    Paint_NewImage(s_black, GFX_PANEL_WIDTH, GFX_PANEL_HEIGHT, ROTATE_0, WHITE);
    Paint_Clear(WHITE);   /* Paint_NewImage() does not clear the buffer */

    /*
     * Full-bleed border, LAYOUT_BORDER_PX thick, as nested 1-pixel outlines.
     * DOT_PIXEL_2X2 on the single rectangle (1,1,800,480) would be tempting
     * (quirk 2: it paints a 3x3 dot) but its bias spans Xpoint-2..Xpoint, so
     * the bottom and right edges reach x == 800 / y == 480. Those slip past
     * Paint_SetPixel()'s off-by-one bounds check (quirk 3) and, on the bottom
     * edge, write up to 100 bytes past the end of the plane. DOT_PIXEL_1X1 has
     * the exact -1 bias of quirk 1, so (1,1,800,480) lands on (0,0)-(799,479)
     * and every nested outline stays inside the buffer.
     */
    for (UWORD i = 0; i < LAYOUT_BORDER_PX; i++) {
        Paint_DrawRectangle((UWORD)(1 + i), (UWORD)(1 + i),
                            (UWORD)(GFX_PANEL_WIDTH - i), (UWORD)(GFX_PANEL_HEIGHT - i),
                            BLACK, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
    }

    /* Corner markers: they show at a glance whether the image is rotated or
     * mirrored on the glass. Font16 is 11 x 16 px. */
    const UWORD cm_right = (UWORD)(GFX_PANEL_WIDTH - LAYOUT_CORNER_INSET - 2 * Font16.Width);
    const UWORD cm_bottom = (UWORD)(GFX_PANEL_HEIGHT - LAYOUT_CORNER_INSET - Font16.Height);
    Paint_DrawString_EN(LAYOUT_CORNER_INSET, LAYOUT_CORNER_INSET, "TL", &Font16, BLACK, WHITE);
    Paint_DrawString_EN(cm_right, LAYOUT_CORNER_INSET, "TR", &Font16, BLACK, WHITE);
    Paint_DrawString_EN(LAYOUT_CORNER_INSET, cm_bottom, "BL", &Font16, BLACK, WHITE);
    Paint_DrawString_EN(cm_right, cm_bottom, "BR", &Font16, BLACK, WHITE);

    /* Title, centred. */
    const UWORD tw = gfx_text_width(k_title, &Font24, LAYOUT_TITLE_SCALE);
    gfx_draw_text_scaled(title_x(tw), LAYOUT_TITLE_Y, k_title, &Font24,
                         LAYOUT_TITLE_SCALE, BLACK, WHITE);

    /* Info block, same runtime values as the UART banner. Font20 is 14 px per
     * character, so a line starting at x = 40 holds at most 54 characters and
     * the probe details need two lines rather than one. */
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint8_t mac[6] = { 0 };
    esp_efuse_mac_get_default(mac);

    /* esp_get_idf_version() is "v5.0.8-452-gd9f9b7d8ed" here. Only the release
     * part fits next to the MAC; the banner still prints it in full. */
    char idf[24];
    snprintf(idf, sizeof(idf), "%s", esp_get_idf_version());
    char *dash = strchr(idf, '-');
    if (dash != NULL) {
        *dash = '\0';
    }

    UWORD y = LAYOUT_INFO_Y;

    snprintf(buf, sizeof(buf),
             "%s rev%u.%u  MAC %02x:%02x:%02x:%02x:%02x:%02x  IDF %s",
             chip_model_str(chip.model),
             chip.revision / 100u, chip.revision % 100u,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], idf);
    draw_line(LAYOUT_INFO_X, y, buf, &Font20);
    y += LAYOUT_INFO_STEP;

    snprintf(buf, sizeof(buf), "Panel %s  chip_rev 0x%02X",
             epd_panel_name(), probe->rev[6]);
    draw_line(LAYOUT_INFO_X, y, buf, &Font20);
    y += LAYOUT_INFO_STEP;

    snprintf(buf, sizeof(buf), "probe %s  T=%dC  busy %d/%d ms",
             probe->verdict, (int)probe->temp_c,
             probe->busy_low_ms, probe->busy_release_ms);
    draw_line(LAYOUT_INFO_X, y, buf, &Font20);
    y += LAYOUT_INFO_STEP;

    /* This firmware refreshes the panel exactly once per boot, hence "#1". */
    snprintf(buf, sizeof(buf), "Reset %s  heap %" PRIu32 "  refresh #1",
             reset_reason_str(esp_reset_reason()), esp_get_free_heap_size());
    draw_line(LAYOUT_INFO_X, y, buf, &Font20);

    /* Test pattern: 10 px cells expose any byte/bit ordering mistake in the
     * plane and any smearing on the glass. */
    gfx_draw_checkerboard(LAYOUT_CHECKER_X, LAYOUT_CHECKER_Y,
                          LAYOUT_CHECKER_W, LAYOUT_CHECKER_H, LAYOUT_CHECKER_CELL);
}

static void compose_red(void)
{
    Paint_NewImage(s_red, GFX_PANEL_WIDTH, GFX_PANEL_HEIGHT, ROTATE_0, WHITE);
    Paint_Clear(WHITE);

    /* Ink in the red plane is still asked for with BLACK: the constants name
     * bit values (0x00 = ink), the plane decides the physical colour - see the
     * framebuffer convention in gfx.h. Nothing drawn here may touch a pixel
     * that the black plane also inks. */

    /* Bar under the title, spanning exactly the title width. */
    const UWORD tw = gfx_text_width(k_title, &Font24, LAYOUT_TITLE_SCALE);
    const UWORD tx = title_x(tw);
    Paint_DrawRectangle(tx, LAYOUT_RED_BAR_Y0, (UWORD)(tx + tw - 1), LAYOUT_RED_BAR_Y1,
                        BLACK, DOT_PIXEL_1X1, DRAW_FILL_FULL);

    /* Filled disc on the right, clear of the info block. */
    Paint_DrawCircle(LAYOUT_DISC_CX, LAYOUT_DISC_CY, LAYOUT_DISC_R,
                     BLACK, DOT_PIXEL_1X1, DRAW_FILL_FULL);

    /* The label goes BELOW the disc, not beside it: at the disc's own rows the
     * black info lines still run out to x ~ 570, and the two planes must stay
     * disjoint (a pixel inked in both is undefined on the glass). */
    static const char label[] = "red plane";
    const UWORD lw = gfx_text_width(label, &Font20, 1);
    draw_line((UWORD)(LAYOUT_DISC_CX - lw / 2),
              (UWORD)(LAYOUT_DISC_CY + LAYOUT_DISC_R + 6), label, &Font20);
}

/*===========================================================================
 * Display bring-up
 *===========================================================================*/

/*
 * Returns ESP_OK only when probe, init, refresh and sleep all completed. Every
 * exit path leaves the panel in deep sleep (best effort), because a panel left
 * powered is damaged by the sustained high voltage.
 */
static esp_err_t display_bringup(void)
{
    static const epd_pins_t pins = {
        .sck    = BOARD_EPD_SCK,
        .mosi   = BOARD_EPD_MOSI,
        .cs     = BOARD_EPD_CS,
        .dc     = BOARD_EPD_DC,
        .rst    = BOARD_EPD_RST,
        .busy   = BOARD_EPD_BUSY,
        .spi_hz = BOARD_EPD_SPI_HZ,
    };

    esp_err_t err = epd_bus_init(&pins);
    if (err != ESP_OK) {
        EPD_TEST_LOG("ERROR stage=bus err=%s", esp_err_to_name(err));
        return err;
    }

    /*
     * The probe is informational: a UC8179 whose register reads come back as
     * 0xFF can still drive the panel perfectly, so the refresh is attempted
     * whatever the verdict. A transport-level failure is reported through
     * ESP_LOGW rather than an [EPD-TEST] ERROR line, which would stop the
     * capture before the refresh that this run is about.
     */
    epd_probe_result_t probe;
    esp_err_t probe_err = epd_probe(&probe);
    if (probe_err != ESP_OK) {
        ESP_LOGW(TAG, "epd_probe() failed: %s (continuing anyway)",
                 esp_err_to_name(probe_err));
    }

    char line[192];
    epd_probe_format(&probe, line, sizeof(line));
    EPD_TEST_LOG("%s", line);

    const int64_t t_init = esp_timer_get_time();
    err = epd_init();
    const int64_t init_ms = (esp_timer_get_time() - t_init) / 1000;
    if (err != ESP_OK) {
        EPD_TEST_LOG("ERROR stage=init err=%s", esp_err_to_name(err));
        (void)epd_sleep();   /* best effort: never leave the panel powered */
        return err;
    }
    ESP_LOGI(TAG, "epd_init() took %lld ms", init_ms);
    EPD_TEST_LOG("init ok");

    compose_black(&probe);
    compose_red();

    const int64_t t_refresh = esp_timer_get_time();
    err = epd_display(s_black, s_red);
    const int64_t refresh_ms = (esp_timer_get_time() - t_refresh) / 1000;
    if (err != ESP_OK) {
        EPD_TEST_LOG("ERROR stage=display err=%s", esp_err_to_name(err));
        (void)epd_sleep();
        return err;
    }
    EPD_TEST_LOG("refresh_ms=%lld", refresh_ms);

    err = epd_sleep();
    if (err != ESP_OK) {
        EPD_TEST_LOG("ERROR stage=sleep err=%s", esp_err_to_name(err));
        return err;
    }
    EPD_TEST_LOG("sleep ok");

    return ESP_OK;
}

void app_main(void)
{
    print_boot_banner();

    /* One refresh per boot: the panel is asleep from here on and this firmware
     * never wakes it again. */
    if (display_bringup() == ESP_OK) {
        EPD_TEST_LOG("DONE");
    }

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_PERIOD_MS));
        EPD_TEST_LOG("alive uptime=%llds heap=%" PRIu32,
                     esp_timer_get_time() / 1000000LL,
                     esp_get_free_heap_size());
    }
}
