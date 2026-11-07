#include "ui.h"

#include <string.h>

#include "display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "camera.h"
#include "power.h"
#include "remote_bridge.h"

static const char *TAG = "ui";

static const ui_screen_t *const SCREENS[] = {
    [UI_SCREEN_CAMERA] = &ui_camera_screen,
    [UI_SCREEN_GALLERY] = &ui_gallery_screen,
    [UI_SCREEN_SETTINGS] = &ui_settings_screen,
};

static const ui_screen_t *s_current;
static volatile int s_pending = -1;

/* ---------- toast ---------- */

static lv_obj_t *s_toast;
static int64_t s_toast_until;

void ui_toast(const char *text, int ms)
{
    const bool locked = display_lock(200);
    if (!s_toast) {
        s_toast = lv_label_create(lv_layer_top());
        lv_obj_set_style_bg_color(s_toast, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_toast, LV_OPA_80, 0);
        lv_obj_set_style_text_color(s_toast, UI_COLOR_FG, 0);
        lv_obj_set_style_text_font(s_toast, &lv_font_montserrat_20, 0);
        lv_obj_set_style_pad_all(s_toast, 10, 0);
        lv_obj_set_style_radius(s_toast, 8, 0);
        lv_obj_set_style_text_align(s_toast, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_label_set_text(s_toast, text);
    lv_obj_set_hidden(s_toast, false);
    lv_obj_align(s_toast, LV_ALIGN_CENTER, 0, 0);
    s_toast_until = esp_timer_get_time() + (int64_t)ms * 1000;
    if (locked) {
        display_unlock();
    }
}

static void toast_tick(void)
{
    if (s_toast && s_toast_until && esp_timer_get_time() > s_toast_until) {
        s_toast_until = 0;
        if (display_lock(200)) {
            lv_obj_set_hidden(s_toast, true);
            display_unlock();
        }
    }
}

/* ---------- menu ---------- */

#define MENU_MAX 16

static struct {
    lv_obj_t *box;
    lv_obj_t *rows[MENU_MAX];
    int count, sel;
    ui_menu_cb cb;
    bool confirm; /* confirm dialogs report 1/0 instead of an index */
} s_menu;

bool ui_menu_is_open(void)
{
    return s_menu.box != NULL;
}

void ui_menu_dismiss(void)
{
    if (s_menu.box && display_lock(200)) {
        lv_obj_delete(s_menu.box);
        display_unlock();
    }
    memset(&s_menu, 0, sizeof(s_menu));
}

static void menu_paint(void)
{
    for (int i = 0; i < s_menu.count; i++) {
        const bool on = i == s_menu.sel;
        lv_obj_set_style_bg_color(s_menu.rows[i], on ? UI_COLOR_ACCENT : lv_color_hex(0x2a2a2a), 0);
        lv_obj_set_style_text_color(s_menu.rows[i], on ? lv_color_black() : UI_COLOR_FG, 0);
        if (on) {
            lv_obj_scroll_to_view(s_menu.rows[i], LV_ANIM_OFF);
        }
    }
}

static void menu_close(int result)
{
    ui_menu_cb cb = s_menu.cb;
    const bool confirm = s_menu.confirm;
    if (display_lock(200)) {
        lv_obj_delete(s_menu.box);
        display_unlock();
    }
    memset(&s_menu, 0, sizeof(s_menu));
    if (cb) {
        cb(confirm ? (result == 1 ? 1 : 0) : result);
    }
}

void ui_menu_open(const char *title, const char *const *items, int count, int initial, ui_menu_cb cb)
{
    if (count > MENU_MAX) {
        count = MENU_MAX;
    }
    const bool locked = display_lock(200);
    if (s_menu.box) {
        lv_obj_delete(s_menu.box);
    }
    memset(&s_menu, 0, sizeof(s_menu));
    lv_obj_t *box = lv_obj_create(lv_layer_top());
    lv_obj_set_size(box, 220, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(box, 300, 0);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x161616), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_90, 0);
    lv_obj_set_style_border_color(box, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_pad_all(box, 8, 0);
    lv_obj_set_style_pad_row(box, 6, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    if (title) {
        lv_obj_t *t = lv_label_create(box);
        lv_label_set_text(t, title);
        lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(t, lv_pct(100));
        lv_obj_set_style_text_color(t, UI_COLOR_FG, 0);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    }
    for (int i = 0; i < count; i++) {
        lv_obj_t *r = lv_label_create(box);
        lv_label_set_text(r, items[i]);
        lv_obj_set_width(r, lv_pct(100));
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(r, 8, 0);
        lv_obj_set_style_radius(r, 6, 0);
        lv_obj_set_style_text_font(r, &lv_font_montserrat_20, 0);
        s_menu.rows[i] = r;
    }
    lv_obj_t *hint = lv_label_create(box);
    lv_label_set_text(hint, "press: down  hold: choose  2x: close");
    lv_obj_set_style_text_color(hint, UI_COLOR_DIM, 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
    s_menu.box = box;
    s_menu.count = count;
    s_menu.sel = (initial >= 0 && initial < count) ? initial : 0;
    s_menu.cb = cb;
    menu_paint();
    if (locked) {
        display_unlock();
    }
}

void ui_confirm(const char *question, const char *action, ui_menu_cb cb)
{
    const char *items[2] = {"Cancel", action};
    ui_menu_open(question, items, 2, 0, cb);
    s_menu.confirm = true;
}

/* Returns true if the menu consumed the event. */
static bool menu_event(input_event_t e)
{
    if (!s_menu.box) {
        return false;
    }
    /* Button only: a camera held at a slight angle would auto-repeat tilt steps and
       carousel through the options. */
    switch (e) {
    case INPUT_SHORT:
        s_menu.sel = (s_menu.sel + 1) % s_menu.count;
        if (display_lock(200)) {
            menu_paint();
            display_unlock();
        }
        break;
    case INPUT_LONG:
        menu_close(s_menu.sel);
        break;
    case INPUT_DOUBLE:
        menu_close(-1);
        break;
    default:
        break;
    }
    return true;
}

/* ---------- screens + task ---------- */

static void switch_to(ui_screen_id_t id)
{
    if (s_menu.box) {
        if (display_lock(200)) {
            lv_obj_delete(s_menu.box);
            display_unlock();
        }
        memset(&s_menu, 0, sizeof(s_menu));
    }
    if (s_current && s_current->leave) {
        s_current->leave();
    }
    display_lock(-1);
    lv_obj_t *old = lv_screen_active();
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_COLOR_BG, 0);
    lv_obj_set_scrollable(scr, false);
    s_current = SCREENS[id];
    s_current->enter(scr);
    lv_screen_load(scr);
    if (old && old != scr) {
        lv_obj_delete(old);
    }
    display_unlock();
    ESP_LOGI(TAG, "screen %d", id);
}

void ui_show(ui_screen_id_t id)
{
    s_pending = id;
}

void ui_show_now(ui_screen_id_t id)
{
    s_pending = -1; /* this switch wins over one picked from a menu in the same pass */
    if (s_current != SCREENS[id]) {
        switch_to(id);
    }
}

void ui_camera_wake(void)
{
    if (s_current == &ui_camera_screen) {
        camera_power(true);
    }
}

void ui_note_activity(void)
{
    power_activity();
}

static void ui_task(void *arg)
{
    switch_to(UI_SCREEN_CAMERA);
    int64_t next_tick = 0;
    for (;;) {
        input_event_t e = input_get(s_current->idle && !power_is_asleep() ? 0 : 20);
        if (e == INPUT_PREV || e == INPUT_NEXT) {
            e = INPUT_NONE; /* tilt drives only the on-screen level */
        }
        if (e != INPUT_NONE) {
            /* A press that wakes the screen does nothing else. */
            const bool was_asleep = power_is_asleep();
            ui_note_activity();
            if (!was_asleep && !menu_event(e) && s_current->event) {
                s_current->event(e);
            }
        }
        remote_bridge_poll();
        if (s_pending >= 0) {
            const int id = s_pending;
            s_pending = -1;
            switch_to((ui_screen_id_t)id);
        }
        const int64_t now = esp_timer_get_time();
        if (now >= next_tick) {
            next_tick = now + 100 * 1000;
            toast_tick();
            power_tick();
            if (s_current->tick) {
                s_current->tick();
            }
        }
        if (s_current->idle && !power_is_asleep()) {
            s_current->idle();
        }
    }
}

void ui_reserve(void)
{
    ui_camera_reserve();
    ui_gallery_reserve();
}

void ui_start(void)
{
    xTaskCreatePinnedToCore(ui_task, "ui", 8192, NULL, 5, NULL, 0);
}
