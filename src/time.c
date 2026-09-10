#include "tempo/time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Duration suffixes, longest first so "ms" is not read as "m" + "s". */
struct unit { const char *suffix; uint64_t ns; };
static const struct unit units[] = {
    { "ns",  1ULL },
    { "us",  TEMPO_NS_PER_US },
    { "ms",  TEMPO_NS_PER_MS },
    { "min", 60ULL * TEMPO_NS_PER_S },
    { "s",   TEMPO_NS_PER_S },
    { "h",   3600ULL * TEMPO_NS_PER_S },
};

int tempo_time_parse(const char *text, tempo_time_t *out)
{
    const char *p;
    char *end;
    double value;
    size_t i, best = (size_t)-1, best_len = 0, tail_len;

    if (!text || !out)
        return TEMPO_ERR_INVALID_ARG;

    while (*text == ' ' || *text == '\t')
        text++;

    value = strtod(text, &end);
    if (end == text)
        return TEMPO_ERR_INVALID_ARG;
    if (value < 0.0)
        return TEMPO_ERR_INVALID_ARG;

    p = end;
    while (*p == ' ' || *p == '\t')
        p++;

    if (*p == '\0') {
        /* A bare number is nanoseconds. */
        if (value > 1.8e19)
            return TEMPO_ERR_OVERFLOW;
        *out = (tempo_time_t)value;
        return TEMPO_OK;
    }

    tail_len = strlen(p);
    for (i = 0; i < sizeof(units) / sizeof(units[0]); i++) {
        size_t n = strlen(units[i].suffix);
        if (n == tail_len && !strcmp(p, units[i].suffix) && n > best_len) {
            best = i;
            best_len = n;
        }
    }
    if (best == (size_t)-1)
        return TEMPO_ERR_INVALID_ARG;

    value *= (double)units[best].ns;
    if (value > 1.8e19)
        return TEMPO_ERR_OVERFLOW;
    /* Round to the nearest nanosecond so "3.5ms" is exact. */
    *out = (tempo_time_t)(value + 0.5);
    return TEMPO_OK;
}

const char *tempo_time_format(tempo_time_t t, char *buf, size_t len)
{
    if (!buf || len == 0)
        return buf;
    if (t == TEMPO_TIME_NEVER) {
        snprintf(buf, len, "never");
        return buf;
    }
    if (t == 0) {
        snprintf(buf, len, "0s");
        return buf;
    }
    /* Largest unit that divides exactly, so round numbers stay readable. */
    if (t % TEMPO_NS_PER_S == 0)
        snprintf(buf, len, "%llus", (unsigned long long)(t / TEMPO_NS_PER_S));
    else if (t % TEMPO_NS_PER_MS == 0)
        snprintf(buf, len, "%llums", (unsigned long long)(t / TEMPO_NS_PER_MS));
    else if (t % TEMPO_NS_PER_US == 0)
        snprintf(buf, len, "%lluus", (unsigned long long)(t / TEMPO_NS_PER_US));
    else
        snprintf(buf, len, "%lluns", (unsigned long long)t);
    return buf;
}
