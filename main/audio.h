#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

typedef enum {
    AUDIO_IDLE,         // listening, mic level updating
    AUDIO_BEEPING,
    AUDIO_RECORDING,
    AUDIO_PLAYING,
} audio_state_t;

#define AUDIO_RECORD_SECONDS 3

/**
 * Bring up the ES8311 codec (on the shared I2C bus from board_i2c_init()), the I2S
 * link and the speaker amplifier, and start the audio task that owns them.
 * The other functions are safe to call from any task, including LVGL callbacks.
 */
esp_err_t audio_init(i2c_master_bus_handle_t bus);

// Both are ignored unless the audio task is idle.
void audio_beep(void);
void audio_record_and_play(void);

void audio_set_volume(int volume);  // 0-100

audio_state_t audio_get_state(void);
int audio_get_mic_level(void);      // 0-100, from the peak of the latest mic block
