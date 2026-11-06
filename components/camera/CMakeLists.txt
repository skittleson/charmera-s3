idf_component_register(SRCS "camera.c"
                       INCLUDE_DIRS "include"
                       REQUIRES looks espressif__esp32-camera
                       PRIV_REQUIRES log esp_timer esp_driver_gpio espressif__esp_new_jpeg)
