#pragma once

/* How the camera is held, and the pixel/EXIF/tilt maths that follow from it.
   Pure C, host-tested.

   "Sensor" is the frame as the sensor delivers it (landscape). "View" is the picture
   as the person holding the camera sees it. */

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    ORIENT_PORTRAIT,   /* USB-C down */
    ORIENT_LAND_RIGHT, /* USB-C right */
    ORIENT_LAND_LEFT,  /* USB-C left */
    ORIENT_COUNT
} orient_t;

/* The setting adds Auto on top of the three grips. */
#define ORIENT_SETTING_AUTO  3
#define ORIENT_SETTING_COUNT 4

/* The landscape grip that shows the sensor image as-is (the other is turned 180).
   Derived from the verified portrait mapping (the sensor's top edge is the view's left
   edge, so turning the camera clockwise into USB-C left makes it upright).
   NOT yet checked on the board: if landscape shows upside down, swap to ORIENT_LAND_RIGHT. */
#ifndef ORIENT_SENSOR_NATIVE
#define ORIENT_SENSOR_NATIVE ORIENT_LAND_LEFT
#endif

/* Gravity angle (see orient_angle_deg) of the USB-C-left grip: +90 or -90. Derived from
   the level line the owner checked in portrait (clockwise tilt = positive roll).
   NOT yet checked on the board: if Auto picks the wrong landscape side, negate. */
#ifndef ORIENT_LEFT_DEG
#define ORIENT_LEFT_DEG 90.0f
#endif

/* "portrait", "landscape-right", "landscape-left", "auto" (setting values 0..3). */
const char *orient_name(int setting);
/* Inverse of orient_name; -1 if unknown. */
int orient_parse(const char *name);

/* View size of a sw x sh sensor image held as o. */
void orient_view_size(orient_t o, int sw, int sh, int *vw, int *vh);
/* View pixel (vx, vy) -> sensor pixel. */
void orient_view_to_sensor(orient_t o, int vx, int vy, int sw, int sh, int *sx, int *sy);
/* EXIF Orientation for a picture stored in sensor orientation and viewed as o. */
uint16_t orient_exif(orient_t o);

/* Gravity in the screen plane, in the portrait axes the input driver reports: down =
   along the portrait vertical, side = along the portrait horizontal (+ = a clockwise
   tilt). Returns degrees in [-90, 90]: 0 = portrait, ORIENT_LEFT_DEG = USB-C left,
   -ORIENT_LEFT_DEG = USB-C right. Upside down folds onto portrait. */
float orient_angle_deg(float down, float side);
float orient_centre_deg(orient_t o);
/* Level-line roll for grip o (+ = tilted clockwise), from the signed axes. */
float orient_roll_deg(orient_t o, float down, float side);

/* Auto: a new grip wins after it has been held for ORIENT_AUTO_DWELL_MS. */
#define ORIENT_AUTO_DWELL_MS  700
#define ORIENT_AUTO_WINDOW_DEG 30.0f /* within this of the grip's centre */
#define ORIENT_AUTO_MIN_PLANE  0.5f  /* in-plane share of gravity; less = lying flat */

typedef struct {
    orient_t current;
    int candidate; /* -1 = none */
    int64_t since_ms;
} orient_auto_t;

void orient_auto_init(orient_auto_t *a, orient_t start);
/* One accelerometer sample (any unit). locked = don't switch now (and restart the
   dwell). Returns true when current changed. */
bool orient_auto_update(orient_auto_t *a, float down, float side, float face, bool locked, int64_t now_ms);
