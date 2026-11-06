#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* ST7789 panel + CST816D touch + LVGL, landscape 320x240. Call after board_init(). */
esp_err_t display_init(void);

/* Guard every LVGL call made outside LVGL callbacks. */
bool display_lock(int timeout_ms);
void display_unlock(void);
