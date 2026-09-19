#include "tiles.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "bsp_sdcard.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
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
#define TILE_URL            "https://basemaps.cartocdn.com/dark_all/%d/%d/%d.png"
#define DISK_DIR            BSP_SDCARD_MOUNT_POINT "/tiles"

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

// Worker state: one HTTP session kept connected so tiles reuse a TLS connection
static net_http_session_t s_http;
static uint8_t *s_png;
static int s_png_len;

static bool fetch_png(int zoom, int x, int y)
{
    char url[96];
    snprintf(url, sizeof(url), TILE_URL, zoom, x, y);
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

static bool decode_into(tile_t *tile)
{
    unsigned char *rgb = NULL;
    unsigned w = 0, h = 0;
    unsigned err = lodepng_decode24(&rgb, &w, &h, s_png, s_png_len);
    if (err || w != GEO_TILE_SIZE || h != GEO_TILE_SIZE) {
        ESP_LOGW(TAG, "%d/%d/%d: PNG decode failed (%u)", tile->zoom, tile->x, tile->y, err);
        lv_free(rgb);
        return false;
    }
    for (int i = 0; i < GEO_TILE_SIZE * GEO_TILE_SIZE; i++) {
        const unsigned char *p = &rgb[i * 3];
        tile->pixels[i] = ((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3);
    }
    lv_free(rgb);
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
        mkdir(DISK_DIR, 0777);
    }
    ESP_LOGI(TAG, "disk cache %s", s_disk ? "on SD card" : "off (no SD card)");

    s_png = heap_caps_malloc(PNG_MAX, MALLOC_CAP_SPIRAM);
    s_http = net_http_session_create();
    for (int i = 0; i < CACHE_TILES; i++) {
        tile_t *t = &s_tiles[i];
        t->pixels = heap_caps_malloc(GEO_TILE_SIZE * GEO_TILE_SIZE * 2, MALLOC_CAP_SPIRAM);
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

uint32_t tiles_generation(void)
{
    return s_generation;
}
