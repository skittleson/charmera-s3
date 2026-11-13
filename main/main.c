#include "esp_log.h"
#include "nvs_flash.h"

#include "app_version.h"
#include "board.h"
#include "camera.h"
#include "display.h"
#include "input.h"
#include "pipeline.h"
#include "orientation.h"
#include "power.h"
#include "remote_bridge.h"
#include "settings.h"
#include "storage.h"
#include "ui/ui.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "charmera-s3 %s boot", APP_VERSION);
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    settings_load();
    ESP_ERROR_CHECK(board_init());
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(input_init());
    /* Reserve the UI's big buffers before the camera's full-res frame buffers, so PSRAM
       fragmentation can't strand them (see ui.h). */
    ui_reserve();
    /* The camera is detected next: the pipeline sizes its buffers for it. */
    if (camera_init() != ESP_OK) {
        ESP_LOGE(TAG, "camera not found");
    }
    ESP_LOGI(TAG, "camera: %s", camera_sensor_name());
    ESP_ERROR_CHECK(pipeline_init("charmera-s3 " APP_VERSION));
    orientation_init();
    storage_status();
    power_init();
    ui_start();
    remote_bridge_init();
    board_backlight_set(settings_brightness());
}
