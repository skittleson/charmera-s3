#pragma once

#include <stdint.h>

#include "esp_err.h"

/* Largest preview: the screen in either orientation (240x320 = 320x240 pixels). */
#define PREVIEW_PIXELS (240 * 320)

/* Decode a JPEG file upright (by its EXIF orientation), scaled to fit out_w x out_h and
   letterboxed on black. RGB565 native order; out holds out_w * out_h pixels. */
esp_err_t jpeg_preview_load(const char *path, uint16_t *out, int out_w, int out_h);

/* Allocate the decoder buffer now (at boot). */
void jpeg_preview_reserve(void);
