/*
 * The demo's audio features on top of the board's codec (bsp_audio): a beep, a
 * 3-second record-and-playback with the recording normalized, and a live mic level.
 *
 * One task owns the codec, so reads, writes and control changes never overlap. While
 * idle it keeps reading the mic to drive the level meter.
 */
#include "demo_audio.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "bsp_audio.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define SAMPLE_RATE         16000
#define BLOCK_SAMPLES       (SAMPLE_RATE / 50)                  // 20ms
#define RECORD_SAMPLES      (SAMPLE_RATE * AUDIO_RECORD_SECONDS)

#define DEFAULT_VOLUME      BSP_AUDIO_DEFAULT_VOLUME

// Recordings are scaled so their peak lands here before playback. The gain is capped so a
// near-silent clip plays back as quiet room noise rather than amplified hiss.
#define NORMALIZE_PEAK_DBFS -3.0f
#define NORMALIZE_MAX_DB    24.0f

#define BEEP_HZ             880.0f
#define BEEP_MS             400
#define BEEP_FADE_SAMPLES   (SAMPLE_RATE / 200)                 // 5ms, avoids clicks
#define BEEP_AMPLITUDE      12000.0f

typedef enum {
    CMD_BEEP,
    CMD_RECORD_AND_PLAY,
} audio_cmd_t;

static const char *TAG = "demo_audio";

static esp_codec_dev_handle_t s_codec;
static QueueHandle_t s_cmds;
static _Atomic audio_state_t s_state = AUDIO_IDLE;
static _Atomic int s_mic_level;
static _Atomic int s_target_volume = DEFAULT_VOLUME;

static int peak_of(const int16_t *samples, int count)
{
    int peak = 0;
    for (int i = 0; i < count; i++) {
        int v = abs(samples[i]);
        if (v > peak) {
            peak = v;
        }
    }
    return peak;
}

static float peak_dbfs(int peak)
{
    return peak > 0 ? 20.0f * log10f(peak / 32768.0f) : -96.0f;
}

// Map -60..0 dBFS onto the 0-100 meter.
static void update_mic_level(const int16_t *samples, int count)
{
    float db = peak_dbfs(peak_of(samples, count));
    int level = (int)((db + 60.0f) * (100.0f / 60.0f));
    s_mic_level = level < 0 ? 0 : level > 100 ? 100 : level;
}

static void play_beep(void)
{
    int16_t block[BLOCK_SAMPLES];
    const int total = SAMPLE_RATE * BEEP_MS / 1000;
    const float step = 2.0f * (float)M_PI * BEEP_HZ / SAMPLE_RATE;
    for (int n = 0; n < total;) {
        int count = total - n < BLOCK_SAMPLES ? total - n : BLOCK_SAMPLES;
        for (int i = 0; i < count; i++, n++) {
            float env = 1.0f;
            if (n < BEEP_FADE_SAMPLES) {
                env = (float)n / BEEP_FADE_SAMPLES;
            } else if (total - n < BEEP_FADE_SAMPLES) {
                env = (float)(total - n) / BEEP_FADE_SAMPLES;
            }
            block[i] = (int16_t)(BEEP_AMPLITUDE * env * sinf(step * n));
        }
        esp_codec_dev_write(s_codec, block, count * sizeof(int16_t));
    }
}

// Returns the gain applied, in dB.
static float normalize(int16_t *samples, int count)
{
    int peak = peak_of(samples, count);
    float gain_db = NORMALIZE_PEAK_DBFS - peak_dbfs(peak);
    if (gain_db > NORMALIZE_MAX_DB) {
        gain_db = NORMALIZE_MAX_DB;
    }
    if (gain_db <= 0.0f) {
        return 0.0f;
    }

    const float gain = powf(10.0f, gain_db / 20.0f);
    for (int i = 0; i < count; i++) {
        float v = samples[i] * gain;
        samples[i] = (int16_t)(v > INT16_MAX ? INT16_MAX : v < INT16_MIN ? INT16_MIN : v);
    }
    return gain_db;
}

