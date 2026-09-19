/*
 * The wake-up sequence. One task owns the codec and the RGB LED, and moves through
 * IDLE -> SUNRISE -> RINGING (-> SNOOZED -> RINGING ...) -> IDLE.
 *
 * `s_handled` is the latest alarm occurrence that has rung or been skipped, so the same
 * occurrence never starts twice. A sunrise that's cancelled because its alarm was
 * switched off or moved doesn't count, so a moved alarm still goes off at its new time.
 */
#include "wake.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "alarms.h"
#include "bsp_led.h"
#include "clock.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SAMPLE_RATE         16000
#define BLOCK_SAMPLES       (SAMPLE_RATE / 50)          // 20ms: how often stop/snooze is checked
#define SUNRISE_SEC         (WAKE_SUNRISE_MINUTES * 60)
#define SNOOZE_SEC          (WAKE_SNOOZE_MINUTES * 60)
#define RING_TIMEOUT_SEC    (15 * 60)                   // give up if nobody responds

// Chime: a two-note "ding-dong" bell, then a pause; the volume steps up each repeat.
#define CHIME_SECONDS       3
#define CHIME_VOLUME_START  45
#define CHIME_VOLUME_STEP   10
#define CHIME_VOLUME_MAX    90

static const char *TAG = "wake";

static esp_codec_dev_handle_t s_codec;
static int16_t *s_chime;            // CHIME_SECONDS of audio in PSRAM
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static wake_status_t s_status;
static time_t s_handled;
static atomic_bool s_stop_request;
static atomic_bool s_snooze_request;
static atomic_bool s_test_request;

static void set_status(wake_state_t state, float sunrise, time_t target)
{
    portENTER_CRITICAL(&s_lock);
    s_status.state = state;
    s_status.sunrise = sunrise;
    s_status.target = target;
    portEXIT_CRITICAL(&s_lock);
}

// A bell-like note: a few harmonics, the higher ones dying away faster. Each partial is
// an oscillator that rotates a phasor and shrinks it by a fixed factor per sample, which
// avoids a sinf()/expf() per sample (that took seconds for the whole chime).
static void add_bell(float *mix, int count, int start, float freq, float amp)
{
    static const float partials[][3] = {  // {frequency ratio, level, decay per second}
        {1.0f, 1.0f, 2.2f},
        {2.0f, 0.45f, 3.5f},
        {3.0f, 0.2f, 5.0f},
        {4.2f, 0.1f, 7.0f},
    };
    const int attack = SAMPLE_RATE * 3 / 1000;     // 3ms fade-in so the note doesn't click
    for (int p = 0; p < 4; p++) {
        const float w = 2.0f * (float)M_PI * freq * partials[p][0] / SAMPLE_RATE;
        const float decay = expf(-partials[p][2] / SAMPLE_RATE);
        // (re, im) is the phasor; its imaginary part is the sine wave
        const float rot_re = cosf(w) * decay;
        const float rot_im = sinf(w) * decay;
        float re = amp * partials[p][1];
        float im = 0;
        for (int i = start; i < count; i++) {
            const int n = i - start;
            mix[i] += n < attack ? im * n / attack : im;
            const float next_re = re * rot_re - im * rot_im;
            im = re * rot_im + im * rot_re;
            re = next_re;
        }
    }
}

