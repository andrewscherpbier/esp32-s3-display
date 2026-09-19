#pragma once

#include "lvgl.h"

#define PLANE_ICON_SIZE 26

/**
 * A white airliner silhouette pointing north (up), with an alpha channel. Rotate it with
 * lv_image_set_rotation(heading * 10) and tint it with the image_recolor style.
 */
const lv_image_dsc_t *plane_icon_get(void);
