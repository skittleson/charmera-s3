#include <math.h>
#include <stdio.h>
#include <string.h>

#include "avi_writer.h"
#include "board.h"
#include "camera.h"
#include "display.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "input.h"
#include "lapse_logic.h"
#include "looks.h"
#include "pipeline.h"
#include "orientation.h"
#include "power.h"
#include "remote_bridge.h"
#include "settings.h"
#include "storage.h"
#include "ui.h"

/* Viewfinder: 240x320 or 320x240 by orientation; the buffer is the same size. */
#define VF_BYTES (CAMERA_PREVIEW_PIXELS * 2)

static const char *TAG = "ui_cam";

static uint16_t *s_vf; /* what the image widget shows */
static lv_image_dsc_t s_dsc = {
    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565},
    .data_size = VF_BYTES,
};

#define HINT_PHOTO "press: shoot   hold: menu"
#define HINT_ARMED "press: start   hold: menu"
#define HINT_RUN   "press: stop"
/* Consecutive failed frames before a time-lapse gives up. */
#define LAPSE_MAX_FAILS 3

/* Video: 320x240 @ 8 fps, up to 10 s or ~2 MB, whichever comes first. */
#define VIDEO_FPS       8
#define VIDEO_FRAME_MS  (1000 / VIDEO_FPS)
#define VIDEO_MAX_MS    (10 * 1000)
#define VIDEO_MAX_BYTES (2u * 1024 * 1024)

typedef enum { LAPSE_OFF, LAPSE_ARMED, LAPSE_RUNNING } lapse_mode_t;
typedef enum { VIDEO_OFF, VIDEO_ARMED, VIDEO_RECORDING } video_mode_t;

static struct {
    lv_obj_t *img, *flash, *look, *remaining, *battery, *banner, *status, *hint, *level, *badge;
    lv_point_precise_t level_pts[2];
    look_t current;
    int64_t look_until;
    int64_t next_status;
    bool busy;
    bool frozen;
    struct {
        lapse_mode_t mode;
        int interval; /* index, see lapse_logic.h */
        uint32_t frames;
        int fails;
        int64_t start_ms, due_ms;
        char folder[9];
    } lapse;
    struct {
        video_mode_t mode;
        FILE *f;
        avi_writer_t w;
        uint8_t *jpeg; /* one encoded frame (PSRAM) */
        int number;
        uint32_t frames, bytes;
        int fails;
        int64_t start_ms, due_ms;
    } video;
} s;

/* Last saved file, for the Bluetooth status; survives screen changes. */
static char s_last[24];

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void set_look_label(void)
{
    lv_label_set_text(s.look, look_name(s.current));
    lv_obj_set_hidden(s.look, false);
    s.look_until = esp_timer_get_time() + 1500 * 1000;
}

static const char *battery_symbol(float v)
{
    if (v >= 4.5f) {
        return LV_SYMBOL_USB;
    }
    if (v >= 4.0f) {
        return LV_SYMBOL_BATTERY_FULL;
    }
    if (v >= 3.8f) {
        return LV_SYMBOL_BATTERY_3;
    }
    if (v >= 3.6f) {
        return LV_SYMBOL_BATTERY_2;
    }
    if (v >= 3.4f) {
        return LV_SYMBOL_BATTERY_1;
    }
    return LV_SYMBOL_BATTERY_EMPTY;
}

/* Refresh card state, remaining count and battery. Display lock NOT held. */
static void refresh_status(void)
{
    const storage_state_t st = storage_status();
    char rem[16] = "";
    const char *banner = NULL;
    if (st != STORAGE_OK) {
        banner = "No card";
    } else if (!storage_numbers_left()) {
        banner = "Card full (photo numbers used up)";
    } else {
        const uint32_t r = storage_photos_remaining_now();
        if (r == 0) {
            banner = "Card full";
        }
        snprintf(rem, sizeof(rem), "%u", (unsigned)r);
    }
    const char *bat = battery_symbol(board_battery_volts());
    if (display_lock(200)) {
        lv_label_set_text(s.remaining, rem);
        lv_label_set_text(s.battery, bat);
        if (banner) {
            lv_label_set_text(s.banner, banner);
            lv_obj_set_hidden(s.banner, false);
        } else {
            lv_obj_set_hidden(s.banner, true);
        }
        display_unlock();
    }
}

