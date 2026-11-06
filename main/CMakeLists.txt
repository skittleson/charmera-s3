idf_component_register(SRCS "main.c" "settings.c" "power.c" "logic/power_logic.c"
                            "ui/ui.c" "ui/screen_camera.c" "ui/screen_gallery.c" "ui/screen_settings.c" "ui/jpeg_preview.c"
                       INCLUDE_DIRS "." "ui" "logic"
                       REQUIRES board display input storage camera looks pipeline nvs_flash esp_timer espressif__esp_new_jpeg)
