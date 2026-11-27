idf_component_register(SRCS "app_clock.c" "timer_face.c" "usage_face.c" "workout_face.c" "weather_face.c" "calendar_face.c"
    INCLUDE_DIRS "include"
    REQUIRES esp-gsp
    PRIV_REQUIRES app_alarm app_voice app_workout espressif__cjson esp-mosaico-bsp esp_timer log
    EMBED_FILES "assets/dial.rgb565" "assets/digits.rgb565" "assets/ring_top.rgba" "assets/ring_bottom.rgba"
        "assets/cities_top.rgba" "assets/cities_bottom.rgba")
