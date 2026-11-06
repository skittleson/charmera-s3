#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_camera.h"
#include "esp_err.h"
#include "looks.h"

/* Held upright with USB-C down, the world's "up" runs along the sensor's long axis,
   so the viewfinder and photos are portrait: the sensor frame turned 90 degrees. */
#define CAMERA_PREVIEW_W 240
#define CAMERA_PREVIEW_H 320

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

/* Portrait photo size (sensor frame turned 90 degrees). */
int camera_photo_width(void);  /* = camera_full_height() */
int camera_photo_height(void); /* = camera_full_width() */

/* Next viewfinder frame: 240x320 RGB565 (native uint16 order), upright. NULL on
   timeout or when there is no camera. Valid until the next call. */
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

/* Raw (OV2640) captures: rows [y0, y0+rows) of the upright portrait photo as RGB888. */
void camera_photo_rows_rgb888(const camera_fb_t *fb, int y0, int rows, uint8_t *out);

/* Any capture scaled to the 240x320 portrait screen, RGB565 native order. */
esp_err_t camera_photo_thumbnail(const camera_fb_t *fb, uint16_t *out);

/* Turn a 320x240 landscape RGB565 (native) image into the 240x320 portrait view, the
   same way the sensor is turned. */
void camera_rotate_to_portrait(const uint16_t *landscape_320x240, uint16_t *out_240x320);
