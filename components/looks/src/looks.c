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
    return l >= LOOK_PIXEL_RED && l < LOOK_COUNT;
}

look_t look_next(look_t l)
{
    return (look_t)((l + 1) % LOOK_COUNT);
}

look_t look_prev(look_t l)
{
    return (look_t)((l + LOOK_COUNT - 1) % LOOK_COUNT);
}

int look_pixel_block(int img_w)
{
    int b = img_w / 80;
    return b < 4 ? 4 : b;
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
            const uint8_t *c = pal[(sum / (uint32_t)(cw * ch)) >= 128 ? 1 : 0];
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
    if (l >= LOOK_PIXEL_RED && l <= LOOK_PIXEL_GRAY) {
        const int b = look_pixel_block(img_w);
        assert(strip_h % b == 0 || y0 + strip_h == img_h);
        pixelate(PALETTES[l - LOOK_PIXEL_RED], rgb, strip_w, strip_h, y0, img_h, b);
    } else if (l >= LOOK_FRAME_STICKER && l < LOOK_COUNT) {
        frame_apply(l, rgb, strip_w, strip_h, y0, img_w, img_h);
    }
}

void look_apply_rgb565(look_t l, uint16_t *px, int w, int h)
{
    if (!look_is_software(l)) {
        return;
    }
    static uint8_t *buf;
    static size_t cap;
    const size_t need = (size_t)w * h * 3;
    if (need > cap) {
        free(buf);
        buf = malloc(need);
        cap = buf ? need : 0;
        if (!buf) {
            return;
        }
    }
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
