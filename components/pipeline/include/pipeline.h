#pragma once

#include <stddef.h>
#include <stdint.h>

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
   so the camera's large frame buffer always finds contiguous PSRAM later.
   software goes into each photo's EXIF, e.g. "charmera-s3 0.1.0". */
esp_err_t pipeline_init(const char *software);

/* Per-photo EXIF text: ImageDescription and UserComment (either may be NULL). */
typedef struct {
    const char *description;
    const char *comment;
} pipe_meta_t;

/* Capture a full-resolution photo, apply the look, encode JPEG and save it. */
pipe_result_t pipeline_take_photo(look_t look, const pipe_meta_t *meta, pipe_captured_cb on_captured,
                                  pipe_progress_cb on_progress, void *ctx, int *out_number);

/* Wall clock for EXIF dates. The board has no RTC: until this is called (from the
   Bluetooth remote) photos carry no date. Lost at power-off. */
void pipeline_set_clock(int64_t epoch, int offset_min);
bool pipeline_clock_is_set(void);

const char *pipeline_result_text(pipe_result_t r);

/* The pipeline's JPEG buffer, for other code to borrow when no photo is being taken
   (the gallery reads files into it). */
uint8_t *pipeline_scratch(size_t *len);

/* Downscale an upright RGB565 image (w x h) and encode a small JPEG into out (cap
   bytes). Returns the encoded size, 0 on failure. Used for the .THM sidecars. */
size_t pipeline_encode_thumb_jpeg(const uint16_t *rgb565, int w, int h, uint8_t *out, size_t cap);

/* Encode an upright RGB565 frame (w x h) with `look` applied as a full-size JPEG into
   out (cap bytes). Returns the encoded size, 0 on failure. Used for video frames; reuses
   the pipeline's PSRAM encoder output (s_obuf), so no new internal-RAM buffers. */
size_t pipeline_encode_video_jpeg(const uint16_t *rgb565, int w, int h, look_t look, uint8_t *out, size_t cap);

/* Encode the shutter thumbnail for `number` and write its .THM sidecar. Best effort:
   absent sidecars simply make the remote list that photo without a preview. */
void pipeline_thumb_sidecar(int number, const uint16_t *thumb, int w, int h);
