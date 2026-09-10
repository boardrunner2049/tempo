/* tempo/federation.h -- the federation compiler.
 *
 * Configuration is compiled once into an execution plan; it is never
 * re-interpreted while the simulation runs (S13, Rule 5). The compiler turns
 *
 *     from = "controller.motor"
 *     to   = "world.motor"
 *
 * into an index pair, so that routing at runtime is
 *
 *     input_slots[3] = output_slots[17];
 *
 * with no string lookup, no hash table, and no type dispatch.
 *
 * Everything that can fail because the configuration is wrong -- a missing
 * participant or port, a type mismatch, two writers into one input -- fails
 * here, before the run, rather than in the hot path (S14).
 */
#ifndef TEMPO_FEDERATION_H
#define TEMPO_FEDERATION_H

#include "tempo/analysis.h"
#include "tempo/base.h"
#include "tempo/port.h"
#include "tempo/time.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Mutable description of an experiment, built by the Python/TOML front end. */
typedef struct tempo_federation_spec tempo_federation_spec_t;
/* Immutable, fully resolved plan handed to the scheduler. */
typedef struct tempo_plan tempo_plan_t;

/* Where a compile failure came from, so the front end can point at the
 * offending line of configuration. */
typedef struct tempo_diagnostics {
    int  status;
    char message[256];
    char location[128];
} tempo_diagnostics_t;

/* One compiled connection. This is the hot-path record: two indices, a
 * delay, and a type tag. */
typedef struct tempo_route {
    uint32_t     src_global;      /* index into the output slot array */
    uint32_t     dst_global;      /* index into the input slot array  */
    tempo_time_t delay;
    uint32_t     connection_id;
    uint32_t     src_participant;
    uint32_t     dst_participant;
    uint16_t     type;            /* tempo_type_t */
    uint16_t     max_bytes;
} tempo_route_t;

/* ---------------------------------------------------------------- spec ---- */

TEMPO_API tempo_federation_spec_t *tempo_spec_new(void);
TEMPO_API void tempo_spec_free(tempo_federation_spec_t *spec);

TEMPO_API int tempo_spec_set_end_time(tempo_federation_spec_t *spec, tempo_time_t t);
TEMPO_API int tempo_spec_set_policy(tempo_federation_spec_t *spec, const char *policy);
TEMPO_API int tempo_spec_set_quantum(tempo_federation_spec_t *spec, tempo_time_t q);
/* Delay applied to a connection that does not declare one. Defaults to the
 * quantum, which is what makes fixed-window conservative scheduling sound;
 * see docs/scheduling.md. */
TEMPO_API int tempo_spec_set_default_delay(tempo_federation_spec_t *spec, tempo_time_t d);
/* Upper bound on same-time iterations, used to detect a non-converging
 * zero-delay cycle (S17). Default 100. */
TEMPO_API int tempo_spec_set_max_microsteps(tempo_federation_spec_t *spec, uint32_t n);
TEMPO_API int tempo_spec_set_trace_capacity(tempo_federation_spec_t *spec, size_t records);

TEMPO_API int tempo_spec_add_participant(tempo_federation_spec_t *spec,
                                         const char *name,
                                         const char *type,
                                         uint32_t   *out_id);

TEMPO_API int tempo_spec_add_port(tempo_federation_spec_t *spec,
                                  uint32_t         participant_id,
                                  const char      *name,
                                  tempo_port_dir_t dir,
                                  tempo_type_t     type,
                                  uint32_t         max_bytes,
                                  uint32_t        *out_local_index);

/* `from` and `to` are dotted "participant.port" references. A delay of
 * TEMPO_TIME_NEVER means "use the spec default". A delay of 0 makes the
 * connection a same-time (zero-delay) dependency. */
TEMPO_API int tempo_spec_add_connection(tempo_federation_spec_t *spec,
                                        const char   *from,
                                        const char   *to,
                                        tempo_time_t  delay,
                                        uint32_t     *out_connection_id);

TEMPO_API int tempo_spec_subscribe(tempo_federation_spec_t *spec,
                                   const tempo_subscription_t *sub);

/* Returns the participant id, or TEMPO_ID_ANY if there is no such name. */
TEMPO_API uint32_t tempo_spec_find_participant(const tempo_federation_spec_t *spec,
                                               const char *name);

