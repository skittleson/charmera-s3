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

/* ---- file streaming (the STREAM characteristic, c4a30004) ----
   Frames: [u8 kind][payload], kind 0 = meta JSON, 1 = data, 2 = end (u32 total),
   3 = error (text: the request was refused, nothing else follows).
   The client must have notifications enabled on STREAM. One stream at a time;
   remote_stream_file returns false if busy, disconnected or not subscribed. */

/* Reads up to max bytes into buf; returns count, 0 at end, negative on error. */
typedef int (*remote_stream_reader_t)(uint8_t *buf, int max, void *ctx);
/* Always called when the stream finishes, succeeds or aborts. May be NULL. */
typedef void (*remote_stream_done_t)(void *ctx);

/* meta_json may be NULL. The stream runs on its own task. */
bool remote_stream_file(const char *meta_json, remote_stream_reader_t read, remote_stream_done_t done,
                        void *ctx);
bool remote_stream_is_busy(void);
bool remote_stream_is_subscribed(void);
/* Tell the client its file request was refused (kind 3 frame). */
void remote_stream_error(const char *msg);
/* Ask the running stream to stop now (disconnect does this automatically). */
void remote_stream_abort(void);
