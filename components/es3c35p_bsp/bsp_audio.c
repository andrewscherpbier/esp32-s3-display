/*
 * Speaker and microphone on the LCDwiki / Hosyond ES3C35P: an ES8311 codec (I2C 0x18)
 * with an on-board analog mic, feeding an SC8002B amplifier.
 *
 * Pin directions and amplifier polarity follow the xiaozhi-esp32 port for this board
 * (main/boards/lcdwiki-es3c35p) and were confirmed with an acoustic loopback test: an
 * 880Hz tone played on GPIO15 shows up on GPIO16 only while GPIO1 is LOW.
 */
#include "bsp_audio.h"

#include <inttypes.h>

#include "bsp_pins.h"
#include "driver/i2s_std.h"
#include "es8311_codec.h"
#include "esp_check.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"

#define BSP_AUDIO_I2C_PORT  I2C_NUM_0
#define BSP_AUDIO_I2S_PORT  I2S_NUM_0

// The ES8311 driver always runs the analog mic PGA at its 30dB maximum; this sets the ADC
// gain scale on top of it, in 6dB steps up to 42dB.
#define MIC_GAIN_DB         42.0f

static const char *TAG = "bsp_audio";

static esp_err_t i2s_init(uint32_t sample_rate, i2s_chan_handle_t *tx, i2s_chan_handle_t *rx)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BSP_AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.auto_clear_after_cb = true;    // play silence, not the last buffer, when starved
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, tx, rx), TAG, "i2s_new_channel failed");

    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_BCLK,
            .ws = BSP_I2S_WS,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DIN,
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(*tx, &std_cfg), TAG, "tx init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(*rx, &std_cfg), TAG, "rx init failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(*tx), TAG, "tx enable failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(*rx), TAG, "rx enable failed");
    return ESP_OK;
}

esp_codec_dev_handle_t bsp_audio_init(i2c_master_bus_handle_t bus, uint32_t sample_rate)
{
    i2s_chan_handle_t tx = NULL;
    i2s_chan_handle_t rx = NULL;
    ESP_RETURN_ON_FALSE(i2s_init(sample_rate, &tx, &rx) == ESP_OK, NULL, TAG, "I2S init failed");

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = BSP_AUDIO_I2S_PORT,
        .rx_handle = rx,
        .tx_handle = tx,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_AUDIO_I2C_PORT,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(data_if && ctrl_if && gpio_if, NULL, TAG, "codec interface init failed");

    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = BSP_AMP_ENABLE,
        .pa_reverted = true,
        .use_mclk = true,
        .hw_gain = {
            .pa_voltage = 5.0,
            .codec_dac_voltage = 3.3,
        },
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(codec_if, NULL, TAG, "ES8311 not responding at 0x%02X",
                        ES8311_CODEC_DEFAULT_ADDR >> 1);

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    esp_codec_dev_handle_t codec = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(codec, NULL, TAG, "esp_codec_dev_new failed");

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = sample_rate,
        .channel = 1,
        .bits_per_sample = 16,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(codec, &fs) == ESP_CODEC_DEV_OK, NULL, TAG, "codec open failed");
    esp_codec_dev_set_in_gain(codec, MIC_GAIN_DB);
    esp_codec_dev_set_out_vol(codec, BSP_AUDIO_DEFAULT_VOLUME);

    ESP_LOGI(TAG, "ES8311 ready, %" PRIu32 " Hz mono", sample_rate);
    return codec;
}
