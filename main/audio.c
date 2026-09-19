/*
 * Speaker and microphone on the LCDwiki / Hosyond ES3C35P: an ES8311 codec (I2C 0x18)
 * with an on-board analog mic, feeding an SC8002B amplifier.
 *
 * Pin directions and amplifier polarity follow the xiaozhi-esp32 port for this board
 * (main/boards/lcdwiki-es3c35p), which uses both mic and speaker: GPIO15 is I2S data
 * out, GPIO16 is data in, and the amplifier's shutdown pin (GPIO1) enables it when LOW.
 *
 * One task owns the codec, so reads, writes and control changes never overlap. While
 * idle it keeps reading the mic to drive the level meter.
 */
#include "audio.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "driver/i2s_std.h"
#include "es8311_codec.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define AUDIO_I2C_PORT      I2C_NUM_0
#define AUDIO_I2S_PORT      I2S_NUM_0
#define AUDIO_PIN_MCLK      GPIO_NUM_17
#define AUDIO_PIN_BCLK      GPIO_NUM_18
#define AUDIO_PIN_WS        GPIO_NUM_21
#define AUDIO_PIN_DOUT      GPIO_NUM_15
#define AUDIO_PIN_DIN       GPIO_NUM_16
#define AUDIO_PIN_PA        GPIO_NUM_1      // SC8002B shutdown, amplifier on when LOW

#define SAMPLE_RATE         16000
#define BLOCK_SAMPLES       (SAMPLE_RATE / 50)                  // 20ms
#define RECORD_SAMPLES      (SAMPLE_RATE * AUDIO_RECORD_SECONDS)

// The ES8311 driver always runs the analog mic PGA at its 30dB maximum; this sets the ADC
// gain scale on top of it, in 6dB steps up to 42dB.
#define MIC_GAIN_DB         42.0f
#define DEFAULT_VOLUME      60

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

static const char *TAG = "audio";

static esp_codec_dev_handle_t s_codec;
static QueueHandle_t s_cmds;
static _Atomic audio_state_t s_state = AUDIO_IDLE;
static _Atomic int s_mic_level;
static _Atomic int s_target_volume = DEFAULT_VOLUME;

static esp_err_t i2s_init(i2s_chan_handle_t *tx, i2s_chan_handle_t *rx)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear_after_cb = true;    // play silence, not the last buffer, when starved
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, tx, rx), TAG, "i2s_new_channel failed");

    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = AUDIO_PIN_MCLK,
            .bclk = AUDIO_PIN_BCLK,
            .ws = AUDIO_PIN_WS,
            .dout = AUDIO_PIN_DOUT,
            .din = AUDIO_PIN_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(*tx, &std_cfg), TAG, "tx init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(*rx, &std_cfg), TAG, "rx init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(*tx), TAG, "tx enable failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(*rx), TAG, "rx enable failed");
    return ESP_OK;
}

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
    i2s_chan_handle_t tx = NULL;
    i2s_chan_handle_t rx = NULL;
    ESP_RETURN_ON_ERROR(i2s_init(&tx, &rx), TAG, "I2S init failed");

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = AUDIO_I2S_PORT,
        .rx_handle = rx,
        .tx_handle = tx,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = AUDIO_I2C_PORT,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && ctrl_if && gpio_if, ESP_FAIL, TAG, "codec interface init failed");

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = AUDIO_PIN_PA,
        .pa_reverted = true,
        .use_mclk = true,
        .hw_gain = {
            .pa_voltage = 5.0,
            .codec_dac_voltage = 3.3,
        },
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(codec_if, ESP_FAIL, TAG, "ES8311 not responding at 0x%02X",
                        ES8311_CODEC_DEFAULT_ADDR >> 1);

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_codec = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_codec, ESP_FAIL, TAG, "esp_codec_dev_new failed");

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = SAMPLE_RATE,
        .channel = 1,
        .bits_per_sample = 16,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_codec, &fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG,
                        "codec open failed");
    esp_codec_dev_set_in_gain(s_codec, MIC_GAIN_DB);
    esp_codec_dev_set_out_vol(s_codec, DEFAULT_VOLUME);

    s_cmds = xQueueCreate(4, sizeof(audio_cmd_t));
    ESP_RETURN_ON_FALSE(s_cmds, ESP_ERR_NO_MEM, TAG, "queue alloc failed");
    ESP_RETURN_ON_FALSE(xTaskCreate(audio_task, "audio", 4096, NULL, 5, NULL) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "task create failed");

    ESP_LOGI(TAG, "ES8311 ready, %d Hz mono", SAMPLE_RATE);
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
