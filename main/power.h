#pragma once

#include <stdbool.h>

/* Idle handling: dim, then screen off; BOOT wakes. */
void power_init(void);
void power_activity(void);
void power_tick(void);
bool power_is_asleep(void);
