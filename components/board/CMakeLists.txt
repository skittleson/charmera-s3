idf_component_register(SRCS "board.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_i2c
                       PRIV_REQUIRES esp_driver_spi esp_driver_ledc esp_adc freertos log)
