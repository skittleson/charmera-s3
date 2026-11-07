#pragma once

#include <stdbool.h>

/* How to show a w x h stored photo (turned = EXIF 5..8 swaps its sides) inside an
   out_w x out_h screen: decode it at dec_w x dec_h (stored orientation; multiples of 8,
   at least 1/8 of the photo, at most max_pixels - the decoder's limits), then scale to
   show_w x show_h (as shown, aspect kept, fits the screen). False if impossible. */
bool preview_fit(int w, int h, bool turned, int out_w, int out_h, int max_pixels, int *dec_w, int *dec_h,
                 int *show_w, int *show_h);
