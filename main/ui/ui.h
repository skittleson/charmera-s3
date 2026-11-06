#pragma once

#include <stdbool.h>

#include "input.h"
#include "lvgl.h"

typedef enum { UI_SCREEN_CAMERA, UI_SCREEN_GALLERY, UI_SCREEN_SETTINGS } ui_screen_id_t;

typedef struct {
    void (*enter)(lv_obj_t *scr); /* build widgets on a fresh screen (display lock held) */
    void (*leave)(void);          /* stop background work; widgets are deleted after */
    void (*event)(input_event_t e);
    void (*tick)(void);           /* ~every 100 ms, display lock NOT held */
    void (*idle)(void);           /* if set, called continuously between input polls */
} ui_screen_t;

/* Pause/resume the camera viewfinder (power saving). */
void ui_camera_freeze(bool on);

extern const ui_screen_t ui_camera_screen;
extern const ui_screen_t ui_gallery_screen;
extern const ui_screen_t ui_settings_screen;

/* Start the UI task on the camera screen. */
void ui_start(void);
void ui_show(ui_screen_id_t id);

/* Called on every input event; feeds the idle timer. */
void ui_note_activity(void);

/* Short message over the current screen for ms milliseconds (display lock held or not). */
void ui_toast(const char *text, int ms);

/* ---- highlight menu (press moves down, hold chooses, double press closes; tilt ignored) ---- */
typedef void (*ui_menu_cb)(int index); /* -1 = cancelled */

void ui_menu_open(const char *title, const char *const *items, int count, int initial, ui_menu_cb cb);
/* Two-choice confirm with Cancel highlighted first. cb(1) = confirmed, cb(0) = cancelled. */
void ui_confirm(const char *question, const char *action, ui_menu_cb cb);
bool ui_menu_is_open(void);

/* Shared styling */
#define UI_COLOR_BG     lv_color_hex(0x101010)
#define UI_COLOR_FG     lv_color_hex(0xf2f2f2)
#define UI_COLOR_ACCENT lv_color_hex(0xffc400)
#define UI_COLOR_DIM    lv_color_hex(0x8a8a8a)
