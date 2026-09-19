/*
 * Bedside alarm clock for the ES3C35P: big clock face with date and weather, alarms with
 * a sunrise wake-up and chime, and a backlight that dims at night.
 */
#include "alarms.h"
#include "bsp_audio.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_led.h"
#include "bsp_touch.h"
#include "clock.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "ui.h"
#include "wake.h"
#include "weather.h"
#include "wifi.h"

#define CLOCK_TZ    "PST8PDT,M3.2.0,M11.1.0"    // US Pacific

static const char *TAG = "main";

void app_main(void)
{
    lv_display_t *disp = bsp_display_init(LV_DISPLAY_ROTATION_90);
    i2c_master_bus_handle_t i2c_bus = bsp_i2c_init();
    bsp_touch_init(disp, i2c_bus);
    bsp_led_init();
    esp_codec_dev_handle_t codec = bsp_audio_init(i2c_bus, 16000);

    ESP_ERROR_CHECK(wifi_init());       // also initializes NVS, which alarms_init() needs
    clock_init(CLOCK_TZ);
    alarms_init();
    ESP_ERROR_CHECK(wake_init(codec));
    weather_start();

    lvgl_port_lock(0);
    ui_init();
    lvgl_port_unlock();

    ESP_LOGI(TAG, "up; free internal RAM %u KB, PSRAM %u KB",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024,
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);
}
