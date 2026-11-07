#include "pipeline.h"

#include <stdlib.h>
#include <string.h>

#include "camera.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_jpeg_enc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "exif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "storage.h"
#include <sys/time.h>

#define JPEG_QUALITY   92 /* firmware-encoded photos (looks, OV2640) */
#define ENC_BLOCK_ROWS 16 /* 4:2:0 MCU height */
/* SD writes go via DMA-capable internal RAM, 32 KB at a time. */
#define WRITE_CHUNK    (32 * 1024)

/* EXIF orientation for photos stored in sensor (landscape) orientation: the stored
   image's first row is the picture's left side and its first column the bottom, so
   viewers turn it 90 degrees counter-clockwise (EXIF value 8). */
#define EXIF_ORIENT_PORTRAIT 8

#define EXIF_ORIENT_UPRIGHT 1
#define EXIF_MAX 1024

static const char *TAG = "pipeline";

static char s_software[32] = "charmera-s3";
static char s_serial[13];
static bool s_clock_set;
static int s_clock_offset_min;

static uint8_t *s_strip;
static size_t s_strip_bytes;
static uint8_t *s_dec_block; /* one decoder block (JPEG-sensor path) */
static size_t s_dec_block_bytes;
static uint8_t *s_obuf;
static size_t s_obuf_bytes;
static uint16_t *s_thumb;
static uint8_t *s_chunk;

static int gcd(int a, int b)
{
    while (b) {
        int t = a % b;
        a = b;
        b = t;
    }
    return a;
}

static int lcm(int a, int b)
{
    return a / gcd(a, b) * b;
}

/* Rows per strip for an image whose pixel-filter cells are `cell` rows tall: a multiple
   of the cell and the encoder block, so cells never straddle two strips. */
static int strip_rows(int cell, int min_rows)
{
    int rows = lcm(ENC_BLOCK_ROWS, cell);
    while (rows < min_rows) {
        rows *= 2;
    }
    return rows;
}

/* Raw path: strips are portrait rows (photo width = sensor height). */
static int raw_strip_rows(void)
{
    return strip_rows(look_pixel_block(camera_photo_width()), 128);
}

/* JPEG path: strips are sensor rows; pixel cells use the portrait width (= sensor height). */
static int sensor_strip_rows(void)
{
    return strip_rows(look_pixel_block(camera_full_height()), 32);
}

