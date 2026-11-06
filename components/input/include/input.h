#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "input_logic.h"

/* BOOT button (GPIO0) + QMI8658 tilt. Call after board_init(). */
esp_err_t input_init(void);

/* Next input event, waiting up to timeout_ms. INPUT_NONE on timeout. */
input_event_t input_get(uint32_t timeout_ms);

/* Drop any queued events (e.g. after a long save). */
void input_flush(void);

void input_set_tilt_enabled(bool on);

/* Latest roll angle in degrees (+ = tilted right), for diagnostics. */
float input_roll_deg(void);
void input_accel_raw(int16_t out[3]);
