#include "bsp_i2c.h"

#include "bsp_pins.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BSP_I2C_PORT I2C_NUM_0

// While the touch controller is held in reset it can pull SDA low, so it is released
// before the bus comes up. This is also the controller's only reset: the ST7123
// component's own reset waits just 10ms before talking to it, where working ports for
// this board wait 30-100ms.
static void touch_reset(void)
{
    const gpio_config_t rst_cfg = {
        .pin_bit_mask = BIT64(BSP_TOUCH_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&rst_cfg));
    gpio_set_level(BSP_TOUCH_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BSP_TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
}

i2c_master_bus_handle_t bsp_i2c_init(void)
{
    touch_reset();

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = BSP_I2C_PORT,
        .sda_io_num = BSP_I2C_SDA,
        .scl_io_num = BSP_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));
    return bus;
}
