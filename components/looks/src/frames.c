#include <stdbool.h>
#include <string.h>

#include "looks_internal.h"

/* Frames are drawn procedurally from absolute image coordinates, so any strip split
   gives the same picture. All decoration stays within FRAME_REACH * t of the edges,
   where t = img_w / 32. */
#define FRAME_REACH 3

typedef struct {
    int w, h, t;
} geom_t;

static void put(uint8_t *p, uint8_t r, uint8_t g, uint8_t b)
{
    p[0] = r;
    p[1] = g;
    p[2] = b;
}

static bool in_circle(int x, int y, int cx, int cy, int r)
{
    const int dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}

static bool sticker(const geom_t *g, int x, int y, uint8_t *p)
{
    static const uint8_t COLOURS[6][3] = {
        {255, 92, 160}, {255, 214, 0}, {0, 200, 230}, {120, 220, 60}, {255, 140, 0}, {160, 90, 230},
    };
    const int t = g->t, r = t * 8 / 5;
    const int cx[6] = {t, g->w / 2, g->w - 1 - t, t, g->w / 2, g->w - 1 - t};
    const int cy[6] = {t, t, t, g->h - 1 - t, g->h - 1 - t, g->h - 1 - t};
    for (int i = 0; i < 6; i++) {
        if (in_circle(x, y, cx[i], cy[i], r)) {
            if (in_circle(x, y, cx[i] - r / 3, cy[i] - r / 3, r / 4)) {
                put(p, 255, 255, 255);
            } else {
                put(p, COLOURS[i][0], COLOURS[i][1], COLOURS[i][2]);
            }
            return true;
        }
    }
    if (x < t || y < t || x >= g->w - t || y >= g->h - t) {
        put(p, 255, 255, 255);
        return true;
    }
    return false;
}

static bool pc_window(const geom_t *g, int x, int y, uint8_t *p)
{
    const int t = g->t, side = t * 3 / 5, bar = 2 * t, line = t / 10 < 1 ? 1 : t / 10;
    if (y < bar) {
        /* Title bar with three window buttons on the right. */
        const int bs = t * 7 / 5, gap = t / 4, top = (bar - bs) / 2;
        for (int i = 0; i < 3; i++) {
            const int bx = g->w - side - gap - (i + 1) * (bs + gap);
            if (x >= bx && x < bx + bs && y >= top && y < top + bs) {
                const bool edge = x < bx + line || x >= bx + bs - line || y < top + line ||
                                  y >= top + bs - line;
                if (edge) {
                    put(p, 40, 40, 40);
                } else {
                    put(p, 192, 192, 192);
                }
                return true;
            }
        }
        put(p, 0, 0, 128);
        return true;
    }
    if (x < side || x >= g->w - side || y >= g->h - side) {
        put(p, 192, 192, 192);
        return true;
    }
    if (x < side + line || x >= g->w - side - line || y < bar + line || y >= g->h - side - line) {
        put(p, 64, 64, 64);
        return true;
    }
    return false;
}

static bool film(const geom_t *g, int x, int y, uint8_t *p)
{
    const int t = g->t, band = 2 * t, side = t / 2;
    const bool top = y < band, bottom = y >= g->h - band;
    if (top || bottom) {
        const int by = top ? 0 : g->h - band;
        const int hole_top = by + (band - t) / 2;
        const int pitch = 2 * t, off = t / 2;
        const bool in_hole = y >= hole_top && y < hole_top + t && ((x + off) % pitch) < t;
        if (in_hole) {
            put(p, 240, 235, 220);
        } else {
            put(p, 16, 16, 16);
        }
        return true;
    }
    if (x < side || x >= g->w - side) {
        put(p, 16, 16, 16);
        return true;
    }
    return false;
}

static bool charmera(const geom_t *g, int x, int y, uint8_t *p)
{
    static const uint8_t STRIPES[3][3] = {{230, 40, 40}, {250, 140, 20}, {255, 205, 0}};
    const int t = g->t, band = 2 * t;
    if (y >= g->h - band) {
        const int sh = t / 3 < 1 ? 1 : t / 3;
        const int s0 = g->h - band + (band - 3 * sh) / 2;
        if (x >= t && x < g->w - t && y >= s0 && y < s0 + 3 * sh) {
            const uint8_t *c = STRIPES[(y - s0) / sh];
            put(p, c[0], c[1], c[2]);
        } else {
            put(p, 250, 240, 215);
        }
        return true;
    }
    if (x < t || y < t || x >= g->w - t) {
        put(p, 250, 240, 215);
        return true;
    }
    return false;
}

void frame_apply(look_t l, uint8_t *rgb, int strip_w, int strip_h, int y0, int img_w, int img_h)
{
    bool (*draw)(const geom_t *, int, int, uint8_t *) = NULL;
    switch (l) {
    case LOOK_FRAME_STICKER: draw = sticker; break;
    case LOOK_FRAME_PC: draw = pc_window; break;
    case LOOK_FRAME_FILM: draw = film; break;
    case LOOK_FRAME_CHARMERA: draw = charmera; break;
    default: return;
    }
    const int t = img_w / 32 < 4 ? 4 : img_w / 32;
    const geom_t g = {img_w, img_h, t};
    const int reach = FRAME_REACH * t;
    for (int sy = 0; sy < strip_h; sy++) {
        const int y = y0 + sy;
        uint8_t *row = rgb + (size_t)sy * strip_w * 3;
        if (y < reach || y >= img_h - reach) {
            for (int x = 0; x < strip_w; x++) {
                draw(&g, x, y, row + x * 3);
            }
        } else {
            for (int x = 0; x < reach && x < strip_w; x++) {
                draw(&g, x, y, row + x * 3);
            }
            for (int x = strip_w - reach; x < strip_w; x++) {
                if (x >= reach) {
                    draw(&g, x, y, row + x * 3);
                }
            }
        }
    }
}
