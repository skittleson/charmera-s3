#pragma once

/* Bluetooth remote protocol: pure C, host-tested. Commands are short UTF-8 text so they
   can be typed in nRF Connect; status and info are compact JSON. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    RC_LOOK,        /* look <n> */
    RC_MODE_SINGLE, /* mode single */
    RC_MODE_LAPSE,  /* mode lapse [n] */
    RC_INTERVAL,    /* interval <n> */
    RC_SHOOT,       /* shoot */
    RC_LAPSE_START, /* lapse start */
    RC_LAPSE_STOP,  /* lapse stop */
    RC_TIME,        /* time <unix seconds> [utc offset minutes] */
    RC_ORIENTATION, /* orientation <portrait|landscape-right|landscape-left|auto> -> arg 0..3 */
    RC_LIST,        /* list -> photo numbers and sizes as JSON (via Status) */
    RC_THUMB,       /* thumb <n> -> stream the .THM sidecar (arg = photo number) */
    RC_GET,         /* get <n> -> stream the full JPEG (arg = photo number) */
} remote_cmd_kind_t;

typedef struct {
    remote_cmd_kind_t kind;
    int arg;        /* -1 = none */
    int64_t epoch;  /* RC_TIME */
    int offset_min; /* RC_TIME, -720..840 */
} remote_cmd_t;

/* Parse len bytes (no NUL needed; surrounding whitespace and case ignored).
   NULL on success, else a short static reason: "unknown command", "bad look", "bad interval", "bad time", "bad orientation". */
const char *remote_parse(const char *text, size_t len, int look_count, int interval_count, remote_cmd_t *out);

/* NULL if c may run now, else "time-lapse running" or "not running". */
const char *remote_check(const remote_cmd_t *c, bool lapse_running);

typedef enum { REMOTE_CARD_OK, REMOTE_CARD_NONE, REMOTE_CARD_FULL } remote_card_t;

typedef struct {
    uint32_t photos, left;
    int battery_pct;
    bool usb;
    const char *camera; /* "OV5640" / "OV2640" / "none" */
    int look;
    const char *look_name;
    bool lapse_mode;
    int interval;
    const char *interval_name;
    bool lapse_running;
    uint32_t frames, elapsed_s;
    char folder[9];
    remote_card_t card;
    bool busy;
    char last[24];  /* "" = none yet */
    char error[32]; /* "" = none */
    bool clock_set;                  /* EXIF dates available (time was sent) */
    const char *orientation;         /* in effect: portrait / landscape-right / landscape-left */
    const char *orientation_setting; /* the setting: those, or auto */
} remote_status_t;

/* Compact JSON. Returns its length, or 0 if it doesn't fit (never truncated). */
size_t remote_status_json(const remote_status_t *s, char *out, size_t len);
size_t remote_info_json(const char *version, const char *const *looks, int nlooks, const char *const *intervals,
                        int nintervals, char *out, size_t len);