/* ------------------------------------------------------------- compile ---- */

/* Validate and compile. On failure returns NULL and fills `diag` if given. */
TEMPO_API tempo_plan_t *tempo_compile(const tempo_federation_spec_t *spec,
                                      tempo_diagnostics_t *diag);
TEMPO_API void tempo_plan_free(tempo_plan_t *plan);

/* ---------------------------------------------------------------- plan ---- */

TEMPO_API uint32_t     tempo_plan_n_participants(const tempo_plan_t *plan);
TEMPO_API const char  *tempo_plan_participant_name(const tempo_plan_t *plan, uint32_t id);
TEMPO_API const char  *tempo_plan_participant_type(const tempo_plan_t *plan, uint32_t id);
TEMPO_API uint32_t     tempo_plan_n_inputs(const tempo_plan_t *plan, uint32_t id);
TEMPO_API uint32_t     tempo_plan_n_outputs(const tempo_plan_t *plan, uint32_t id);
TEMPO_API uint32_t     tempo_plan_n_input_slots(const tempo_plan_t *plan);
TEMPO_API uint32_t     tempo_plan_n_output_slots(const tempo_plan_t *plan);
TEMPO_API uint32_t     tempo_plan_n_routes(const tempo_plan_t *plan);
TEMPO_API int          tempo_plan_route(const tempo_plan_t *plan, uint32_t i, tempo_route_t *out);
TEMPO_API tempo_time_t tempo_plan_end_time(const tempo_plan_t *plan);
TEMPO_API tempo_time_t tempo_plan_quantum(const tempo_plan_t *plan);
TEMPO_API uint32_t     tempo_plan_max_microsteps(const tempo_plan_t *plan);
TEMPO_API const char  *tempo_plan_policy(const tempo_plan_t *plan);
TEMPO_API size_t       tempo_plan_trace_capacity(const tempo_plan_t *plan);

/* Resolve a dotted "participant.port" reference. */
TEMPO_API int tempo_plan_find_port(const tempo_plan_t *plan,
                                   const char       *dotted,
                                   tempo_port_dir_t  dir,
                                   tempo_port_desc_t *out);
/* Port descriptor by global slot index. */
TEMPO_API const tempo_port_desc_t *tempo_plan_port(const tempo_plan_t *plan,
                                                   tempo_port_dir_t dir,
                                                   uint32_t global_index);

/* --- dependency structure (S15, S16) ---
 *
 * Two graphs are kept. The *dependency* graph contains every connection and
 * is what a future causal-slicing pass needs (S28). The *same-time* graph
 * contains only zero-delay connections; a delayed connection cannot create a
 * same-time cycle, so it is the same-time graph whose strongly connected
 * components decide execution order. */
TEMPO_API uint32_t tempo_plan_n_successors(const tempo_plan_t *plan, uint32_t id);
TEMPO_API uint32_t tempo_plan_successor(const tempo_plan_t *plan, uint32_t id, uint32_t i);

TEMPO_API uint32_t tempo_plan_n_components(const tempo_plan_t *plan);
TEMPO_API uint32_t tempo_plan_component_of(const tempo_plan_t *plan, uint32_t id);
TEMPO_API uint32_t tempo_plan_component_size(const tempo_plan_t *plan, uint32_t comp);
TEMPO_API bool     tempo_plan_component_is_cyclic(const tempo_plan_t *plan, uint32_t comp);

/* Precomputed execution order: participant ids in topological order of the
 * same-time condensation, ties broken by id so the order is deterministic. */
TEMPO_API uint32_t tempo_plan_order_at(const tempo_plan_t *plan, uint32_t i);

TEMPO_API uint32_t tempo_plan_n_subscriptions(const tempo_plan_t *plan);
TEMPO_API int      tempo_plan_subscription(const tempo_plan_t *plan, uint32_t i,
                                           tempo_subscription_t *out);
TEMPO_API uint64_t tempo_plan_trace_core_mask(const tempo_plan_t *plan);
TEMPO_API uint32_t tempo_plan_trace_ns_mask(const tempo_plan_t *plan);

/* Human-readable dump of the compiled plan, for `tempo-run --explain`. */
TEMPO_API int tempo_plan_describe(const tempo_plan_t *plan, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* TEMPO_FEDERATION_H */
