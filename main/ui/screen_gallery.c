#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "avi_writer.h"
#include "board.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "jpeg_preview.h"
#include "storage.h"
#include "ui.h"

static const char *TAG = "ui_gallery";

/* One video frame, 320x240 RGB565 (the decoder's output for a clip frame). */
#define VIDEO_FRAME_BYTES (320 * 240 * 2)

static uint16_t *s_px;
static uint8_t *s_vdec; /* one decoded video frame (PSRAM) */
static uint8_t *s_vjpeg; /* one JPEG frame from a clip (PSRAM) */
static lv_image_dsc_t s_dsc = {
    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565},
    .data_size = PREVIEW_PIXELS * 2,
};

static struct {
    lv_obj_t *img, *pos, *msg, *hint;
    int *nums;
    size_t count;
    int index; /* into nums; count-1 = newest */
    /* Video playback: the clip at s.index, looping until tapped. */
    bool playing;
    avi_reader_t *rd;
    int64_t due_ms; /* when the next frame is due */
} s;

static void show_message(const char *text)
{
    lv_label_set_text(s.msg, text);
    lv_obj_set_hidden(s.msg, false);
    lv_obj_set_hidden(s.img, true);
    lv_label_set_text(s.pos, "");
}

static void video_play_teardown(void);
static bool video_poster(void);

/* Load the current photo (or a clip's poster frame) and update labels. Display lock NOT held. */
static void show_current(void)
{
    if (s.playing) {
        video_play_teardown(); /* stepping away from a clip stops it */
    }
    if (s.count == 0) {
        if (display_lock(200)) {
            show_message(storage_status() == STORAGE_OK ? "No photos yet" : "No card");
            display_unlock();
        }
        return;
    }
    const int num = s.nums[s.index];
    char pos[40];
    if (storage_is_video(num)) {
        /* A clip: show its first frame as a still; a tap plays it. */
        snprintf(pos, sizeof(pos), "%u / %u   IMG_%04d.AVI", (unsigned)(s.count - s.index), (unsigned)s.count, num);
        const bool ok = video_poster();
        if (display_lock(200)) {
            lv_label_set_text(s.pos, pos);
            if (ok) {
                lv_obj_set_hidden(s.msg, true);
                lv_obj_set_hidden(s.img, false);
                lv_obj_invalidate(s.img);
            } else {
                lv_label_set_text(s.msg, "Can't show this clip");
                lv_obj_set_hidden(s.msg, false);
                lv_obj_set_hidden(s.img, true);
            }
            lv_label_set_text(s.hint, "press: play   hold: menu  2x: back");
            display_unlock();
        }
        return;
    }
    char path[64];
    storage_photo_path(num, path, sizeof(path));
    const esp_err_t err = jpeg_preview_load(path, s_px, s_dsc.header.w, s_dsc.header.h);
    snprintf(pos, sizeof(pos), "%u / %u   IMG_%04d", (unsigned)(s.count - s.index), (unsigned)s.count, num);
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
        lv_label_set_text(s.hint, "press: older  hold: menu  2x: back");
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

/* ---------- video playback ---------- */

#define VIDEO_PLAY_MS 125 /* 8 fps, matching the recording */

static void video_play_teardown(void)
{
    if (s.rd) {
        board_spi_lock();
        avi_reader_close(s.rd);
        board_spi_unlock();
        s.rd = NULL;
    }
    s.playing = false;
}

/* Decode one clip frame (a plain JPEG) into s_vdec and letterbox it onto the canvas. */
static bool video_show_frame(void)
{
    const size_t len = avi_reader_next(s.rd, s_vjpeg, 128 * 1024);
    if (!len) {
        return false; /* end of clip: the caller loops back to the start */
    }
    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_dec_handle_t dec = NULL;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        return false;
    }
    jpeg_dec_io_t io = {.inbuf = s_vjpeg, .inbuf_len = (int)len, .outbuf = s_vdec};
    jpeg_dec_header_info_t info;
    bool ok = false;
    if (jpeg_dec_parse_header(dec, &io, &info) == JPEG_ERR_OK && info.width > 0 && info.height > 0 &&
        (size_t)info.width * info.height <= 320 * 240 && jpeg_dec_process(dec, &io) == JPEG_ERR_OK) {
        /* Nearest-neighbour letterbox into the canvas. */
        const int out_w = display_width(), out_h = display_height();
        int dw = out_w, dh = (int)((long)info.height * out_w / info.width);
        if (dh > out_h) {
            dh = out_h;
            dw = (int)((long)info.width * out_h / info.height);
        }
        const int ox = (out_w - dw) / 2, oy = (out_h - dh) / 2;
        memset(s_px, 0, (size_t)out_w * out_h * 2);
        for (int y = 0; y < dh; y++) {
            uint16_t *row = s_px + (size_t)(oy + y) * out_w + ox;
            const int fy = y * info.height / dh;
            for (int x = 0; x < dw; x++) {
                row[x] = s_vdec[(size_t)fy * info.width + (size_t)x * info.width / dw];
            }
        }
        ok = true;
    }
    jpeg_dec_close(dec);
    return ok;
}

