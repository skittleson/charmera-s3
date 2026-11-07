idf_component_register(SRCS "pipeline.c" "src/exif.c"
                       INCLUDE_DIRS "include"
                       REQUIRES looks
                       PRIV_REQUIRES camera storage log esp_timer esp_hw_support espressif__esp_new_jpeg)
