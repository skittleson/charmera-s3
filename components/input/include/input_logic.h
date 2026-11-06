#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    INPUT_NONE,
    INPUT_PREV,   /* tilt left */
    INPUT_NEXT,   /* tilt right */
    INPUT_SHORT,  /* BOOT short press: shutter / OK */
    INPUT_LONG,   /* BOOT held: menu */
    INPUT_DOUBLE, /* BOOT pressed twice: back */
} input_event_t;

#define BTN_LONG_MS    800
#define BTN_DOUBLE_MS  350
#define TILT_ON_DEG    18.0f
#define TILT_OFF_DEG   8.0f
#define TILT_HOLD_MS   250
#define TILT_REPEAT_MS 600

typedef struct {
    bool down;
    uint32_t down_at, up_at;
    int presses;
    bool long_fired;
} btn_state_t;

typedef struct {
    int dir; /* -1, 0, +1: direction currently held past TILT_ON_DEG */
    uint32_t since, last_step;
    bool stepped;
    bool armed;
} tilt_state_t;

void btn_init(btn_state_t *s);
/* Call every ~10 ms with the current button level. */
input_event_t btn_update(btn_state_t *s, bool pressed, uint32_t now_ms);

void tilt_init(tilt_state_t *s);
/* Positive roll = tilted right = INPUT_NEXT. */
input_event_t tilt_update(tilt_state_t *s, float roll_deg, uint32_t now_ms);
