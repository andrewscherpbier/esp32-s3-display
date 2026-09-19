#pragma once

#include <stdint.h>

#include "esp_err.h"

// Set up the on-board WS2812 RGB LED (GPIO40) and turn it off.
esp_err_t bsp_led_init(void);

/**
 * Set the LED colour, 0-255 per channel. Values are scaled down before they reach the
 * LED, since full power is uncomfortably bright up close. Call from one task only.
 */
void bsp_led_set(uint8_t r, uint8_t g, uint8_t b);