/* Show the clip's first frame as a still (the poster). Display lock NOT held. */
static bool video_poster(void)
{
    if (!s_vdec) {
        s_vdec = heap_caps_aligned_alloc(16, VIDEO_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    }
    if (!s_vjpeg) {
        s_vjpeg = heap_caps_malloc(128 * 1024, MALLOC_CAP_SPIRAM);
    }
    if (!s_vdec || !s_vjpeg) {
        return false;
    }
    char path[64];
    storage_clip_path(s.nums[s.index], path, sizeof(path));
    board_spi_lock();
    s.rd = avi_reader_open(path);
    board_spi_unlock();
    if (!s.rd) {
        return false;
    }
    const bool ok = video_show_frame();
    video_play_teardown();
    return ok;
}

static void video_play_start(void)
{
    if (s.count == 0 || !storage_is_video(s.nums[s.index])) {
        return;
    }
    if (!s_vdec) {
        s_vdec = heap_caps_aligned_alloc(16, VIDEO_FRAME_BYTES, MALLOC_CAP_SPIRAM);
    }
    if (!s_vjpeg) {
        s_vjpeg = heap_caps_malloc(128 * 1024, MALLOC_CAP_SPIRAM);
    }
    if (!s_vdec || !s_vjpeg) {
        return;
    }
    char path[64];
    storage_clip_path(s.nums[s.index], path, sizeof(path));
    board_spi_lock();
    s.rd = avi_reader_open(path);
    board_spi_unlock();
    if (!s.rd) {
        ui_toast("Can't play this clip", 1500);
        return;
    }
    s.playing = true;
    s.due_ms = 0; /* first frame now */
    if (display_lock(200)) {
        lv_label_set_text(s.hint, "press: stop");
        display_unlock();
    }
}

static void video_play_tick(void)
{
    if (!s.playing || esp_timer_get_time() / 1000 < s.due_ms) {
        return;
    }
    if (!video_show_frame()) {
        /* End of clip: loop back to the first frame. */
        char path[64];
        storage_clip_path(s.nums[s.index], path, sizeof(path));
        board_spi_lock();
        avi_reader_close(s.rd);
        s.rd = avi_reader_open(path);
        board_spi_unlock();
        if (!s.rd) {
            video_play_teardown();
            return;
        }
    }
    s.due_ms = esp_timer_get_time() / 1000 + VIDEO_PLAY_MS;
    if (display_lock(200)) {
        lv_obj_invalidate(s.img);
        display_unlock();
    }
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

void ui_gallery_reserve(void)
{
    if (!s_px) {
        s_px = heap_caps_calloc(1, PREVIEW_PIXELS * 2, MALLOC_CAP_SPIRAM);
        s_dsc.data = (const uint8_t *)s_px;
    }
    jpeg_preview_reserve();
}

static void enter(lv_obj_t *scr)
{
    ui_gallery_reserve();
    memset(&s, 0, sizeof(s));
    s.img = lv_image_create(scr);
    s_dsc.header.w = display_width(); /* the screen in its current orientation */
    s_dsc.header.h = display_height();
    s_dsc.header.stride = s_dsc.header.w * 2;
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
    video_play_teardown();
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
        if (s.playing) {
            video_play_teardown(); /* a tap stops the clip and shows its poster */
            show_current();
        } else if (s.count > 0 && storage_is_video(s.nums[s.index])) {
            video_play_start(); /* a tap on a clip plays it */
        } else if (s.count > 1) {
            s.index = s.index > 0 ? s.index - 1 : (int)s.count - 1;
            show_current();
        }
        break;
    case INPUT_LONG:
        if (s.playing) {
            video_play_teardown();
            show_current();
        }
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
    video_play_tick();
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
