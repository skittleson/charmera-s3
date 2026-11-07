#include "orient.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define RAD2DEG (180.0f / (float)M_PI)

static const char *const NAMES[ORIENT_SETTING_COUNT] = {"portrait", "landscape-right", "landscape-left", "auto"};

const char *orient_name(int setting)
{
    return setting >= 0 && setting < ORIENT_SETTING_COUNT ? NAMES[setting] : NAMES[0];
}

int orient_parse(const char *name)
{
    for (int i = 0; name && i < ORIENT_SETTING_COUNT; i++) {
        if (strcmp(name, NAMES[i]) == 0) {
            return i;
        }
    }
    return -1;
}

void orient_view_size(orient_t o, int sw, int sh, int *vw, int *vh)
{
    *vw = o == ORIENT_PORTRAIT ? sh : sw;
    *vh = o == ORIENT_PORTRAIT ? sw : sh;
}

void orient_view_to_sensor(orient_t o, int vx, int vy, int sw, int sh, int *sx, int *sy)
{
    if (o == ORIENT_PORTRAIT) {
        *sx = sw - 1 - vy; /* sensor turned 90 deg counter-clockwise */
        *sy = vx;
    } else if (o == ORIENT_SENSOR_NATIVE) {
        *sx = vx;
        *sy = vy;
    } else {
        *sx = sw - 1 - vx; /* turned 180 */
        *sy = sh - 1 - vy;
    }
}

uint16_t orient_exif(orient_t o)
{
    if (o == ORIENT_PORTRAIT) {
        return 8;
    }
    return o == ORIENT_SENSOR_NATIVE ? 1 : 3;
}

float orient_angle_deg(float down, float side)
{
    return atan2f(side, fabsf(down)) * RAD2DEG;
}

float orient_centre_deg(orient_t o)
{
    switch (o) {
    case ORIENT_LAND_LEFT: return ORIENT_LEFT_DEG;
    case ORIENT_LAND_RIGHT: return -ORIENT_LEFT_DEG;
    default: return 0.0f;
    }
}

float orient_roll_deg(orient_t o, float down, float side)
{
    if (o == ORIENT_PORTRAIT) {
        return atan2f(side, fabsf(down)) * RAD2DEG; /* the level's original formula */
    }
    /* Signed angle of gravity in the portrait axes, measured from this grip's centre. */
    float r = atan2f(side, down) * RAD2DEG - orient_centre_deg(o);
    while (r > 180.0f) {
        r -= 360.0f;
    }
    while (r < -180.0f) {
        r += 360.0f;
    }
    return r;
}

void orient_auto_init(orient_auto_t *a, orient_t start)
{
    a->current = start;
    a->candidate = -1;
    a->since_ms = 0;
}

/* The grip whose centre the angle is within the window of, or -1 (between grips). */
static int grip_for(float angle)
{
    for (int o = 0; o < ORIENT_COUNT; o++) {
        if (fabsf(angle - orient_centre_deg((orient_t)o)) < ORIENT_AUTO_WINDOW_DEG) {
            return o;
        }
    }
    return -1;
}

bool orient_auto_update(orient_auto_t *a, float down, float side, float face, bool locked, int64_t now_ms)
{
    const float plane = sqrtf(down * down + side * side);
    const float total = sqrtf(down * down + side * side + face * face);
    int g = -1;
    if (!locked && total > 0.0f && plane >= ORIENT_AUTO_MIN_PLANE * total) {
        g = grip_for(orient_angle_deg(down, side));
    }
    if (g < 0 || g == (int)a->current) {
        a->candidate = -1; /* between grips, flat, locked or settled: restart the dwell */
        return false;
    }
    if (g != a->candidate) {
        a->candidate = g;
        a->since_ms = now_ms;
        return false;
    }
    if (now_ms - a->since_ms < ORIENT_AUTO_DWELL_MS) {
        return false;
    }
    a->current = (orient_t)g;
    a->candidate = -1;
    return true;
}
