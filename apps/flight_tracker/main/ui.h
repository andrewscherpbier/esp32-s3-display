#pragma once

#include "lvgl.h"

#define UI_COLOR_BG         0x0B0B0B    // close to the map's own background
#define UI_COLOR_PANEL      0x161B22
#define UI_COLOR_TEXT       0xE6EDF3
#define UI_COLOR_MUTED      0x8B949E
#define UI_COLOR_FOLLOW     0x00E5FF

// Build the map screen and start its timers. Call with the LVGL lock held.
void ui_map_init(int zoom);
lv_obj_t *ui_map_screen(void);

// Screen for typing a flight number to follow (ui_follow.c); returns to the map.
void ui_follow_open(void);
