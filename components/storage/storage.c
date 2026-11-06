#include "storage.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "board.h"
#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "nvs.h"
#include "sdmmc_cmd.h"

#define MOUNT_RETRY_US   (2 * 1000 * 1000)
#define NVS_NS           "storage"
#define NVS_AVG_KEY      "avg_sz"
/* Smaller files can't be real 5 MP photos; keep them out of the size estimate. */
#define MIN_PHOTO_BYTES  (64 * 1024)

static const char *TAG = "storage";

static sdmmc_card_t *s_card;
static bool s_mounted;
static int64_t s_last_mount_try;
static int s_next_number = 1;
static uint32_t s_photo_count;
static uint32_t s_avg_size;

static void load_avg(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, NVS_AVG_KEY, &s_avg_size);
        nvs_close(h);
    }
    if (s_avg_size < MIN_PHOTO_BYTES) {
        s_avg_size = 0;
    }
}

static void save_avg(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, NVS_AVG_KEY, s_avg_size);
        nvs_commit(h);
        nvs_close(h);
    }
}

static bool has_suffix_ci(const char *name, const char *suffix)
{
    size_t n = strlen(name), m = strlen(suffix);
    return n >= m && strcasecmp(name + n - m, suffix) == 0;
}

/* Remove leftover temp files and recount photos. Caller holds the SPI lock. */
static void rescan(void)
{
    mkdir(STORAGE_MOUNT "/DCIM", 0775);
    mkdir(STORAGE_PHOTO_DIR, 0775);
    s_photo_count = 0;
    int max = 0;
    DIR *d = opendir(STORAGE_PHOTO_DIR);
    if (!d) {
        s_next_number = 1;
        return;
    }
    struct dirent *e;
    char path[300];
    while ((e = readdir(d)) != NULL) {
        if (has_suffix_ci(e->d_name, ".TMP")) {
            snprintf(path, sizeof(path), STORAGE_PHOTO_DIR "/%s", e->d_name);
            unlink(path);
            ESP_LOGW(TAG, "removed leftover %s", e->d_name);
            continue;
        }
        int v = storage_parse_photo_number(e->d_name);
        if (v > 0) {
            s_photo_count++;
            if (v > max) {
                max = v;
            }
        }
    }
    closedir(d);
    s_next_number = max >= STORAGE_MAX_NUMBER ? -1 : max + 1;
}

static void unmount(void)
{
    if (s_mounted) {
        esp_vfs_fat_sdcard_unmount(STORAGE_MOUNT, s_card);
        s_mounted = false;
        s_card = NULL;
    }
}

static bool try_mount(void)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = BOARD_SPI_HOST;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = BOARD_PIN_SD_CS;
    slot.host_id = BOARD_SPI_HOST;
    const esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 32 * 1024,
        .disk_status_check_enable = true,
    };
    esp_err_t err = esp_vfs_fat_sdspi_mount(STORAGE_MOUNT, &host, &slot, &cfg, &s_card);
    if (err != ESP_OK) {
        s_card = NULL;
        return false;
    }
    s_mounted = true;
    load_avg();
    rescan();
    ESP_LOGI(TAG, "card mounted: %s, %u photos, next %d", s_card->cid.name,
             (unsigned)s_photo_count, s_next_number);
    return true;
}

storage_state_t storage_status(void)
{
    board_spi_lock();
    if (s_mounted && sdmmc_get_status(s_card) != ESP_OK) {
        ESP_LOGW(TAG, "card removed");
        unmount();
    }
    if (!s_mounted) {
        const int64_t now = esp_timer_get_time();
        if (s_last_mount_try == 0 || now - s_last_mount_try >= MOUNT_RETRY_US) {
            s_last_mount_try = now;
            try_mount();
        }
    }
    storage_state_t st = s_mounted ? STORAGE_OK : STORAGE_NO_CARD;
    board_spi_unlock();
    return st;
}

