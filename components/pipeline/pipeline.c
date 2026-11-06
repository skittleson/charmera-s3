#include "pipeline.h"

#include <stdlib.h>
#include <string.h>

#include "camera.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_enc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "storage.h"

#define JPEG_QUALITY   92
#define OUT_BUF_BYTES  (1600 * 1024)
#define ENC_BLOCK_ROWS 16 /* 4:2:0 MCU height */
/* SD writes go via DMA-capable internal RAM, 32 KB at a time. */
#define WRITE_CHUNK    (32 * 1024)

#define MAX_PHOTO_W 1944 /* room for an OV5640 photo (1944 x 2592 portrait) */

static const char *TAG = "pipeline";

static uint8_t *s_strip;
static size_t s_strip_bytes;
static uint8_t *s_obuf;
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

/* Rows per strip: a multiple of both the encoder block and the pixel-filter cell, so
   pixel cells never straddle two strips. */
static int strip_rows(int img_w)
{
    const int cell = look_pixel_block(img_w);
    int rows = ENC_BLOCK_ROWS / gcd(ENC_BLOCK_ROWS, cell) * cell;
    while (rows < 128) {
        rows *= 2;
    }
    return rows;
}

esp_err_t pipeline_init(void)
{
    s_strip_bytes = (size_t)MAX_PHOTO_W * strip_rows(MAX_PHOTO_W) * 3;
    s_strip = jpeg_calloc_align(s_strip_bytes, 16);
    s_obuf = heap_caps_malloc(OUT_BUF_BYTES, MALLOC_CAP_SPIRAM);
    s_thumb = heap_caps_malloc(240 * 320 * 2, MALLOC_CAP_SPIRAM);
    s_chunk = heap_caps_malloc(WRITE_CHUNK, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_strip || !s_obuf || !s_thumb || !s_chunk || !look_reserve_rgb565(240, 320)) {
        ESP_LOGE(TAG, "buffer allocation failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

uint8_t *pipeline_scratch(size_t *len)
{
    *len = OUT_BUF_BYTES;
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

static pipe_result_t encode(const camera_fb_t *fb, look_t look, pipe_progress_cb on_progress, void *ctx,
                            uint8_t **out, int *out_len)
{
    const int w = camera_photo_width(), h = camera_photo_height();
    jpeg_enc_config_t cfg = DEFAULT_JPEG_ENC_CONFIG();
    cfg.width = w;
    cfg.height = h;
    cfg.src_type = JPEG_PIXEL_FORMAT_RGB888;
    cfg.subsampling = JPEG_SUBSAMPLE_420;
    cfg.quality = JPEG_QUALITY;
    cfg.task_enable = true; /* Huffman stage on the other core */
    cfg.hfm_task_priority = 5;
    cfg.hfm_task_core = 1;
    jpeg_enc_handle_t enc = NULL;
    if (jpeg_enc_open(&cfg, &enc) != JPEG_ERR_OK) {
        return PIPE_ENCODE_FAILED;
    }
    const int block = jpeg_enc_get_block_size(enc);
    const int rows = strip_rows(w);
    uint8_t *strip = s_strip;
    uint8_t *obuf = s_obuf;
    pipe_result_t res = PIPE_OK;
    if (!strip || (size_t)w * rows * 3 > s_strip_bytes || block <= 0 || block % (w * 3) != 0) {
        ESP_LOGE(TAG, "setup failed: block %d", block);
        res = PIPE_ENCODE_FAILED;
        goto done;
    }
    int len = 0;
    int64_t t_rows = 0, t_look = 0, t_enc = 0;
    for (int y0 = 0; y0 < h && res == PIPE_OK; y0 += rows) {
        const int n = (h - y0) < rows ? (h - y0) : rows;
        int64_t a = esp_timer_get_time();
        camera_photo_rows_rgb888(fb, y0, n, strip);
        int64_t b = esp_timer_get_time();
        look_apply_rgb888(look, strip, w, n, y0, w, h);
        int64_t c = esp_timer_get_time();
        t_rows += b - a;
        t_look += c - b;
        for (int off = 0; off < w * n * 3; off += block) {
            jpeg_error_t e = jpeg_enc_process_with_block(enc, strip + off, block, obuf, OUT_BUF_BYTES, &len);
            if (e < JPEG_ERR_OK) {
                ESP_LOGE(TAG, "encode error %d at row %d", e, y0);
                res = PIPE_ENCODE_FAILED;
                break;
            }
        }
        t_enc += esp_timer_get_time() - c;
        if (on_progress) {
            on_progress((y0 + n) * 100 / h, ctx);
        }
    }
    ESP_LOGD(TAG, "rows %lld ms, look %lld ms, jpeg %lld ms", t_rows / 1000, t_look / 1000, t_enc / 1000);
    if (res == PIPE_OK) {
        *out = obuf;
        *out_len = len;
    }
done:
    jpeg_enc_close(enc);
    return res;
}


static size_t write_all(FILE *f, const uint8_t *data, size_t len)
{
    uint8_t *chunk = s_chunk;
    size_t done = 0;
    while (done < len) {
        const size_t n = (len - done) < WRITE_CHUNK ? (len - done) : WRITE_CHUNK;
        memcpy(chunk, data + done, n);
        if (fwrite(chunk, 1, n, f) != n) {
            break;
        }
        done += n;
    }
    return done;
}

pipe_result_t pipeline_take_photo(look_t look, pipe_captured_cb on_captured, pipe_progress_cb on_progress,
                                  void *ctx, int *out_number)
{
    if (storage_status() != STORAGE_OK) {
        return PIPE_NO_CARD;
    }
    if (!storage_numbers_left() || storage_photos_remaining_now() == 0) {
        return PIPE_CARD_FULL;
    }
    const int64_t t0 = esp_timer_get_time();
    camera_focus(1500);
    camera_fb_t *fb = camera_capture_full();
    if (!fb) {
        return PIPE_CAPTURE_FAILED;
    }
    const int64_t t1 = esp_timer_get_time();
    /* The thumbnail also sets the pixel-filter threshold from this scene, so the
       full-size photo splits its tones exactly like the viewfinder did. */
    camera_photo_thumbnail(fb, s_thumb);
    look_apply_rgb565(look, s_thumb, 240, 320);
    if (on_captured) {
        on_captured(s_thumb, ctx);
    }
    uint8_t *jpeg = NULL;
    int jpeg_len = 0;
    pipe_result_t res = encode(fb, look, on_progress, ctx, &jpeg, &jpeg_len);
    camera_capture_done(fb);
    const int64_t t2 = esp_timer_get_time();
    if (res != PIPE_OK) {
        return res;
    }
    int number = 0;
    FILE *f = storage_begin_photo(&number);
    if (!f) {
        return storage_status() == STORAGE_OK ? PIPE_WRITE_FAILED : PIPE_NO_CARD;
    }
    const size_t written = write_all(f, jpeg, jpeg_len);
    if (written != (size_t)jpeg_len) {
        storage_abort_photo(f, number);
        return PIPE_WRITE_FAILED;
    }
    if (storage_commit_photo(f, number) != ESP_OK) {
        return PIPE_WRITE_FAILED;
    }
    ESP_LOGI(TAG, "IMG_%04d: %s, %d bytes, capture %lld ms, encode %lld ms, write %lld ms", number,
             look_name(look), jpeg_len, (t1 - t0) / 1000, (t2 - t1) / 1000,
             (esp_timer_get_time() - t2) / 1000);
    if (out_number) {
        *out_number = number;
    }
    return PIPE_OK;
}
