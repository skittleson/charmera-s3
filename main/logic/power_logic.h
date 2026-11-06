#pragma once

#include <stdint.h>

#ifndef POWER_DIM_MS
#define POWER_DIM_MS   (30 * 1000)
#endif
#ifndef POWER_SLEEP_MS
#define POWER_SLEEP_MS (3 * 60 * 1000)
#endif

typedef enum { POWER_AWAKE, POWER_DIM, POWER_ASLEEP } power_state_t;

/* State for a given idle time. */
power_state_t power_state_for_idle(uint32_t idle_ms);
