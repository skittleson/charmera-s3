#pragma once

#include <stdbool.h>

/* App side of the Bluetooth remote: owns the info JSON and turns the stack on/off. */
void remote_bridge_init(void);           /* at boot; starts Bluetooth if the setting is on */
void remote_bridge_set_enabled(bool on); /* from Settings */
