/* tempo/trace.h -- native trace buffers.
 *
 * No JSON is produced on the execution path (S22). Events go into a
 * preallocated ring buffer as fixed-size binary records; a separate tool
 * converts them into JSONL, CSV, or a Chrome trace.
 *
 * The no-observer path is a single predictable branch (S21):
 *
 *     if (TEMPO_UNLIKELY(tempo_trace_wants(tr, type, pid)))
 *         tempo_trace_emit(tr, ...);
 */
#ifndef TEMPO_TRACE_H
#define TEMPO_TRACE_H

#include "tempo/base.h"
#include "tempo/event.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tempo_trace tempo_trace_t;

/* File format: an 64-byte header followed by packed tempo_event_t records. */
#define TEMPO_TRACE_MAGIC   "TEMPOTR1"
#define TEMPO_TRACE_VERSION 1u

typedef struct tempo_trace_file_header {
    char     magic[8];
    uint32_t version;
    uint32_t record_size;
    uint64_t record_count;
    uint64_t dropped_count;
    uint32_t n_participants;
    uint32_t flags;
    uint64_t reserved[3];
} tempo_trace_file_header_t;

typedef struct tempo_trace_config {
    /* Capacity in records. Rounded up to a power of two. 0 uses 65536. */
    size_t capacity;
    /* When the buffer fills: keep the newest records (true) or stop
     * recording and count drops (false). */
    bool overwrite;
} tempo_trace_config_t;

TEMPO_API tempo_trace_t *tempo_trace_new(const tempo_trace_config_t *cfg);
TEMPO_API void           tempo_trace_free(tempo_trace_t *tr);

/* Enable/disable the whole buffer. A disabled trace makes tempo_trace_wants()
 * false for every event with no further work. */
TEMPO_API void tempo_trace_set_enabled(tempo_trace_t *tr, bool enabled);

/* Coarse filters, compiled from subscriptions before the run starts.
 * `core_mask` bit N enables core event type N. `ns_mask` bit N enables
 * non-core namespace N (namespace >> 8). */
TEMPO_API void tempo_trace_set_masks(tempo_trace_t *tr, uint64_t core_mask, uint32_t ns_mask);
TEMPO_API void tempo_trace_set_participant_filter(tempo_trace_t *tr,
                                                  const uint8_t *enabled,
                                                  uint32_t n);

/* Hot-path predicate. Cheap enough to inline at every emit site. */
TEMPO_API bool tempo_trace_wants(const tempo_trace_t *tr, uint16_t type, uint32_t participant_id);

/* Record one event. Assigns the global sequence number. Never allocates. */
TEMPO_API void tempo_trace_emit(tempo_trace_t *tr,
                                tempo_time_t  logical_time,
                                uint32_t      microstep,
                                uint32_t      participant_id,
                                uint16_t      type,
                                uint16_t      flags,
                                uint32_t      arg0,
                                uint64_t      arg1,
                                uint64_t      cause);

/* Statistics. */
TEMPO_API uint64_t tempo_trace_count(const tempo_trace_t *tr);   /* records held    */
TEMPO_API uint64_t tempo_trace_total(const tempo_trace_t *tr);   /* records emitted */
TEMPO_API uint64_t tempo_trace_dropped(const tempo_trace_t *tr);

/* Read record `i` in emission order, oldest first. Returns NULL past the end.
 * Valid until the next emit. */
TEMPO_API const tempo_event_t *tempo_trace_at(const tempo_trace_t *tr, uint64_t i);

TEMPO_API void tempo_trace_clear(tempo_trace_t *tr);

/* Write the buffer to `path` in the binary format above. */
TEMPO_API int tempo_trace_write_file(const tempo_trace_t *tr, const char *path);

#ifdef __cplusplus
}
#endif
#endif /* TEMPO_TRACE_H */
