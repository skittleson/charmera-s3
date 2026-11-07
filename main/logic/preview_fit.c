#include "preview_fit.h"

static int round8_up(int v)
{
    return (v + 7) & ~7;
}

bool preview_fit(int w, int h, bool turned, int out_w, int out_h, int max_pixels, int *dec_w, int *dec_h,
                 int *show_w, int *show_h)
{
    if (w <= 0 || h <= 0 || out_w <= 0 || out_h <= 0) {
        return false;
    }
    /* As shown: fit the turned (or not) photo inside the screen, aspect kept. */
    const int aw = turned ? h : w, ah = turned ? w : h;
    int sw = out_w, sh = (int)((long)ah * out_w / aw);
    if (sh > out_h) {
        sh = out_h;
        sw = (int)((long)aw * out_h / ah);
    }
    /* Decode at about the shown size, but never below 1/8 (the decoder's limit). */
    int a = turned ? sh : sw, b = turned ? sw : sh; /* stored orientation */
    const int min_a = round8_up((w + 7) / 8), min_b = round8_up((h + 7) / 8);
    a = round8_up(a < min_a ? min_a : a);
    b = round8_up(b < min_b ? min_b : b);
    if (a > w) {
        a = w & ~7;
    }
    if (b > h) {
        b = h & ~7;
    }
    if (a < 8 || b < 8 || a * 8 < w || b * 8 < h || (long)a * b > max_pixels) {
        return false;
    }
    *dec_w = a;
    *dec_h = b;
    *show_w = sw;
    *show_h = sh;
    return true;
}
