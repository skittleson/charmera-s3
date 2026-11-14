#include "storage.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "board.h"
#include "driver/sdspi_host.h"
#include "esp_heap_caps.h"
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
/* Which photo numbers are video clips (IMG_####.AVI) rather than photos (.JPG).
   PSRAM: 9999 bits, set at rescan so the gallery can label and play clips. */
static uint8_t *s_video_bits;
/* Where photos are written: the photo folder, or a time-lapse run's own folder. */
static char s_dir[32] = STORAGE_PHOTO_DIR;
static bool s_lapse;
static int s_lapse_next;

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
    if (!s_video_bits) {
        s_video_bits = heap_caps_malloc((STORAGE_MAX_NUMBER + 7) / 8, MALLOC_CAP_SPIRAM);
    }
    if (s_video_bits) {
        memset(s_video_bits, 0, (STORAGE_MAX_NUMBER + 7) / 8);
    }
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
            if (s_video_bits && has_suffix_ci(e->d_name, ".AVI")) {
                s_video_bits[(v - 1) / 8] |= (uint8_t)(1u << ((v - 1) % 8));
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
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED; /* 40 MHz; the card negotiates down if needed */
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
    ESP_LOGI(TAG, "card mounted: %s @ %d kHz, %u photos, next %d", s_card->cid.name, s_card->real_freq_khz,
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
    snprintf(out, len, "%s/IMG_%04d.TMP", s_dir, number);
}

static void final_path(int number, char *out, size_t len)
{
    char name[13];
    storage_photo_name(number, name);
    snprintf(out, len, "%s/%s", s_dir, name);
}

esp_err_t storage_thumb_path(int number, char *out, size_t len)
{
    char name[13];
    storage_photo_name(number, name);
    name[9] = 'T'; /* IMG_0001.JPG -> IMG_0001.THM */
    name[10] = 'H';
    name[11] = 'M';
    const int n = snprintf(out, len, STORAGE_PHOTO_DIR "/%s", name);
    return n > 0 && n < (int)len ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

bool storage_file_size(const char *path, size_t *out)
{
    board_spi_lock();
    FILE *f = fopen(path, "rb");
    bool ok = false;
    if (f) {
        ok = fseek(f, 0, SEEK_END) == 0;
        if (ok) {
            *out = (size_t)ftell(f);
        }
        fclose(f);
    }
    board_spi_unlock();
    return ok;
}

/* Best effort: the photo saves even if the sidecar fails. */
void storage_thumb_write(int number, const uint8_t *jpeg, size_t len)
{
    char path[64];
    /* Gallery photos only: time-lapse frames never show in the gallery. */
    if (s_lapse || storage_thumb_path(number, path, sizeof(path)) != ESP_OK || storage_status() != STORAGE_OK) {
        return;
    }
    char tmp[64];
    /* 8.3 and ending in .TMP: the rescan at mount deletes it if power dies mid-write. */
    snprintf(tmp, sizeof(tmp), STORAGE_PHOTO_DIR "/THM_%04d.TMP", number);
    board_spi_lock();
    FILE *f = fopen(tmp, "wb");
    bool ok = f && fwrite(jpeg, 1, len, f) == len;
    if (f) {
        fflush(f);
        fclose(f);
    }
    if (ok && rename(tmp, path) != 0) {
        ok = false;
    }
    if (!ok) {
        unlink(tmp);
    }
    board_spi_unlock();
}

static int current_number(void)
{
    return s_lapse ? s_lapse_next : s_next_number;
}

FILE *storage_begin_photo(int *out_number)
{
    if (storage_status() != STORAGE_OK) {
        return NULL;
    }
    board_spi_lock();
    const int number = current_number();
    if (number < 1) {
        board_spi_unlock();
        return NULL;
    }
    mkdir(STORAGE_MOUNT "/DCIM", 0775);
    mkdir(s_dir, 0775); /* also re-creates a run's folder after a card swap */
    char path[64];
    tmp_path(number, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (f) {
        *out_number = number;
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
    final_path(number, final, sizeof(final));
    if (ok && rename(tmp, final) == 0) {
        const int next = number >= STORAGE_MAX_NUMBER ? -1 : number + 1;
        if (s_lapse) {
            s_lapse_next = next; /* time-lapse frames stay out of the gallery count */
        } else {
            s_photo_count++;
            s_next_number = next;
        }
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

/* ---------- video clips (IMG_####.AVI, same number space as photos) ---------- */

static void clip_tmp_path(int number, char *out, size_t len)
{
    snprintf(out, len, "%s/VIO_%04d.TMP", s_dir, number);
}

static void clip_final_path(int number, char *out, size_t len)
{
    char name[13];
    storage_clip_name(number, name);
    snprintf(out, len, "%s/%s", s_dir, name);
}

bool storage_is_video(int number)
{
    return s_video_bits && number >= 1 && number <= STORAGE_MAX_NUMBER &&
           (s_video_bits[(number - 1) / 8] & (1u << ((number - 1) % 8)));
}

esp_err_t storage_clip_path(int number, char *out, size_t len)
{
    char name[13];
    storage_clip_name(number, name);
    return snprintf(out, len, STORAGE_PHOTO_DIR "/%s", name) < (int)len ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

/* Open the next VIO_####.TMP in the photo folder for a clip. NULL if no card or
   numbering is used up. */
FILE *storage_video_begin(int *out_number)
{
    if (storage_status() != STORAGE_OK || s_lapse) {
        return NULL; /* clips are gallery photos, never time-lapse frames */
    }
    board_spi_lock();
    const int number = current_number();
    if (number < 1) {
        board_spi_unlock();
        return NULL;
    }
    mkdir(STORAGE_MOUNT "/DCIM", 0775);
    mkdir(s_dir, 0775);
    char path[64];
    clip_tmp_path(number, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (f) {
        *out_number = number;
    } else {
        ESP_LOGE(TAG, "open %s failed: %d", path, errno);
    }
    board_spi_unlock();
    return f;
}

/* Rename VIO_####.TMP -> IMG_####.AVI, marking the number as a clip. The caller has
   already closed the file (the AVI writer does). */
esp_err_t storage_video_commit(int number)
{
    board_spi_lock();
    char tmp[64], final[64];
    clip_tmp_path(number, tmp, sizeof(tmp));
    clip_final_path(number, final, sizeof(final));
    if (rename(tmp, final) == 0) {
        const int next = number >= STORAGE_MAX_NUMBER ? -1 : number + 1;
        s_photo_count++;
        s_next_number = next;
        if (s_video_bits) {
            s_video_bits[(number - 1) / 8] |= (uint8_t)(1u << ((number - 1) % 8));
        }
        board_spi_unlock();
        return ESP_OK;
    }
    unlink(tmp);
    board_spi_unlock();
    ESP_LOGE(TAG, "commit of clip %d failed", number);
    return ESP_FAIL;
}

/* Delete the VIO temp: a power loss or an error mid-clip leaves no gallery entry. */
void storage_video_abort(int number)
{
    board_spi_lock();
    char tmp[64];
    clip_tmp_path(number, tmp, sizeof(tmp));
    unlink(tmp);
    board_spi_unlock();
}

esp_err_t storage_delete_photo(int number)
{
    if (storage_status() != STORAGE_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    char path[64], clip[64];
    storage_photo_path(number, path, sizeof(path));
    storage_clip_path(number, clip, sizeof(clip));
    board_spi_lock();
    /* A number is either a photo or a clip; unlink both so it works for either. */
    esp_err_t err = (unlink(path) == 0 || unlink(clip) == 0) ? ESP_OK : ESP_FAIL;
    char thm[64];
    if (storage_thumb_path(number, thm, sizeof(thm)) == ESP_OK) {
        unlink(thm); /* best effort: no sidecar is no error */
    }
    if (s_video_bits && number >= 1 && number <= STORAGE_MAX_NUMBER) {
        s_video_bits[(number - 1) / 8] &= (uint8_t)~(1u << ((number - 1) % 8));
    }
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
    return current_number() >= 1;
}

esp_err_t storage_lapse_begin(char *out_folder, size_t len)
{
    if (storage_status() != STORAGE_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    board_spi_lock();
    mkdir(STORAGE_MOUNT "/DCIM", 0775);
    size_t cap = 16, n = 0;
    char (*names)[9] = malloc(cap * sizeof(*names));
    DIR *d = opendir(STORAGE_MOUNT "/DCIM");
    struct dirent *e;
    while (names && d && (e = readdir(d)) != NULL) {
        if (storage_parse_folder_number(e->d_name) < 0) {
            continue;
        }
        if (n == cap) {
            cap *= 2;
            char (*nn)[9] = realloc(names, cap * sizeof(*names));
            if (!nn) {
                break;
            }
            names = nn;
        }
        strlcpy(names[n++], e->d_name, sizeof(names[0]));
    }
    if (d) {
        closedir(d);
    }
    if (!names) {
        board_spi_unlock();
        return ESP_ERR_NO_MEM;
    }
    const char *ptrs[n ? n : 1];
    for (size_t i = 0; i < n; i++) {
        ptrs[i] = names[i];
    }
    const int folder = storage_next_folder_number(ptrs, n);
    free(names);
    if (folder < 0) {
        board_spi_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    char name[9];
    storage_lapse_folder_name(folder, name);
    snprintf(s_dir, sizeof(s_dir), STORAGE_MOUNT "/DCIM/%s", name);
    esp_err_t err = (mkdir(s_dir, 0775) == 0 || errno == EEXIST) ? ESP_OK : ESP_FAIL;
    if (err == ESP_OK) {
        s_lapse = true;
        s_lapse_next = 1;
        if (out_folder) {
            strlcpy(out_folder, name, len);
        }
        ESP_LOGI(TAG, "time-lapse folder %s", s_dir);
    } else {
        strlcpy(s_dir, STORAGE_PHOTO_DIR, sizeof(s_dir));
        ESP_LOGE(TAG, "mkdir %s failed: %d", s_dir, errno);
    }
    board_spi_unlock();
    return err;
}

void storage_lapse_end(void)
{
    board_spi_lock();
    s_lapse = false;
    strlcpy(s_dir, STORAGE_PHOTO_DIR, sizeof(s_dir));
    board_spi_unlock();
}
