#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "jpeg_preview.h"
#include "storage.h"
#include "ui.h"

static const char *TAG = "ui_gallery";

static uint16_t *s_px;
static lv_image_dsc_t s_dsc = {
    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565, .w = PREVIEW_W,
               .h = PREVIEW_H, .stride = PREVIEW_W * 2},
    .data_size = PREVIEW_W * PREVIEW_H * 2,
};

static struct {
    lv_obj_t *img, *pos, *msg, *hint;
    int *nums;
    size_t count;
    int index; /* into nums; count-1 = newest */
} s;

static void show_message(const char *text)
{
    lv_label_set_text(s.msg, text);
    lv_obj_set_hidden(s.msg, false);
    lv_obj_set_hidden(s.img, true);
    lv_label_set_text(s.pos, "");
}

/* Load the current photo and update labels. Display lock NOT held. */
static void show_current(void)
{
    if (s.count == 0) {
        if (display_lock(200)) {
            show_message(storage_status() == STORAGE_OK ? "No photos yet" : "No card");
            display_unlock();
        }
        return;
    }
    char path[64];
    storage_photo_path(s.nums[s.index], path, sizeof(path));
    const esp_err_t err = jpeg_preview_load(path, s_px);
    char pos[40];
    snprintf(pos, sizeof(pos), "%u / %u   IMG_%04d", (unsigned)(s.count - s.index), (unsigned)s.count,
             s.nums[s.index]);
    if (display_lock(200)) {
        lv_label_set_text(s.pos, pos);
        if (err == ESP_OK) {
            lv_obj_set_hidden(s.msg, true);
            lv_obj_set_hidden(s.img, false);
            lv_obj_invalidate(s.img);
        } else {
            lv_label_set_text(s.msg, "Can't show this photo");
            lv_obj_set_hidden(s.msg, false);
            lv_obj_set_hidden(s.img, true);
        }
        display_unlock();
    }
}

static void reload(int keep_number)
{
    free(s.nums);
    s.nums = NULL;
    s.count = 0;
    storage_list_photos(&s.nums, &s.count);
    s.index = (int)s.count - 1;
    if (keep_number > 0) {
        for (size_t i = 0; i < s.count; i++) {
            if (s.nums[i] <= keep_number) {
                s.index = (int)i;
            }
        }
    }
    show_current();
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_color(l, UI_COLOR_FG, 0);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_bg_color(l, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_60, 0);
    lv_obj_set_style_pad_hor(l, 6, 0);
    lv_obj_set_style_pad_ver(l, 2, 0);
    lv_obj_set_style_radius(l, 4, 0);
    return l;
}

static void enter(lv_obj_t *scr)
{
    if (!s_px) {
        s_px = heap_caps_calloc(1, PREVIEW_W * PREVIEW_H * 2, MALLOC_CAP_SPIRAM);
        s_dsc.data = (const uint8_t *)s_px;
    }
    memset(&s, 0, sizeof(s));
    s.img = lv_image_create(scr);
    lv_image_set_src(s.img, &s_dsc);
    lv_obj_center(s.img);
    s.msg = label(scr, &lv_font_montserrat_20);
    lv_obj_center(s.msg);
    lv_obj_set_hidden(s.msg, true);
    s.pos = label(scr, &lv_font_montserrat_14);
    lv_obj_align(s.pos, LV_ALIGN_TOP_MID, 0, 4);
    s.hint = label(scr, &lv_font_montserrat_14);
    lv_label_set_text(s.hint, "press: older  hold: menu  2x: back");
    lv_obj_align(s.hint, LV_ALIGN_BOTTOM_MID, 0, -4);
}

static void leave(void)
{
    free(s.nums);
    s.nums = NULL;
    s.count = 0;
}

static int s_pending_delete; /* 1 = this, 2 = all */

static void on_confirm(int yes)
{
    if (!yes || s.count == 0) {
        return;
    }
    if (s_pending_delete == 2) {
        ui_toast("Deleting...", 10000);
        const esp_err_t err = storage_delete_all_photos();
        ui_toast(err == ESP_OK ? "All photos deleted" : "Delete failed", 1500);
        ESP_LOGI(TAG, "delete all -> %s", esp_err_to_name(err));
        reload(0);
    } else {
        const int num = s.nums[s.index];
        const esp_err_t err = storage_delete_photo(num);
        ui_toast(err == ESP_OK ? "Deleted" : "Delete failed", 1200);
        ESP_LOGI(TAG, "delete IMG_%04d -> %s", num, esp_err_to_name(err));
        reload(num - 1);
    }
}

static void on_menu(int index)
{
    if (index == 0) {
        s_pending_delete = 1;
        ui_confirm("Delete this photo?", "Delete", on_confirm);
    } else if (index == 1) {
        s_pending_delete = 2;
        ui_confirm("Delete ALL photos on the card?", "Delete all", on_confirm);
    } else if (index == 2) {
        ui_show(UI_SCREEN_CAMERA);
    }
}

static void on_empty_menu(int index)
{
    if (index == 0) {
        ui_show(UI_SCREEN_CAMERA);
    }
}

static void event(input_event_t e)
{
    switch (e) {
    case INPUT_SHORT:
        if (s.count > 1) {
            s.index = s.index > 0 ? s.index - 1 : (int)s.count - 1;
            show_current();
        }
        break;
    case INPUT_LONG:
        if (s.count > 0) {
            static const char *const items[] = {"Delete this", "Delete all", "Back to camera", "Cancel"};
            ui_menu_open("Photo", items, 4, 3, on_menu);
        } else {
            static const char *const items[] = {"Back to camera", "Cancel"};
            ui_menu_open("Gallery", items, 2, 0, on_empty_menu);
        }
        break;
    case INPUT_DOUBLE:
        ui_show(UI_SCREEN_CAMERA);
        break;
    default:
        break;
    }
}

static bool s_loaded;

static void tick(void)
{
    if (!s_loaded) {
        s_loaded = true;
        reload(0);
    }
}

static void enter_wrap(lv_obj_t *scr)
{
    s_loaded = false;
    enter(scr);
}

const ui_screen_t ui_gallery_screen = {
    .enter = enter_wrap,
    .leave = leave,
    .event = event,
    .tick = tick,
};
