idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES board display input storage camera pipeline nvs_flash esp_timer mbedtls)
