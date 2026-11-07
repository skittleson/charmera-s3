#include <stdbool.h>
#include <string.h>

#include "looks_internal.h"

/* Frames are drawn procedurally from absolute image coordinates, so any strip split
   gives the same picture. All decoration stays within FRAME_REACH * t of the edges,
   where t = the short edge / 32. */
#define FRAME_REACH 6 /* the film rails are about 5 units wide */

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

/* ---- Film Frame: a strip of 35 mm film ----
   Real 35 mm stock is 35 mm wide; the sprocket rails along both long edges are 5.5 mm
   each, with KS perforations 2.8 mm across x 2.0 mm along on a 4.75 mm pitch, and edge
   printing in the outer margin. The film runs along the photo's long side, so the film
   width is the short edge. Distances are tenths of a millimetre of that film. */

#define FILM_RAIL     55  /* 5.5 mm */
#define FILM_HOLE_OUT 13  /* hole starts 1.3 mm from the film edge */
#define FILM_HOLE_AX  28  /* hole size across the film */
#define FILM_HOLE_AL  20  /* hole size along the film */
#define FILM_PITCH    475 /* hundredths of a mm: 4.75 mm */
#define FILM_RADIUS   5   /* hole corner radius 0.5 mm */
#define FILM_TEXT_AT  3   /* edge print from 0.3 mm ... */
#define FILM_TEXT_H   8   /* ... 0.8 mm tall */

/* 5x7 glyphs for the edge print, one byte per row, bit 4 = left column. */
static const char FILM_TEXT[] = "CHARMERA 400  >12  >12A  ";
static uint8_t film_glyph_row(char c, int row)
{
    static const struct {
        char c;
        uint8_t rows[7];
    } GLYPHS[] = {
        {'C', {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e}}, {'H', {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}},
        {'A', {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}}, {'R', {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11}},
        {'M', {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11}}, {'E', {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f}},
        {'4', {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}}, {'0', {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}},
        {'1', {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e}}, {'2', {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}},
        {'>', {0x10, 0x18, 0x1c, 0x1e, 0x1c, 0x18, 0x10}}, /* the frame-number arrow, filled */
    };
    for (size_t i = 0; i < sizeof(GLYPHS) / sizeof(GLYPHS[0]); i++) {
        if (GLYPHS[i].c == c) {
            return GLYPHS[i].rows[row];
        }
    }
    return 0; /* space */
}

/* True if (along, across) inside a w x h rectangle with corner radius r is in the shape. */
static bool rounded_rect(int along, int across, int w, int h, int r)
{
    if (along < 0 || across < 0 || along >= w || across >= h) {
        return false;
    }
    const int cx = along < r ? r : (along >= w - r ? w - 1 - r : along);
    const int cy = across < r ? r : (across >= h - r ? h - 1 - r : across);
    const int dx = along - cx, dy = across - cy;
    return dx * dx + dy * dy <= r * r;
}

static bool film(const geom_t *g, int x, int y, uint8_t *p)
{
    const bool tall = g->h >= g->w;
    const int S = tall ? g->w : g->h; /* film width */
    const int across_px = tall ? x : y;
    const int along = tall ? y : x;
#define FMM(tenths) (S * (tenths) / 350)
    const int rail = FMM(FILM_RAIL);
    int d; /* distance from the nearer film edge */
    bool first_edge;
    if (across_px < rail) {
        d = across_px;
        first_edge = true;
    } else if (across_px >= S - rail) {
        d = S - 1 - across_px;
        first_edge = false;
    } else {
        return false; /* the picture */
    }
    /* Sprocket holes: backlit, rounded, centred on the pitch. */
    const int pitch = S * FILM_PITCH / 3500;
    const int hole_al = FMM(FILM_HOLE_AL), hole_ax = FMM(FILM_HOLE_AX);
    const int radius = FMM(FILM_RADIUS) < 1 ? 1 : FMM(FILM_RADIUS);
    const int in_pitch = pitch > 0 ? (along + (hole_al - pitch) / 2 % pitch + pitch) % pitch : 0;
    if (rounded_rect(in_pitch, d - FMM(FILM_HOLE_OUT), hole_al, hole_ax, radius)) {
        put(p, 238, 232, 214);
        return true;
    }
    /* Edge print in the outer margin of the first rail, reading along the film. */
    const int px = FMM(FILM_TEXT_H) / 7 < 1 ? 1 : FMM(FILM_TEXT_H) / 7;
    const int ty = d - FMM(FILM_TEXT_AT);
    if (first_edge && ty >= 0 && ty < 7 * px) {
        const int cell = 6 * px, len = (int)sizeof(FILM_TEXT) - 1;
        const int col = along / px % 6, ch = along / cell % len;
        /* text runs along the film; glyph rows run away from the edge */
        /* Landscape: letter tops face the film edge. Portrait (text turned clockwise to
           read downwards): letter tops face the picture, so rows count back from it. */
        const int row = tall ? 6 - ty / px : ty / px;
        if (col < 5 && (film_glyph_row(FILM_TEXT[ch], row) >> (4 - col) & 1)) {
            put(p, 242, 150, 40);
            return true;
        }
    }
    put(p, 22, 19, 17); /* film base */
    return true;
#undef FMM
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

typedef bool (*draw_fn)(const geom_t *, int, int, uint8_t *);

static draw_fn drawer(look_t l)
{
    switch (l) {
    case LOOK_FRAME_STICKER: return sticker;
    case LOOK_FRAME_PC: return pc_window;
    case LOOK_FRAME_FILM: return film;
    case LOOK_FRAME_CHARMERA: return charmera;
    default: return NULL;
    }
}

static int frame_unit(int w, int h)
{
    const int short_edge = w < h ? w : h;
    return short_edge / 32 < 4 ? 4 : short_edge / 32;
}

void frame_apply_sensor(look_t l, uint8_t *rgb, int sensor_w, int rows, int y0, int img_sensor_w,
                        int img_sensor_h, orient_t o)
{
    draw_fn draw = drawer(l);
    if (!draw) {
        return;
    }
    int vw, vh;
    orient_view_size(o, img_sensor_w, img_sensor_h, &vw, &vh);
    const int t = frame_unit(vw, vh);
    const geom_t g = {vw, vh, t};
    const int reach = FRAME_REACH * t;
    for (int r = 0; r < rows; r++) {
        const int sy = y0 + r;
        uint8_t *row = rgb + (size_t)r * sensor_w * 3;
        /* A sensor row is one view column (portrait) or one view row (landscape); the
           view coordinate along the sensor row runs from one view edge to the other. */
        int vx0, vy0;
        orient_sensor_to_view(o, 0, sy, img_sensor_w, img_sensor_h, &vx0, &vy0);
        const int across = o == ORIENT_PORTRAIT ? vx0 : vy0;
        const int span = o == ORIENT_PORTRAIT ? vw : vh;
        const bool whole = across < reach || across >= span - reach;
        for (int sx = 0; sx < sensor_w; sx++) {
            if (!whole && sx == reach && sensor_w - reach > reach) {
                sx = sensor_w - reach; /* skip the middle: only the two view edges */
            }
            int vx, vy;
            orient_sensor_to_view(o, sx, sy, img_sensor_w, img_sensor_h, &vx, &vy);
            draw(&g, vx, vy, row + sx * 3);
        }
    }
}

void frame_apply(look_t l, uint8_t *rgb, int strip_w, int strip_h, int y0, int img_w, int img_h)
{
    draw_fn draw = drawer(l);
    if (!draw) {
        return;
    }
    const int t = frame_unit(img_w, img_h);
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
