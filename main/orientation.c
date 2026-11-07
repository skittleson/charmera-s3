#include "orientation.h"

#include "camera.h"
#include "display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "input.h"
#include "settings.h"
#include "ui.h"

static const char *TAG = "orientation";

static orient_t s_current = ORIENT_PORTRAIT;
static orient_auto_t s_auto;

static const char *label(orient_t o)
{
    switch (o) {
    case ORIENT_LAND_RIGHT: return "Landscape (USB-C right)";
    case ORIENT_LAND_LEFT: return "Landscape (USB-C left)";
    default: return "Portrait";
    }
}

static void apply(orient_t o, bool relayout)
{
    if (o == s_current) {
        return;
    }
    ESP_LOGI(TAG, "%s -> %s", orient_name(s_current), orient_name(o));
    s_current = o;
    camera_set_orientation(o);
    display_set_orientation(o);
    if (relayout) {
        ui_relayout(); /* rebuild the current screen for the new size */
    }
}

void orientation_init(void)
{
    const int setting = settings_orientation();
    const orient_t start = setting == ORIENT_SETTING_AUTO ? ORIENT_PORTRAIT : (orient_t)setting;
    s_current = ORIENT_PORTRAIT;
    apply(start, false);
    orient_auto_init(&s_auto, s_current);
    float down, side, face;
    input_gravity(&down, &side, &face);
    /* For checking the derived constants on the board: hold it portrait at boot and
       down should be about +1 g (8192), side about 0. */
    ESP_LOGI(TAG, "setting %s, start %s, gravity down %.0f side %.0f face %.0f", orient_name(setting),
             orient_name(start), down, side, face);
}

orient_t orientation_current(void)
{
    return s_current;
}

void orientation_setting_changed(void)
{
    const int setting = settings_orientation();
    if (setting == ORIENT_SETTING_AUTO) {
        orient_auto_init(&s_auto, s_current); /* Auto takes over from the current grip */
        return;
    }
    apply((orient_t)setting, true);
}

void orientation_auto_poll(bool locked)
{
    if (settings_orientation() != ORIENT_SETTING_AUTO) {
        return;
    }
    float down, side, face;
    input_gravity(&down, &side, &face);
    if (orient_auto_update(&s_auto, down, side, face, locked, esp_timer_get_time() / 1000)) {
        apply(s_auto.current, true);
        ui_toast(label(s_auto.current), 1000);
    }
}
