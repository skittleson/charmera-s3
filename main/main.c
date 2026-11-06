#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "nvs_flash.h"

#include "app_version.h"
#include "board.h"
#include "camera.h"
#include "display.h"
#include "input.h"
#include "pipeline.h"
#include "storage.h"

static const char *TAG = "main";
#ifndef AEC_SCALE_LOG
#define AEC_SCALE_LOG 256
#endif

static int mean_luma565(const uint16_t *p, int n)
{
    long sum = 0;
    for (int i = 0; i < n; i++) {
        const uint16_t v = p[i];
        sum += (77 * (((v >> 11) & 31) << 3) + 150 * (((v >> 5) & 63) << 2) + 29 * ((v & 31) << 3)) >> 8;
    }
    return (int)(sum / n);
}

static int s_thumb_luma;
static void on_captured(const uint16_t *thumb, void *ctx)
{
    s_thumb_luma = mean_luma565(thumb, 240 * 320);
}

static void dump_file(int number)
{
    char path[64];
    storage_photo_path(number, path, sizeof(path));
    board_spi_lock();
    FILE *f = fopen(path, "rb");
    if (!f) {
        board_spi_unlock();
        return;
    }
    static uint8_t buf[480];
    static unsigned char line[700];
    printf("\nFILE IMG_%04d.JPG\n", number);
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        size_t olen;
        mbedtls_base64_encode(line, sizeof(line), &olen, buf, n);
        line[olen] = 0;
        printf("%s\n", line);
    }
    printf("ENDFILE\n");
    fflush(stdout);
    fclose(f);
    board_spi_unlock();
}

void app_main(void)
{
    ESP_LOGI(TAG, "charmera-s3 %s boot", APP_VERSION);
    esp_err_t nerr = nvs_flash_init();
    if (nerr == ESP_ERR_NVS_NO_FREE_PAGES || nerr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    ESP_ERROR_CHECK(board_init());
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(pipeline_init());
    ESP_ERROR_CHECK(camera_init());
    board_backlight_set(60);
    storage_status();

    const look_t looks[] = {LOOK_COOL, LOOK_WARM, LOOK_BW, LOOK_PIXEL_YELLOW, LOOK_FRAME_CHARMERA,
                            LOOK_FRAME_STICKER, LOOK_FRAME_PC};
    for (size_t i = 0; i < sizeof(looks) / sizeof(looks[0]); i++) {
        for (int k = 0; k < 30; k++) {
            camera_preview_frame();
        }
        ESP_LOGI(TAG, "largest PSRAM block %u", (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        int number = 0;
        pipe_result_t r = pipeline_take_photo(looks[i], on_captured, NULL, NULL, &number);
        ESP_LOGI(TAG, "%s -> %s", look_name(looks[i]), pipeline_result_text(r));
        if (r == PIPE_OK) {
            dump_file(number);
        }
    }
    ESP_LOGI(TAG, "done");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
