#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#define TILES_ZOOM_MIN      3
#define TILES_ZOOM_MAX      14
#define TILES_ATTRIBUTION   "\xC2\xA9 Stadia Maps \xC2\xA9 OpenMapTiles \xC2\xA9 OpenStreetMap contributors"

// False when no map API key is configured, so the UI can say so.
bool tiles_map_key_set(void);

/**
 * Map tiles (Stadia Maps "Alidade Smooth Dark", 256x256) with three levels of cache: decoded RGB565
 * tiles in PSRAM, PNG files on the SD card under /sdcard/tiles, and finally the network.
 * A background task does the loading and PNG decoding. Works without an SD card, just
 * without the disk cache. Call after the SD card is mounted and Wi-Fi started.
 */
void tiles_init(void);

/**
 * The tile at (zoom, x, y) if it's ready, otherwise NULL after queueing it for loading.
 * `frame` should increase with every map redraw: tiles used in the latest frames are
 * never evicted, so an image widget showing one stays valid. LVGL context only.
 */
const lv_image_dsc_t *tiles_get(int zoom, int x, int y, uint32_t frame);

// Changes whenever a queued tile finishes loading, so the map knows to redraw.
uint32_t tiles_generation(void);
