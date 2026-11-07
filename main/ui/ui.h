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
/* Turn the camera back on if the camera screen is showing (after sleep). */
void ui_camera_wake(void);

extern const ui_screen_t ui_camera_screen;
extern const ui_screen_t ui_gallery_screen;
extern const ui_screen_t ui_settings_screen;

/* Allocate the screens' long-lived buffers. Call at boot before camera_init() so they
   don't fragment the PSRAM the camera's full-res frame needs. */
void ui_reserve(void);
void ui_camera_reserve(void);
void ui_gallery_reserve(void);

/* Start the UI task on the camera screen. */
void ui_start(void);
void ui_show(ui_screen_id_t id);
/* Rebuild the current screen (after the orientation changed). Safe from event handlers. */
void ui_relayout(void);
/* True inside the leave()/enter() of such a rebuild: keep state, only redo widgets. */
bool ui_is_relayout(void);
/* True while a screen switch or rebuild is waiting to run. */
bool ui_switch_pending(void);
/* Switch at once if another screen is showing. UI task only. */
void ui_show_now(ui_screen_id_t id);

/* ---- Bluetooth remote hooks (camera screen; UI task only) ---- */
#include "remote_proto.h"
/* Run a remote command on the camera screen. NULL, or a short error reason. */
const char *ui_camera_remote(const remote_cmd_t *c);
/* Fill look, mode, interval, lapse, busy and last. */
void ui_camera_fill_status(remote_status_t *st);

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
/* Close any open menu or confirm without calling its callback. UI task only. */
void ui_menu_dismiss(void);

/* Shared styling */
#define UI_COLOR_BG     lv_color_hex(0x101010)
#define UI_COLOR_FG     lv_color_hex(0xf2f2f2)
#define UI_COLOR_ACCENT lv_color_hex(0xffc400)
#define UI_COLOR_DIM    lv_color_hex(0x8a8a8a)
