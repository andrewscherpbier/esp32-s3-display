/*
 * Flight tracker for the ES3C35P: live aircraft around home on a dark map, with zoom,
 * pan, details on tap, and a mode that follows one flight anywhere in the world.
 */
#include <stdlib.h>

#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_sdcard.h"
#include "bsp_touch.h"
#include "clock.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "flights.h"
#include "sdkconfig.h"
#include "tiles.h"
#include "ui.h"
#include "wifi.h"

#define CLOCK_TZ    "PST8PDT,M3.2.0,M11.1.0"    // US Pacific

static const char *TAG = "main";

void app_main(void)
{
    lv_display_t *disp = bsp_display_init(LV_DISPLAY_ROTATION_90);
    i2c_master_bus_handle_t i2c_bus = bsp_i2c_init();
    bsp_touch_init(disp, i2c_bus);
    bsp_sdcard_init();          // tile cache; the map still works without a card

    ESP_ERROR_CHECK(wifi_init());
    clock_init(CLOCK_TZ);
    tiles_init();

    // Home comes from menuconfig ("Flight tracker"), which lives in the uncommitted
    // sdkconfig; left empty, it's looked up from the IP address instead
    const char *lat = CONFIG_FLIGHT_HOME_LAT;
    const char *lon = CONFIG_FLIGHT_HOME_LON;
    bool home_set = lat[0] && lon[0];
    flights_start(home_set, home_set ? strtod(lat, NULL) : 0, home_set ? strtod(lon, NULL) : 0);

    lvgl_port_lock(0);
    ui_map_init(CONFIG_FLIGHT_START_ZOOM);
    lvgl_port_unlock();
    bsp_display_set_brightness(80);

    ESP_LOGI(TAG, "up; home %s; free internal RAM %u KB, PSRAM %u KB", home_set ? "from menuconfig" : "from IP address",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024, heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
}
