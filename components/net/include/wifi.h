#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_wifi_types.h"

typedef enum {
    WIFI_STATE_UNCONFIGURED,    // no saved network and none being tried
    WIFI_STATE_CONNECTING,      // includes waiting to retry a saved network
    WIFI_STATE_CONNECTED,       // associated and has an IP address
    WIFI_STATE_FAILED,          // a newly entered network failed and none is saved
} wifi_state_t;

typedef struct {
    wifi_state_t state;
    char ssid[33];
    char ip[16];                // valid when CONNECTED
    int rssi;                   // dBm, valid when CONNECTED
    const char *error;          // why the last newly entered network failed, or NULL
    char error_ssid[33];        // the network `error` refers to
} wifi_status_t;

/**
 * Bring up NVS, the network stack and the Wi-Fi station, and connect to the saved
 * network if there is one. A saved network is retried with backoff for as long as it
 * stays unreachable.
 */
esp_err_t wifi_init(void);

void wifi_get_status(wifi_status_t *out);

/**
 * Switch to a new network. The credentials are saved only once it connects, so a wrong
 * password never replaces a working network. After a few rejected attempts `error` is
 * set and the station falls back to the saved network, or to WIFI_STATE_FAILED if there
 * is none. Pass "" as the password for an open network.
 */
esp_err_t wifi_connect(const char *ssid, const char *password);

// Disconnect and erase the saved network.
void wifi_forget(void);

// Start an asynchronous scan. When it finishes, wifi_scan_generation() changes.
esp_err_t wifi_scan_start(void);
int wifi_scan_generation(void);

// Copy the latest scan results, strongest first, one entry per network name.
int wifi_scan_results(wifi_ap_record_t *out, int max);
