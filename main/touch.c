/*
 * Touch on the LCDwiki / Hosyond ES3C35P: a Sitronix controller built into the panel,
 * I2C address 0x55, with the same register map as the ST7123. Reports are in the
 * panel's native portrait frame (320x480), so no swap, mirror or scaling is needed.
 *
 * The controller is reset by board_i2c_init(), not by the driver. INT (GPIO47) is
 * untested on this board; esp_lvgl_port polls instead.
 */
#include "touch.h"

#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch_st7123.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

#include "display.h"

static const char *TAG = "touch";

lv_indev_t *touch_init(lv_display_t *disp, i2c_master_bus_handle_t bus)
{
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_ST7123_CONFIG();
    io_cfg.scl_speed_hz = 400 * 1000;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io));

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = PANEL_WIDTH,
        .y_max = PANEL_HEIGHT,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
    };
    esp_lcd_touch_handle_t tp = NULL;
    esp_err_t err = esp_lcd_touch_new_i2c_st7123(io, &tp_cfg, &tp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch controller not responding at 0x%02X: %s",
                 ESP_LCD_TOUCH_IO_I2C_ST7123_ADDRESS, esp_err_to_name(err));
        return NULL;
    }

    const lvgl_port_touch_cfg_t lvgl_touch_cfg = {
        .disp = disp,
        .handle = tp,
    };
    return lvgl_port_add_touch(&lvgl_touch_cfg);
}
