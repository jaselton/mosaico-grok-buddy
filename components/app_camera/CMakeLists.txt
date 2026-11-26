idf_component_register(SRCS "app_camera.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common
    PRIV_REQUIRES mosaico_module_camera espressif__esp_new_jpeg esp_driver_ppa esp_mm log freertos)
