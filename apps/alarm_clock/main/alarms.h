#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#define ALARMS_MAX          5

// Day mask bits follow struct tm's tm_wday: bit 0 = Sunday ... bit 6 = Saturday
#define ALARM_EVERY_DAY     0x7F
#define ALARM_WEEKDAYS      0x3E

typedef struct {
    bool enabled;
    uint8_t hour;       // 0-23
    uint8_t minute;     // 0-59
    uint8_t days;       // at least one bit set
} alarm_t;

// Load the saved alarms. NVS must be initialized first (wifi_init() does it).
void alarms_init(void);

// Copy all alarms into `out` (room for ALARMS_MAX); returns how many there are.
int alarms_get(alarm_t *out);

// Replace alarm `index`, or add one when `index` equals the current count. Saved at once.
esp_err_t alarms_put(int index, const alarm_t *alarm);
esp_err_t alarms_remove(int index);

// Earliest enabled alarm occurrence strictly after `after`, in local time.
bool alarms_next(time_t after, time_t *when);

// "Every day", "Weekdays", "Weekends" or e.g. "Mon Wed Fri".
void alarms_describe_days(uint8_t days, char *buf, size_t len);

// "06:45" style (24-hour).
void alarms_format_time(int hour, int minute, char *buf, size_t len);
