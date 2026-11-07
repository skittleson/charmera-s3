#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_camera.h"
#include "esp_err.h"
#include "looks.h"
#include "orient.h"

/* The viewfinder and photos follow how the camera is held (orient.h): portrait is
   the sensor frame turned 90 degrees, landscape is the sensor frame as-is or turned 180.
   The viewfinder is 240x320 or 320x240; the buffer is the same size either way. */
#define CAMERA_PREVIEW_W 240
#define CAMERA_PREVIEW_H 320
#define CAMERA_PREVIEW_PIXELS (CAMERA_PREVIEW_W * CAMERA_PREVIEW_H)

/* How the camera is held. Takes effect for the next viewfinder frame and capture. */
void camera_set_orientation(orient_t o);
orient_t camera_orientation(void);
/* Viewfinder size for the current orientation. */
int camera_preview_w(void);
int camera_preview_h(void);

/* Detects the fitted sensor (OV2640 or OV5640) and picks its best capture mode:
   - OV2640: 1600x1200 raw RGB565 (its JPEG is broken); photos are rotated and encoded
     by the firmware to 1200x1600.
   - OV5640: 2560x1920 sensor JPEG with autofocus; saved as-is with an EXIF
     orientation tag so viewers show it portrait. */
esp_err_t camera_init(void);

/* "OV2640", "OV5640" or "none". */
const char *camera_sensor_name(void);

/* Full-resolution frame in sensor orientation (landscape). */
int camera_full_width(void);
int camera_full_height(void);
/* True if full-resolution frames are JPEG (OV5640); false if raw RGB565 (OV2640). */
bool camera_full_is_jpeg(void);

/* Photo size as seen, for the current orientation (raw captures are stored this way). */
int camera_photo_width(void);
int camera_photo_height(void);

/* Next viewfinder frame: camera_preview_w() x camera_preview_h() RGB565 (native uint16
   order), upright. NULL on timeout or when there is no camera. Valid until the next call. */
uint16_t *camera_preview_frame(void);

esp_err_t camera_set_look(look_t l);

bool camera_has_af(void);
/* Single autofocus in viewfinder mode. ESP_OK when focused or when there is no AF. */
esp_err_t camera_focus(uint32_t timeout_ms);

/* Switch to full resolution and grab one frame (see camera_full_is_jpeg()). Caller
   must call camera_capture_done(). */
camera_fb_t *camera_capture_full(void);
void camera_capture_done(camera_fb_t *fb);

/* Power the sensor up or down (PWDN). Off = no streaming, lens released. The UI keeps
   it on only while the viewfinder is on screen. */
void camera_power(bool on);
bool camera_is_on(void);

/* Raw (OV2640) captures: rows [y0, y0+rows) of the upright photo as RGB888. */
void camera_photo_rows_rgb888(const camera_fb_t *fb, int y0, int rows, uint8_t *out);

/* Any capture scaled to the viewfinder size, upright, RGB565 native order. */
esp_err_t camera_photo_thumbnail(const camera_fb_t *fb, uint16_t *out);

/* Turn a 320x240 sensor-orientation RGB565 image into the view for o. */
void camera_rotate_to_view(const uint16_t *landscape_320x240, orient_t o, uint16_t *out);
