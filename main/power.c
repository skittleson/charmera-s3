#include "power.h"

#include "board.h"
#include "camera.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "input.h"
#include "power_logic.h"
#include "settings.h"
#include "ui.h"

#define DIM_PERCENT 20

static const char *TAG = "power";

static int64_t s_last_activity;
static power_state_t s_state = POWER_AWAKE;

static void apply(power_state_t st)
{
    if (st == s_state) {
        return;
    }
    ESP_LOGI(TAG, "%d -> %d", s_state, st);
    if (st == POWER_ASLEEP) {
        ui_camera_freeze(true);
        board_backlight_set(0);
        camera_power(false);
    } else {
        if (s_state == POWER_ASLEEP) {
            ui_camera_freeze(false);
            ui_camera_wake();
        }
        const int b = settings_brightness();
        board_backlight_set(st == POWER_DIM && b > DIM_PERCENT ? DIM_PERCENT : b);
    }
    s_state = st;
}

void power_init(void)
{
    s_last_activity = esp_timer_get_time();
}

void power_activity(void)
{
    s_last_activity = esp_timer_get_time();
    apply(POWER_AWAKE);
}

void power_tick(void)
{
    const int64_t idle_ms = (esp_timer_get_time() - s_last_activity) / 1000;
    apply(power_state_for_idle(idle_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)idle_ms));
}

bool power_is_asleep(void)
{
    return s_state == POWER_ASLEEP;
}
