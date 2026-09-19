#pragma once

#include <time.h>

#include "esp_codec_dev.h"
#include "esp_err.h"

#define WAKE_SUNRISE_MINUTES    10
#define WAKE_SNOOZE_MINUTES     9

typedef enum {
    WAKE_IDLE,
    WAKE_SUNRISE,       // lights ramping up before an alarm
    WAKE_RINGING,       // chime playing until stopped, snoozed or timed out
    WAKE_SNOOZED,
} wake_state_t;

typedef struct {
    wake_state_t state;
    float sunrise;      // 0..1 through the sunrise ramp (SUNRISE only)
    time_t target;      // when the alarm rings (SUNRISE) or rings again (SNOOZED)
} wake_status_t;

/**
 * Start the task that watches the alarms and runs the wake-up sequence: a sunrise on
 * the RGB LED (the UI mirrors it on the backlight) for WAKE_SUNRISE_MINUTES, then a
 * chime that gets louder each repeat. `codec` may be NULL, which leaves out the chime.
 */
esp_err_t wake_init(esp_codec_dev_handle_t codec);

void wake_get_status(wake_status_t *out);

// Dismiss the alarm that's ringing, snoozed, or in its sunrise.
void wake_stop(void);
void wake_snooze(void);

// Ring right away, for trying out the chime.
void wake_test(void);
