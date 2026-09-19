idf_component_register(
    SRCS "main.c" "alarms.c" "wake.c" "weather.c" "ui_clock.c" "ui_alarms.c" "font_clock_120.c"
    INCLUDE_DIRS "."
    REQUIRES es3c35p_bsp net esp_http_client json nvs_flash mbedtls
)
