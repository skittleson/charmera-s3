#include "remote.h"

#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/bas/ble_svc_bas.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define DEVICE_NAME  "Charmera-S3"
#define STATUS_MAX   512
#define INFO_MAX     512
#define CMD_MAX      64
#define QUEUE_DEPTH  4

static const char *TAG = "remote";

/* c4a3000N-7b6e-4f0a-9d2e-5a1c8e3f6b20, little-endian as NimBLE wants it. */
#define UUID_C4A3(n)                                                                                   \
    BLE_UUID128_INIT(0x20, 0x6b, 0x3f, 0x8e, 0x1c, 0x5a, 0x2e, 0x9d, 0x0a, 0x4f, 0x6e, 0x7b, (n), 0x00, \
                     0xa3, 0xc4)

static const ble_uuid128_t SVC_UUID = UUID_C4A3(0x00);
static const ble_uuid128_t STATUS_UUID = UUID_C4A3(0x01);
static const ble_uuid128_t COMMAND_UUID = UUID_C4A3(0x02);
static const ble_uuid128_t INFO_UUID = UUID_C4A3(0x03);
static const ble_uuid16_t USER_DESC_UUID = BLE_UUID16_INIT(0x2901);

static SemaphoreHandle_t s_lock;
static QueueHandle_t s_queue;
static char s_status[STATUS_MAX] = "{}";
static size_t s_status_len = 2;
static char s_info[INFO_MAX];
static char s_error[32];
static int s_looks = 1, s_intervals = 1;
static bool s_running;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static bool s_subscribed;
static uint16_t s_status_handle;
static uint8_t s_addr_type;

static void advertise(void);

static int append(struct ble_gatt_access_ctxt *ctxt, const void *data, size_t len)
{
    return os_mbuf_append(ctxt->om, data, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int status_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int rc = append(ctxt, s_status, s_status_len);
    xSemaphoreGive(s_lock);
    return rc;
}

static int info_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return append(ctxt, s_info, strlen(s_info));
}

static int command_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    char buf[CMD_MAX];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    remote_cmd_t c;
    const char *err = remote_parse(buf, len, s_looks, s_intervals, &c);
    ESP_LOGI(TAG, "command \"%.*s\" -> %s", (int)len, buf, err ? err : "queued");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (err) {
        strlcpy(s_error, err, sizeof(s_error));
    } else if (xQueueSend(s_queue, &c, 0) != pdTRUE) {
        strlcpy(s_error, "busy, try again", sizeof(s_error));
    }
    xSemaphoreGive(s_lock);
    return 0;
}

static int desc_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const char *text = arg;
    return append(ctxt, text, strlen(text));
}

#define USER_DESC(text)                                                                              \
    (struct ble_gatt_dsc_def[])                                                                      \
    {                                                                                                \
        {.uuid = &USER_DESC_UUID.u, .att_flags = BLE_ATT_F_READ, .access_cb = desc_access, .arg = (void *)(text)}, \
        {0}                                                                                          \
    }

static const struct ble_gatt_svc_def SERVICES[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &SVC_UUID.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {.uuid = &STATUS_UUID.u,
                 .access_cb = status_access,
                 .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                 .val_handle = &s_status_handle,
                 .descriptors = USER_DESC("Status JSON")},
                {.uuid = &COMMAND_UUID.u,
                 .access_cb = command_access,
                 .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                 .descriptors = USER_DESC("Command")},
                {.uuid = &INFO_UUID.u,
                 .access_cb = info_access,
                 .flags = BLE_GATT_CHR_F_READ,
                 .descriptors = USER_DESC("Info JSON")},
                {0},
            },
    },
    {0},
};

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
            ESP_LOGI(TAG, "connected");
        } else {
            advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected (reason 0x%x)", ev->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == s_status_handle) {
            s_subscribed = ev->subscribe.cur_notify;
        }
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu %d", ev->mtu.value);
        break;
    default:
        break;
    }
    return 0;
}

