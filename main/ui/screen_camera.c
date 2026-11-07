#include <math.h>
#include <stdio.h>
#include <string.h>

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
#include "power.h"
#include "settings.h"
#include "storage.h"
#include "ui.h"

#define VF_W CAMERA_PREVIEW_W
#define VF_H CAMERA_PREVIEW_H

static const char *TAG = "ui_cam";

static uint16_t *s_vf; /* what the image widget shows */
static lv_image_dsc_t s_dsc = {
    .header = {.magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565, .w = VF_W, .h = VF_H,
               .stride = VF_W * 2},
    .data_size = VF_W * VF_H * 2,
};

#define HINT_PHOTO "press: shoot   hold: menu"
#define HINT_ARMED "press: start   hold: menu"
#define HINT_RUN   "press: stop"
/* Consecutive failed frames before a time-lapse gives up. */
#define LAPSE_MAX_FAILS 3

typedef enum { LAPSE_OFF, LAPSE_ARMED, LAPSE_RUNNING } lapse_mode_t;

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
} s;

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
        s_vf = heap_caps_calloc(1, VF_W * VF_H * 2, MALLOC_CAP_SPIRAM);
        s_dsc.data = (const uint8_t *)s_vf;
    }
}

static void enter(lv_obj_t *scr)
{
    ui_camera_reserve();
    memset(&s, 0, sizeof(s));
    s.current = settings_look();
    s.lapse.interval = settings_lapse_interval();
    camera_power(true);
    camera_set_look(s.current);

    s.img = lv_image_create(scr);
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
    lv_obj_set_size(s.level, VF_W, VF_H);
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

static void leave(void)
{
    if (s.lapse.mode == LAPSE_RUNNING) {
        lapse_stop(NULL);
    }
    camera_power(false); /* nothing to show: don't run the sensor */
}

/* Horizon line through the centre, counter-rotated by the camera's roll so it stays
   level with the world; green within 1.5 degrees. */
static void level_step(void)
{
    if (!settings_level() || s.busy) {
        return;
    }
    const float deg = input_roll_deg();
    const float rad = -deg * 3.14159265f / 180.0f;
    const float half = 70.0f, cx = VF_W / 2.0f, cy = VF_H / 2.0f;
    const float dx = half * cosf(rad), dy = half * sinf(rad);
    s.level_pts[0] = (lv_point_precise_t){cx - dx, cy - dy};
    s.level_pts[1] = (lv_point_precise_t){cx + dx, cy + dy};
    const bool flat = fabsf(deg) < 1.5f;
    if (display_lock(50)) {
        lv_line_set_points(s.level, s.level_pts, 2);
        lv_obj_set_style_line_color(s.level, flat ? lv_color_hex(0x3ce05a) : UI_COLOR_FG, 0);
        lv_obj_set_style_line_opa(s.level, flat ? LV_OPA_COVER : LV_OPA_70, 0);
        display_unlock();
    }
}

/* Called between input polls: grab a viewfinder frame and show it. */
static void viewfinder_step(void)
{
    if (s.busy || s.frozen) {
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
    look_apply_rgb565(s.current, f, VF_W, VF_H);
    level_step();
    if (display_lock(50)) {
        memcpy(s_vf, f, VF_W * VF_H * 2);
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
        memcpy(s_vf, thumb, VF_W * VF_H * 2);
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
    int number = 0;
    const pipe_result_t r = pipeline_take_photo(s.current, on_captured, on_progress, NULL, &number);
    ESP_LOGI(TAG, "shot -> %s (IMG_%04d)", pipeline_result_text(r), number);
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
}

/* ---------- time-lapse ---------- */

static bool lapse_sensor_sleeps(void)
{
    return s.lapse.mode == LAPSE_RUNNING && lapse_interval_ms(s.lapse.interval) >= LAPSE_SENSOR_SLEEP_MS;
}

/* Badge and hint for the current mode. Display lock held. */
static void lapse_paint(void)
{
    char t[64];
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
    set_status("Time-lapse frame...");
    int number = 0;
    const pipe_result_t r = pipeline_take_photo(s.current, on_captured, NULL, NULL, &number);
    set_status(NULL);
    s.busy = false;
    s.next_status = 0;
    s.lapse.due_ms = lapse_next_due(s.lapse.due_ms, lapse_interval_ms(s.lapse.interval), now_ms());
    if (r == PIPE_OK) {
        s.lapse.frames++;
        s.lapse.fails = 0;
        ESP_LOGI(TAG, "time-lapse frame %u (%s/IMG_%04d)", (unsigned)s.lapse.frames, s.lapse.folder, number);
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
}

static void on_stop_confirm(int yes)
{
    if (yes) {
        lapse_stop(NULL);
    }
}

static void on_interval(int index)
{
    if (index < 0 || index >= LAPSE_INTERVAL_COUNT) {
        return;
    }
    settings_set_lapse_interval(index);
    s.lapse.interval = index;
    s.lapse.mode = LAPSE_ARMED;
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
    switch (index) {
    case 0:
        s.lapse.mode = LAPSE_OFF; /* plain photos */
        lapse_repaint();
        break;
    case 1: open_looks(); break;
    case 2: open_intervals(); break;
    case 3: ui_show(UI_SCREEN_GALLERY); break;
    case 4: ui_show(UI_SCREEN_SETTINGS); break;
    default: break;
    }
}

static void event(input_event_t e)
{
    if (s.busy) {
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
        if (s.lapse.mode == LAPSE_ARMED) {
            lapse_start();
        } else {
            shoot();
        }
        break;
    case INPUT_LONG: {
        static const char *const items[] = {"Camera", "Looks", "Time-lapse", "Gallery", "Settings"};
        ui_menu_open("Menu", items, 5, 1, on_menu);
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
    if (s.lapse.mode == LAPSE_RUNNING && !s.busy) {
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


const ui_screen_t ui_camera_screen = {
    .enter = enter,
    .leave = leave,
    .event = event,
    .tick = tick,
    .idle = viewfinder_step,
};
