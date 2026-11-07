#include <stdio.h>
#include <string.h>

#include "app_version.h"
#include "board.h"
#include "display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "remote_bridge.h"
#include "settings.h"
#include "storage.h"
#include "ui.h"

static const char *TAG = "ui_settings";

enum { ROW_BRIGHTNESS, ROW_LEVEL, ROW_BLUETOOTH, ROW_FORMAT, ROW_BACK, ROW_COUNT };

static struct {
    lv_obj_t *info;
    lv_obj_t *rows[ROW_COUNT];
    int sel;
    bool adjusting; /* brightness edit mode */
    int64_t next_info;
} s;

static int battery_pct(float v)
{
    int p = (int)((v - 3.3f) / (4.2f - 3.3f) * 100.0f + 0.5f);
    return p < 0 ? 0 : (p > 100 ? 100 : p);
}

static void fmt_gb(char *out, size_t n, uint64_t bytes)
{
    if (bytes >= 1000ull * 1000 * 1000) {
        snprintf(out, n, "%.1f GB", bytes / 1e9);
    } else {
        snprintf(out, n, "%.0f MB", bytes / 1e6);
    }
}

/* Display lock NOT held. */
static void refresh_info(void)
{
    char text[200];
    storage_info_t info;
    const float v = board_battery_volts();
    char bat[32];
    if (v >= 4.5f) {
        snprintf(bat, sizeof(bat), "USB power");
    } else {
        snprintf(bat, sizeof(bat), "%.2f V (%d%%)", v, battery_pct(v));
    }
    if (storage_info(&info) == ESP_OK) {
        char total[16], freeb[16], used[16];
        fmt_gb(total, sizeof(total), info.total_bytes);
        fmt_gb(freeb, sizeof(freeb), info.free_bytes);
        fmt_gb(used, sizeof(used), info.total_bytes - info.free_bytes);
        snprintf(text, sizeof(text),
                 "Card    %s\nFree    %s\nUsed    %s\nPhotos  %u\nBattery %s\nCharmera-S3 %s", total, freeb,
                 used, (unsigned)info.photo_count, bat, APP_VERSION);
    } else {
        snprintf(text, sizeof(text), "Card    No card\n\n\n\nBattery %s\nCharmera-S3 %s", bat, APP_VERSION);
    }
    if (display_lock(200)) {
        lv_label_set_text(s.info, text);
        display_unlock();
    }
}

/* Display lock held. */
static void paint(void)
{
    char t[40];
    snprintf(t, sizeof(t), s.adjusting ? "Brightness  < %u%% >" : "Brightness  %u%%",
             (unsigned)settings_brightness());
    lv_label_set_text(s.rows[ROW_BRIGHTNESS], t);
    lv_label_set_text(s.rows[ROW_LEVEL], settings_level() ? "Level  On" : "Level  Off");
    lv_label_set_text(s.rows[ROW_BLUETOOTH], settings_bluetooth() ? "Bluetooth  On" : "Bluetooth  Off");
    lv_label_set_text(s.rows[ROW_FORMAT], storage_status() == STORAGE_OK ? "Format card" : "Format card (no card)");
    lv_label_set_text(s.rows[ROW_BACK], "Back to camera");
    for (int i = 0; i < ROW_COUNT; i++) {
        const bool on = i == s.sel;
        lv_obj_set_style_bg_color(s.rows[i], on ? UI_COLOR_ACCENT : lv_color_hex(0x2a2a2a), 0);
        lv_obj_set_style_text_color(s.rows[i], on ? lv_color_black() : UI_COLOR_FG, 0);
        if (on) {
            lv_obj_scroll_to_view(s.rows[i], LV_ANIM_OFF); /* the list is taller than the screen */
        }
    }
}

static void repaint(void)
{
    if (display_lock(200)) {
        paint();
        display_unlock();
    }
}

static void enter(lv_obj_t *scr)
{
    memset(&s, 0, sizeof(s));
    lv_obj_set_scrollable(scr, true);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, 8, 0);
    lv_obj_set_style_pad_row(scr, 6, 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, UI_COLOR_ACCENT, 0);

    s.info = lv_label_create(scr);
    lv_obj_set_style_text_color(s.info, UI_COLOR_FG, 0);
    lv_label_set_text(s.info, "...");

    for (int i = 0; i < ROW_COUNT; i++) {
        lv_obj_t *r = lv_label_create(scr);
        lv_obj_set_width(r, lv_pct(100));
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(r, 7, 0);
        lv_obj_set_style_radius(r, 6, 0);
        lv_obj_set_style_text_font(r, &lv_font_montserrat_20, 0);
        s.rows[i] = r;
    }
    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "press: down  hold: choose  2x: back");
    lv_obj_set_style_text_color(hint, UI_COLOR_DIM, 0);
    paint();
}

static void on_format(int yes)
{
    if (!yes) {
        return;
    }
    ui_toast("Formatting...", 60000);
    const esp_err_t err = storage_format();
    ESP_LOGI(TAG, "format -> %s", esp_err_to_name(err));
    ui_toast(err == ESP_OK ? "Card formatted" : "Format failed", 2000);
    input_flush();
    ui_note_activity();
    s.next_info = 0;
    repaint();
}

static void activate(void)
{
    switch (s.sel) {
    case ROW_BRIGHTNESS:
        s.adjusting = true;
        break;
    case ROW_LEVEL:
        settings_set_level(!settings_level());
        break;
    case ROW_BLUETOOTH:
        settings_set_bluetooth(!settings_bluetooth());
        remote_bridge_set_enabled(settings_bluetooth());
        ui_toast(settings_bluetooth() ? "Bluetooth on" : "Bluetooth off", 1200);
        break;
    case ROW_FORMAT:
        if (storage_status() == STORAGE_OK) {
            ui_confirm("This erases every photo and file on the card. Format?", "Format", on_format);
        } else {
            ui_toast("No card", 1500);
        }
        break;
    case ROW_BACK:
        ui_show(UI_SCREEN_CAMERA);
        break;
    }
    repaint();
}

static void event(input_event_t e)
{
    if (s.adjusting) {
        if (e == INPUT_SHORT) {
            int b = settings_brightness() + 10;
            settings_set_brightness(b > 100 ? 10 : b);
            board_backlight_set(settings_brightness());
        } else if (e == INPUT_LONG || e == INPUT_DOUBLE) {
            s.adjusting = false;
        }
        repaint();
        return;
    }
    switch (e) {
    case INPUT_SHORT:
        s.sel = (s.sel + 1) % ROW_COUNT;
        repaint();
        break;
    case INPUT_LONG:
        activate();
        break;
    case INPUT_DOUBLE:
        ui_show(UI_SCREEN_CAMERA);
        break;
    default:
        break;
    }
}

static void tick(void)
{
    const int64_t now = esp_timer_get_time();
    if (now >= s.next_info) {
        s.next_info = now + 3 * 1000 * 1000;
        refresh_info();
        repaint();
    }
}

const ui_screen_t ui_settings_screen = {
    .enter = enter,
    .event = event,
    .tick = tick,
};
