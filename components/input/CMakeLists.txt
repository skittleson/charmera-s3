idf_component_register(SRCS "input.c" "src/input_logic.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES board esp_driver_gpio esp_driver_i2c esp_timer log)
