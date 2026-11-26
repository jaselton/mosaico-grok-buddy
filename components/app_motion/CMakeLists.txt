idf_component_register(SRCS "app_motion.c"
    INCLUDE_DIRS "include"
    REQUIRES app_buddy esp_common
    PRIV_REQUIRES esp-mosaico-bsp esp_timer log)
