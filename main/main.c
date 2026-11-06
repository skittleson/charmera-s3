#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs_flash.h"

#include "app_version.h"
#include "board.h"
#include "camera.h"
#include "display.h"
#include "input.h"
#include "storage.h"

static const char *TAG = "main";

#include "mbedtls/base64.h"
static void dump565(const char *name, const uint16_t *px, int w, int h)
{
    static unsigned char line[200];
    printf("\nDUMP %s %d %d\n", name, w, h);
    const uint8_t *b = (const uint8_t *)px;
    const size_t n = (size_t)w * h * 2;
    for (size_t i = 0; i < n; i += 120) {
        size_t olen;
        mbedtls_base64_encode(line, sizeof(line), &olen, b + i, (n - i) < 120 ? n - i : 120);
        line[olen] = 0;
        printf("%s\n", line);
    }
    printf("END %s\n", name);
    fflush(stdout);
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
    ESP_ERROR_CHECK(input_init());
    ESP_ERROR_CHECK(camera_init());

    static uint16_t *canvas_buf;
    canvas_buf = heap_caps_malloc(CAMERA_PREVIEW_W * CAMERA_PREVIEW_H * 2, MALLOC_CAP_SPIRAM);
    static lv_image_dsc_t dsc = {
        .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                   .w = CAMERA_PREVIEW_W, .h = CAMERA_PREVIEW_H, .stride = CAMERA_PREVIEW_W * 2},
        .data_size = CAMERA_PREVIEW_W * CAMERA_PREVIEW_H * 2,
    };
    dsc.data = (const uint8_t *)canvas_buf;
    display_lock(0);
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
    lv_obj_t *img = lv_image_create(lv_screen_active());
    lv_obj_center(img);
    lv_image_set_src(img, &dsc);
    lv_obj_t *top = lv_label_create(lv_screen_active());
    lv_label_set_text(top, "TOP");
    lv_obj_set_style_text_color(top, lv_color_hex(0xffff00), 0);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 4);
    display_unlock();
    board_backlight_set(80);

    int frames = 0;
    int64_t t0 = esp_timer_get_time();
    for (;;) {
        uint16_t *f = camera_preview_frame();
        if (f && display_lock(100)) {
            memcpy(canvas_buf, f, CAMERA_PREVIEW_W * CAMERA_PREVIEW_H * 2);
            lv_obj_invalidate(img);
            display_unlock();
            frames++;
        }
        if (esp_timer_get_time() - t0 > 3000000) {
            ESP_LOGI(TAG, "preview fps %.1f", frames / ((esp_timer_get_time() - t0) / 1e6));
            frames = 0;
            t0 = esp_timer_get_time();
        }
        static bool auto_done;
        if (!auto_done && esp_timer_get_time() > 6000000) {
            auto_done = true;
        } else if (input_get(0) != INPUT_SHORT) {
            continue;
        }
        {
            int64_t c0 = esp_timer_get_time();
            esp_err_t ferr = camera_focus(1500);
            int64_t c1 = esp_timer_get_time();
            camera_fb_t *fb = camera_capture_full();
            int64_t c2 = esp_timer_get_time();
            if (fb) {
                ESP_LOGI(TAG, "capture %ux%u len=%u b0=%02x%02x focus=%s focus_ms=%lld cap_ms=%lld",
                         (unsigned)fb->width, (unsigned)fb->height, (unsigned)fb->len, fb->buf[0],
                         fb->buf[1], esp_err_to_name(ferr), (c1 - c0) / 1000, (c2 - c1) / 1000);
            } else {
                ESP_LOGE(TAG, "capture failed");
            }
            if (fb && display_lock(100)) {
                camera_photo_thumbnail(fb, canvas_buf);
                dump565("photo", canvas_buf, CAMERA_PREVIEW_W, CAMERA_PREVIEW_H);
                lv_obj_invalidate(img);
                display_unlock();
                vTaskDelay(pdMS_TO_TICKS(4000));
            }
            camera_capture_done(fb);
            ESP_LOGI(TAG, "back to preview in %lld ms", (esp_timer_get_time() - c2) / 1000);
        }
    }
}
