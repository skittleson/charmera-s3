idf_component_register(SRCS "display.c"
                       INCLUDE_DIRS "include"
                       REQUIRES orient
                       PRIV_REQUIRES board esp_lcd log)
