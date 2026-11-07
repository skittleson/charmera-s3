idf_component_register(SRCS "main.c" "settings.c" "remote_bridge.c" "orientation.c" "power.c" "logic/power_logic.c" "logic/lapse_logic.c" "logic/preview_fit.c"
                            "ui/ui.c" "ui/screen_camera.c" "ui/screen_gallery.c" "ui/screen_settings.c" "ui/jpeg_preview.c"
                       INCLUDE_DIRS "." "ui" "logic"
                       REQUIRES board display input storage camera looks pipeline remote orient nvs_flash esp_timer espressif__esp_new_jpeg)
