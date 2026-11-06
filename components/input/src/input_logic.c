#include "input_logic.h"

#include <math.h>
#include <string.h>

void btn_init(btn_state_t *s)
{
    memset(s, 0, sizeof(*s));
}

input_event_t btn_update(btn_state_t *s, bool pressed, uint32_t now_ms)
{
    if (pressed && !s->down) {
        s->down = true;
        s->down_at = now_ms;
        s->long_fired = false;
        return INPUT_NONE;
    }
    if (pressed) {
        if (!s->long_fired && now_ms - s->down_at >= BTN_LONG_MS) {
            s->long_fired = true;
            s->presses = 0;
            return INPUT_LONG;
        }
        return INPUT_NONE;
    }
    if (s->down) {
        s->down = false;
        s->up_at = now_ms;
        if (s->long_fired) {
            s->presses = 0;
            return INPUT_NONE;
        }
        if (++s->presses == 2) {
            s->presses = 0;
            return INPUT_DOUBLE;
        }
        return INPUT_NONE;
    }
    if (s->presses == 1 && now_ms - s->up_at >= BTN_DOUBLE_MS) {
        s->presses = 0;
        return INPUT_SHORT;
    }
    return INPUT_NONE;
}

void tilt_init(tilt_state_t *s)
{
    memset(s, 0, sizeof(*s));
    s->armed = true;
}

input_event_t tilt_update(tilt_state_t *s, float roll_deg, uint32_t now_ms)
{
    const float mag = fabsf(roll_deg);
    if (mag < TILT_OFF_DEG) {
        s->armed = true;
        s->dir = 0;
        return INPUT_NONE;
    }
    if (mag <= TILT_ON_DEG) {
        /* Dropping out of a tilt after it stepped needs a return to level to re-arm. */
        if (s->dir != 0 && s->stepped) {
            s->armed = false;
        }
        s->dir = 0;
        return INPUT_NONE;
    }
    if (!s->armed) {
        return INPUT_NONE;
    }
    const int d = roll_deg > 0 ? 1 : -1;
    if (s->dir != d) {
        s->dir = d;
        s->since = now_ms;
        s->stepped = false;
        return INPUT_NONE;
    }
    const bool due = s->stepped ? (now_ms - s->last_step >= TILT_REPEAT_MS)
                                : (now_ms - s->since >= TILT_HOLD_MS);
    if (!due) {
        return INPUT_NONE;
    }
    s->stepped = true;
    s->last_step = now_ms;
    return d > 0 ? INPUT_NEXT : INPUT_PREV;
}
