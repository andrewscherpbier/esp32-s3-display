idf_component_register(
    SRCS "bsp_i2c.c" "bsp_display.c" "bsp_touch.c" "bsp_audio.c" "bsp_led.c" "bsp_sdcard.c"
    INCLUDE_DIRS "include"
    REQUIRES driver
    PRIV_REQUIRES esp_lcd fatfs sdmmc
)
