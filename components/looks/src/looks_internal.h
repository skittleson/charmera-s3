#pragma once

#include <stdint.h>

#include "looks.h"

/* Draw a frame look over a strip; see look_apply_rgb888 for the strip contract. */
void frame_apply(look_t l, uint8_t *rgb, int strip_w, int strip_h, int y0, int img_w, int img_h);

/* Frame on a sensor-orientation strip (see look_apply_rgb888_sensor). */
void frame_apply_sensor(look_t l, uint8_t *rgb, int sensor_w, int rows, int y0, int img_sensor_w,
                        int img_sensor_h);
