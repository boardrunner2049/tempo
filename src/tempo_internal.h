/* tempo_internal.h -- private layouts shared inside the library.
 *
 * Nothing here is part of the public ABI. Python and external adapters see
 * only the opaque handles declared in include/tempo.
 */
#ifndef TEMPO_INTERNAL_H
#define TEMPO_INTERNAL_H

#include "tempo/federation.h"
#include "tempo/participant.h"
#include "tempo/scheduler.h"
#include "tempo/trace.h"

#include <stdlib.h>
#include <string.h>

/* Largest payload a BYTES port may declare. Bigger messages are the job of
 * the shared-memory transport (S19), not of a value copied through the
 * delivery queue. */
#define TEMPO_PORT_MAX_BYTES 4096

/* ------------------------------------------------------------ growable ---- */

/* Small helper for the setup path only; never used while the simulation runs. */
int tempo__grow(void **items, uint32_t *cap, uint32_t need, size_t item_size);

#define TEMPO_RESERVE(vecp, capp, need, type)                                  \
    tempo__grow((void **)(vecp), (capp), (need), sizeof(type))

char *tempo__strdup(const char *s);

/* ---------------------------------------------------------------- spec ---- */

typedef struct {
    char            *name;
    tempo_type_t     type;
    uint32_t         max_bytes;
    tempo_port_dir_t dir;
} spec_port_t;

typedef struct {
    char        *name;
    char        *type;
    spec_port_t *ports;
    uint32_t     n_ports;
    uint32_t     cap_ports;
} spec_participant_t;

typedef struct {
    char        *from;
    char        *to;
    tempo_time_t delay;   /* TEMPO_TIME_NEVER = use the spec default */
} spec_conn_t;

struct tempo_federation_spec {
    spec_participant_t   *parts;
    uint32_t              n_parts, cap_parts;
    spec_conn_t          *conns;
    uint32_t              n_conns, cap_conns;
    tempo_subscription_t *subs;
    uint32_t              n_subs, cap_subs;

    tempo_time_t end_time;
    tempo_time_t quantum;
    tempo_time_t default_delay;   /* TEMPO_TIME_NEVER = follow the quantum */
    uint32_t     max_microsteps;
    size_t       trace_capacity;
    char         policy[32];
};

/* ---------------------------------------------------------------- plan ---- */

typedef struct {
    char    *name;
    char    *type;
    uint32_t input_base,  n_inputs;
    uint32_t output_base, n_outputs;
    uint32_t component;
    uint32_t succ_off, n_succ;      /* into plan->succ */
} plan_participant_t;

struct tempo_plan {
    plan_participant_t *parts;
    uint32_t            n_parts;

    tempo_port_desc_t *inputs;
    uint32_t           n_input_slots;
    tempo_port_desc_t *outputs;
    uint32_t           n_output_slots;

    tempo_route_t *routes;
    uint32_t       n_routes;
    bool           has_zero_delay;

    /* Route that writes each input slot, or TEMPO_ID_ANY. Inputs are
     * exclusive: two connections into one input is a compile error. */
    uint32_t *input_writer;

    /* Outgoing routes per output slot, as a CSR list, so harvesting a changed
     * output is a contiguous scan. */
    uint32_t *out_route_off;   /* n_output_slots + 1 */
    uint32_t *out_route_idx;   /* n_routes */

    /* Full dependency graph, every connection, CSR by participant (S15). */
    uint32_t *succ;
    uint32_t  n_succ_total;

    /* Strongly connected components of the *zero-delay* subgraph (S16). */
    uint32_t  n_components;
    uint32_t *comp_size;
    uint8_t  *comp_cyclic;
    uint32_t *order;           /* execution order, n_parts entries */

    tempo_subscription_t *subs;
    uint32_t              n_subs;
    uint64_t              trace_core_mask;
    uint32_t              trace_ns_mask;
    /* Per participant: 1 if any subscription can match it. */
    uint8_t              *trace_participants;

    tempo_time_t end_time;
    tempo_time_t quantum;
    tempo_time_t default_delay;
    uint32_t     max_microsteps;
    size_t       trace_capacity;
    char         policy[32];
};

/* Tarjan on the zero-delay subgraph. Fills comp/of, sizes, cyclic flags and a
 * deterministic execution order. Implemented in src/federation/graph.c. */
int tempo__compute_components(struct tempo_plan *plan);

/* ----------------------------------------------------------- deliveries --- */

/* A value in flight: written by its source at `time - route.delay`, made
 * visible at `time`.
 *
 * This struct is copied on every heap sift, so it stays small: 40 bytes, with
 * scalar values inline. A BYTES payload lives in the queue's arena instead,
 * because making every delivery as large as the largest possible message
 * would put a multi-kilobyte memcpy on the routing path. */
typedef struct {
    tempo_time_t time;
    uint64_t     seq;          /* enqueue order, the final tie-break */
    uint32_t     route;
    uint32_t     len;          /* BYTES: payload length */
    uint32_t     payload;      /* BYTES: offset into the queue arena */
    uint32_t     _pad;
    union {
        bool     b;
        int64_t  i64;
        float    f32;
        double   f64;
        uint64_t raw;
    } scalar;
} tempo_delivery_t;

/* Deterministic min-heap ordered by (time, connection_id, seq). Host thread
 * only; no locking.
 *
 * `arena` is a bump allocator for BYTES payloads. It is rewound whenever the
 * queue is empty and a new value arrives, which is the common case because
 * every communication point drains it -- so it does not grow without bound
 * while still keeping a popped entry's payload readable until the next push.
 */
typedef struct {
    tempo_delivery_t    *items;
    uint32_t             n, cap;
    uint8_t             *arena;
    uint32_t             arena_len, arena_cap;
    const tempo_route_t *routes;   /* borrowed, for the connection-id key */
} tempo_delivery_queue_t;

void tempo__dq_init(tempo_delivery_queue_t *q, const tempo_route_t *routes);
void tempo__dq_clear(tempo_delivery_queue_t *q);
void tempo__dq_free(tempo_delivery_queue_t *q);
/* Push a delivery. `payload`/`payload_len` are used only for BYTES routes;
 * the queue copies them into its arena and fills in d->payload and d->len. */
int  tempo__dq_push(tempo_delivery_queue_t *q, tempo_delivery_t *d,
                    const void *payload, uint32_t payload_len);
/* Payload of a delivery, valid until the next push. */
const uint8_t *tempo__dq_payload(const tempo_delivery_queue_t *q,
                                 const tempo_delivery_t *d);
/* Peek/pop the earliest delivery. Returns NULL / false when empty. */
const tempo_delivery_t *tempo__dq_peek(const tempo_delivery_queue_t *q);
bool tempo__dq_pop(tempo_delivery_queue_t *q, tempo_delivery_t *out);

#endif /* TEMPO_INTERNAL_H */
