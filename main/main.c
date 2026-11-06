#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_version.h"
#include "board.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "charmera-s3 %s boot", APP_VERSION);
    ESP_LOGI(TAG, "PSRAM: %u", (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
    ESP_ERROR_CHECK(board_init());

    for (int p = 0; p <= 100; p += 5) {
        board_backlight_set(p);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    for (;;) {
        ESP_LOGI(TAG, "battery=%.2fV", board_battery_volts());
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
