#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "app_version.h"
#include "board.h"
#include "display.h"
#include "input.h"

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
    ESP_ERROR_CHECK(input_init());
    static const char *names[] = {"none", "PREV", "NEXT", "SHORT", "LONG", "DOUBLE"};
    uint32_t n = 0;
    for (;;) {
        input_event_t e = input_get(50);
        if (e != INPUT_NONE) {
            ESP_LOGI(TAG, "event %s", names[e]);
        }
        if (++n % 5 == 0) {
            int16_t r[3];
            input_accel_raw(r);
            ESP_LOGI(TAG, "accel %6d %6d %6d roll %5.1f", r[0], r[1], r[2], input_roll_deg());
        }
    }
}
