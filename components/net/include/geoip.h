#pragma once

#include <stdbool.h>

typedef struct {
    double latitude;
    double longitude;
    char city[48];
    char country[4];    // ISO 3166 alpha-2, e.g. "US"
} geoip_location_t;

/**
 * Approximate location (city level) from the public IP address, via ipapi.co with
 * ipwho.is as a fallback; neither needs a key. Uses `buf` (at least 4KB) as scratch
 * space. Blocks for the HTTPS requests.
 */
bool geoip_locate(geoip_location_t *out, char *buf, int size);
