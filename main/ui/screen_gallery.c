#include "ui.h"

/* Filled in by a later task. */
static void enter(lv_obj_t *scr)
{
    lv_obj_t *l = lv_label_create(scr);
    lv_label_set_text(l, "gallery\n(coming soon)\n\ndouble press: back");
    lv_obj_set_style_text_color(l, UI_COLOR_FG, 0);
    lv_obj_center(l);
}

static void event(input_event_t e)
{
    if (e == INPUT_DOUBLE) {
        ui_show(UI_SCREEN_CAMERA);
    }
}

const ui_screen_t ui_gallery_screen = {.enter = enter, .event = event};
