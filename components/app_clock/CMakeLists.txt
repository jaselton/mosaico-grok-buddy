idf_component_register(SRCS "app_clock.c" "timer_face.c"
    INCLUDE_DIRS "include"
    REQUIRES esp-gsp
    PRIV_REQUIRES app_alarm esp-mosaico-bsp esp_timer log
    EMBED_FILES "assets/dial.rgb565" "assets/digits.rgb565" "assets/ring_top.rgba" "assets/ring_bottom.rgba"
        "assets/cities_top.rgba" "assets/cities_bottom.rgba")
