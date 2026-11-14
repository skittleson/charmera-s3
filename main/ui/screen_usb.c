#include <string.h>

#include "display.h"
#include "ui.h"
#include "usbdisk.h"

/* Dedicated page while the card is served read-only over USB. Disconnecting is
   possible from here (touch) even though serial and flashing are gone. */

static struct {
    lv_obj_t *btn;
} s;

static void enter(lv_obj_t *scr)
{
    memset(&s, 0, sizeof(s));
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, 16, 0);
    lv_obj_set_style_pad_row(scr, 12, 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "USB Mode");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, UI_COLOR_ACCENT, 0);

    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, "Photos on your computer\nread-only");
    lv_obj_set_style_text_color(sub, UI_COLOR_FG, 0);

    s.btn = lv_button_create(scr);
    lv_obj_set_width(s.btn, lv_pct(100));
    lv_obj_set_style_bg_color(s.btn, UI_COLOR_ACCENT, 0);
    lv_obj_t *bl = lv_label_create(s.btn);
    lv_label_set_text(bl, "Disconnect");
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(bl, lv_color_black(), 0);
}

static void on_disconnect(void)
{
    usbdisk_stop();
    ui_show(UI_SCREEN_CAMERA);
}

static void event(input_event_t e)
{
    switch (e) {
    case INPUT_SHORT: /* the button is the only thing on screen */
    case INPUT_LONG:
    case INPUT_DOUBLE:
        on_disconnect();
        break;
    default:
        break;
    }
}

static void tick(void)
{
    /* Unplugged (or host ejected): leave by itself. */
    if (!usbdisk_is_active()) {
        ui_show(UI_SCREEN_CAMERA);
        return;
    }
    usbdisk_notify_unplug();
    if (!usbdisk_is_active()) {
        ui_show(UI_SCREEN_CAMERA);
    }
}

const ui_screen_t ui_usb_screen = {
    .enter = enter,
    .event = event,
    .tick = tick,
};
