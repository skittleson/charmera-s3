#include "usbdisk.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_private/usb_phy.h"
#include "tusb.h"

#include "fat_image.h"
#include "fat_walk.h"
#include "storage.h"

static const char *TAG = "usbdisk";

#define MAX_FILES 800

static volatile bool s_active;
static volatile bool s_unplug;
static usb_phy_handle_t s_phy;
static fat_image_t s_img;
static TaskHandle_t s_task;

/* Gallery collected at start. fat_walk's alloc callback hands ownership of each
   entry's cluster array to us — steal it, nothing frees it until usbdisk stop. */

static fat_image_file_t *s_files;
static size_t s_nfiles;

static bool raw_read(void *ctx, uint32_t lba, uint32_t count, uint8_t *buf)
{
    (void)ctx;
    return storage_raw_read(lba, count, buf) == ESP_OK;
}

static void *psram_alloc(size_t n)
{
    return malloc(n); /* SPIRAM_USE_MALLOC routes big blocks to PSRAM */
}

/* ---- TinyUSB MSC callbacks ---- */

uint8_t tud_msc_get_maxlun_cb(void)
{
    return 1;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void)lun;
    *block_count = s_img.total_sectors;
    *block_size = 512;
}

bool tud_msc_is_writable_cb(uint8_t lun)
{
    (void)lun;
    return false;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void)lun;
    (void)power_condition;
    (void)start;
    (void)load_eject;
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    (void)lun;
    if (offset != 0) {
        return -1; /* CFG_TUD_MSC_BUFSIZE == one sector, so offset is always 0 */
    }
    uint8_t sec[512];
    uint32_t done = 0;
    while (done < bufsize) {
        memset(sec, 0, sizeof(sec));
        fat_image_read_ram(&s_img, lba + done / 512, 1, sec);
        if (fat_image_is_card_sector(&s_img, lba + done / 512)) {
            if (storage_raw_read(fat_image_card_sector(&s_img, lba + done / 512), 1, sec) != ESP_OK) {
                return -1;
            }
        }
        const uint32_t n = bufsize - done < 512 ? bufsize - done : 512;
        memcpy((uint8_t *)buffer + done, sec, n);
        done += n;
    }
    return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    (void)lun;
    (void)lba;
    (void)offset;
    (void)buffer;
    (void)bufsize;
    return -1; /* read-only */
}

bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    (void)lun;
    return s_active;
}

/* Vendor/builtin-overflow SCSI: nothing else is supported on a read-only LUN. */
int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer, uint16_t bufsize)
{
    (void)scsi_cmd;
    (void)buffer;
    (void)bufsize;
    tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
    return -1;
}

void tud_mount_cb(void)
{
}

void tud_umount_cb(void)
{
    /* The host released the drive (eject): safe to exit; UI task acts on the flag. */
    s_unplug = true;
}

void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    s_unplug = true; /* cable pulled (or host asleep): exit drive mode */
}

/* ---- descriptors: one MSC interface ---- */

#define ITF_MSC 0
#define EP_MSC_OUT 0x01
#define EP_MSC_IN 0x81

static const tusb_desc_device_t s_desc_dev = {
    .bLength = sizeof(s_desc_dev),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A, /* Espressif VID */
    .idProduct = 0x4005,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x00,
    .bNumConfigurations = 0x01,
};

static const uint8_t s_desc_cfg[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, 9 + TUD_MSC_DESC_LEN, 0, 100),
    TUD_MSC_DESCRIPTOR(ITF_MSC, 0x04, EP_MSC_OUT, EP_MSC_IN, 64),
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (const uint8_t *)&s_desc_dev;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return s_desc_cfg;
}

static const char *const s_strs[] = {"Charmera", "Charmera Camera", "Photos"};

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    static uint16_t buf[32];
    const char *s = (index < 3) ? s_strs[index] : NULL;
    if (index == 0) { /* language: English */
        buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | 4);
        buf[1] = 0x0409;
        return buf;
    }
    if (!s) {
        return NULL;
    }
    size_t n = 0;
    while (s[n] && n < 30) {
        buf[1 + n] = s[n];
        n++;
    }
    buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 + 2 * n));
    return buf;
}

