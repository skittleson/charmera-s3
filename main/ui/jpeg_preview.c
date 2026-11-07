#include "jpeg_preview.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "exif.h"
#include "pipeline.h"

static const char *TAG = "preview";

static uint16_t *s_dec_out; /* the photo decoded at its stored orientation, scaled */

void jpeg_preview_reserve(void)
{
    if (!s_dec_out) {
        s_dec_out = jpeg_calloc_align(PREVIEW_PIXELS * 2, 16);
    }
}

/* Stored pixel shown at display (x, y) for EXIF orientation e of a sw x sh image. */
static void stored_at(int e, int x, int y, int sw, int sh, int *u, int *v)
{
    switch (e) {
    case 2: *u = sw - 1 - x; *v = y; break;
    case 3: *u = sw - 1 - x; *v = sh - 1 - y; break;
    case 4: *u = x; *v = sh - 1 - y; break;
    case 5: *u = y; *v = x; break;
    case 6: *u = y; *v = sh - 1 - x; break;
    case 7: *u = sw - 1 - y; *v = sh - 1 - x; break;
    case 8: *u = sw - 1 - y; *v = x; break;
    default: *u = x; *v = y; break;
    }
}

/* Decoded size (stored orientation) for a w x h photo shown turned (or not) inside
   out_w x out_h: keeps the aspect, multiples of 8, at least 1/8 scale (decoder limits). */
static bool fit(int w, int h, bool turned, int out_w, int out_h, int *sw, int *sh)
{
    const int dw = turned ? h : w, dh = turned ? w : h; /* as shown */
    int tw = out_w, th = dh * out_w / dw;
    if (th > out_h) {
        th = out_h;
        tw = dw * out_h / dh;
    }
    int a = (turned ? th : tw) & ~7, b = (turned ? tw : th) & ~7;
    if (a < 8 || b < 8 || a * 8 < w || b * 8 < h || a > w || b > h) {
        return false;
    }
    *sw = a;
    *sh = b;
    return true;
}

esp_err_t jpeg_preview_load(const char *path, uint16_t *out, int out_w, int out_h)
{
    size_t cap;
    uint8_t *buf = pipeline_scratch(&cap);
    jpeg_preview_reserve();
    if (!buf || !s_dec_out || out_w * out_h > PREVIEW_PIXELS) {
        return ESP_ERR_NO_MEM;
    }
    board_spi_lock();
    FILE *f = fopen(path, "rb");
    size_t len = 0;
    if (f) {
        len = fread(buf, 1, cap, f);
        fclose(f);
    }
    board_spi_unlock();
    if (len == 0 || len == cap) {
        return ESP_FAIL;
    }
    const int e = exif_read_orientation(buf, len);
    const bool turned = e >= 5; /* 5..8 swap width and height */

    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_dec_handle_t dec = NULL;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        return ESP_FAIL;
    }
    jpeg_dec_io_t io = {.inbuf = buf, .inbuf_len = (int)len};
    jpeg_dec_header_info_t info;
    int sw = 0, sh = 0;
    const bool ok_header = jpeg_dec_parse_header(dec, &io, &info) == JPEG_ERR_OK;
    jpeg_dec_close(dec);
    dec = NULL;
    if (!ok_header || !fit(info.width, info.height, turned, out_w, out_h, &sw, &sh)) {
        ESP_LOGW(TAG, "%s: unsupported size %dx%d", path, info.width, info.height);
        return ESP_FAIL;
    }
    cfg.scale.width = (sw == info.width) ? 0 : sw;
    cfg.scale.height = (sh == info.height) ? 0 : sh;
    esp_err_t res = ESP_FAIL;
    if (jpeg_dec_open(&cfg, &dec) == JPEG_ERR_OK) {
        io = (jpeg_dec_io_t){.inbuf = buf, .inbuf_len = (int)len, .outbuf = (uint8_t *)s_dec_out};
        if (jpeg_dec_parse_header(dec, &io, &info) == JPEG_ERR_OK && jpeg_dec_process(dec, &io) == JPEG_ERR_OK) {
            res = ESP_OK;
        }
        jpeg_dec_close(dec);
    }
    if (res != ESP_OK) {
        return res;
    }
    /* Turn it upright into the middle of the output. */
    memset(out, 0, (size_t)out_w * out_h * 2);
    const int dw = turned ? sh : sw, dh = turned ? sw : sh;
    const int ox = (out_w - dw) / 2, oy = (out_h - dh) / 2;
    for (int y = 0; y < dh; y++) {
        uint16_t *row = out + (size_t)(oy + y) * out_w + ox;
        for (int x = 0; x < dw; x++) {
            int u, v;
            stored_at(e, x, y, sw, sh, &u, &v);
            row[x] = s_dec_out[(size_t)v * sw + u];
        }
    }
    return ESP_OK;
}
