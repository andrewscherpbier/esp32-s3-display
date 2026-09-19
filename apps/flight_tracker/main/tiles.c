#include "tiles.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "bsp_sdcard.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "geo.h"
#include "libs/lodepng/lodepng.h"
#include "net_http.h"
#include "wifi.h"

#define CACHE_TILES         32          // 128KB each, in PSRAM
#define PNG_MAX             (96 * 1024)
#define RETRY_FAILED_US     (30 * 1000000LL)
// Stadia Maps needs a free account; the key comes from menuconfig, which keeps it out of
// the repository. The style name is part of the cache path so tiles from a different style
// are never mistaken for these.
#define TILE_URL            "https://tiles.stadiamaps.com/tiles/alidade_smooth_dark/%d/%d/%d.png?api_key=%s"
#define MAP_API_KEY         CONFIG_FLIGHT_MAP_API_KEY
#define DISK_DIR            BSP_SDCARD_MOUNT_POINT "/tiles/stadia"

/*
 * Dark basemaps are built for data overlays on big screens and can be too dim to read on a
 * 3.5" panel. Every channel is brightened through this gamma curve as tiles are decoded,
 * which lifts the dark greys of roads and coastlines more than the near-black background.
 * 1.0 leaves tiles exactly as the style intends.
 */
#define MAP_GAMMA           1.2f

typedef enum {
    TILE_EMPTY,
    TILE_LOADING,
    TILE_READY,
    TILE_FAILED,
} tile_state_t;

typedef struct {
    int zoom, x, y;
    tile_state_t state;
    uint32_t last_used;         // frame number
    int64_t failed_at;
    uint16_t *pixels;           // RGB565, GEO_TILE_SIZE^2
    lv_image_dsc_t dsc;
} tile_t;

static const char *TAG = "tiles";

static tile_t s_tiles[CACHE_TILES];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_requests;    // tile indexes
static volatile uint32_t s_generation;
static bool s_disk;
static uint8_t s_gamma[256];

// Worker state: one HTTP session kept connected so tiles reuse a TLS connection
static net_http_session_t s_http;
static uint8_t *s_png;
static int s_png_len;

static bool fetch_png(int zoom, int x, int y)
{
    if (!MAP_API_KEY[0]) {
        return false;
    }
    char url[160];
    snprintf(url, sizeof(url), TILE_URL, zoom, x, y, MAP_API_KEY);
    s_png_len = net_http_session_get(s_http, url, (char *)s_png, PNG_MAX);
    return s_png_len > 0;
}

static void disk_path(char *buf, size_t len, int zoom, int x, int y)
{
    snprintf(buf, len, DISK_DIR "/%d/%d/%d.png", zoom, x, y);
}

static bool read_disk(int zoom, int x, int y)
{
    char path[64];
    disk_path(path, sizeof(path), zoom, x, y);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    s_png_len = fread(s_png, 1, PNG_MAX, f);
    fclose(f);
    return s_png_len > 0;
}

static void write_disk(int zoom, int x, int y)
{
    char path[64];
    snprintf(path, sizeof(path), DISK_DIR "/%d", zoom);
    mkdir(path, 0777);
    snprintf(path, sizeof(path), DISK_DIR "/%d/%d", zoom, x);
    mkdir(path, 0777);
    disk_path(path, sizeof(path), zoom, x, y);
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "can't write %s: %s", path, strerror(errno));
        return;
    }
    fwrite(s_png, 1, s_png_len, f);
    fclose(f);
}

/*
 * Decode the PNG in s_png into the tile's RGB565 buffer.
 *
 * LVGL patches lodepng: lodepng_decode*() hands back an lv_draw_buf_t (not raw pixels),
 * always ARGB8888 whatever format was asked for, allocated with plain malloc. The header
 * is checked below so a future LVGL change fails loudly instead of drawing rubbish.
 */
static bool decode_into(tile_t *tile)
{
    unsigned char *out = NULL;
    unsigned w = 0, h = 0;
    unsigned err = lodepng_decode32(&out, &w, &h, s_png, s_png_len);
    lv_draw_buf_t *decoded = (lv_draw_buf_t *)out;
    if (err || !decoded || w != GEO_TILE_SIZE || h != GEO_TILE_SIZE) {
        ESP_LOGW(TAG, "%d/%d/%d: PNG decode failed (%u, %ux%u)", tile->zoom, tile->x, tile->y, err, w, h);
        lv_draw_buf_destroy(decoded);
        return false;
    }
    if (decoded->header.magic != LV_IMAGE_HEADER_MAGIC || decoded->header.cf != LV_COLOR_FORMAT_ARGB8888) {
        ESP_LOGE(TAG, "lodepng returned format %d, not the expected ARGB8888 draw buffer", decoded->header.cf);
        lv_draw_buf_destroy(decoded);
        return false;
    }

    for (int y = 0; y < GEO_TILE_SIZE; y++) {
        const uint8_t *row = decoded->data + (size_t)y * decoded->header.stride;
        uint16_t *dst = &tile->pixels[y * GEO_TILE_SIZE];
        for (int x = 0; x < GEO_TILE_SIZE; x++) {
            const uint8_t *p = &row[x * 4];     // B, G, R, A
            dst[x] = ((s_gamma[p[2]] >> 3) << 11) | ((s_gamma[p[1]] >> 2) << 5) | (s_gamma[p[0]] >> 3);
        }
    }
    lv_draw_buf_destroy(decoded);
    return true;
}