/* ---- lifecycle ---- */

static bool add_file(void *ctx, const fat_walk_entry_t *e)
{
    (void)ctx;
    if (s_nfiles >= MAX_FILES) {
        return false; /* full: stop, what we have is served */
    }
    fat_image_file_t *f = &s_files[s_nfiles];
    memcpy(f->name, e->name, sizeof(f->name));
    f->size = e->size;
    f->nclusters = e->nclusters;
    f->clusters = e->clusters; /* stolen from fat_walk's alloc */
    s_nfiles++;
    return true;
}

bool usbdisk_can_start(const char **reason)
{
    if (s_active) {
        *reason = "already on";
        return false;
    }
    if (storage_status() != STORAGE_OK) {
        *reason = "no card";
        return false;
    }
    return true;
}

static void usb_task(void *arg)
{
    (void)arg;
    while (s_active) {
        tud_task();
    }
    s_task = NULL;
    vTaskDelete(NULL);
}

bool usbdisk_start(void)
{
    if (s_active) {
        return true;
    }
    bool ok = false;
    uint8_t bpb[512];
    if (storage_drive_take() != ESP_OK) {
        ESP_LOGE(TAG, "card take failed");
        return false;
    }
    /* Mirror the card's cluster size so its chains map 1:1 into our FAT. */
    if (storage_raw_read(0, 1, bpb) != ESP_OK || !bpb[13]) {
        goto give;
    }
    s_files = psram_alloc(MAX_FILES * sizeof(fat_image_file_t));
    if (!s_files) {
        goto give;
    }
    s_nfiles = 0;
    if (!fat_walk(raw_read, NULL, psram_alloc, "DCIM/100CHARM", add_file, NULL)) {
        ESP_LOGE(TAG, "gallery walk failed");
        goto give;
    }
    if (!fat_image_build(s_files, s_nfiles, bpb[13], &s_img)) {
        ESP_LOGE(TAG, "image build failed (%u files)", (unsigned)s_nfiles);
        goto give;
    }
    /* Hand the USB pads from USB-Serial-JTAG to the OTG device stack. */
    usb_phy_config_t phy = {
        .controller = USB_PHY_CTRL_OTG,
        .target = USB_PHY_TARGET_INT,
        .otg_mode = USB_OTG_MODE_DEVICE,
    };
    if (usb_new_phy(&phy, &s_phy) != ESP_OK) {
        ESP_LOGE(TAG, "phy claim failed");
        goto give;
    }
    s_active = true; /* task loop condition before the task starts */
    const tusb_rhport_init_t rh = {.role = TUSB_ROLE_DEVICE, .speed = TUSB_SPEED_FULL};
    if (!tusb_rhport_init(0, &rh)) {
        ESP_LOGE(TAG, "tusb init failed");
        s_active = false;
        usb_del_phy(s_phy);
        s_phy = NULL;
        goto give;
    }
    xTaskCreate(usb_task, "usbdisk", 4096, NULL, 6, &s_task);
    ESP_LOGI(TAG, "drive mode on: %u files, %lu sectors", (unsigned)s_nfiles, (unsigned long)s_img.total_sectors);
    ok = true;
give:
    if (!ok) {
        storage_drive_give();
    }
    return ok;
}

void usbdisk_stop(void)
{
    if (!s_active) {
        return;
    }
    tud_disconnect();
    tusb_deinit(0);
    s_active = false;
    for (int i = 0; i < 300 && s_task; i++) {
        vTaskDelay(1);
    }
    if (s_phy) {
        usb_del_phy(s_phy);
        s_phy = NULL; /* pads return to USB-Serial-JTAG: logs and flashing again */
    }
    fat_image_free(&s_img);
    for (size_t i = 0; i < s_nfiles; i++) {
        free((void *)s_files[i].clusters);
    }
    free(s_files);
    s_files = NULL;
    s_nfiles = 0;
    s_unplug = false;
    storage_drive_give();
    ESP_LOGI(TAG, "drive mode off");
}

bool usbdisk_is_active(void)
{
    return s_active;
}

void usbdisk_notify_unplug(void)
{
    if (s_unplug && s_active) {
        usbdisk_stop();
    }
}
