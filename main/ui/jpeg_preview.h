#pragma once

#include <stdint.h>

#include "esp_err.h"

#define PREVIEW_W 240
#define PREVIEW_H 320

/* Decode a JPEG file scaled to fit 240x320 (RGB565, native order, letterboxed on
   black). out must hold 240*320 pixels. */
esp_err_t jpeg_preview_load(const char *path, uint16_t *out);