static void advertise(void)
{
    if (!s_running) {
        return;
    }
    /* Flags + 128-bit service UUID in the advert, the name in the scan response
       (both together exceed 31 bytes). */
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = (ble_uuid128_t *)&SVC_UUID;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&f);
    struct ble_hs_adv_fields r = {0};
    r.name = (const uint8_t *)DEVICE_NAME;
    r.name_len = strlen(DEVICE_NAME);
    r.name_is_complete = 1;
    rc = rc ? rc : ble_gap_adv_rsp_set_fields(&r);
    struct ble_gap_adv_params p = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = BLE_GAP_ADV_ITVL_MS(500),
        .itvl_max = BLE_GAP_ADV_ITVL_MS(700),
    };
    rc = rc ? rc : ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "advertise failed: %d", rc);
    }
}

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_addr_type);
    advertise();
    ESP_LOGI(TAG, "advertising as %s", DEVICE_NAME);
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset: %d", reason);
}

static void host_task(void *arg)
{
    nimble_port_run(); /* returns after nimble_port_stop() */
    nimble_port_freertos_deinit();
}

void remote_set_counts(int looks, int intervals)
{
    s_looks = looks;
    s_intervals = intervals;
}

esp_err_t remote_start(const char *info_json)
{
    if (s_running) {
        return ESP_OK;
    }
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        s_queue = xQueueCreate(QUEUE_DEPTH, sizeof(remote_cmd_t));
        if (!s_lock || !s_queue) {
            return ESP_ERR_NO_MEM;
        }
    }
    strlcpy(s_info, info_json ? info_json : "{}", sizeof(s_info));
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble init failed: %s", esp_err_to_name(err));
        return err;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_bas_init();
    int rc = ble_gatts_count_cfg(SERVICES);
    rc = rc ? rc : ble_gatts_add_svcs(SERVICES);
    rc = rc ? rc : ble_svc_gap_device_name_set(DEVICE_NAME);
    if (rc) {
        ESP_LOGE(TAG, "gatt setup failed: %d", rc);
        nimble_port_deinit();
        return ESP_FAIL;
    }
    s_running = true;
    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

void remote_stop(void)
{
    if (!s_running) {
        return;
    }
    s_running = false; /* stops advertise() from restarting */
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    ble_gap_adv_stop();
    if (nimble_port_stop() == 0) {
        nimble_port_deinit();
    }
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_subscribed = false;
    xQueueReset(s_queue);
    ESP_LOGI(TAG, "stopped");
}

bool remote_is_running(void)
{
    return s_running;
}

bool remote_is_connected(void)
{
    return s_running && s_conn != BLE_HS_CONN_HANDLE_NONE;
}

bool remote_take_command(remote_cmd_t *out)
{
    return s_queue && xQueueReceive(s_queue, out, 0) == pdTRUE;
}

const char *remote_take_error(void)
{
    static char out[32];
    out[0] = '\0';
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(out, s_error, sizeof(out));
        s_error[0] = '\0';
        xSemaphoreGive(s_lock);
    }
    return out;
}

void remote_publish(const char *status_json, int battery_pct)
{
    if (!s_running || !status_json) {
        return;
    }
    const size_t len = strlen(status_json);
    if (len >= STATUS_MAX) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool changed = len != s_status_len || memcmp(s_status, status_json, len) != 0;
    if (changed) {
        memcpy(s_status, status_json, len + 1);
        s_status_len = len;
    }
    xSemaphoreGive(s_lock);
    static int last_bat = -1;
    if (battery_pct != last_bat) {
        last_bat = battery_pct;
        ble_svc_bas_battery_level_set((uint8_t)(battery_pct < 0 ? 0 : battery_pct > 100 ? 100 : battery_pct));
    }
    if (changed && s_subscribed && s_conn != BLE_HS_CONN_HANDLE_NONE) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(status_json, len);
        if (om) {
            ble_gatts_notify_custom(s_conn, s_status_handle, om); /* consumes om */
        }
    }
}
