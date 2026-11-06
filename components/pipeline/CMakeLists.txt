idf_component_register(SRCS "pipeline.c"
                       INCLUDE_DIRS "include"
                       REQUIRES looks
                       PRIV_REQUIRES camera storage log esp_timer espressif__esp_new_jpeg)