static esp_err_t build_chime(void)
{
    const int count = SAMPLE_RATE * CHIME_SECONDS;
    float *mix = heap_caps_calloc(count, sizeof(float), MALLOC_CAP_SPIRAM);
    s_chime = heap_caps_malloc(count * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(mix && s_chime, ESP_ERR_NO_MEM, TAG, "no memory for the chime");

    add_bell(mix, count, 0, 659.25f, 1.0f);                     // E5
    add_bell(mix, count, (int)(0.55f * SAMPLE_RATE), 523.25f, 1.0f);  // C5
    float peak = 0;
    for (int i = 0; i < count; i++) {
        peak = fmaxf(peak, fabsf(mix[i]));
    }
    const float scale = 0.8f * 32767.0f / peak;
    for (int i = 0; i < count; i++) {
        s_chime[i] = (int16_t)(mix[i] * scale);
    }
    free(mix);
    return ESP_OK;
}

// Warm red -> orange -> warm white as the sunrise progresses, brightening all the way.
static void show_sunrise(float p)
{
    float r = 255, g, b;
    if (p < 0.5f) {
        g = 20 + (110 - 20) * (p / 0.5f);
        b = 0;
    } else {
        g = 110 + (200 - 110) * ((p - 0.5f) / 0.5f);
        b = 120 * ((p - 0.5f) / 0.5f);
    }
    float level = 0.08f + 0.92f * p;
    bsp_led_set((uint8_t)(r * level), (uint8_t)(g * level), (uint8_t)(b * level));
}

// Play the chime once; returns early if stop or snooze is requested.
static void play_chime(int volume)
{
    if (!s_codec || !s_chime) {
        vTaskDelay(pdMS_TO_TICKS(CHIME_SECONDS * 1000));
        return;
    }
    esp_codec_dev_set_out_vol(s_codec, volume);
    const int count = SAMPLE_RATE * CHIME_SECONDS;
    for (int n = 0; n < count; n += BLOCK_SAMPLES) {
        if (s_stop_request || s_snooze_request) {
            return;
        }
        esp_codec_dev_write(s_codec, &s_chime[n], BLOCK_SAMPLES * sizeof(int16_t));
    }
}

static void ring(time_t target)
{
    ESP_LOGI(TAG, "ringing");
    set_status(WAKE_RINGING, 1.0f, target);
    bsp_led_set(255, 200, 120);
    const time_t started = time(NULL);
    int volume = CHIME_VOLUME_START;

    for (;;) {
        play_chime(volume);
        volume = volume + CHIME_VOLUME_STEP > CHIME_VOLUME_MAX ? CHIME_VOLUME_MAX : volume + CHIME_VOLUME_STEP;

        time_t now = time(NULL);
        if (s_snooze_request) {
            s_snooze_request = false;
            ESP_LOGI(TAG, "snoozed for %d min", WAKE_SNOOZE_MINUTES);
            set_status(WAKE_SNOOZED, 0, now + SNOOZE_SEC);
            bsp_led_set(80, 25, 0);
            return;
        }
        if (s_stop_request || now - started > RING_TIMEOUT_SEC) {
            s_stop_request = false;
            ESP_LOGI(TAG, "%s", now - started > RING_TIMEOUT_SEC ? "timed out" : "stopped");
            set_status(WAKE_IDLE, 0, 0);
            bsp_led_set(0, 0, 0);
            return;
        }
    }
}

static void wake_task(void *arg)
{
    for (;;) {
        wake_status_t status;
        wake_get_status(&status);
        time_t now = time(NULL);
        struct tm local;
        bool synced = clock_now(&local);

        if (s_test_request) {
            s_test_request = false;
            s_stop_request = false;
            s_snooze_request = false;
            ring(now);
            continue;
        }

        switch (status.state) {
        case WAKE_IDLE: {
            s_stop_request = false;
            s_snooze_request = false;
            time_t next;
            if (synced && alarms_next(now > s_handled ? now : s_handled, &next) && next - now <= SUNRISE_SEC) {
                ESP_LOGI(TAG, "sunrise started for an alarm in %lld s", (long long)(next - now));
                set_status(WAKE_SUNRISE, 0, next);
            }
            break;
        }
        case WAKE_SUNRISE: {
            // Cancelled if stopped, or if the alarm was switched off or changed meanwhile
            time_t next;
            bool still_due = alarms_next(status.target - 1, &next) && next == status.target;
            if (s_stop_request || !still_due) {
                ESP_LOGI(TAG, "sunrise %s", s_stop_request ? "skipped" : "cancelled");
                if (s_stop_request) {
                    s_handled = status.target;
                }
                s_stop_request = false;
                set_status(WAKE_IDLE, 0, 0);
                bsp_led_set(0, 0, 0);
            } else if (now >= status.target) {
                s_handled = status.target;
                ring(status.target);
            } else {
                float p = 1.0f - (float)(status.target - now) / SUNRISE_SEC;
                p = p < 0 ? 0 : p;
                set_status(WAKE_SUNRISE, p, status.target);
                show_sunrise(p);
            }
            break;
        }
        case WAKE_SNOOZED:
            if (s_stop_request) {
                s_stop_request = false;
                set_status(WAKE_IDLE, 0, 0);
                bsp_led_set(0, 0, 0);
            } else if (now >= status.target) {
                ring(status.target);
            }
            break;
        case WAKE_RINGING:      // ring() only returns once it has left this state
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

esp_err_t wake_init(esp_codec_dev_handle_t codec)
{
    s_codec = codec;
    if (codec) {
        build_chime();
    }
    set_status(WAKE_IDLE, 0, 0);
    return xTaskCreate(wake_task, "wake", 4096, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void wake_get_status(wake_status_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_lock);
}

void wake_stop(void)
{
    s_stop_request = true;
}

void wake_snooze(void)
{
    s_snooze_request = true;
}

void wake_test(void)
{
    s_test_request = true;
}
