#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "looks.h"

/* Persistent device settings (NVS). */
void settings_load(void);

uint8_t settings_brightness(void); /* 10..100, default 80 */
void settings_set_brightness(uint8_t pct);

bool settings_level(void); /* on-screen level while shooting, default off */
void settings_set_level(bool on);

look_t settings_look(void);
void settings_set_look(look_t l);

int settings_lapse_interval(void); /* index into the time-lapse intervals, default 10 s */
void settings_set_lapse_interval(int index);

bool settings_bluetooth(void); /* Bluetooth remote, default on */
void settings_set_bluetooth(bool on);

int settings_orientation(void); /* 0 portrait, 1 landscape right, 2 landscape left, 3 auto */
void settings_set_orientation(int setting);
