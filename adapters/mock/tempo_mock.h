/* tempo_mock.h -- scriptable mock participant.
 *
 * Phase 1 deliberately integrates no real simulator (S37). A mock is driven
 * by a script of timed actions, which lets a test express exactly the
 * behaviours the scheduler must handle:
 *
 *   - a participant that always reaches its deadline,
 *   - a participant that returns early at 3.5 ms,
 *   - a participant that produces an output event at 6 ms.
 */
#ifndef TEMPO_MOCK_H
#define TEMPO_MOCK_H

#include "tempo/participant.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum tempo_mock_action {
    /* Write `value` to output slot `slot` and trap out to the scheduler. */
    TEMPO_MOCK_EMIT_SYNC = 0,
    /* Write `value` to output slot `slot` and keep running. */
    TEMPO_MOCK_EMIT_OBSERVE = 1,
    /* Trap out to the scheduler without writing anything. */
    TEMPO_MOCK_TRAP = 2,
    /* Record a user-namespace trace event without stopping. */
    TEMPO_MOCK_MARK = 3,
    /* Stop for good. */
    TEMPO_MOCK_FINISH = 4
} tempo_mock_action_t;

typedef struct tempo_mock_step {
    tempo_time_t        time;
    tempo_mock_action_t action;
    uint32_t            slot;   /* local output index */
    double              value;
    uint16_t            event_type; /* MARK only; 0 uses TEMPO_EV_USER_EVENT */
} tempo_mock_step_t;

typedef struct tempo_mock_config {
    /* Script, which must be sorted by non-decreasing time. Copied. */
    const tempo_mock_step_t *steps;
    uint32_t                 n_steps;
    /* If non-zero, never advance further than this per advance_to() call,
     * which models a participant with a coarse internal step. */
    tempo_time_t             max_step;
    /* Advertised capabilities beyond reset + early return. */
    uint32_t                 extra_caps;
    /* When true, the mock copies each visible input onto the output slot of
     * the same local index on its next advance, which is enough to build
     * feedback loops and same-time chains in tests. */
    bool                     echo_inputs;
    /* Scale applied by echo_inputs. */
    double                   echo_gain;
} tempo_mock_config_t;

typedef struct tempo_mock tempo_mock_t;

TEMPO_API tempo_mock_t *tempo_mock_new(const tempo_mock_config_t *cfg);
TEMPO_API void          tempo_mock_free(tempo_mock_t *m);
TEMPO_API const tempo_participant_vtable_t *tempo_mock_vtable(void);
TEMPO_API tempo_capabilities_t tempo_mock_capabilities(const tempo_mock_t *m);

/* Introspection for tests. */
TEMPO_API tempo_time_t tempo_mock_time(const tempo_mock_t *m);
TEMPO_API uint32_t     tempo_mock_advance_count(const tempo_mock_t *m);
TEMPO_API double       tempo_mock_last_input(const tempo_mock_t *m, uint32_t local_index);

/* Build a mock from a compact textual script, so Python and TOML can create
 * one without marshalling structs:
 *
 *     "3.5ms:trap; 6ms:emit_sync 0 0.7; 9ms:finish"
 *
 * Returns NULL and sets *err (borrowed, static) on a parse failure. */
TEMPO_API tempo_mock_t *tempo_mock_from_script(const char *script,
                                               tempo_time_t max_step,
                                               bool echo_inputs,
                                               double echo_gain,
                                               const char **err);

#ifdef __cplusplus
}
#endif
#endif /* TEMPO_MOCK_H */