esp_err_t pipeline_init(const char *software)
{
    if (software) {
        strlcpy(s_software, software, sizeof(s_software));
    }
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(s_serial, sizeof(s_serial), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
                 mac[5]);
    }
    if (camera_full_is_jpeg()) {
        const int w = camera_full_width();
        s_strip_bytes = (size_t)w * sensor_strip_rows() * 3;
        s_dec_block_bytes = (size_t)w * 16 * 3;
        s_dec_block = jpeg_calloc_align(s_dec_block_bytes, 16);
        s_obuf_bytes = 2600 * 1024; /* re-encoded 5 MP at q92 */
    } else {
        const int w = camera_photo_width() > 0 ? camera_photo_width() : 1200;
        s_strip_bytes = (size_t)w * raw_strip_rows() * 3;
        s_obuf_bytes = 1600 * 1024;
    }
    s_strip = jpeg_calloc_align(s_strip_bytes, 16);
    s_obuf = heap_caps_malloc(s_obuf_bytes, MALLOC_CAP_SPIRAM);
    s_thumb = jpeg_calloc_align(240 * 320 * 2, 16);
    s_chunk = heap_caps_malloc(WRITE_CHUNK, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_strip || !s_obuf || !s_thumb || !s_chunk || (camera_full_is_jpeg() && !s_dec_block) ||
        !look_reserve_rgb565(240, 320)) {
        ESP_LOGE(TAG, "buffer allocation failed");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "buffers: strip %u, out %u, largest free PSRAM %u", (unsigned)s_strip_bytes,
             (unsigned)s_obuf_bytes, (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    return ESP_OK;
}

uint8_t *pipeline_scratch(size_t *len)
{
    *len = s_obuf_bytes;
    return s_obuf;
}

const char *pipeline_result_text(pipe_result_t r)
{
    switch (r) {
    case PIPE_OK: return "Saved";
    case PIPE_NO_CARD: return "No card";
    case PIPE_CARD_FULL: return "Card full";
    case PIPE_CAPTURE_FAILED: return "Camera error";
    case PIPE_ENCODE_FAILED: return "Couldn't process photo";
    case PIPE_WRITE_FAILED: return "Couldn't save photo";
    }
    return "?";
}

typedef struct {
    jpeg_enc_handle_t enc;
    int block;
    int len;
    jpeg_error_t last;
} encoder_t;

static bool enc_open(encoder_t *e, int w, int h)
{
    jpeg_enc_config_t cfg = DEFAULT_JPEG_ENC_CONFIG();
    cfg.width = w;
    cfg.height = h;
    cfg.src_type = JPEG_PIXEL_FORMAT_RGB888;
    cfg.subsampling = JPEG_SUBSAMPLE_420;
    cfg.quality = JPEG_QUALITY;
    cfg.task_enable = true; /* Huffman stage on the other core */
    cfg.hfm_task_priority = 5;
    cfg.hfm_task_core = 1;
    memset(e, 0, sizeof(*e));
    e->last = JPEG_ERR_FAIL;
    if (jpeg_enc_open(&cfg, &e->enc) != JPEG_ERR_OK) {
        return false;
    }
    e->block = jpeg_enc_get_block_size(e->enc);
    return e->block > 0 && e->block % (w * 3) == 0;
}

/* Feed whole encoder blocks from rgb (bytes must be a multiple of the block size). */
static bool enc_feed(encoder_t *e, const uint8_t *rgb, size_t bytes)
{
    for (size_t off = 0; off < bytes; off += e->block) {
        e->last = jpeg_enc_process_with_block(e->enc, rgb + off, e->block, s_obuf, (int)s_obuf_bytes, &e->len);
        if (e->last < JPEG_ERR_OK) {
            ESP_LOGE(TAG, "encode error %d", e->last);
            return false;
        }
    }
    return true;
}

/* The last block must report a finished image, and output that (nearly) fills the
   buffer means the encoder ran out of room: never save a truncated JPEG. */
static bool enc_finish(encoder_t *e)
{
    jpeg_enc_close(e->enc);
    if (e->last != JPEG_ERR_OK || e->len <= 0 || (size_t)e->len >= s_obuf_bytes - 4096) {
        ESP_LOGE(TAG, "encode incomplete: last=%d len=%d", e->last, e->len);
        return false;
    }
    return true;
}

/* OV2640: raw sensor frame -> upright portrait strips -> look -> JPEG. */
static pipe_result_t encode_raw(const camera_fb_t *fb, look_t look, pipe_progress_cb on_progress, void *ctx,
                                int *out_len)
{
    const int w = camera_photo_width(), h = camera_photo_height();
    const int rows = raw_strip_rows();
    encoder_t e = {0};
    if ((size_t)w * rows * 3 > s_strip_bytes || !enc_open(&e, w, h)) {
        if (e.enc) {
            jpeg_enc_close(e.enc);
        }
        return PIPE_ENCODE_FAILED;
    }
    for (int y0 = 0; y0 < h; y0 += rows) {
        const int n = (h - y0) < rows ? (h - y0) : rows;
        camera_photo_rows_rgb888(fb, y0, n, s_strip);
        look_apply_rgb888(look, s_strip, w, n, y0, w, h);
        if (!enc_feed(&e, s_strip, (size_t)w * n * 3)) {
            jpeg_enc_close(e.enc);
            return PIPE_ENCODE_FAILED;
        }
        if (on_progress) {
            on_progress((y0 + n) * 100 / h, ctx);
        }
        vTaskDelay(1); /* let the idle task run (watchdog) */
    }
    if (!enc_finish(&e)) {
        return PIPE_ENCODE_FAILED;
    }
    *out_len = e.len;
    return PIPE_OK;
}

/* OV5640 with a look: decode the sensor JPEG in blocks, apply the look in sensor
   orientation, re-encode. Stays landscape; the EXIF tag makes it portrait. */
static pipe_result_t encode_sensor_jpeg(const camera_fb_t *fb, look_t look, pipe_progress_cb on_progress,
                                        void *ctx, int *out_len)
{
    const int w = camera_full_width(), h = camera_full_height();
    const int rows = sensor_strip_rows();
    jpeg_dec_config_t dc = DEFAULT_JPEG_DEC_CONFIG();
    dc.output_type = JPEG_PIXEL_FORMAT_RGB888;
    dc.block_enable = true;
    jpeg_dec_handle_t dec = NULL;
    encoder_t e = {0};
    pipe_result_t res = PIPE_ENCODE_FAILED;
    jpeg_dec_io_t io = {.inbuf = fb->buf, .inbuf_len = (int)fb->len, .outbuf = s_dec_block};
    jpeg_dec_header_info_t info;
    int block_len = 0, count = 0;
    if (jpeg_dec_open(&dc, &dec) != JPEG_ERR_OK || jpeg_dec_parse_header(dec, &io, &info) != JPEG_ERR_OK ||
        info.width != w || info.height != h || jpeg_dec_get_outbuf_len(dec, &block_len) != JPEG_ERR_OK ||
        block_len <= 0 || (size_t)block_len > s_dec_block_bytes || block_len % (w * 3) != 0 ||
        jpeg_dec_get_process_count(dec, &count) != JPEG_ERR_OK || (size_t)w * rows * 3 > s_strip_bytes ||
        !enc_open(&e, w, h)) {
        ESP_LOGE(TAG, "sensor JPEG setup failed (%dx%d, block %d)", info.width, info.height, block_len);
        goto done;
    }
    const int block_rows = block_len / (w * 3);
    if (rows % block_rows != 0) {
        goto done;
    }
    int y0 = 0, filled = 0;
    for (int i = 0; i < count; i++) {
        if (jpeg_dec_process(dec, &io) != JPEG_ERR_OK) {
            ESP_LOGE(TAG, "decode error at block %d", i);
            goto done;
        }
        memcpy(s_strip + (size_t)filled * w * 3, s_dec_block, (size_t)block_len);
        filled += block_rows;
        const bool last = i == count - 1;
        if (filled == rows || last) {
            if (y0 + filled > h) {
                filled = h - y0;
            }
            look_apply_rgb888_sensor(look, s_strip, w, filled, y0, w, h, ORIENT_PORTRAIT); /* Task 4 */
            if (!enc_feed(&e, s_strip, (size_t)w * filled * 3)) {
                goto done;
            }
            y0 += filled;
            filled = 0;
            if (on_progress) {
                on_progress(y0 * 100 / h, ctx);
            }
            vTaskDelay(1); /* let the idle task run (watchdog) */
        }
    }
    if (y0 == h) {
        const bool ok = enc_finish(&e); /* closes the encoder */
        e.enc = NULL;
        if (ok) {
            *out_len = e.len;
            res = PIPE_OK;
        }
    }
done:
    if (e.enc) {
        jpeg_enc_close(e.enc);
    }
    if (dec) {
        jpeg_dec_close(dec);
    }
    return res;
}

static size_t write_all(FILE *f, const uint8_t *data, size_t len)
{
    size_t done = 0;
    while (done < len) {
        const size_t n = (len - done) < WRITE_CHUNK ? (len - done) : WRITE_CHUNK;
        memcpy(s_chunk, data + done, n);
        if (fwrite(s_chunk, 1, n, f) != n) {
            break;
        }
        done += n;
    }
    return done;
}

void pipeline_set_clock(int64_t epoch, int offset_min)
{
    const struct timeval tv = {.tv_sec = (time_t)epoch};
    settimeofday(&tv, NULL);
    s_clock_offset_min = offset_min;
    s_clock_set = true;
    ESP_LOGI(TAG, "clock set: %lld, UTC%+d min", (long long)epoch, offset_min);
}

bool pipeline_clock_is_set(void)
{
    return s_clock_set;
}

/* EXIF for one photo: stored size and orientation, plus the caller's text. */
static size_t build_exif(uint8_t *out, size_t cap, const pipe_meta_t *meta, uint16_t orientation, int w, int h)
{
    char model[40], dt[20], off[7];
    snprintf(model, sizeof(model), "Charmera-S3 (%s)", camera_sensor_name());
    if (s_clock_set) {
        exif_format_datetime((int64_t)time(NULL), s_clock_offset_min, dt, off);
    }
    const exif_info_t info = {
        .make = "Charmera-S3",
        .model = model,
        .software = s_software,
        .serial = s_serial[0] ? s_serial : NULL,
        .description = meta ? meta->description : NULL,
        .comment = meta ? meta->comment : NULL,
        .datetime = s_clock_set ? dt : NULL,
        .offset = s_clock_set ? off : NULL,
        .orientation = orientation,
        .width = (uint32_t)w,
        .height = (uint32_t)h,
    };
    return exif_build(&info, out, cap);
}

/* Save a JPEG (starting with FFD8) with an EXIF segment inserted after SOI. */
static pipe_result_t save_jpeg(const uint8_t *jpeg, size_t len, const pipe_meta_t *meta, uint16_t orientation,
                               int w, int h, int *out_number)
{
    static uint8_t exif[EXIF_MAX];
    const size_t exif_len = build_exif(exif, sizeof(exif), meta, orientation, w, h);
    if (!exif_len) {
        ESP_LOGW(TAG, "EXIF didn't fit; saving without it");
    }
    storage_info_t info;
    if (storage_info(&info) != ESP_OK) {
        return PIPE_NO_CARD;
    }
    if (info.free_bytes < len + 64 * 1024) {
        return PIPE_CARD_FULL;
    }
    int number = 0;
    FILE *f = storage_begin_photo(&number);
    if (!f) {
        return storage_status() == STORAGE_OK ? PIPE_WRITE_FAILED : PIPE_NO_CARD;
    }
    const bool ok = write_all(f, jpeg, 2) == 2 && write_all(f, exif, exif_len) == exif_len &&
                    write_all(f, jpeg + 2, len - 2) == len - 2;
    if (!ok) {
        storage_abort_photo(f, number);
        return storage_status() == STORAGE_OK ? PIPE_WRITE_FAILED : PIPE_NO_CARD;
    }
    if (storage_commit_photo(f, number) != ESP_OK) {
        return storage_status() == STORAGE_OK ? PIPE_WRITE_FAILED : PIPE_NO_CARD;
    }
    *out_number = number;
    return PIPE_OK;
}

pipe_result_t pipeline_take_photo(look_t look, const pipe_meta_t *meta, pipe_captured_cb on_captured,
                                  pipe_progress_cb on_progress, void *ctx, int *out_number)
{
    if (storage_status() != STORAGE_OK) {
        return PIPE_NO_CARD;
    }
    if (!storage_numbers_left() || storage_photos_remaining_now() == 0) {
        return PIPE_CARD_FULL;
    }
    const int64_t t0 = esp_timer_get_time();
    const esp_err_t ferr = camera_focus(1500);
    const int64_t tf = esp_timer_get_time();
    camera_fb_t *fb = camera_capture_full();
    ESP_LOGD(TAG, "focus %s %lld ms, capture %lld ms", esp_err_to_name(ferr), (tf - t0) / 1000,
             (esp_timer_get_time() - tf) / 1000);
    if (!fb) {
        return PIPE_CAPTURE_FAILED;
    }
    const int64_t t1 = esp_timer_get_time();
    /* The thumbnail also sets the pixel-filter threshold from this scene, so the
       full-size photo splits its tones exactly like the screen does. */
    if (camera_photo_thumbnail(fb, s_thumb) == ESP_OK) {
        look_apply_rgb565(look, s_thumb, 240, 320);
        if (on_captured) {
            on_captured(s_thumb, ctx);
        }
    }
    const bool sensor_jpeg = fb->format == PIXFORMAT_JPEG;
    pipe_result_t res;
    int len = 0;
    int number = 0;
    if (sensor_jpeg && look == LOOK_NORMAL) {
        /* Best quality: the sensor's own JPEG, untouched. */
        res = save_jpeg(fb->buf, fb->len, meta, EXIF_ORIENT_PORTRAIT, fb->width, fb->height, &number);
        len = (int)fb->len;
        camera_capture_done(fb);
    } else {
        /* Sensor-JPEG looks stay landscape (EXIF turns them); raw photos are rotated upright. */
        const int w = sensor_jpeg ? camera_full_width() : camera_photo_width();
        const int h = sensor_jpeg ? camera_full_height() : camera_photo_height();
        res = sensor_jpeg ? encode_sensor_jpeg(fb, look, on_progress, ctx, &len)
                          : encode_raw(fb, look, on_progress, ctx, &len);
        camera_capture_done(fb);
        if (res == PIPE_OK) {
            res = save_jpeg(s_obuf, len, meta, sensor_jpeg ? EXIF_ORIENT_PORTRAIT : EXIF_ORIENT_UPRIGHT, w, h,
                            &number);
        }
    }
    if (res != PIPE_OK) {
        return res;
    }
    ESP_LOGI(TAG, "IMG_%04d: %s, %s, %d bytes, capture %lld ms, total %lld ms", number, camera_sensor_name(),
             look_name(look), len, (t1 - t0) / 1000, (esp_timer_get_time() - t0) / 1000);
    if (out_number) {
        *out_number = number;
    }
    return PIPE_OK;
}
