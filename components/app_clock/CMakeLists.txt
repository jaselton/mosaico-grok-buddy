idf_component_register(SRCS "app_clock.c"
    INCLUDE_DIRS "include"
    REQUIRES esp-gsp
    PRIV_REQUIRES app_alarm esp-mosaico-bsp espressif__libpng esp_timer log
    EMBED_FILES "assets/worldtimer_dial.png" "assets/worldtimer_ring.png" "assets/worldtimer_ring_bottom.png" "assets/worldtimer_cities_top.png" "assets/worldtimer_cities_bottom.png" "assets/digits.png")