static void record_and_play(void)
{
    int16_t *buf = heap_caps_malloc(RECORD_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGE(TAG, "no memory for a %d s recording", AUDIO_RECORD_SECONDS);
        return;
    }

    s_state = AUDIO_RECORDING;
    for (int n = 0; n < RECORD_SAMPLES; n += BLOCK_SAMPLES) {
        esp_codec_dev_read(s_codec, &buf[n], BLOCK_SAMPLES * sizeof(int16_t));
        update_mic_level(&buf[n], BLOCK_SAMPLES);
    }
    float peak_db = peak_dbfs(peak_of(buf, RECORD_SAMPLES));
    float gain_db = normalize(buf, RECORD_SAMPLES);
    ESP_LOGI(TAG, "recorded %d s, peak %.1f dBFS, normalized +%.1f dB", AUDIO_RECORD_SECONDS,
             peak_db, gain_db);

    s_state = AUDIO_PLAYING;
    s_mic_level = 0;
    for (int n = 0; n < RECORD_SAMPLES; n += BLOCK_SAMPLES) {
        esp_codec_dev_write(s_codec, &buf[n], BLOCK_SAMPLES * sizeof(int16_t));
    }
    free(buf);
}

static void audio_task(void *arg)
{
    int16_t block[BLOCK_SAMPLES];
    int volume = DEFAULT_VOLUME;

    // The ADC pops for a moment after the codec opens; let it settle so the check below
    // and the meter don't start with a full-scale spike.
    for (int n = 0; n < SAMPLE_RATE / 4; n += BLOCK_SAMPLES) {
        esp_codec_dev_read(s_codec, block, sizeof(block));
    }

    // Log what the mic hears for a second. A quiet room reads around -60 dBFS; exactly
    // -96 (all zeros) or a constant value means no data is arriving from the codec.
    int startup_peak = 0;
    for (int n = 0; n < SAMPLE_RATE; n += BLOCK_SAMPLES) {
        esp_codec_dev_read(s_codec, block, sizeof(block));
        int peak = peak_of(block, BLOCK_SAMPLES);
        startup_peak = peak > startup_peak ? peak : startup_peak;
    }
    ESP_LOGI(TAG, "mic check: 1 s ambient peak %.1f dBFS", peak_dbfs(startup_peak));

    for (;;) {
        if (s_target_volume != volume) {
            volume = s_target_volume;
            esp_codec_dev_set_out_vol(s_codec, volume);
        }

        audio_cmd_t cmd;
        if (xQueueReceive(s_cmds, &cmd, 0)) {
            if (cmd == CMD_BEEP) {
                s_state = AUDIO_BEEPING;
                play_beep();
            } else {
                record_and_play();
            }
            s_state = AUDIO_IDLE;
        }

        esp_codec_dev_read(s_codec, block, sizeof(block));
        update_mic_level(block, BLOCK_SAMPLES);
    }
}

esp_err_t audio_init(i2c_master_bus_handle_t bus)
{
    s_codec = bsp_audio_init(bus, SAMPLE_RATE);
    ESP_RETURN_ON_FALSE(s_codec, ESP_FAIL, TAG, "codec init failed");

    s_cmds = xQueueCreate(4, sizeof(audio_cmd_t));
    ESP_RETURN_ON_FALSE(s_cmds, ESP_ERR_NO_MEM, TAG, "queue alloc failed");
    ESP_RETURN_ON_FALSE(xTaskCreate(audio_task, "audio", 4096, NULL, 5, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task create failed");

    return ESP_OK;
}

static void send_if_idle(audio_cmd_t cmd)
{
    if (s_cmds && s_state == AUDIO_IDLE) {
        xQueueSend(s_cmds, &cmd, 0);
    }
}

void audio_beep(void)
{
    send_if_idle(CMD_BEEP);
}

void audio_record_and_play(void)
{
    send_if_idle(CMD_RECORD_AND_PLAY);
}

void audio_set_volume(int volume)
{
    s_target_volume = volume < 0 ? 0 : volume > 100 ? 100 : volume;
}

audio_state_t audio_get_state(void)
{
    return s_state;
}

int audio_get_mic_level(void)
{
    return s_mic_level;
}
