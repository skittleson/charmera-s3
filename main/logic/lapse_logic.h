#pragma once

#include <stddef.h>
#include <stdint.h>

/* Time-lapse intervals offered in the menu. */
#define LAPSE_INTERVAL_COUNT   5
#define LAPSE_INTERVAL_DEFAULT 1 /* 10 s */
/* At or above this interval the sensor sleeps between frames (it runs hot). */
#define LAPSE_SENSOR_SLEEP_MS  (30 * 1000)
/* Frames per second the README's ffmpeg recipe plays a run back at. */
#define LAPSE_PLAYBACK_FPS     24

uint32_t lapse_interval_ms(int index);   /* out-of-range -> default */
const char *lapse_interval_name(int index);

/* When the next frame is due after one that was due at prev_due_ms. Slots missed
   because a save ran long are skipped, not caught up in a burst. */
int64_t lapse_next_due(int64_t prev_due_ms, uint32_t interval_ms, int64_t now_ms);

/* 75 -> "1:15", 3725 -> "1:02:05" */
void lapse_format_duration(uint32_t seconds, char *out, size_t len);
