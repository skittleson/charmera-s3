#pragma once

#include "esp_err.h"
#include "looks.h"

typedef enum {
    PIPE_OK,
    PIPE_NO_CARD,
    PIPE_CARD_FULL,
    PIPE_CAPTURE_FAILED,
    PIPE_ENCODE_FAILED,
    PIPE_WRITE_FAILED,
} pipe_result_t;

/* Progress 0..100, called from the calling task while the photo is processed. */
typedef void (*pipe_progress_cb)(int percent, void *ctx);

/* Called once right after the sensor frame is captured (the shutter moment), with an
   upright 240x320 RGB565 thumbnail of it, so the UI can freeze on it at once. */
typedef void (*pipe_captured_cb)(const uint16_t *thumb_240x320, void *ctx);

/* Allocate the pipeline's working buffers. Call once at boot, before camera_init(),
   so the camera's large frame buffer always finds contiguous PSRAM later. */
esp_err_t pipeline_init(void);

/* Capture a full-resolution photo, apply the look, encode JPEG and save it. */
pipe_result_t pipeline_take_photo(look_t look, pipe_captured_cb on_captured, pipe_progress_cb on_progress,
                                  void *ctx, int *out_number);

const char *pipeline_result_text(pipe_result_t r);
