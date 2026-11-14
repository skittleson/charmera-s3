#include "usbdisk.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
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

/* Gallery collected at start: names/sizes plus card backing runs, bump-allocated
   from one PSRAM arena (chains are almost always a single contiguous run). */

#define RUN_ARENA 16384 /* run entries; 128 KB */

static fat_image_file_t *s_files;
static size_t s_nfiles;
static uint64_t *s_arena;
static size_t s_arena_used;
static bool s_arena_full; /* arena exhausted: drop remaining files */

/* The card may be MBR-partitioned (format does this): all FAT reads go through
   the volume's start sector, and reported cluster sectors stay volume-relative —
   add_file makes them absolute. */
static uint32_t s_base;

static bool raw_read(void *ctx, uint32_t lba, uint32_t count, uint8_t *buf)
{
    (void)ctx;
    return storage_raw_read(s_base + lba, count, buf) == ESP_OK;
}

/* Volume start: sector 0 when it is itself a FAT boot sector, else the first
   MBR partition entry. false when neither looks like a FAT volume. */
static bool find_volume(uint32_t *boot_lba, uint8_t bpb[512])
{
    if (storage_raw_read(0, 1, bpb) != ESP_OK) {
        return false;
    }
    const bool bpb_ok = bpb[510] == 0x55 && bpb[511] == 0xAA && bpb[13] && !(bpb[13] & (bpb[13] - 1));
    if (bpb_ok && (bpb[17] || bpb[22])) {
        *boot_lba = 0; /* FAT12/16 boot sector */
        return true;
    }
    if (bpb_ok && bpb[17] == 0 && bpb[36]) {
        *boot_lba = 0; /* FAT32 boot sector */
        return true;
    }
    if (bpb[510] == 0x55 && bpb[511] == 0xAA) {
        for (int i = 0; i < 4; i++) {
            const uint8_t *e = bpb + 446 + i * 16;
            const uint8_t type = e[4];
            if (type == 0x0B || type == 0x0C || type == 0x06 || type == 0x04) {
                *boot_lba = (uint32_t)e[8] | ((uint32_t)e[9] << 8) | ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
                return storage_raw_read(*boot_lba, 1, bpb) == ESP_OK;
            }
        }
    }
    return false;
}

static void *psram_alloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        ESP_LOGE(TAG, "alloc %u failed: psram-free %u, largest %u", (unsigned)n,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    }
    return p;
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

static bool begin_file(void *ctx, const char *name, uint32_t size)
{
    (void)ctx;
    if (s_nfiles >= MAX_FILES || s_arena_used + 8 > RUN_ARENA) {
        s_arena_full = true;
        return false; /* full: stop, what we have is served */
    }
    fat_image_file_t *f = &s_files[s_nfiles];
    memcpy(f->name, name, sizeof(f->name));
    f->size = size;
    f->runs = s_arena + s_arena_used;
    f->nruns = 0;
    s_nfiles++;
    return true;
}

static void add_run(void *ctx, uint32_t volume_lba, uint32_t clusters)
{
    (void)ctx;
    if (s_arena_used >= RUN_ARENA) {
        s_arena_full = true;
        s_files[s_nfiles - 1].nruns = 0; /* poison: dropped after the walk */
        return;
    }
    fat_image_file_t *f = &s_files[s_nfiles - 1];
    /* run = (absolute card lba << 32 | cluster count) */
    s_arena[s_arena_used++] = ((uint64_t)(volume_lba + s_base) << 32) | clusters;
    f->nruns++;
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
    uint32_t boot_lba = 0;
    s_base = 0;
    if (storage_drive_take() != ESP_OK) {
        ESP_LOGE(TAG, "card take failed");
        return false;
    }
    if (!find_volume(&boot_lba, bpb) || !bpb[13]) {
        ESP_LOGE(TAG, "no FAT volume on card");
        goto give;
    }
    s_base = boot_lba;
    /* Mirror the volume's cluster size so its chains map 1:1 into our FAT. */
    s_arena = psram_alloc(RUN_ARENA * sizeof(uint64_t));
    s_files = psram_alloc(MAX_FILES * sizeof(fat_image_file_t));
    if (!s_arena || !s_files) {
        goto give;
    }
    s_nfiles = 0;
    s_arena_used = 0;
    s_arena_full = false;
    if (!fat_walk(raw_read, NULL, "DCIM/100CHARM", begin_file, add_run, NULL)) {
        ESP_LOGE(TAG, "gallery walk failed");
        goto give;
    }
    while (s_nfiles && s_files[s_nfiles - 1].nruns == 0) {
        s_nfiles--; /* overflow-poisoned tail (a partial file is never served) */
    }
    if (s_arena_full) {
        ESP_LOGW(TAG, "gallery truncated at %u files", (unsigned)s_nfiles);
    }
    /* Mirror the card volume's geometry: rsvd, FAT size, cluster size, and the
       volume's own sector count (raw_sectors minus the MBR offset). */
    const uint16_t rsvd = (uint16_t)(bpb[14] | (bpb[15] << 8));
    const uint32_t fatsz = (uint32_t)bpb[36] | ((uint32_t)bpb[37] << 8) | ((uint32_t)bpb[38] << 16) |
                           ((uint32_t)bpb[39] << 24);
    uint32_t vol = storage_raw_sectors();
    if (!fat_image_build(s_files, s_nfiles, bpb[13], rsvd, fatsz, vol - s_base, &s_img)) {
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
    s_active = false; /* ends the task loop; no callbacks after it exits */
    for (int i = 0; i < 300 && s_task; i++) {
        vTaskDelay(1);
    }
    /* Drop the OTG PHY without tusb_deinit: usb_del_phy hands the pads back to
       USB-Serial-JTAG (esp_tinyusb's uninstall does exactly this). */
    if (s_phy) {
        usb_del_phy(s_phy);
        s_phy = NULL; /* pads return to USB-Serial-JTAG: logs and flashing again */
    }
    fat_image_free(&s_img);
    free(s_files);
    s_files = NULL;
    free(s_arena);
    s_arena = NULL;
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
