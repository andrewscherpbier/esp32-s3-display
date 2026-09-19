/*
 * The ES3C35P's single WS2812 RGB LED on GPIO40, driven by the RMT peripheral through
 * espressif/led_strip. Its colour order is GRB (per github.com/jlmeredith/ES3C35P).
 */
#include "bsp_led.h"

#include "bsp_pins.h"
#include "esp_check.h"
#include "led_strip.h"

#define LED_MAX_LEVEL   40              // of 255: full power is glaring at arm's length

static const char *TAG = "led";

static led_strip_handle_t s_strip;

esp_err_t bsp_led_init(void)
{
    const led_strip_config_t strip_cfg = {
        .strip_gpio_num = BSP_RGB_LED,
        .max_leds = 1,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
    };
    const led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    ESP_RETURN_ON_ERROR(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip), TAG,
                        "LED init failed");
    return led_strip_clear(s_strip);
}

void bsp_led_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip) {
        return;
    }
    led_strip_set_pixel(s_strip, 0, r * LED_MAX_LEVEL / 255, g * LED_MAX_LEVEL / 255,
                        b * LED_MAX_LEVEL / 255);
    led_strip_refresh(s_strip);
}
