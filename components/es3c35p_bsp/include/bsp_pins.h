#pragma once

/*
 * GPIO map of the LCDwiki / Hosyond ES3C35P (ESP32-S3R8, 3.5" 320x480 ST77922 QSPI).
 * Every assignment here has been exercised on real hardware unless noted.
 */

#include "driver/gpio.h"

// LCD, ST77922 over QSPI. Its reset line is tied to CHIP_PU (no GPIO).
#define BSP_LCD_CS          GPIO_NUM_10
#define BSP_LCD_CLK         GPIO_NUM_12
#define BSP_LCD_D0          GPIO_NUM_11
#define BSP_LCD_D1          GPIO_NUM_13
#define BSP_LCD_D2          GPIO_NUM_14
#define BSP_LCD_D3          GPIO_NUM_9
#define BSP_LCD_BACKLIGHT   GPIO_NUM_41     // active high
#define BSP_LCD_TE          GPIO_NUM_42     // tearing effect; unused, per factory firmware

// Shared I2C bus: touch (0x55), ES8311 codec (0x18), expansion header
#define BSP_I2C_SDA         GPIO_NUM_38
#define BSP_I2C_SCL         GPIO_NUM_39

// Touch controller (Sitronix, ST7123 register map)
#define BSP_TOUCH_RST       GPIO_NUM_48     // active low
#define BSP_TOUCH_INT       GPIO_NUM_47     // unused; touch is polled

// ES8311 audio codec over I2S, and the SC8002B speaker amplifier
#define BSP_I2S_MCLK        GPIO_NUM_17
#define BSP_I2S_BCLK        GPIO_NUM_18
#define BSP_I2S_WS          GPIO_NUM_21
#define BSP_I2S_DOUT        GPIO_NUM_15     // ESP32 -> codec (speaker)
#define BSP_I2S_DIN         GPIO_NUM_16     // codec -> ESP32 (microphone)
#define BSP_AMP_ENABLE      GPIO_NUM_1      // amplifier on when LOW

// WS2812 RGB LED, colour order GRB
#define BSP_RGB_LED         GPIO_NUM_40

// microSD, 4-bit SDMMC; no card-detect line
#define BSP_SD_CLK          GPIO_NUM_5
#define BSP_SD_CMD          GPIO_NUM_4
#define BSP_SD_D0           GPIO_NUM_6
#define BSP_SD_D1           GPIO_NUM_7
#define BSP_SD_D2           GPIO_NUM_2
#define BSP_SD_D3           GPIO_NUM_3

#define BSP_BOOT_BUTTON     GPIO_NUM_0      // active low
#define BSP_BATTERY_ADC     GPIO_NUM_8      // ADC1_CH7; untested (no battery available)
