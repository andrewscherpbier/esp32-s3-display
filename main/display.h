#pragma once

#include "lvgl.h"

// Native panel geometry (portrait). Touch reports in this frame too.
#define PANEL_WIDTH 320
#define PANEL_HEIGHT 480

// LVGL rotates in software on top of the native frame, giving a 480x320 landscape
// display; touch points are rotated to match by LVGL. Use LV_DISPLAY_ROTATION_270
// to turn the image the other way up.
#define DISPLAY_ROTATION LV_DISPLAY_ROTATION_90

/**
 * Bring up the ES3C35P's ST77922 QSPI panel, start the LVGL port task and
 * register the panel as an LVGL display rotated by DISPLAY_ROTATION. The backlight stays off until
 * display_set_brightness() so the panel's power-on garbage is never visible.
 *
 * All LVGL calls made afterwards must be wrapped in lvgl_port_lock()/unlock().
 */
lv_display_t *display_init(void);

// 0 (off) to 100, on a perceptual curve. Safe to call from any task.
void display_set_brightness(int percent);
