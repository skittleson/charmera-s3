#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    LOOK_NORMAL,
    LOOK_COOL,
    LOOK_WARM,
    LOOK_BW,
    LOOK_PIXEL_RED,
    LOOK_PIXEL_YELLOW,
    LOOK_PIXEL_BLUE,
    LOOK_PIXEL_GRAY,
    LOOK_FRAME_STICKER,
    LOOK_FRAME_PC,
    LOOK_FRAME_FILM,
    LOOK_FRAME_CHARMERA,
    LOOK_COUNT
} look_t;

const char *look_name(look_t l);

/* Every look except Normal is pixel processing (the OV2640 sensor effects were unusable). */
bool look_is_software(look_t l);

look_t look_next(look_t l);
look_t look_prev(look_t l);

/* Pixel-filter cell size for an image this wide: img_w / 80, at least 4. */
int look_pixel_block(int img_w);

/* Apply a look to a horizontal strip of an RGB888 image. The strip is strip_w x strip_h
   and its first row is row y0 of an img_w x img_h image. y0 must be a multiple of
   look_pixel_block(img_w), and strip_h too unless the strip ends the image. Applying
   strip by strip gives exactly the same result as applying to the whole image. */
void look_apply_rgb888(look_t l, uint8_t *rgb, int strip_w, int strip_h, int y0, int img_w,
                       int img_h);

/* Viewfinder version on a whole RGB565 image (native uint16 order). Same maths as
   look_apply_rgb888 after expanding each channel by left shift. */
void look_apply_rgb565(look_t l, uint16_t *px, int w, int h);

/* Pixel filters split each cell into the light or dark tone at this luma (0..255).
   Use the scene's mean so dim and bright scenes both keep two tones. Default 128. */
void look_set_pixel_threshold(int luma);
int look_mean_luma_rgb888(const uint8_t *rgb, int n);
int look_mean_luma_rgb565(const uint16_t *px, int n);

/* Allocate look_apply_rgb565's scratch buffer now (call at boot, before large
   allocations, to avoid fragmenting memory later). */
bool look_reserve_rgb565(int w, int h);
