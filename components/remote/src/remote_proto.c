#include "remote_proto.h"

#include "orient.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CMD 40
#define EPOCH_MIN 1577836800LL /* 2020-01-01: anything earlier is a bad clock */
#define OFFSET_MIN_LO (-12 * 60)
#define OFFSET_MIN_HI (14 * 60)
#define MAX_WORDS 3

/* Number in [0, count) with nothing trailing; -1 otherwise. */
static int parse_index(const char *word, int count)
{
    if (!word || !*word) {
        return -1;
    }
    char *end;
    const long v = strtol(word, &end, 10);
    return (*end == '\0' && v >= 0 && v < count) ? (int)v : -1;
}

const char *remote_parse(const char *text, size_t len, int look_count, int interval_count, remote_cmd_t *out)
{
    /* Lower-case copy, split on whitespace. */
    char buf[MAX_CMD + 1];
    if (!text || len > MAX_CMD) {
        return "unknown command";
    }
    for (size_t i = 0; i < len; i++) {
        buf[i] = (char)tolower((unsigned char)text[i]);
    }
    buf[len] = '\0';
    char *w[MAX_WORDS + 1] = {0};
    int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, " \t\r\n", &save); tok; tok = strtok_r(NULL, " \t\r\n", &save)) {
        if (n == MAX_WORDS) {
            return "unknown command";
        }
        w[n++] = tok;
    }
    if (n == 0) {
        return "unknown command";
    }
    out->arg = -1;
    out->epoch = 0;
    out->offset_min = 0;
    if (strcmp(w[0], "time") == 0) {
        char *end;
        if (n < 2 || n > 3) {
            return "bad time";
        }
        const long long e = strtoll(w[1], &end, 10);
        if (*end || e < EPOCH_MIN) {
            return "bad time";
        }
        if (n == 3) {
            const long o = strtol(w[2], &end, 10);
            if (*end || o < OFFSET_MIN_LO || o > OFFSET_MIN_HI) {
                return "bad time";
            }
            out->offset_min = (int)o;
        }
        out->kind = RC_TIME;
        out->epoch = e;
        return NULL;
    }
    if (strcmp(w[0], "orientation") == 0) {
        if (n != 2 || (out->arg = orient_parse(w[1])) < 0) {
            return "bad orientation";
        }
        out->kind = RC_ORIENTATION;
        return NULL;
    }
    if (strcmp(w[0], "look") == 0) {
        if (n != 2 || (out->arg = parse_index(w[1], look_count)) < 0) {
            return "bad look";
        }
        out->kind = RC_LOOK;
        return NULL;
    }
    if (strcmp(w[0], "interval") == 0) {
        if (n != 2 || (out->arg = parse_index(w[1], interval_count)) < 0) {
            return "bad interval";
        }
        out->kind = RC_INTERVAL;
        return NULL;
    }
    if (strcmp(w[0], "mode") == 0 && n >= 2) {
        if (strcmp(w[1], "single") == 0 && n == 2) {
            out->kind = RC_MODE_SINGLE;
            return NULL;
        }
        if (strcmp(w[1], "lapse") == 0) {
            if (n == 3 && (out->arg = parse_index(w[2], interval_count)) < 0) {
                return "bad interval";
            }
            out->kind = RC_MODE_LAPSE;
            return NULL;
        }
        return "unknown command";
    }
    if (strcmp(w[0], "shoot") == 0 && n == 1) {
        out->kind = RC_SHOOT;
        return NULL;
    }
    if (strcmp(w[0], "lapse") == 0 && n == 2) {
        if (strcmp(w[1], "start") == 0) {
            out->kind = RC_LAPSE_START;
            return NULL;
        }
        if (strcmp(w[1], "stop") == 0) {
            out->kind = RC_LAPSE_STOP;
            return NULL;
        }
    }
    if (strcmp(w[0], "list") == 0 && n == 1) {
        out->kind = RC_LIST;
        return NULL;
    }
    if ((strcmp(w[0], "thumb") == 0 || strcmp(w[0], "get") == 0) && n == 2) {
        char *end;
        const long v = strtol(w[1], &end, 10);
        if (*end || v < 1 || v > 9999) {
            return "bad photo number";
        }
        out->kind = w[0][0] == 't' ? RC_THUMB : RC_GET;
        out->arg = (int)v;
        return NULL;
    }
    return "unknown command";
}

