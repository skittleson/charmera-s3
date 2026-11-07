#pragma once

#include <stdbool.h>

#include "orient.h"

/* How the camera is held right now (the setting, or what Auto chose). */
void orientation_init(void); /* at boot, after the display and camera */
orient_t orientation_current(void);
/* Apply settings_orientation() now (from Settings or Bluetooth). UI task. */
void orientation_setting_changed(void);
/* Auto: feed the tilt sensor. Call often from the camera screen; locked = don't switch
   now (busy, time-lapse running, menu open). UI task. */
void orientation_auto_poll(bool locked);
