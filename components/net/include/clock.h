#pragma once

#include <stdbool.h>
#include <time.h>

/**
 * Set the local timezone and sync the time over NTP whenever Wi-Fi gets an IP address.
 * `tz` is a POSIX TZ string, e.g. "PST8PDT,M3.2.0,M11.1.0" for US Pacific; see
 * https://github.com/nayarsystems/posix_tz_db for others. Call after wifi_init().
 */
void clock_init(const char *tz);

// Fill `out` with the local time. Returns false until the first NTP sync has completed.
bool clock_now(struct tm *out);
