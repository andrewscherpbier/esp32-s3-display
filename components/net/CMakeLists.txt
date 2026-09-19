idf_component_register(
    SRCS "wifi.c" "wifi_screen.c" "clock.c" "net_http.c" "geoip.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi
    PRIV_REQUIRES esp_netif esp_event esp_timer nvs_flash esp_http_client json mbedtls
)
