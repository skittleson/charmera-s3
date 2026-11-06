#include "looks.h"

#include <assert.h>
#include <stdlib.h>

#include "looks_internal.h"

static const char *const NAMES[LOOK_COUNT] = {
    "Normal",       "Cool Tone",   "Warm Tone",     "Black & White",
    "Pixel Red",    "Pixel Yellow", "Pixel Blue",   "Pixel Gray",
    "Sticker Frame", "PC Frame",   "Film Frame",    "Charmera Frame",
};

/* dark, light */
static const uint8_t PALETTES[4][2][3] = {
    {{90, 10, 20}, {255, 96, 96}},     /* red */
    {{110, 78, 0}, {255, 226, 64}},    /* yellow */
    {{12, 30, 110}, {112, 182, 255}},  /* blue */
    {{48, 48, 48}, {212, 212, 212}},   /* gray */
};

const char *look_name(look_t l)
{
    return (l >= 0 && l < LOOK_COUNT) ? NAMES[l] : "?";
}

bool look_is_software(look_t l)
{
    return l > LOOK_NORMAL && l < LOOK_COUNT;
}

static inline uint8_t clamp8(int v)
{
    return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v);
}

/* Per-channel gains in 1/256 and offsets: cool pushes blue, warm pushes amber. */
static void tone(uint8_t *rgb, size_t n, int gr, int gg, int gb, int ob)
{
    for (size_t i = 0; i < n; i++, rgb += 3) {
        rgb[0] = clamp8((rgb[0] * gr) >> 8);
        rgb[1] = clamp8((rgb[1] * gg) >> 8);
        rgb[2] = clamp8(((rgb[2] * gb) >> 8) + ob);
    }
}

static void mono(uint8_t *rgb, size_t n)
{
    for (size_t i = 0; i < n; i++, rgb += 3) {
        const int y = (77 * rgb[0] + 150 * rgb[1] + 29 * rgb[2]) >> 8;
        const uint8_t v = clamp8(((y - 128) * 300 >> 8) + 128); /* ~1.17x contrast */
        rgb[0] = rgb[1] = rgb[2] = v;
    }
}

look_t look_next(look_t l)
{
    return (look_t)((l + 1) % LOOK_COUNT);
}

look_t look_prev(look_t l)
{
    return (look_t)((l + LOOK_COUNT - 1) % LOOK_COUNT);
}

static int s_threshold = 128;

void look_set_pixel_threshold(int luma)
{
    s_threshold = luma < 1 ? 1 : (luma > 254 ? 254 : luma);
}

int look_mean_luma_rgb888(const uint8_t *rgb, int n)
{
    uint64_t sum = 0;
    for (int i = 0; i < n; i++, rgb += 3) {
        sum += (77u * rgb[0] + 150u * rgb[1] + 29u * rgb[2]) >> 8;
    }
    return n ? (int)(sum / n) : 128;
}

int look_mean_luma_rgb565(const uint16_t *px, int n)
{
    uint64_t sum = 0;
    for (int i = 0; i < n; i++) {
        const uint16_t v = px[i];
        sum += (77u * (((v >> 11) & 0x1f) << 3) + 150u * (((v >> 5) & 0x3f) << 2) + 29u * ((v & 0x1f) << 3)) >> 8;
    }
    return n ? (int)(sum / n) : 128;
}

int look_pixel_block(int img_w)
{
    /* 80 cells across at every size, so the viewfinder and the photo match. */
    int b = img_w / 80;
    return b < 2 ? 2 : b;
}

static void pixelate(const uint8_t pal[2][3], uint8_t *rgb, int w, int h, int y0, int img_h, int b)
{
    (void)img_h;
    assert(y0 % b == 0);
    for (int cy = 0; cy < h; cy += b) {
        const int ch = (cy + b <= h) ? b : h - cy;
        for (int cx = 0; cx < w; cx += b) {
            const int cw = (cx + b <= w) ? b : w - cx;
            uint32_t sum = 0;
            for (int y = cy; y < cy + ch; y++) {
                const uint8_t *p = rgb + ((size_t)y * w + cx) * 3;
                for (int x = 0; x < cw; x++, p += 3) {
                    sum += (77u * p[0] + 150u * p[1] + 29u * p[2]) >> 8;
                }
            }
            const uint8_t *c = pal[(int)(sum / (uint32_t)(cw * ch)) >= s_threshold ? 1 : 0];
            for (int y = cy; y < cy + ch; y++) {
                uint8_t *p = rgb + ((size_t)y * w + cx) * 3;
                for (int x = 0; x < cw; x++, p += 3) {
                    p[0] = c[0];
                    p[1] = c[1];
                    p[2] = c[2];
                }
            }
        }
    }
}

