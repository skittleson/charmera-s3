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

/* ---- file service: list / thumb / get (streamed over BLE) ---- */

typedef struct {
    FILE *f;
} file_reader_t;

static int file_reader(uint8_t *buf, int max, void *ctx)
{
    file_reader_t *r = (file_reader_t *)ctx;
    board_spi_lock();
    const size_t n = r->f ? fread(buf, 1, (size_t)max, r->f) : 0;
    board_spi_unlock();
    return n > 0 ? (int)n : 0;
}

static void file_done(void *ctx)
{
    file_reader_t *r = (file_reader_t *)ctx;
    if (r->f) {
        board_spi_lock();
        fclose(r->f);
        board_spi_unlock();
    }
    free(r);
}

typedef struct {
    char *buf;
    size_t len, pos;
} mem_reader_t;

static int mem_reader(uint8_t *out, int max, void *ctx)
{
    mem_reader_t *r = (mem_reader_t *)ctx;
    const size_t left = r->len - r->pos;
    const size_t n = left < (size_t)max ? left : (size_t)max;
    memcpy(out, r->buf + r->pos, n);
    r->pos += n;
    return (int)n;
}

static void mem_done(void *ctx)
{
    mem_reader_t *r = (mem_reader_t *)ctx;
    free(r->buf);
    free(r);
}

/* Photo number -> JPEG path; sidecar when thumb. */
static bool photo_path(int number, bool thumb, char *out, size_t len)
{
    if (thumb) {
        return storage_thumb_path(number, out, len) == ESP_OK;
    }
    return storage_photo_path(number, out, len) == ESP_OK;
}

static const char *stream_photo(int number, bool thumb)
{
    char path[64];
    if (!photo_path(number, thumb, path, sizeof(path)) || storage_status() != STORAGE_OK) {
        return "no photo";
    }
    file_reader_t *r = malloc(sizeof(file_reader_t));
    if (!r) {
        return "no memory";
    }
    board_spi_lock();
    r->f = fopen(path, "rb");
    board_spi_unlock();
    if (!r->f) {
        free(r);
        return thumb ? "no preview" : "no photo";
    }
    size_t size = 0;
    storage_file_size(path, &size);
    char meta[96];
    snprintf(meta, sizeof(meta), "{\"kind\":\"%s\",\"n\":%d,\"size\":%u}", thumb ? "thumb" : "get",
             number, (unsigned)size);
    if (!remote_stream_file(meta, file_reader, file_done, r)) {
        file_done(r);
        return remote_stream_is_subscribed() ? "stream busy" : "enable stream notifications";
    }
    return NULL;
}

static const char *stream_list(void)
{
    int *nums;
    size_t n;
    if (storage_status() != STORAGE_OK || storage_list_photos(&nums, &n) != ESP_OK) {
        return "no card";
    }
    /* [[102,12345],[103,...]] — 19 bytes per photo worst case (incl. NUL). */
    char *json = heap_caps_malloc(n * 19 + 3, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!json) {
        free(nums);
        return "no memory";
    }
    size_t used = 0;
    size_t entries = 0;
    json[used++] = '[';
    for (size_t i = 0; i < n; i++) {
        char path[64];
        size_t size = 0;
        storage_photo_path(nums[i], path, sizeof(path));
        if (storage_file_size(path, &size)) {
            used += snprintf(json + used, 20, "%s[%d,%u]", entries ? "," : "", nums[i], (unsigned)size);
            entries++;
        }
    }
    json[used++] = ']';
    free(nums);
    mem_reader_t *r = malloc(sizeof(mem_reader_t));
    if (!r) {
        free(json);
        return "no memory";
    }
    r->buf = json;
    r->len = used;
    r->pos = 0;
    char meta[64];
    snprintf(meta, sizeof(meta), "{\"kind\":\"list\",\"count\":%u,\"bytes\":%u}", (unsigned)n,
             (unsigned)used);
    if (!remote_stream_file(meta, mem_reader, mem_done, r)) {
        mem_done(r);
        return remote_stream_is_subscribed() ? "stream busy" : "enable stream notifications";
    }
    return NULL;
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
    } else if (got && (c.kind == RC_LIST || c.kind == RC_THUMB || c.kind == RC_GET)) {
        /* File service: no screen switch, no wake — the page drives these. */
        const char *err = c.kind == RC_LIST ? stream_list() : stream_photo(c.arg, c.kind == RC_THUMB);
        if (err) {
            remote_stream_error(err);
            strlcpy(s_error, err, sizeof(s_error));
        } else if (!rejected[0]) {
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
