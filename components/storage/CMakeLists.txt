idf_component_register(SRCS "storage.c" "src/storage_naming.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES board fatfs sdmmc esp_driver_sdspi esp_timer nvs_flash vfs log)