void look_apply_rgb888(look_t l, uint8_t *rgb, int strip_w, int strip_h, int y0, int img_w,
                       int img_h)
{
    const size_t n = (size_t)strip_w * strip_h;
    if (l == LOOK_COOL) {
        tone(rgb, n, 218, 246, 296, 10);
    } else if (l == LOOK_WARM) {
        tone(rgb, n, 290, 264, 205, 0);
    } else if (l == LOOK_BW) {
        mono(rgb, n);
    } else if (l >= LOOK_PIXEL_RED && l <= LOOK_PIXEL_GRAY) {
        const int b = look_pixel_block(img_w);
        assert(strip_h % b == 0 || y0 + strip_h == img_h);
        pixelate(PALETTES[l - LOOK_PIXEL_RED], rgb, strip_w, strip_h, y0, img_h, b);
    } else if (l >= LOOK_FRAME_STICKER && l < LOOK_COUNT) {
        frame_apply(l, rgb, strip_w, strip_h, y0, img_w, img_h);
    }
}

static uint8_t *s_buf;
static size_t s_cap;

bool look_reserve_rgb565(int w, int h)
{
    const size_t need = (size_t)w * h * 3;
    if (need > s_cap) {
        free(s_buf);
        s_buf = malloc(need);
        s_cap = s_buf ? need : 0;
    }
    return s_buf != NULL;
}

/* Pixel cells for a sensor-orientation strip. Portrait columns are sensor rows (cells
   start at row 0); portrait rows run right-to-left across the sensor (cells start at
   the sensor's right edge). */
static void pixelate_sensor(const uint8_t pal[2][3], uint8_t *rgb, int w, int h, int b)
{
    for (int cy = 0; cy < h; cy += b) {
        const int ch = (cy + b <= h) ? b : h - cy;
        for (int right = w; right > 0; right -= b) {
            const int cx = right - b > 0 ? right - b : 0;
            const int cw = right - cx;
            uint32_t sum = 0;
            for (int y = cy; y < cy + ch; y++) {
                const uint8_t *p = rgb + ((size_t)y * w + cx) * 3;
                for (int x = 0; x < cw; x++, p += 3) {
                    sum += (77u * p[0] + 150u * p[1] + 29u * p[2]) >> 8;
                }
            }
            const uint8_t *c = pal[(int)(sum / (uint32_t)(cw * ch)) >= s_threshold ? 1 : 0];
            for (int y = cy; y < cy + ch; y++) {
                uint8_t *p = rgb + ((size_t)y * w + cx) * 3;
                for (int x = 0; x < cw; x++, p += 3) {
                    p[0] = c[0];
                    p[1] = c[1];
                    p[2] = c[2];
                }
            }
        }
    }
}

void look_apply_rgb888_sensor(look_t l, uint8_t *rgb, int sensor_w, int rows, int y0, int img_sensor_w,
                              int img_sensor_h)
{
    const size_t n = (size_t)sensor_w * rows;
    if (l == LOOK_COOL) {
        tone(rgb, n, 218, 246, 296, 10);
    } else if (l == LOOK_WARM) {
        tone(rgb, n, 290, 264, 205, 0);
    } else if (l == LOOK_BW) {
        mono(rgb, n);
    } else if (l >= LOOK_PIXEL_RED && l <= LOOK_PIXEL_GRAY) {
        const int b = look_pixel_block(img_sensor_h); /* portrait width */
        assert(y0 % b == 0);
        assert(rows % b == 0 || y0 + rows == img_sensor_h);
        pixelate_sensor(PALETTES[l - LOOK_PIXEL_RED], rgb, sensor_w, rows, b);
    } else if (l >= LOOK_FRAME_STICKER && l < LOOK_COUNT) {
        frame_apply_sensor(l, rgb, sensor_w, rows, y0, img_sensor_w, img_sensor_h);
    }
}

void look_apply_rgb565(look_t l, uint16_t *px, int w, int h)
{
    if (!look_is_software(l) || !look_reserve_rgb565(w, h)) {
        return;
    }
    if (l >= LOOK_PIXEL_RED && l <= LOOK_PIXEL_GRAY) {
        look_set_pixel_threshold(look_mean_luma_rgb565(px, w * h));
    }
    uint8_t *buf = s_buf;
    for (int i = 0; i < w * h; i++) {
        const uint16_t v = px[i];
        buf[i * 3 + 0] = (uint8_t)(((v >> 11) & 0x1f) << 3);
        buf[i * 3 + 1] = (uint8_t)(((v >> 5) & 0x3f) << 2);
        buf[i * 3 + 2] = (uint8_t)((v & 0x1f) << 3);
    }
    look_apply_rgb888(l, buf, w, h, 0, w, h);
    for (int i = 0; i < w * h; i++) {
        px[i] = (uint16_t)(((buf[i * 3] >> 3) << 11) | ((buf[i * 3 + 1] >> 2) << 5) |
                           (buf[i * 3 + 2] >> 3));
    }
}
