#include "remote_bridge.h"

#include "app_version.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lapse_logic.h"
#include "looks.h"
#include "remote.h"
#include "settings.h"

static const char *TAG = "bridge";

static char s_info[512];

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
