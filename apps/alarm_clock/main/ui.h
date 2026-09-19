#pragma once

#include "lvgl.h"

#define UI_COLOR_BG     0x0B0F14
#define UI_COLOR_TEXT   0xF2F4F7
#define UI_COLOR_MUTED  0x8A94A3
#define UI_COLOR_ACCENT 0xFFB347        // warm amber, the sunrise colour

LV_FONT_DECLARE(font_clock_120);

// Build the clock face and start the timers that keep it, the wake-up overlay and the
// backlight up to date. Call with the LVGL lock held.
void ui_init(void);

// Alarm list and editor (ui_alarms.c). Back returns to the clock face.
void ui_alarms_open(void);

// Shared by the screens
lv_obj_t *ui_new_screen(void);
lv_obj_t *ui_header_button(lv_obj_t *parent, const char *text, lv_align_t align, lv_event_cb_t cb);
