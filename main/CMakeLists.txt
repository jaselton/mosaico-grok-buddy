idf_component_register(SRCS "main.c" "settings.c"
    PRIV_REQUIRES app_config app_display app_alarm app_buddy app_camera app_clock app_motion app_voice app_workout esp-mosaico-bsp esp_iris esp_mosaico_app_recovery esp_wifi esp_netif esp_timer
        esp_mosaico_gsp_bundle esp_mosaico_gsp_iris mbedtls nvs_flash log)
