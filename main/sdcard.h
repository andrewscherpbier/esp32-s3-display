#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define SDCARD_MOUNT_POINT "/sdcard"

/**
 * Mount the microSD card's FAT filesystem at SDCARD_MOUNT_POINT. Never formats: a card
 * that can't be mounted is left untouched and an error is returned.
 */
esp_err_t sdcard_init(void);

// Capacity and free space in bytes, as measured at mount. Returns false when no card is mounted.
bool sdcard_get_space(uint64_t *total, uint64_t *free);
