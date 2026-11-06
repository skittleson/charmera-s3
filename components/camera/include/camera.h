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

esp_err_t camera_init(void);

/* Sensor frame size at full resolution (landscape, as the sensor sees it):
   1600x1200 for OV2640, 2592x1944 for OV5640. Photos are this rotated to portrait. */
int camera_full_width(void);
int camera_full_height(void);

/* Next viewfinder frame: CAMERA_PREVIEW_W x CAMERA_PREVIEW_H RGB565 (native uint16
   order), already rotated upright. NULL on timeout. Valid until the next call. */
uint16_t *camera_preview_frame(void);

/* Sensor-side part of a look (white balance / effect). Software looks reset these. */
esp_err_t camera_set_look(look_t l);

bool camera_has_af(void);
/* Single autofocus. ESP_OK when focused or when there is no AF. */
esp_err_t camera_focus(uint32_t timeout_ms);

/* Switch to full resolution and grab one raw RGB565 frame in sensor orientation. Caller must call camera_capture_done(). */
camera_fb_t *camera_capture_full(void);
/* Return the frame and go back to viewfinder size. */
void camera_capture_done(camera_fb_t *fb);

void camera_power(bool on);

/* Portrait photo size: the sensor frame turned 90 degrees. */
int camera_photo_width(void);  /* = camera_full_height() */
int camera_photo_height(void); /* = camera_full_width() */

/* Rows [y0, y0+rows) of the upright portrait photo from a full-res capture, as RGB888
   (camera_photo_width() pixels per row). */
void camera_photo_rows_rgb888(const camera_fb_t *fb, int y0, int rows, uint8_t *out);

/* The whole capture scaled down to the 240x320 portrait screen, RGB565 native order. */
void camera_photo_thumbnail(const camera_fb_t *fb, uint16_t *out);
