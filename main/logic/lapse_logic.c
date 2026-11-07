#include "lapse_logic.h"

#include <stdio.h>

static const struct {
    uint32_t ms;
    const char *name;
} INTERVALS[LAPSE_INTERVAL_COUNT] = {
    {5 * 1000, "5 s"}, {10 * 1000, "10 s"}, {30 * 1000, "30 s"}, {60 * 1000, "1 min"}, {5 * 60 * 1000, "5 min"},
};

static int valid(int index)
{
    return index >= 0 && index < LAPSE_INTERVAL_COUNT ? index : LAPSE_INTERVAL_DEFAULT;
}

uint32_t lapse_interval_ms(int index)
{
    return INTERVALS[valid(index)].ms;
}

const char *lapse_interval_name(int index)
{
    return INTERVALS[valid(index)].name;
}

int64_t lapse_next_due(int64_t prev_due_ms, uint32_t interval_ms, int64_t now_ms)
{
    const int64_t next = prev_due_ms + interval_ms;
    return next > now_ms ? next : now_ms;
}

void lapse_format_duration(uint32_t seconds, char *out, size_t len)
{
    const unsigned h = seconds / 3600, m = (seconds / 60) % 60, s = seconds % 60;
    if (h) {
        snprintf(out, len, "%u:%02u:%02u", h, m, s);
    } else {
        snprintf(out, len, "%u:%02u", m, s);
    }
}
