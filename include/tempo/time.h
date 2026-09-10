/* tempo/time.h -- logical time.
 *
 * Logical time is an unsigned nanosecond count. It is never derived from
 * wall-clock time, host thread scheduling, or sleeping (design doc S7).
 *
 * Events that occur at the same logical time are ordered by a microstep
 * index rather than by inventing artificial time offsets (S17). The pair
 * (time, microstep) is a "tag" and is the scheduler's real clock.
 */
#ifndef TEMPO_TIME_H
#define TEMPO_TIME_H

#include "tempo/base.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t tempo_time_t;
typedef uint32_t tempo_microstep_t;

/* "No such time" / infinitely far in the future. */
#define TEMPO_TIME_NEVER UINT64_MAX
#define TEMPO_TIME_ZERO  ((tempo_time_t)0)

#define TEMPO_NS_PER_US 1000ULL
#define TEMPO_NS_PER_MS 1000000ULL
#define TEMPO_NS_PER_S  1000000000ULL

typedef struct tempo_tag {
    tempo_time_t      time;
    tempo_microstep_t microstep;
    uint32_t          _pad;
} tempo_tag_t;

TEMPO_INLINE tempo_tag_t tempo_tag(tempo_time_t t, tempo_microstep_t m)
{
    tempo_tag_t tag;
    tag.time = t;
    tag.microstep = m;
    tag._pad = 0;
    return tag;
}

/* Total order on tags: time first, then microstep. Returns <0, 0, >0. */
TEMPO_INLINE int tempo_tag_cmp(tempo_tag_t a, tempo_tag_t b)
{
    if (a.time != b.time)
        return a.time < b.time ? -1 : 1;
    if (a.microstep != b.microstep)
        return a.microstep < b.microstep ? -1 : 1;
    return 0;
}

TEMPO_INLINE tempo_time_t tempo_time_min(tempo_time_t a, tempo_time_t b)
{
    return a < b ? a : b;
}

/* Saturating add, so TEMPO_TIME_NEVER stays absorbing and delays cannot wrap. */
TEMPO_INLINE tempo_time_t tempo_time_add(tempo_time_t t, tempo_time_t d)
{
    tempo_time_t sum;
    if (t == TEMPO_TIME_NEVER || d == TEMPO_TIME_NEVER)
        return TEMPO_TIME_NEVER;
    sum = t + d;
    return sum < t ? TEMPO_TIME_NEVER : sum;
}

/* Parse a duration such as "10s", "100us", "3.5ms", "250" (bare = ns).
 * Accepted suffixes: ns, us, ms, s, min, h. Returns TEMPO_OK on success. */
TEMPO_API int tempo_time_parse(const char *text, tempo_time_t *out);

/* Format `t` into `buf` using the largest exact unit ("100us", "3500000ns").
 * Returns `buf`. */
TEMPO_API const char *tempo_time_format(tempo_time_t t, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* TEMPO_TIME_H */
