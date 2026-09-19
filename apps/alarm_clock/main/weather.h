#pragma once

#include <stdbool.h>
#include <time.h>

typedef struct {
    bool valid;
    float temperature;      // current, in `unit`
    float high;             // today's forecast
    float low;
    int code;               // WMO weather code, see weather_describe()
    bool is_day;
    char unit;              // 'F' or 'C'
    char city[48];
    time_t updated;
} weather_t;

/**
 * Start the background task that keeps the weather current. On first use it looks up the
 * approximate location from the public IP address (cached in NVS afterwards), then
 * fetches Open-Meteo every 30 minutes while Wi-Fi is up. Fahrenheit in the US,
 * Celsius elsewhere. Call after wifi_init().
 */
void weather_start(void);

// Latest reading; `valid` is false until the first successful fetch.
void weather_get(weather_t *out);

// Short text for a WMO weather code, e.g. "Partly cloudy".
const char *weather_describe(int code);
