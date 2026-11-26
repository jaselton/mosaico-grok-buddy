idf_component_register(SRCS "app_alarm.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES esp_netif nvs_flash log)
