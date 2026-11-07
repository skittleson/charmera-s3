#pragma once

/* Bluetooth LE remote (NimBLE peripheral). Serves Status (read/notify JSON),
   Command (write text) and Info (read JSON), plus the standard Battery Service.
   Commands are parsed here and queued; the UI task pops and runs them. */

#include <stdbool.h>

#include "esp_err.h"
#include "remote_proto.h"

/* Bounds for command numbers. Call before remote_start. */
void remote_set_counts(int looks, int intervals);

/* Start the stack and advertise as "Charmera-S3". info_json is copied. */
esp_err_t remote_start(const char *info_json);
/* Stop advertising, drop any connection and free the stack. */
void remote_stop(void);
bool remote_is_running(void);
bool remote_is_connected(void);

/* Non-blocking pop of the next parsed command. */
bool remote_take_command(remote_cmd_t *out);
/* Last rejected command's reason ("" if none); clears it. */
const char *remote_take_error(void);

/* New status: stored for reads, notified when connected and changed. */
void remote_publish(const char *status_json, int battery_pct);
