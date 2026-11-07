#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "orient.h"

/* ST7789 panel + LVGL, 240x320 portrait at boot. Call after board_init(). */
esp_err_t display_init(void);

/* Turn the whole UI for how the camera is held (panel axes swapped/mirrored in
   hardware). Display lock NOT held. */
void display_set_orientation(orient_t o);
/* Current UI size: 240x320 portrait, 320x240 landscape. */
int display_width(void);
int display_height(void);

/* Guard every LVGL call made outside LVGL callbacks. */
bool display_lock(int timeout_ms);
void display_unlock(void);
