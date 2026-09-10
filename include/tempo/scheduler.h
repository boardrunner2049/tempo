/* tempo/scheduler.h -- the logical-time authority.
 *
 * The scheduler owns logical time, decides who runs and until when, makes
 * values logically visible, and records events. It knows nothing about any
 * particular simulator (Rule 1) and Python never enters its loop (Rule 2).
 *
 * It does not synchronize merely because time passed: it synchronizes when
 * the policy's communication window closes or when a participant traps out
 * because of a causal interaction (Rule 3).
 */
#ifndef TEMPO_SCHEDULER_H
#define TEMPO_SCHEDULER_H

#include "tempo/base.h"
#include "tempo/federation.h"
#include "tempo/participant.h"
#include "tempo/time.h"
#include "tempo/trace.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tempo_scheduler tempo_scheduler_t;

/* The plan must outlive the scheduler. */
TEMPO_API tempo_scheduler_t *tempo_scheduler_new(const tempo_plan_t *plan);
TEMPO_API void tempo_scheduler_free(tempo_scheduler_t *sched);

/* Attach an implementation to a compiled participant id. Every participant in
 * the plan must be bound before init. `caps` may be NULL, meaning no optional
 * capability. */
TEMPO_API int tempo_scheduler_bind(tempo_scheduler_t                *sched,
                                   uint32_t                          participant_id,
                                   const tempo_participant_vtable_t *vt,
                                   void                             *self,
                                   const tempo_capabilities_t       *caps);

/* Attach a trace buffer. The scheduler borrows it and applies the plan's
 * compiled filters to it. Pass NULL to detach. */
TEMPO_API int            tempo_scheduler_set_trace(tempo_scheduler_t *sched, tempo_trace_t *tr);
TEMPO_API tempo_trace_t *tempo_scheduler_trace(const tempo_scheduler_t *sched);

/* Initialize every bound participant and emit SIM_START. */
TEMPO_API int tempo_scheduler_init(tempo_scheduler_t *sched);

/* Execute exactly one communication window. Returns TEMPO_OK, or 1 when the
 * run is complete, or a negative status on failure. Useful for tests, for
 * interactive stepping, and for driving the scheduler from a debugger. */
TEMPO_API int tempo_scheduler_step(tempo_scheduler_t *sched);

/* Run until the plan's end time. */
TEMPO_API int tempo_scheduler_run(tempo_scheduler_t *sched);
/* Run until `until`, which must not exceed the plan's end time. */
TEMPO_API int tempo_scheduler_run_until(tempo_scheduler_t *sched, tempo_time_t until);

/* Return every participant to its initial state and logical time to zero.
 * Requires TEMPO_CAP_RESET on every bound participant. */
TEMPO_API int tempo_scheduler_reset(tempo_scheduler_t *sched);

TEMPO_API tempo_tag_t  tempo_scheduler_now(const tempo_scheduler_t *sched);
TEMPO_API bool         tempo_scheduler_finished(const tempo_scheduler_t *sched);
TEMPO_API tempo_time_t tempo_scheduler_participant_time(const tempo_scheduler_t *sched,
                                                        uint32_t participant_id);
/* Last failure message, or "" if none. */
TEMPO_API const char  *tempo_scheduler_error(const tempo_scheduler_t *sched);

/* Direct slot access, for the host process: tests, tools, and in-process
 * adapters. Indices are the plan's global slot indices. */
TEMPO_API tempo_slot_t *tempo_scheduler_input_slots(tempo_scheduler_t *sched);
TEMPO_API tempo_slot_t *tempo_scheduler_output_slots(tempo_scheduler_t *sched);

typedef struct tempo_scheduler_stats {
    uint64_t windows;          /* communication windows closed          */
    uint64_t advance_calls;    /* advance_to() invocations              */
    uint64_t skipped_advances; /* participants that needed no work       */
    uint64_t early_returns;    /* advance_to() returning before target   */
    uint64_t deliveries;       /* values made logically visible          */
    uint64_t microsteps;       /* same-time iterations performed          */
    uint64_t max_microstep;    /* deepest same-time iteration seen        */
} tempo_scheduler_stats_t;

TEMPO_API tempo_scheduler_stats_t tempo_scheduler_stats(const tempo_scheduler_t *sched);

#ifdef __cplusplus
}
#endif
#endif /* TEMPO_SCHEDULER_H */
