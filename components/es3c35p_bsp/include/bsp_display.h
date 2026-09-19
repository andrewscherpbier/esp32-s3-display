#pragma once

#include "lvgl.h"

// Native panel geometry (portrait). Touch reports in this frame too.
#define BSP_PANEL_WIDTH  320
#define BSP_PANEL_HEIGHT 480

/**
 * Bring up the ES3C35P's ST77922 QSPI panel, start the LVGL port task and register the
 * panel as an LVGL display. The panel is natively portrait; `rotation` is applied in
 * software (the controller can't swap axes), e.g. LV_DISPLAY_ROTATION_90 for a 480x320
 * landscape display. Touch points are rotated to match by LVGL.
 *
 * The backlight stays off until bsp_display_set_brightness(), so the panel's power-on
 * garbage is never visible. All LVGL calls made afterwards must be wrapped in
 * lvgl_port_lock()/unlock().
 */
lv_display_t *bsp_display_init(lv_display_rotation_t rotation);

// 0 (off) to 100, on a perceptual curve. Safe to call from any task.
void bsp_display_set_brightness(int percent);
