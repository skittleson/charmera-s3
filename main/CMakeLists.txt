idf_component_register(SRCS "main.c" "settings.c" "power.c"
                            "ui/ui.c" "ui/screen_camera.c" "ui/screen_gallery.c" "ui/screen_settings.c" "ui/jpeg_preview.c"
                       INCLUDE_DIRS "." "ui"
                       REQUIRES board display input storage camera looks pipeline nvs_flash esp_timer nvs_flash espressif__esp_new_jpeg)