esp_err_t storage_info(storage_info_t *out)
{
    if (storage_status() != STORAGE_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    board_spi_lock();
    esp_err_t err = esp_vfs_fat_info(STORAGE_MOUNT, &out->total_bytes, &out->free_bytes);
    out->photo_count = s_photo_count;
    board_spi_unlock();
    return err;
}

static int cmp_int(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

esp_err_t storage_list_photos(int **numbers, size_t *count)
{
    *numbers = NULL;
    *count = 0;
    if (storage_status() != STORAGE_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    board_spi_lock();
    DIR *d = opendir(STORAGE_PHOTO_DIR);
    if (!d) {
        board_spi_unlock();
        return ESP_OK;
    }
    size_t cap = 64, n = 0;
    int *v = malloc(cap * sizeof(int));
    struct dirent *e;
    while (v && (e = readdir(d)) != NULL) {
        int num = storage_parse_photo_number(e->d_name);
        if (num <= 0) {
            continue;
        }
        if (n == cap) {
            cap *= 2;
            int *nv = realloc(v, cap * sizeof(int));
            if (!nv) {
                free(v);
                v = NULL;
                break;
            }
            v = nv;
        }
        v[n++] = num;
    }
    closedir(d);
    board_spi_unlock();
    if (!v) {
        return ESP_ERR_NO_MEM;
    }
    qsort(v, n, sizeof(int), cmp_int);
    *numbers = v;
    *count = n;
    return ESP_OK;
}

esp_err_t storage_photo_path(int number, char *out, size_t len)
{
    char name[13];
    storage_photo_name(number, name);
    return snprintf(out, len, STORAGE_PHOTO_DIR "/%s", name) < (int)len ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static void tmp_path(int number, char *out, size_t len)
{
    snprintf(out, len, STORAGE_PHOTO_DIR "/IMG_%04d.TMP", number);
}

FILE *storage_begin_photo(int *out_number)
{
    if (storage_status() != STORAGE_OK) {
        return NULL;
    }
    board_spi_lock();
    if (s_next_number < 1) {
        board_spi_unlock();
        return NULL;
    }
    mkdir(STORAGE_MOUNT "/DCIM", 0775);
    mkdir(STORAGE_PHOTO_DIR, 0775);
    char path[64];
    tmp_path(s_next_number, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (f) {
        *out_number = s_next_number;
    } else {
        ESP_LOGE(TAG, "open %s failed: %d", path, errno);
    }
    board_spi_unlock();
    return f;
}

esp_err_t storage_commit_photo(FILE *f, int number)
{
    board_spi_lock();
    bool ok = fflush(f) == 0;
    long size = ftell(f);
    ok = ok && fsync(fileno(f)) == 0;
    ok = (fclose(f) == 0) && ok;
    char tmp[64], final[64];
    tmp_path(number, tmp, sizeof(tmp));
    storage_photo_path(number, final, sizeof(final));
    if (ok && rename(tmp, final) == 0) {
        s_photo_count++;
        s_next_number = number >= STORAGE_MAX_NUMBER ? -1 : number + 1;
        if (size >= MIN_PHOTO_BYTES) {
            s_avg_size = s_avg_size ? (s_avg_size * 3 + (uint32_t)size) / 4 : (uint32_t)size;
            save_avg();
        }
        board_spi_unlock();
        return ESP_OK;
    }
    unlink(tmp);
    board_spi_unlock();
    ESP_LOGE(TAG, "commit of photo %d failed", number);
    return ESP_FAIL;
}

void storage_abort_photo(FILE *f, int number)
{
    board_spi_lock();
    if (f) {
        fclose(f);
    }
    char tmp[64];
    tmp_path(number, tmp, sizeof(tmp));
    unlink(tmp);
    board_spi_unlock();
}

esp_err_t storage_delete_photo(int number)
{
    if (storage_status() != STORAGE_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    char path[64];
    storage_photo_path(number, path, sizeof(path));
    board_spi_lock();
    esp_err_t err = unlink(path) == 0 ? ESP_OK : ESP_FAIL;
    if (err == ESP_OK && s_photo_count > 0) {
        s_photo_count--;
    }
    board_spi_unlock();
    return err;
}

esp_err_t storage_delete_all_photos(void)
{
    int *nums;
    size_t n;
    esp_err_t err = storage_list_photos(&nums, &n);
    if (err != ESP_OK) {
        return err;
    }
    for (size_t i = 0; i < n && err == ESP_OK; i++) {
        err = storage_delete_photo(nums[i]);
    }
    free(nums);
    return err;
}

esp_err_t storage_format(void)
{
    if (storage_status() != STORAGE_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    board_spi_lock();
    esp_err_t err = esp_vfs_fat_sdcard_format(STORAGE_MOUNT, s_card);
    if (err == ESP_OK) {
        rescan();
    } else {
        ESP_LOGE(TAG, "format failed: %s", esp_err_to_name(err));
    }
    board_spi_unlock();
    return err;
}

uint32_t storage_photos_remaining_now(void)
{
    storage_info_t info;
    if (storage_info(&info) != ESP_OK || s_next_number < 1) {
        return 0;
    }
    return storage_photos_remaining(info.free_bytes, s_avg_size);
}

bool storage_numbers_left(void)
{
    return s_next_number >= 1;
}
