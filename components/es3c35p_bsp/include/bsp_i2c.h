#pragma once

#include "driver/i2c_master.h"

/**
 * Create the I2C bus on GPIO38/39. It is shared by the touch controller (0x55), the
 * ES8311 audio codec (0x18) and the expansion header, so every driver on it must use
 * this handle.
 */
i2c_master_bus_handle_t bsp_i2c_init(void);
