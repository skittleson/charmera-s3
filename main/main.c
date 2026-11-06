#include <stdlib.h>
#include <string.h>

#include <dirent.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "app_version.h"
#include "board.h"
#include "display.h"
#include "input.h"
#include "nvs_flash.h"
#include "storage.h"

static const char *TAG = "main";

static void corner(lv_obj_t *scr, lv_align_t align, lv_color_t colour, const char *text)
{
    lv_obj_t *box = lv_obj_create(scr);
    lv_obj_set_size(box, 60, 60);
    lv_obj_set_style_bg_color(box, colour, 0);
    lv_obj_set_style_radius(box, 0, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_align(box, align, 0, 0);
    lv_obj_t *l = lv_label_create(box);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_black(), 0);
    lv_obj_center(l);
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

    display_lock(0);
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x202020), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    corner(scr, LV_ALIGN_TOP_LEFT, lv_color_hex(0xff0000), "TL red");
    corner(scr, LV_ALIGN_TOP_RIGHT, lv_color_hex(0x00ff00), "TR grn");
    corner(scr, LV_ALIGN_BOTTOM_LEFT, lv_color_hex(0x0000ff), "BL blu");
    corner(scr, LV_ALIGN_BOTTOM_RIGHT, lv_color_hex(0xffffff), "BR wht");
    display_unlock();

    for (int p = 0; p <= 80; p += 5) {
        board_backlight_set(p);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    for (;;) {
        storage_state_t st = storage_status();
        ESP_LOGI(TAG, "storage_status=%d", st);
        if (st == STORAGE_OK) {
            storage_info_t info;
            if (storage_info(&info) == ESP_OK) {
                ESP_LOGI(TAG, "total=%llu free=%llu photos=%u remaining=%u",
                         (unsigned long long)info.total_bytes, (unsigned long long)info.free_bytes,
                         (unsigned)info.photo_count, (unsigned)storage_photos_remaining_now());
            }
            static bool listed;
            if (!listed) {
                listed = true;
                DIR *d = opendir(STORAGE_MOUNT);
                struct dirent *e;
                while (d && (e = readdir(d))) {
                    ESP_LOGI(TAG, "root: %s", e->d_name);
                }
                if (d) {
                    closedir(d);
                }
            }
            static bool wrote;
            if (!wrote) {
                wrote = true;
                int num = 0;
                FILE *f = storage_begin_photo(&num);
                if (f) {
                    static uint8_t buf[1024];
                    memset(buf, 0xAB, sizeof(buf));
                    fwrite(buf, 1, sizeof(buf), f);
                    ESP_LOGI(TAG, "commit test photo %d -> %s", num,
                             esp_err_to_name(storage_commit_photo(f, num)));
                    int *nums;
                    size_t n;
                    storage_list_photos(&nums, &n);
                    for (size_t i = 0; i < n; i++) {
                        ESP_LOGI(TAG, "listed %d", nums[i]);
                    }
                    free(nums);
                    ESP_LOGI(TAG, "delete test photo -> %s", esp_err_to_name(storage_delete_photo(num)));
                } else {
                    ESP_LOGE(TAG, "begin_photo failed");
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1500));
    }
}
