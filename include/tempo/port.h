/* tempo/port.h -- typed communication ports and value slots.
 *
 * Participants never call each other. They read and write typed slots that
 * the scheduler owns (S18). A slot array is plain memory, so the same layout
 * works for an in-process participant and, later, for a shared-memory
 * segment mapped into another process (S19).
 *
 * The scheduler decides *when* a written value becomes logically visible;
 * writing a slot is not itself a synchronizing act.
 */
#ifndef TEMPO_PORT_H
#define TEMPO_PORT_H

#include "tempo/base.h"
#include "tempo/time.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum tempo_type {
    TEMPO_TYPE_BOOL    = 0,
    TEMPO_TYPE_INT64   = 1,
    TEMPO_TYPE_FLOAT32 = 2,
    TEMPO_TYPE_FLOAT64 = 3,
    TEMPO_TYPE_BYTES   = 4,
    TEMPO_TYPE__COUNT
} tempo_type_t;

typedef enum tempo_port_dir {
    TEMPO_PORT_INPUT  = 0,
    TEMPO_PORT_OUTPUT = 1
} tempo_port_dir_t;

TEMPO_API const char *tempo_type_str(tempo_type_t t);
/* Parse "bool" | "int64" | "float32" | "float64" | "bytes". */
TEMPO_API int tempo_type_parse(const char *name, tempo_type_t *out);

/* A single value slot. Layout is part of the ABI: participants and adapters
 * read and write these directly on the fast path, with no function call.
 *
 * `generation` is bumped by the scheduler each time it makes a new value
 * visible in this slot. A participant can therefore detect a changed input
 * with one integer compare instead of a callback. */
typedef struct tempo_slot {
    uint64_t generation;
    uint32_t len;      /* BYTES: number of valid bytes in `bytes`  */
    uint32_t capacity; /* BYTES: allocated size of `bytes`         */
    union {
        bool     b;
        int64_t  i64;
        float    f32;
        double   f64;
        uint64_t raw;
    } scalar;
    uint8_t *bytes;    /* BYTES only; NULL for scalar ports */
} tempo_slot_t;

/* Compiled description of one port. Produced by the federation compiler;
 * the runtime never looks a port up by name (S13). */
typedef struct tempo_port_desc {
    const char      *name;           /* owned by the plan */
    uint32_t         participant_id;
    uint32_t         local_index;    /* index within the participant's array   */
    uint32_t         global_index;   /* index into the federation slot array   */
    tempo_type_t     type;
    uint32_t         max_bytes;      /* BYTES only */
    tempo_port_dir_t dir;
} tempo_port_desc_t;

/* Convenience accessors. These exist for adapters written in C; they compile
 * to a single load or store. */
TEMPO_INLINE void tempo_slot_set_f64(tempo_slot_t *s, double v) { s->scalar.f64 = v; }
TEMPO_INLINE void tempo_slot_set_f32(tempo_slot_t *s, float v)  { s->scalar.f32 = v; }
TEMPO_INLINE void tempo_slot_set_i64(tempo_slot_t *s, int64_t v){ s->scalar.i64 = v; }
TEMPO_INLINE void tempo_slot_set_bool(tempo_slot_t *s, bool v)  { s->scalar.b = v; }
TEMPO_INLINE double  tempo_slot_f64(const tempo_slot_t *s)  { return s->scalar.f64; }
TEMPO_INLINE float   tempo_slot_f32(const tempo_slot_t *s)  { return s->scalar.f32; }
TEMPO_INLINE int64_t tempo_slot_i64(const tempo_slot_t *s)  { return s->scalar.i64; }
TEMPO_INLINE bool    tempo_slot_bool(const tempo_slot_t *s) { return s->scalar.b; }

/* Copy a BYTES payload into a slot, truncating at the slot's capacity.
 * Returns the number of bytes stored. */
TEMPO_API uint32_t tempo_slot_set_bytes(tempo_slot_t *s, const void *data, uint32_t len);

#ifdef __cplusplus
}
#endif
#endif /* TEMPO_PORT_H */
