/* tempo/participant.h -- the one runtime abstraction the scheduler knows.
 *
 * A participant is anything whose logical execution can be advanced (S6).
 * The scheduler knows nothing about QEMU, FMI, firmware, or physics; an
 * adapter translates advance_to() into whatever its simulator requires
 * (S5, Rule 1).
 *
 * The contract of
 *
 *     advance_to(T)
 *
 * is: execute without advancing beyond logical time T. Reaching T is not
 * required. Returning early is normal, not exceptional (S6).
 */
#ifndef TEMPO_PARTICIPANT_H
#define TEMPO_PARTICIPANT_H

#include "tempo/base.h"
#include "tempo/port.h"
#include "tempo/time.h"

#ifdef __cplusplus
extern "C" {
#endif

struct tempo_trace;

typedef enum tempo_stop_reason {
    /* Participant is at the requested target time. */
    TEMPO_TARGET_REACHED = 0,
    /* Participant stopped early because something externally relevant
     * happened -- a scheduler trap (S10). */
    TEMPO_EVENT          = 1,
    /* Participant cannot make progress until it receives new input. */
    TEMPO_BLOCKED        = 2,
    /* Participant will never make progress again. */
    TEMPO_FINISHED       = 3,
    /* Participant failed. The scheduler aborts the run. */
    TEMPO_ERROR          = 4
} tempo_stop_reason_t;

TEMPO_API const char *tempo_stop_reason_str(tempo_stop_reason_t r);

typedef struct tempo_advance_result {
    tempo_time_t        reached_time;
    tempo_stop_reason_t reason;
    uint32_t            _pad;
} tempo_advance_result_t;

/* Capability flags (S30). The scheduler branches on capabilities, never on
 * "if participant == QEMU". */
#define TEMPO_CAP_RESET             (1u << 0)
#define TEMPO_CAP_SNAPSHOT          (1u << 1)
#define TEMPO_CAP_RESTORE           (1u << 2)
#define TEMPO_CAP_EARLY_RETURN      (1u << 3)
#define TEMPO_CAP_PREDICT_NEXT      (1u << 4)
#define TEMPO_CAP_SAME_TIME_ITER    (1u << 5)

typedef struct tempo_capabilities {
    uint32_t     flags;
    tempo_time_t preferred_step;  /* 0 = no preference */
    tempo_time_t time_resolution; /* 0 = unspecified   */
} tempo_capabilities_t;

/* Everything a participant needs, handed to it once at initialize().
 *
 * `inputs` and `outputs` point into the federation's slot arrays. A
 * participant reads and writes them directly; there is no per-value call
 * into the scheduler. */
typedef struct tempo_participant_context {
    uint32_t                 participant_id;
    const char              *name;
    tempo_slot_t            *inputs;
    tempo_slot_t            *outputs;
    const tempo_port_desc_t *input_desc;
    const tempo_port_desc_t *output_desc;
    uint32_t                 n_inputs;
    uint32_t                 n_outputs;
    struct tempo_trace      *trace;   /* may be NULL */
    void                    *userdata;
} tempo_participant_context_t;

/* A participant implementation. All optional entries may be NULL; the
 * scheduler consults `capabilities` rather than testing for NULL on the
 * hot path. */
typedef struct tempo_participant_vtable {
    const char *type_name;

    /* Required. */
    int  (*initialize)(void *self, const tempo_participant_context_t *ctx);
    int  (*advance_to)(void *self, tempo_time_t target, tempo_advance_result_t *out);
    void (*destroy)(void *self);

    /* Optional, gated by capability flags. */
    int          (*reset)(void *self);
    tempo_time_t (*next_event_time)(void *self);
    int          (*snapshot)(void *self, void **blob, size_t *len);
    int          (*restore)(void *self, const void *blob, size_t len);

    /* Optional slow-path notification, called at a communication point only,
     * after the scheduler has made new input values visible. Adapters that
     * must push values into a foreign simulator (FMI, QEMU) use this; a
     * participant that reads its input slots directly does not need it. */
    int (*inputs_changed)(void *self, const uint32_t *slots, uint32_t n);
} tempo_participant_vtable_t;

#ifdef __cplusplus
}
#endif
#endif /* TEMPO_PARTICIPANT_H */
