/* tempo/analysis.h -- selective observability.
 *
 * A subscription says what an analysis wants to see. Subscriptions are
 * compiled into native filters before the run starts, so that asking for
 * "MMIO writes from firmware A to 0x40012000" does not mean generating and
 * discarding every other event (S21, S24).
 *
 * The distinction between observing and intervening is explicit (S25):
 * a subscription with TEMPO_SUB_BREAK stops the participant; without it,
 * the event is only recorded.
 */
#ifndef TEMPO_ANALYSIS_H
#define TEMPO_ANALYSIS_H

#include "tempo/base.h"
#include "tempo/event.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Record the event in the trace buffer. */
#define TEMPO_SUB_OBSERVE (1u << 0)
/* Stop the participant and return control to the scheduler -- the slow path. */
#define TEMPO_SUB_BREAK   (1u << 1)

typedef struct tempo_subscription {
    /* Participant to match, or TEMPO_ID_ANY. */
    uint32_t participant_id;
    /* Event type to match, or TEMPO_ID_ANY (low 16 bits used). */
    uint32_t type;
    /* Optional match on the event's arg1, used for things like an MMIO
     * address: the event matches when (arg1 & arg_mask) == arg_value.
     * An arg_mask of 0 matches any value. */
    uint64_t arg_mask;
    uint64_t arg_value;
    uint32_t flags;
    uint32_t _pad;
} tempo_subscription_t;

TEMPO_INLINE tempo_subscription_t tempo_subscription_all(void)
{
    tempo_subscription_t s;
    s.participant_id = TEMPO_ID_ANY;
    s.type = TEMPO_ID_ANY;
    s.arg_mask = 0;
    s.arg_value = 0;
    s.flags = TEMPO_SUB_OBSERVE;
    s._pad = 0;
    return s;
}

#ifdef __cplusplus
}
#endif
#endif /* TEMPO_ANALYSIS_H */
