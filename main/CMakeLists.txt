idf_component_register(
    SRCS "main.c" "board.c" "display.c" "touch.c" "audio.c" "wifi.c" "wifi_screen.c" "clock.c" "led.c" "sdcard.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd driver esp_wifi esp_netif esp_event esp_timer nvs_flash fatfs sdmmc
)
