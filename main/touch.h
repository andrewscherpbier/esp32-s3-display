#pragma once

#include "driver/i2c_master.h"
#include "lvgl.h"

/**
 * Bring up the ES3C35P's capacitive touch controller on the shared I2C bus (see
 * board_i2c_init(), which also resets the controller) and register it with LVGL as a
 * pointer input on `disp`. Must be called after display_init().
 *
 * Returns NULL (and logs why) if the controller doesn't answer, so the UI can
 * still run without touch.
 */
lv_indev_t *touch_init(lv_display_t *disp, i2c_master_bus_handle_t bus);
