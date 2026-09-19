idf_component_register(
    SRCS "main.c" "tiles.c" "flights.c" "airlines.c" "plane_icon.c" "ui_map.c" "ui_follow.c"
    INCLUDE_DIRS "."
    REQUIRES es3c35p_bsp net json esp_timer
)
