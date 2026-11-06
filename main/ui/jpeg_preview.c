#include "jpeg_preview.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "camera.h"
#include "esp_jpeg_dec.h"
#include "esp_log.h"
#include "pipeline.h"

static const char *TAG = "preview";

static uint16_t *s_dec_out;
static uint16_t *s_land;

/* Largest size that fits w x h inside 240x320, keeps the aspect, is a multiple of 8 and
   is no smaller than 1/8 of the source (the decoder's limits). */
static bool fit(int w, int h, int *sw, int *sh)
{
    int tw = PREVIEW_W, th = h * PREVIEW_W / w;
    if (th > PREVIEW_H) {
        th = PREVIEW_H;
        tw = w * PREVIEW_H / h;
    }
    tw &= ~7;
    th &= ~7;
    if (tw < 8 || th < 8 || tw * 8 < w || th * 8 < h || tw > w || th > h) {
        return false;
    }
    *sw = tw;
    *sh = th;
    return true;
}

void jpeg_preview_reserve(void)
{
    if (!s_dec_out) {
        s_dec_out = jpeg_calloc_align(PREVIEW_W * PREVIEW_H * 2, 16);
    }
    if (!s_land) {
        s_land = jpeg_calloc_align(320 * 240 * 2, 16);
    }
}

/* EXIF orientation from the APP1 segment we write right after SOI (0 if none). */
static int exif_orientation(const uint8_t *b, size_t len)
{
    if (len < 34 || b[2] != 0xff || b[3] != 0xe1 || memcmp(b + 6, "Exif\0\0MM", 8) != 0) {
        return 0;
    }
    if (b[20] == 0x01 && b[21] == 0x12) { /* first IFD0 entry is Orientation */
        return b[29];
    }
    return 0;
}

esp_err_t jpeg_preview_load(const char *path, uint16_t *out)
{
    size_t cap;
    uint8_t *buf = pipeline_scratch(&cap);
    jpeg_preview_reserve();
    if (!buf || !s_dec_out) {
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
    const bool portrait_tag = exif_orientation(buf, len) == 8;

    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_dec_handle_t dec = NULL;
    if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK) {
        return ESP_FAIL;
    }
    jpeg_dec_io_t io = {.inbuf = buf, .inbuf_len = (int)len};
    jpeg_dec_header_info_t info;
    esp_err_t res = ESP_FAIL;
    int sw = 0, sh = 0;
    if (portrait_tag && jpeg_dec_parse_header(dec, &io, &info) == JPEG_ERR_OK) {
        /* Stored landscape, shown portrait: decode to 320x240 and turn it. */
        jpeg_dec_close(dec);
        dec = NULL;
        if (!s_land) {
            s_land = jpeg_calloc_align(320 * 240 * 2, 16);
        }
        cfg.scale.width = info.width == 320 ? 0 : 320;
        cfg.scale.height = info.height == 240 ? 0 : 240;
        if (s_land && jpeg_dec_open(&cfg, &dec) == JPEG_ERR_OK) {
            io = (jpeg_dec_io_t){.inbuf = buf, .inbuf_len = (int)len, .outbuf = (uint8_t *)s_land};
            if (jpeg_dec_parse_header(dec, &io, &info) == JPEG_ERR_OK && jpeg_dec_process(dec, &io) == JPEG_ERR_OK) {
                camera_rotate_to_portrait(s_land, out);
                res = ESP_OK;
            }
            jpeg_dec_close(dec);
        }
        return res;
    }
    if (jpeg_dec_parse_header(dec, &io, &info) == JPEG_ERR_OK && fit(info.width, info.height, &sw, &sh)) {
        jpeg_dec_close(dec);
        cfg.scale.width = (sw == info.width) ? 0 : sw;
        cfg.scale.height = (sh == info.height) ? 0 : sh;
        if (jpeg_dec_open(&cfg, &dec) == JPEG_ERR_OK) {
            io = (jpeg_dec_io_t){.inbuf = buf, .inbuf_len = (int)len, .outbuf = (uint8_t *)s_dec_out};
            if (jpeg_dec_parse_header(dec, &io, &info) == JPEG_ERR_OK && jpeg_dec_process(dec, &io) == JPEG_ERR_OK) {
                res = ESP_OK;
            }
        } else {
            dec = NULL;
        }
    } else {
        ESP_LOGW(TAG, "%s: unsupported size %dx%d", path, info.width, info.height);
    }
    if (dec) {
        jpeg_dec_close(dec);
    }
    if (res != ESP_OK) {
        return res;
    }
    memset(out, 0, PREVIEW_W * PREVIEW_H * 2);
    const int ox = (PREVIEW_W - sw) / 2, oy = (PREVIEW_H - sh) / 2;
    for (int y = 0; y < sh; y++) {
        memcpy(out + (size_t)(oy + y) * PREVIEW_W + ox, s_dec_out + (size_t)y * sw, sw * 2);
    }
    return ESP_OK;
}
