idf_component_register(SRCS "main.c"
    PRIV_REQUIRES app_config app_display app_alarm app_buddy app_motion app_voice esp_iris esp_mosaico_app_recovery
        esp_mosaico_gsp_bundle esp_mosaico_gsp_iris mbedtls nvs_flash log)
