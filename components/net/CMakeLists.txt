idf_component_register(
    SRCS "wifi.c" "wifi_screen.c" "clock.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi
    PRIV_REQUIRES esp_netif esp_event esp_timer nvs_flash
)
