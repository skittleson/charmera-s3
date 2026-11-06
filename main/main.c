#include "esp_log.h"
#include "nvs_flash.h"

#include "app_version.h"
#include "board.h"
#include "camera.h"
#include "display.h"
#include "input.h"
#include "pipeline.h"
#include "power.h"
#include "settings.h"
#include "storage.h"
#include "ui/ui.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "charmera-s3 %s boot", APP_VERSION);
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    settings_load();
    ESP_ERROR_CHECK(board_init());
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(input_init());
    ESP_ERROR_CHECK(pipeline_init());
    if (camera_init() != ESP_OK) {
        ESP_LOGE(TAG, "camera not found");
    }
    storage_status();
    power_init();
    ui_start();
    board_backlight_set(settings_brightness());
}
