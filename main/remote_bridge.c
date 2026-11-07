#include "remote_bridge.h"

#include <string.h>

#include "app_version.h"
#include "board.h"
#include "camera.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lapse_logic.h"
#include "looks.h"
#include "remote.h"
#include "pipeline.h"
#include "settings.h"
#include "storage.h"
#include "ui.h"

static const char *TAG = "bridge";

#define CACHE_US   (5 * 1000 * 1000)
#define PUBLISH_US (250 * 1000)

static char s_info[512];
static char s_error[32];

/* Card and battery reads touch SPI and the ADC: refresh at most every 5 s. */
static struct {
    int64_t at;
    uint32_t photos, left;
    remote_card_t card;
    float volts;
} s_cache;

static void log_heap(const char *when)
{
    ESP_LOGI(TAG, "internal heap %s: free %u, largest block %u", when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

void remote_bridge_set_enabled(bool on)
{
    if (on == remote_is_running()) {
        return;
    }
    log_heap(on ? "before Bluetooth" : "with Bluetooth");
    if (on) {
        if (remote_start(s_info) != ESP_OK) {
            ESP_LOGE(TAG, "Bluetooth failed to start");
        }
    } else {
        remote_stop();
        s_error[0] = '\0';
    }
    log_heap(on ? "with Bluetooth" : "after Bluetooth off");
}

void remote_bridge_init(void)
{
    const char *looks[LOOK_COUNT], *intervals[LAPSE_INTERVAL_COUNT];
    for (int i = 0; i < LOOK_COUNT; i++) {
        looks[i] = look_name((look_t)i);
    }
    for (int i = 0; i < LAPSE_INTERVAL_COUNT; i++) {
        intervals[i] = lapse_interval_name(i);
    }
    if (!remote_info_json(APP_VERSION, looks, LOOK_COUNT, intervals, LAPSE_INTERVAL_COUNT, s_info, sizeof(s_info))) {
        ESP_LOGE(TAG, "info JSON too big");
    }
    remote_set_counts(LOOK_COUNT, LAPSE_INTERVAL_COUNT);
    if (settings_bluetooth()) {
        remote_bridge_set_enabled(true);
    }
}

static void refresh_cache(void)
{
    const int64_t now = esp_timer_get_time();
    if (s_cache.at && now - s_cache.at < CACHE_US) {
        return;
    }
    s_cache.at = now;
    s_cache.volts = board_battery_volts();
    storage_info_t info;
    if (storage_info(&info) != ESP_OK) {
        s_cache.card = REMOTE_CARD_NONE;
        s_cache.photos = s_cache.left = 0;
        return;
    }
    s_cache.photos = info.photo_count;
    s_cache.left = storage_photos_remaining_now();
    s_cache.card = (!storage_numbers_left() || s_cache.left == 0) ? REMOTE_CARD_FULL : REMOTE_CARD_OK;
}

void remote_bridge_publish(void)
{
    if (!remote_is_running()) {
        return;
    }
    refresh_cache();
    remote_status_t st = {0};
    st.photos = s_cache.photos;
    st.left = s_cache.left;
    st.card = s_cache.card;
    st.usb = s_cache.volts >= 4.5f;
    st.battery_pct = st.usb ? 100 : board_battery_percent(s_cache.volts);
    st.camera = camera_sensor_name();
    ui_camera_fill_status(&st);
    strlcpy(st.error, s_error, sizeof(st.error));
    st.clock_set = pipeline_clock_is_set();
    static char json[512];
    if (remote_status_json(&st, json, sizeof(json))) {
        remote_publish(json, st.battery_pct);
    }
}

void remote_bridge_poll(void)
{
    if (!remote_is_running()) {
        return;
    }
    static int64_t next;
    const int64_t now = esp_timer_get_time();
    bool now_please = false;
    const char *rejected = remote_take_error();
    if (rejected[0]) {
        strlcpy(s_error, rejected, sizeof(s_error));
        now_please = true;
    }
    remote_cmd_t c;
    const bool got = remote_take_command(&c);
    if (got && c.kind == RC_TIME) {
        pipeline_set_clock(c.epoch, c.offset_min); /* no screen switch, no wake-up */
        if (!rejected[0]) {
            s_error[0] = '\0';
        }
        now_please = true;
    } else if (got) {
        ui_show_now(UI_SCREEN_CAMERA);
        ui_menu_dismiss(); /* an open menu's callback could otherwise undo the command */
        ui_note_activity(); /* also wakes a sleeping screen */
        const char *err = ui_camera_remote(&c);
        if (err || !rejected[0]) {
            strlcpy(s_error, err ? err : "", sizeof(s_error));
        }
        s_cache.at = 0; /* counts may have changed */
        now_please = true;
    }
    if (now_please || now >= next) {
        next = now + PUBLISH_US;
        remote_bridge_publish();
    }
}