static bool wifi_up(void)
{
    wifi_status_t status;
    wifi_get_status(&status);
    return status.state == WIFI_STATE_CONNECTED;
}

static void tiles_task(void *arg)
{
    for (;;) {
        int index;
        xQueueReceive(s_requests, &index, portMAX_DELAY);
        tile_t *tile = &s_tiles[index];

        // The entry may have been reused for another tile since it was queued
        portENTER_CRITICAL(&s_lock);
        bool wanted = tile->state == TILE_LOADING;
        int zoom = tile->zoom, x = tile->x, y = tile->y;
        portEXIT_CRITICAL(&s_lock);
        if (!wanted) {
            continue;
        }

        bool ok = s_disk && read_disk(zoom, x, y);
        if (!ok && wifi_up() && fetch_png(zoom, x, y)) {
            ok = true;
            if (s_disk) {
                write_disk(zoom, x, y);
            }
        }
        ok = ok && decode_into(tile);

        portENTER_CRITICAL(&s_lock);
        if (tile->state == TILE_LOADING && tile->zoom == zoom && tile->x == x && tile->y == y) {
            tile->state = ok ? TILE_READY : TILE_FAILED;
            tile->failed_at = esp_timer_get_time();
        }
        portEXIT_CRITICAL(&s_lock);
        s_generation++;
    }
}

void tiles_init(void)
{
    uint64_t total, free;
    s_disk = bsp_sdcard_get_space(&total, &free);
    if (s_disk) {
        mkdir(BSP_SDCARD_MOUNT_POINT "/tiles", 0777);
        mkdir(DISK_DIR, 0777);
    }
    if (!MAP_API_KEY[0]) {
        ESP_LOGW(TAG, "no map API key: run idf.py menuconfig -> Flight tracker");
    }
    ESP_LOGI(TAG, "disk cache %s", s_disk ? "on SD card" : "off (no SD card)");

    for (int v = 0; v < 256; v++) {
        s_gamma[v] = (uint8_t)lroundf(255.0f * powf(v / 255.0f, 1.0f / MAP_GAMMA));
    }
    s_png = heap_caps_malloc(PNG_MAX, MALLOC_CAP_SPIRAM);
    s_http = net_http_session_create();
    for (int i = 0; i < CACHE_TILES; i++) {
        tile_t *t = &s_tiles[i];
        t->pixels = heap_caps_aligned_alloc(64, GEO_TILE_SIZE * GEO_TILE_SIZE * 2, MALLOC_CAP_SPIRAM);
        t->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        t->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        t->dsc.header.w = GEO_TILE_SIZE;
        t->dsc.header.h = GEO_TILE_SIZE;
        t->dsc.header.stride = GEO_TILE_SIZE * 2;
        t->dsc.data_size = GEO_TILE_SIZE * GEO_TILE_SIZE * 2;
        t->dsc.data = (const uint8_t *)t->pixels;
        t->zoom = -1;
    }
    s_requests = xQueueCreate(CACHE_TILES, sizeof(int));
    // Big stack: TLS handshakes and PNG decoding both need it
    xTaskCreate(tiles_task, "tiles", 10240, NULL, 4, NULL);
}

const lv_image_dsc_t *tiles_get(int zoom, int x, int y, uint32_t frame)
{
    int victim = -1;
    for (int i = 0; i < CACHE_TILES; i++) {
        tile_t *t = &s_tiles[i];
        if (t->zoom == zoom && t->x == x && t->y == y) {
            t->last_used = frame;
            portENTER_CRITICAL(&s_lock);
            tile_state_t state = t->state;
            bool retry = state == TILE_FAILED && esp_timer_get_time() - t->failed_at > RETRY_FAILED_US;
            if (retry) {
                t->state = TILE_LOADING;
            }
            portEXIT_CRITICAL(&s_lock);
            if (retry) {
                xQueueSend(s_requests, &i, 0);
            }
            return state == TILE_READY ? &t->dsc : NULL;
        }
        // Least recently used entry that isn't being loaded or shown right now
        if (t->state != TILE_LOADING && t->last_used + 2 < frame &&
            (victim < 0 || t->last_used < s_tiles[victim].last_used)) {
            victim = i;
        }
    }
    if (victim < 0) {
        return NULL;        // every entry is busy; asked again next frame
    }

    // Reusing the entry just refills its pixel buffer: LVGL draws an in-memory RGB565
    // image straight from that buffer rather than caching a decoded copy.
    tile_t *t = &s_tiles[victim];
    portENTER_CRITICAL(&s_lock);
    t->zoom = zoom;
    t->x = x;
    t->y = y;
    t->state = TILE_LOADING;
    portEXIT_CRITICAL(&s_lock);
    t->last_used = frame;
    if (xQueueSend(s_requests, &victim, 0) != pdTRUE) {
        t->state = TILE_EMPTY;
        t->zoom = -1;
    }
    return NULL;
}

bool tiles_map_key_set(void)
{
    return MAP_API_KEY[0] != '\0';
}

uint32_t tiles_generation(void)
{
    return s_generation;
}