static lv_obj_t *overlay_label(lv_obj_t *parent, const lv_font_t *font)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_color(l, UI_COLOR_FG, 0);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_bg_color(l, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(l, LV_OPA_50, 0);
    lv_obj_set_style_pad_hor(l, 6, 0);
    lv_obj_set_style_pad_ver(l, 2, 0);
    lv_obj_set_style_radius(l, 4, 0);
    return l;
}

void ui_camera_reserve(void)
{
    if (!s_vf) {
        s_vf = heap_caps_calloc(1, VF_BYTES, MALLOC_CAP_SPIRAM);
        s_dsc.data = (const uint8_t *)s_vf;
    }
}

static void enter(lv_obj_t *scr)
{
    ui_camera_reserve();
    const bool turning = ui_is_relayout();
    const __typeof__(s.lapse) keep_lapse = s.lapse;
    const __typeof__(s.video) keep_video = s.video;
    memset(&s, 0, sizeof(s));
    if (turning) {
        s.lapse = keep_lapse; /* a rebuild for the new orientation keeps the mode */
        s.video = keep_video; /* and a recording clip keeps going (its FILE* stays open) */
    }
    s.current = settings_look(); /* every look change is saved, so this survives a turn */
    s.lapse.interval = settings_lapse_interval();
    camera_power(true);
    camera_set_look(s.current);

    s.img = lv_image_create(scr);
    s_dsc.header.w = camera_preview_w();
    s_dsc.header.h = camera_preview_h();
    s_dsc.header.stride = s_dsc.header.w * 2;
    lv_image_set_src(s.img, &s_dsc);
    lv_obj_center(s.img);

    s.battery = overlay_label(scr, &lv_font_montserrat_14);
    lv_obj_align(s.battery, LV_ALIGN_TOP_LEFT, 4, 4);
    s.remaining = overlay_label(scr, &lv_font_montserrat_14);
    lv_obj_align(s.remaining, LV_ALIGN_TOP_RIGHT, -4, 4);

    s.look = overlay_label(scr, &lv_font_montserrat_20);
    lv_obj_align(s.look, LV_ALIGN_TOP_MID, 0, 30);

    s.banner = overlay_label(scr, &lv_font_montserrat_20);
    lv_obj_set_style_text_color(s.banner, UI_COLOR_ACCENT, 0);
    lv_obj_align(s.banner, LV_ALIGN_CENTER, 0, -40);
    lv_obj_set_hidden(s.banner, true);

    s.status = overlay_label(scr, &lv_font_montserrat_20);
    lv_obj_align(s.status, LV_ALIGN_BOTTOM_MID, 0, -34);
    lv_obj_set_hidden(s.status, true);

    s.hint = overlay_label(scr, &lv_font_montserrat_14);
    lv_label_set_text(s.hint, HINT_PHOTO);
    lv_obj_align(s.hint, LV_ALIGN_BOTTOM_MID, 0, -4);

    s.badge = overlay_label(scr, &lv_font_montserrat_14);
    lv_obj_set_style_text_color(s.badge, lv_color_hex(0xff5a4a), 0);
    lv_obj_align(s.badge, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_set_hidden(s.badge, true);

    s.level = lv_line_create(scr);
    lv_obj_set_style_line_width(s.level, 3, 0);
    lv_obj_set_style_line_rounded(s.level, true, 0);
    lv_obj_set_size(s.level, display_width(), display_height());
    lv_obj_set_pos(s.level, 0, 0);
    if (!settings_level()) {
        lv_obj_set_hidden(s.level, true);
    }

    s.flash = lv_obj_create(scr);
    lv_obj_set_size(s.flash, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s.flash, lv_color_white(), 0);
    lv_obj_set_style_border_width(s.flash, 0, 0);
    lv_obj_set_style_radius(s.flash, 0, 0);
    lv_obj_set_hidden(s.flash, true);

    set_look_label();
}

static void lapse_stop(const char *why);
static void video_stop(const char *why);

static void leave(void)
{
    if (ui_is_relayout()) {
        return; /* just turning: keep the mode, the armed time-lapse and the sensor on */
    }
    if (s.video.mode == VIDEO_RECORDING) {
        video_stop(NULL); /* leaving the screen saves the clip */
    }
    s.video.mode = VIDEO_OFF;
    if (s.lapse.mode == LAPSE_RUNNING) {
        lapse_stop(NULL);
    }
    s.lapse.mode = LAPSE_OFF; /* the camera screen always opens in single shot */
    camera_power(false); /* nothing to show: don't run the sensor */
}

/* Horizon line through the centre, counter-rotated by the camera's roll so it stays
   level with the world; green within 1.5 degrees. */
static void level_step(void)
{
    if (!settings_level() || s.busy) {
        return;
    }
    float down, side, face;
    input_gravity(&down, &side, &face);
    /* Lying flat or aimed straight up/down: a left/right tilt has no meaning. */
    const bool lying = fabsf(face) > 0.7f * sqrtf(down * down + side * side + face * face);
    const float deg = lying ? 0.0f : orient_roll_deg(orientation_current(), down, side);
    const float rad = -deg * 3.14159265f / 180.0f;
    const float half = 70.0f, cx = display_width() / 2.0f, cy = display_height() / 2.0f;
    const float dx = half * cosf(rad), dy = half * sinf(rad);
    const bool flat = fabsf(deg) < 1.5f;
    if (display_lock(50)) {
        /* Points are set under the lock: LVGL's flush task reads them during refresh. */
        s.level_pts[0] = (lv_point_precise_t){cx - dx, cy - dy};
        s.level_pts[1] = (lv_point_precise_t){cx + dx, cy + dy};
        lv_line_set_points(s.level, s.level_pts, 2);
        lv_obj_set_style_line_color(s.level, flat ? lv_color_hex(0x3ce05a) : UI_COLOR_FG, 0);
        lv_obj_set_style_line_opa(s.level, flat ? LV_OPA_COVER : LV_OPA_70, 0);
        display_unlock();
    }
}

/* Called between input polls: grab a viewfinder frame and show it. */
static void viewfinder_step(void)
{
    if (s.busy || s.frozen || ui_switch_pending()) {
        vTaskDelay(pdMS_TO_TICKS(20)); /* nothing to grab (or about to rebuild): don't spin */
        return;
    }
    uint16_t *f = camera_preview_frame();
    static int fps_n;
    static int64_t fps_t;
    if (f && ++fps_n == 30) {
        const int64_t now = esp_timer_get_time();
        ESP_LOGD(TAG, "viewfinder %.1f fps", 30e6 / (double)(now - fps_t));
        fps_t = now;
        fps_n = 0;
    }
    if (!f) {
        vTaskDelay(pdMS_TO_TICKS(20)); /* no camera: don't spin the UI task */
        return;
    }
    look_apply_rgb565(s.current, f, camera_preview_w(), camera_preview_h());
    level_step();
    if (display_lock(50)) {
        memcpy(s_vf, f, VF_BYTES);
        lv_obj_invalidate(s.img);
        display_unlock();
    }
}

static void set_status(const char *text)
{
    if (display_lock(200)) {
        if (text) {
            lv_label_set_text(s.status, text);
            lv_obj_set_hidden(s.status, false);
        } else {
            lv_obj_set_hidden(s.status, true);
        }
        display_unlock();
    }
}

static void on_captured(const uint16_t *thumb, void *ctx)
{
    if (display_lock(200)) {
        memcpy(s_vf, thumb, VF_BYTES);
        lv_obj_invalidate(s.img);
        lv_label_set_text(s.status, "Saving...");
        display_unlock();
    }
}

static void on_progress(int pct, void *ctx)
{
    char t[24];
    snprintf(t, sizeof(t), "Saving... %d%%", pct);
    set_status(t);
}

static void shoot(void)
{
    const storage_state_t st = storage_status();
    if (st != STORAGE_OK) {
        ui_toast("No card", 1500);
        return;
    }
    if (!storage_numbers_left() || storage_photos_remaining_now() == 0) {
        ui_toast("Card full", 1500);
        return;
    }
    s.busy = true;
    remote_bridge_publish();
    /* Shutter feedback right away: flash, then hold the last frame while the sensor
       switches to full resolution. */
    if (display_lock(200)) {
        lv_obj_set_hidden(s.flash, false);
        lv_obj_set_hidden(s.hint, true);
        display_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(120));
    if (display_lock(200)) {
        lv_obj_set_hidden(s.flash, true);
        lv_label_set_text(s.status, "Hold still...");
        lv_obj_set_hidden(s.status, false);
        display_unlock();
    }
    char desc[96], comment[160];
    snprintf(desc, sizeof(desc), "Charmera-S3 photo, look %s", look_name(s.current));
    snprintf(comment, sizeof(comment), "mode=single; look=%s; camera=%s; orientation=%s", look_name(s.current),
             camera_sensor_name(), orient_name(orientation_current()));
    const pipe_meta_t meta = {desc, comment};
    int number = 0;
    const pipe_result_t r = pipeline_take_photo(s.current, &meta, on_captured, on_progress, NULL, &number);
    ESP_LOGI(TAG, "shot -> %s (IMG_%04d)", pipeline_result_text(r), number);
    if (r == PIPE_OK) {
        snprintf(s_last, sizeof(s_last), "IMG_%04d", number);
    }
    set_status(NULL);
    ui_toast(pipeline_result_text(r), r == PIPE_OK ? 1000 : 2500);
    if (display_lock(200)) {
        lv_obj_set_hidden(s.hint, false);
        display_unlock();
    }
    /* Presses and tilts made while saving are dropped, not replayed. */
    input_flush();
    ui_note_activity(); /* idle time counts from the end of the save */
    s.busy = false;
    s.next_status = 0;
    remote_bridge_publish();
}

/* ---------- time-lapse ---------- */

static bool lapse_sensor_sleeps(void)
{
    return s.lapse.mode == LAPSE_RUNNING && lapse_interval_ms(s.lapse.interval) >= LAPSE_SENSOR_SLEEP_MS;
}

/* Badge and hint for the current mode (time-lapse or video). Display lock held. */
static void lapse_paint(void)
{
    char t[64];
    if (s.video.mode == VIDEO_RECORDING) {
        const int64_t now = now_ms();
        char run[16];
        lapse_format_duration((uint32_t)((now - s.video.start_ms) / 1000), run, sizeof(run));
        snprintf(t, sizeof(t), "REC  %s  %u kB", run, (unsigned)(s.video.bytes / 1024));
        lv_label_set_text(s.hint, HINT_RUN);
        lv_label_set_text(s.badge, t);
        lv_obj_set_hidden(s.badge, false);
        return;
    }
    if (s.video.mode == VIDEO_ARMED) {
        snprintf(t, sizeof(t), "VIDEO  %d fps", VIDEO_FPS);
        lv_label_set_text(s.hint, HINT_ARMED);
        lv_label_set_text(s.badge, t);
        lv_obj_set_hidden(s.badge, false);
        return;
    }
    switch (s.lapse.mode) {
    case LAPSE_OFF:
        lv_obj_set_hidden(s.badge, true);
        lv_label_set_text(s.hint, HINT_PHOTO);
        return;
    case LAPSE_ARMED:
        snprintf(t, sizeof(t), "TIME-LAPSE  every %s", lapse_interval_name(s.lapse.interval));
        lv_label_set_text(s.hint, HINT_ARMED);
        break;
    case LAPSE_RUNNING: {
        const int64_t now = now_ms();
        char run[16], next[16];
        lapse_format_duration((uint32_t)((now - s.lapse.start_ms) / 1000), run, sizeof(run));
        const int64_t wait = s.lapse.due_ms > now ? (s.lapse.due_ms - now + 999) / 1000 : 0;
        lapse_format_duration((uint32_t)wait, next, sizeof(next));
        snprintf(t, sizeof(t), "REC  %u frames  %s  next %s", (unsigned)s.lapse.frames, run, next);
        lv_label_set_text(s.hint, HINT_RUN);
        break;
    }
    }
    lv_label_set_text(s.badge, t);
    lv_obj_set_hidden(s.badge, false);
}

static void lapse_repaint(void)
{
    if (display_lock(200)) {
        lapse_paint();
        display_unlock();
    }
}

static void lapse_start(void)
{
    if (storage_status() != STORAGE_OK) {
        ui_toast("No card", 1500);
        return;
    }
    if (storage_photos_remaining_now() == 0) {
        ui_toast("Card full", 1500);
        return;
    }
    if (storage_lapse_begin(s.lapse.folder, sizeof(s.lapse.folder)) != ESP_OK) {
        ui_toast("Can't start time-lapse", 2500);
        return;
    }
    s.lapse.mode = LAPSE_RUNNING;
    s.lapse.frames = 0;
    s.lapse.fails = 0;
    s.lapse.start_ms = s.lapse.due_ms = now_ms(); /* first frame on the next tick */
    ESP_LOGI(TAG, "time-lapse start: every %s into %s", lapse_interval_name(s.lapse.interval), s.lapse.folder);
    lapse_repaint();
}

/* End the run. why == NULL: stopped by the user (or by leaving the screen). */
static void lapse_stop(const char *why)
{
    if (s.lapse.mode != LAPSE_RUNNING) {
        return;
    }
    storage_lapse_end();
    s.lapse.mode = LAPSE_ARMED; /* stay ready for another run */
    ESP_LOGI(TAG, "time-lapse stop (%s): %u frames in %s", why ? why : "user", (unsigned)s.lapse.frames,
             s.lapse.folder);
    char t[80];
    snprintf(t, sizeof(t), "%s%s%u frames saved\nin %s", why ? why : "", why ? "\n" : "",
             (unsigned)s.lapse.frames, s.lapse.folder);
    ui_toast(t, 3000);
    s.frozen = false;
    if (!power_is_asleep()) {
        camera_power(true); /* the sensor may have been sleeping between frames */
    }
    lapse_repaint();
}

static void lapse_frame(void)
{
    const bool asleep = power_is_asleep();
    camera_power(true);
    s.busy = true;
    remote_bridge_publish();
    set_status("Time-lapse frame...");
    const unsigned frame = (unsigned)s.lapse.frames + 1;
    const unsigned every_s = (unsigned)(lapse_interval_ms(s.lapse.interval) / 1000);
    char desc[96], comment[200];
    snprintf(desc, sizeof(desc), "Time-lapse %s frame %u, every %s, look %s", s.lapse.folder, frame,
             lapse_interval_name(s.lapse.interval), look_name(s.current));
    snprintf(comment, sizeof(comment),
             "mode=timelapse; folder=%s; frame=%u; interval_s=%u; elapsed_s=%u; look=%s; camera=%s; orientation=%s",
             s.lapse.folder, frame, every_s, (unsigned)((now_ms() - s.lapse.start_ms) / 1000), look_name(s.current),
             camera_sensor_name(), orient_name(orientation_current()));
    const pipe_meta_t meta = {desc, comment};
    int number = 0;
    const pipe_result_t r = pipeline_take_photo(s.current, &meta, on_captured, NULL, NULL, &number);
    set_status(NULL);
    s.busy = false;
    s.next_status = 0;
    s.lapse.due_ms = lapse_next_due(s.lapse.due_ms, lapse_interval_ms(s.lapse.interval), now_ms());
    if (r == PIPE_OK) {
        s.lapse.frames++;
        s.lapse.fails = 0;
        ESP_LOGI(TAG, "time-lapse frame %u (%s/IMG_%04d)", (unsigned)s.lapse.frames, s.lapse.folder, number);
        snprintf(s_last, sizeof(s_last), "%s/IMG_%04d", s.lapse.folder, number);
    } else {
        ESP_LOGW(TAG, "time-lapse frame failed: %s", pipeline_result_text(r));
        const bool fatal = r == PIPE_NO_CARD || r == PIPE_CARD_FULL || r == PIPE_WRITE_FAILED;
        if (fatal || ++s.lapse.fails >= LAPSE_MAX_FAILS) {
            lapse_stop(pipeline_result_text(r));
            return;
        }
    }
    if (asleep || lapse_sensor_sleeps()) {
        camera_power(false); /* keep the last frame on screen; the sensor runs hot */
        s.frozen = true;
    }
    lapse_repaint();
    remote_bridge_publish();
}

/* ---------- video ---------- */

#define VIDEO_JPEG_CAP (128 * 1024)

/* Select video mode: one clip per shutter, armed and ready. Clears the other modes. */
static void select_video(void)
{
    s.lapse.mode = LAPSE_OFF;
    s.video.mode = VIDEO_ARMED;
    lapse_repaint();
}

static void video_start(void)
{
    if (storage_status() != STORAGE_OK) {
        ui_toast("No card", 1500);
        return;
    }
    if (!storage_numbers_left() || storage_photos_remaining_now() == 0) {
        ui_toast("Card full", 1500);
        return;
    }
    int number = 0;
    FILE *f = storage_video_begin(&number);
    if (!f) {
        ui_toast("Can't start video", 2500);
        return;
    }
    if (!s.video.jpeg) {
        s.video.jpeg = heap_caps_malloc(VIDEO_JPEG_CAP, MALLOC_CAP_SPIRAM);
    }
    const int w = camera_preview_w(), h = camera_preview_h();
    if (!s.video.jpeg || !avi_writer_open(&s.video.w, f, w, h)) {
        fclose(f);
        storage_video_abort(number);
        ui_toast("Can't start video", 2500);
        return;
    }
    s.video.mode = VIDEO_RECORDING;
    s.video.number = number;
    s.video.frames = 0;
    s.video.bytes = 0;
    s.video.fails = 0;
    s.video.start_ms = s.video.due_ms = now_ms(); /* first frame on the next tick */
    ESP_LOGI(TAG, "video start: %dx%d @%d fps into IMG_%04d.AVI", w, h, VIDEO_FPS, number);
    lapse_repaint();
}

/* End the clip. why == NULL: stopped by the user or the cap (or by leaving the screen). */
static void video_stop(const char *why)
{
    if (s.video.mode != VIDEO_RECORDING) {
        return;
    }
    const bool ok = avi_writer_close(&s.video.w); /* patches the header and closes the file */
    s.video.f = NULL;
    if (ok && storage_video_commit(s.video.number) == ESP_OK) {
        snprintf(s_last, sizeof(s_last), "IMG_%04d", s.video.number);
        char t[80];
        snprintf(t, sizeof(t), "%s%u frames saved\nIMG_%04d.AVI", why ? why : "", (unsigned)s.video.frames,
                 s.video.number);
        ui_toast(t, 3000);
    } else {
        storage_video_abort(s.video.number); /* a failed clip leaves no gallery entry */
        ui_toast(why ? why : "Clip discarded", 2500);
    }
    s.video.mode = VIDEO_ARMED; /* stay ready for another clip */
    lapse_repaint();
}

static void video_frame(void)
{
    s.busy = true;
    remote_bridge_publish();
    uint16_t *f = camera_preview_frame();
    if (!f) {
        s.busy = false;
        if (++s.video.fails >= LAPSE_MAX_FAILS) {
            video_stop("camera error");
        }
        return;
    }
    const size_t len = pipeline_encode_video_jpeg(f, camera_preview_w(), camera_preview_h(), s.current,
                                                  s.video.jpeg, VIDEO_JPEG_CAP);
    if (!len) {
        s.busy = false;
        ESP_LOGW(TAG, "video frame encode failed");
        if (++s.video.fails >= LAPSE_MAX_FAILS) {
            video_stop("encode failed");
        }
        return;
    }
    if (!avi_writer_frame(&s.video.w, s.video.jpeg, len)) {
        s.busy = false;
        video_stop("write failed");
        return;
    }
    s.video.frames++;
    s.video.bytes += (uint32_t)len;
    s.video.fails = 0;
    s.busy = false;
    const int64_t elapsed = now_ms() - s.video.start_ms;
    if (elapsed >= VIDEO_MAX_MS || s.video.bytes >= VIDEO_MAX_BYTES) {
        video_stop(NULL); /* hit the cap: save what we have */
        return;
    }
    s.video.due_ms = now_ms() + VIDEO_FRAME_MS;
    lapse_repaint();
    remote_bridge_publish();
}

static void on_video_stop_confirm(int yes)
{
    if (yes) {
        video_stop(NULL);
    }
}

static void on_stop_confirm(int yes)
{
    if (yes) {
        lapse_stop(NULL);
    }
}

static void on_interval(int index)
{
    if (index < 0 || index >= LAPSE_INTERVAL_COUNT || s.lapse.mode == LAPSE_RUNNING) {
        return;
    }
    settings_set_lapse_interval(index);
    s.lapse.interval = index;
    s.lapse.mode = LAPSE_ARMED;
    s.video.mode = VIDEO_OFF; /* the modes are mutually exclusive */
    lapse_repaint();
}

static void open_intervals(void)
{
    static const char *names[LAPSE_INTERVAL_COUNT];
    for (int i = 0; i < LAPSE_INTERVAL_COUNT; i++) {
        names[i] = lapse_interval_name(i);
    }
    ui_menu_open("Time-lapse: one photo every", names, LAPSE_INTERVAL_COUNT, settings_lapse_interval(),
                 on_interval);
}

static void on_look(int index)
{
    if (index < 0 || index >= LOOK_COUNT) {
        return;
    }
    s.current = (look_t)index;
    camera_set_look(s.current);
    settings_set_look(s.current);
    if (display_lock(200)) {
        set_look_label();
        display_unlock();
    }
}

static void open_looks(void)
{
    static const char *names[LOOK_COUNT];
    for (int i = 0; i < LOOK_COUNT; i++) {
        names[i] = look_name((look_t)i);
    }
    ui_menu_open("Looks", names, LOOK_COUNT, s.current, on_look);
}

static void on_menu(int index)
{
    if (s.lapse.mode == LAPSE_RUNNING || s.video.mode == VIDEO_RECORDING) {
        return; /* a run started (remotely) while the menu was open */
    }
    switch (index) {
    case 0:
        s.lapse.mode = LAPSE_OFF;
        s.video.mode = VIDEO_OFF; /* plain photos */
        lapse_repaint();
        break;
    case 1: open_looks(); break;
    case 2: open_intervals(); break;
    case 3: select_video(); break;
    case 4: ui_show(UI_SCREEN_GALLERY); break;
    case 5: ui_show(UI_SCREEN_SETTINGS); break;
    default: break;
    }
}

static void event(input_event_t e)
{
    if (s.busy) {
        return;
    }
    if (s.video.mode == VIDEO_RECORDING) {
        /* A bump shouldn't end a clip: always ask, Cancel first. */
        if (e == INPUT_SHORT || e == INPUT_LONG) {
            ui_confirm("Stop video?", "Stop", on_video_stop_confirm);
        }
        return;
    }
    if (s.lapse.mode == LAPSE_RUNNING) {
        /* A bump shouldn't end a long run: always ask, Cancel first. */
        if (e == INPUT_SHORT || e == INPUT_LONG) {
            ui_confirm("Stop time-lapse?", "Stop", on_stop_confirm);
        }
        return;
    }
    switch (e) {
    case INPUT_SHORT:
        if (s.video.mode == VIDEO_ARMED) {
            video_start();
        } else if (s.lapse.mode == LAPSE_ARMED) {
            lapse_start();
        } else {
            shoot();
        }
        break;
    case INPUT_LONG: {
        static const char *const items[] = {"Camera", "Looks", "Time-lapse", "Video", "Gallery", "Settings"};
        ui_menu_open("Menu", items, 6, 1, on_menu);
        break;
    }
    default:
        break;
    }
}

static void tick(void)
{
    const int64_t now = esp_timer_get_time();
    if (s.look_until && now > s.look_until) {
        s.look_until = 0;
        if (display_lock(200)) {
            lv_obj_set_hidden(s.look, true);
            display_unlock();
        }
    }
    if (!s.busy && now >= s.next_status) {
        s.next_status = now + 5 * 1000 * 1000;
        refresh_status();
    }
    /* Auto orientation: only here on the camera screen, and never mid-save, during a
       time-lapse run (its frames must match), with a menu open or the screen asleep. */
    orientation_auto_poll(s.busy || s.lapse.mode == LAPSE_RUNNING || s.video.mode == VIDEO_RECORDING ||
                          ui_menu_is_open() || power_is_asleep());
    if (s.video.mode == VIDEO_RECORDING && !s.busy) {
        if (now_ms() >= s.video.due_ms) {
            video_frame(); /* keeps recording while the screen sleeps or a dialog is open */
        } else {
            static int64_t next_paint;
            if (now >= next_paint) {
                next_paint = now + 1000 * 1000;
                lapse_repaint();
            }
        }
    } else if (s.lapse.mode == LAPSE_RUNNING && !s.busy) {
        if (now_ms() >= s.lapse.due_ms) {
            lapse_frame(); /* keeps shooting while the screen sleeps or a dialog is open */
        } else {
            if (lapse_sensor_sleeps() && camera_is_on()) {
                camera_power(false); /* woken screen turned it back on */
                s.frozen = true;
            }
            static int64_t next_paint;
            if (now >= next_paint) {
                next_paint = now + 1000 * 1000;
                lapse_repaint();
            }
        }
    }
}

void ui_camera_freeze(bool on)
{
    s.frozen = on;
}


/* ---------- Bluetooth remote ---------- */

const char *ui_camera_remote(const remote_cmd_t *c)
{
    const remote_run_t run = s.video.mode == VIDEO_RECORDING ? RUN_VIDEO
                        : s.lapse.mode == LAPSE_RUNNING ? RUN_LAPSE : RUN_NONE;
    const char *err = remote_check(c, run);
    if (err) {
        return err;
    }
    switch (c->kind) {
    case RC_LIST: /* file service: handled by the bridge, never reaches here */
    case RC_THUMB:
    case RC_GET:
    case RC_USBDISK: /* drive mode and format: handled by the bridge */
    case RC_FORMAT:
        return NULL;
    case RC_LOOK:
        on_look(c->arg);
        break;
    case RC_MODE_SINGLE:
        s.lapse.mode = LAPSE_OFF;
        s.video.mode = VIDEO_OFF;
        lapse_repaint();
        break;
    case RC_MODE_LAPSE:
        on_interval(c->arg >= 0 ? c->arg : s.lapse.interval);
        break;
    case RC_MODE_VIDEO:
        select_video();
        break;
    case RC_INTERVAL:
        settings_set_lapse_interval(c->arg);
        s.lapse.interval = c->arg;
        lapse_repaint();
        break;
    case RC_SHOOT:
        if (s.video.mode == VIDEO_ARMED) {
            video_start(); /* the shutter starts a clip in video mode */
            return s.video.mode == VIDEO_RECORDING ? NULL : "couldn't start";
        }
        if (storage_status() != STORAGE_OK) {
            return "no card";
        }
        if (!storage_numbers_left() || storage_photos_remaining_now() == 0) {
            return "card full";
        }
        shoot(); /* one ordinary photo, whatever the mode */
        break;
    case RC_LAPSE_START:
        if (storage_status() != STORAGE_OK) {
            return "no card";
        }
        if (storage_photos_remaining_now() == 0) {
            return "card full";
        }
        if (s.lapse.mode == LAPSE_OFF) {
            on_interval(s.lapse.interval);
        }
        lapse_start();
        if (s.lapse.mode != LAPSE_RUNNING) {
            return "couldn't start";
        }
        break;
    case RC_LAPSE_STOP:
        if (s.video.mode == VIDEO_RECORDING) {
            video_stop(NULL); /* "lapse stop" is the generic stop: it ends a clip too */
        } else {
            lapse_stop(NULL); /* the page asked for confirmation already */
        }
        break;
    case RC_TIME:
        break; /* handled by the bridge */
    case RC_ORIENTATION:
        settings_set_orientation(c->arg);
        orientation_setting_changed(); /* rebuilds this screen if the grip changes */
        break;
    }
    return NULL;
}

void ui_camera_fill_status(remote_status_t *st)
{
    st->look = settings_look();
    st->look_name = look_name(settings_look());
    st->lapse_mode = s.lapse.mode != LAPSE_OFF;
    st->video_mode = s.video.mode != VIDEO_OFF;
    st->interval = settings_lapse_interval();
    st->interval_name = lapse_interval_name(st->interval);
    st->lapse_running = s.lapse.mode == LAPSE_RUNNING;
    st->video_recording = s.video.mode == VIDEO_RECORDING;
    if (st->video_recording) {
        st->frames = s.video.frames;
        st->elapsed_s = (uint32_t)((now_ms() - s.video.start_ms) / 1000);
    } else if (st->lapse_running) {
        st->frames = s.lapse.frames;
        st->elapsed_s = (uint32_t)((now_ms() - s.lapse.start_ms) / 1000);
    }
    strlcpy(st->folder, s.lapse.folder, sizeof(st->folder));
    st->busy = s.busy;
    strlcpy(st->last, s_last, sizeof(st->last));
    st->orientation = orient_name(orientation_current());
    st->orientation_setting = orient_name(settings_orientation());
}

const ui_screen_t ui_camera_screen = {
    .enter = enter,
    .leave = leave,
    .event = event,
    .tick = tick,
    .idle = viewfinder_step,
};
