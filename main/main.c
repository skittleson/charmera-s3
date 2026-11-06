#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_version.h"

static const char *TAG = "main";

void app_main(void)
{
    for (;;) {
        ESP_LOGI(TAG, "charmera-s3 %s boot", APP_VERSION);
        ESP_LOGI(TAG, "PSRAM: %u", (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
