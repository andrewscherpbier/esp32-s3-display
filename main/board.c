#include "board.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define BOARD_I2C_PORT      I2C_NUM_0
#define BOARD_PIN_I2C_SDA   GPIO_NUM_38
#define BOARD_PIN_I2C_SCL   GPIO_NUM_39
#define BOARD_PIN_TOUCH_RST GPIO_NUM_48     // active low

// While the touch controller is held in reset it can pull SDA low, so it is released
// before the bus comes up. This is also the controller's only reset: the ST7123
// component's own reset waits just 10ms before talking to it, where working ports for
// this board wait 30-100ms.
static void touch_reset(void)
{
    const gpio_config_t rst_cfg = {
        .pin_bit_mask = BIT64(BOARD_PIN_TOUCH_RST),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&rst_cfg));
    gpio_set_level(BOARD_PIN_TOUCH_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BOARD_PIN_TOUCH_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
}

i2c_master_bus_handle_t board_i2c_init(void)
{
    touch_reset();

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = BOARD_I2C_PORT,
        .sda_io_num = BOARD_PIN_I2C_SDA,
        .scl_io_num = BOARD_PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus = NULL;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));
    return bus;
}
