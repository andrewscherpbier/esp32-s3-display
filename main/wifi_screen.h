#pragma once

/**
 * Show the Wi-Fi setup screen: connection status, a scan list of nearby networks, and
 * on-screen password entry. Its Back button returns to whichever screen was active.
 * Call from LVGL context (an event callback or with lvgl_port_lock() held).
 */
void wifi_screen_open(void);
