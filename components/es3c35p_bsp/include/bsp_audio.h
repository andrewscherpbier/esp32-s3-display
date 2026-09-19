#pragma once

#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_codec_dev.h"

#define BSP_AUDIO_DEFAULT_VOLUME 60

/**
 * Bring up the ES8311 codec (on the shared I2C bus from bsp_i2c_init()), its I2S link
 * and the speaker amplifier, and open it for 16-bit mono at `sample_rate` in both
 * directions. Returns the handle for esp_codec_dev_read()/write(), or NULL (logged).
 *
 * Board notes:
 * - Mic gain is set to the maximum the driver offers (30dB analog PGA + 42dB ADC scale).
 *   The on-board mic is still quiet, with speech peaking around -25 dBFS, so normalize
 *   recordings before playing them back.
 * - The ADC pops to full scale for ~200ms after opening; discard that much input.
 * - Output starts at BSP_AUDIO_DEFAULT_VOLUME; change it with esp_codec_dev_set_out_vol().
 * - Keep reads, writes and control calls on the handle in one task.
 */
esp_codec_dev_handle_t bsp_audio_init(i2c_master_bus_handle_t bus, uint32_t sample_rate);
