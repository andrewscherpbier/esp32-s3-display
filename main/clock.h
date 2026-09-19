#pragma once

#include <stdbool.h>
#include <time.h>

/**
 * Set the local timezone and sync the time over NTP whenever Wi-Fi gets an IP address.
 * Call after wifi_init().
 */
void clock_init(void);

// Fill `out` with the local time. Returns false until the first NTP sync has completed.
bool clock_now(struct tm *out);