const char *remote_check(const remote_cmd_t *c, bool lapse_running)
{
    if (c->kind == RC_TIME) {
        return NULL; /* setting the clock never disturbs a run */
    }
    if (c->kind == RC_LAPSE_STOP) {
        return lapse_running ? NULL : "not running";
    }
    return lapse_running ? "time-lapse running" : NULL;
}

/* Appends into out[*pos..len); sets *ok false once anything doesn't fit. */
typedef struct {
    char *out;
    size_t len, pos;
    bool ok;
} sink_t;

static void put(sink_t *k, const char *fmt, ...)
{
    if (!k->ok) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(k->out + k->pos, k->len - k->pos, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= k->len - k->pos) {
        k->ok = false;
        return;
    }
    k->pos += (size_t)n;
}

static void put_str(sink_t *k, const char *s)
{
    put(k, "\"");
    for (const char *p = s ? s : ""; *p && k->ok; p++) {
        if (*p == '"' || *p == '\\') {
            put(k, "\\%c", *p);
        } else if ((unsigned char)*p < 0x20) {
            put(k, "\\u%04x", (unsigned char)*p);
        } else {
            put(k, "%c", *p);
        }
    }
    put(k, "\"");
}

static size_t finish(sink_t *k)
{
    if (!k->ok) {
        if (k->len) {
            k->out[0] = '\0';
        }
        return 0;
    }
    return k->pos;
}

static const char *const CARD[] = {"ok", "none", "full"};

size_t remote_status_json(const remote_status_t *s, char *out, size_t len)
{
    sink_t k = {out, len, 0, len > 0};
    put(&k, "{\"photos\":%u,\"left\":%u,\"battery\":%d,\"usb\":%s,\"camera\":", (unsigned)s->photos,
        (unsigned)s->left, s->battery_pct, s->usb ? "true" : "false");
    put_str(&k, s->camera);
    put(&k, ",\"look\":%d,\"lookName\":", s->look);
    put_str(&k, s->look_name);
    put(&k, ",\"mode\":\"%s\",\"interval\":%d,\"intervalName\":", s->lapse_mode ? "lapse" : "single", s->interval);
    put_str(&k, s->interval_name);
    put(&k, ",\"running\":%s,\"frames\":%u,\"elapsed\":%u,\"folder\":", s->lapse_running ? "true" : "false",
        (unsigned)s->frames, (unsigned)s->elapsed_s);
    put_str(&k, s->folder);
    put(&k, ",\"card\":\"%s\",\"busy\":%s,\"last\":", CARD[s->card <= REMOTE_CARD_FULL ? s->card : 0],
        s->busy ? "true" : "false");
    put_str(&k, s->last);
    put(&k, ",\"error\":");
    put_str(&k, s->error);
    put(&k, ",\"clock\":%s,\"orientation\":", s->clock_set ? "true" : "false");
    put_str(&k, s->orientation ? s->orientation : "portrait");
    put(&k, ",\"orientationSetting\":");
    put_str(&k, s->orientation_setting ? s->orientation_setting : "portrait");
    put(&k, "}");
    return finish(&k);
}

static void put_list(sink_t *k, const char *const *items, int n)
{
    put(k, "[");
    for (int i = 0; i < n; i++) {
        if (i) {
            put(k, ",");
        }
        put_str(k, items[i]);
    }
    put(k, "]");
}

size_t remote_info_json(const char *version, const char *const *looks, int nlooks, const char *const *intervals,
                        int nintervals, char *out, size_t len)
{
    sink_t k = {out, len, 0, len > 0};
    put(&k, "{\"version\":");
    put_str(&k, version);
    put(&k, ",\"looks\":");
    put_list(&k, looks, nlooks);
    put(&k, ",\"intervals\":");
    put_list(&k, intervals, nintervals);
    put(&k, "}");
    return finish(&k);
}
