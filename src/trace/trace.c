/* Native trace ring buffer (S22).
 *
 * A power-of-two capacity keeps the index arithmetic to a mask. The writer
 * never allocates and never formats a string; conversion to a readable
 * format is the job of `tempo-trace`.
 */
#include "tempo/trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEMPO_TRACE_DEFAULT_CAPACITY 65536u

struct tempo_trace {
    tempo_event_t *records;
    size_t         capacity;   /* power of two */
    size_t         mask;
    uint64_t       total;      /* records ever emitted (also the sequence) */
    uint64_t       dropped;
    uint64_t       held;       /* records currently in the buffer */
    uint64_t       base_seq;   /* sequence of the oldest held record */
    uint64_t       core_mask;
    uint32_t       ns_mask;
    uint8_t       *participants;
    uint32_t       n_participants;
    bool           enabled;
    bool           overwrite;
};

static size_t round_pow2(size_t n)
{
    size_t p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

tempo_trace_t *tempo_trace_new(const tempo_trace_config_t *cfg)
{
    tempo_trace_t *tr;
    size_t cap = cfg && cfg->capacity ? cfg->capacity : TEMPO_TRACE_DEFAULT_CAPACITY;

    tr = (tempo_trace_t *)calloc(1, sizeof(*tr));
    if (!tr)
        return NULL;
    tr->capacity = round_pow2(cap);
    tr->mask = tr->capacity - 1;
    tr->records = (tempo_event_t *)calloc(tr->capacity, sizeof(tempo_event_t));
    if (!tr->records) {
        free(tr);
        return NULL;
    }
    tr->enabled = true;
    tr->overwrite = cfg ? cfg->overwrite : false;
    /* Until a plan installs its filters, record everything. */
    tr->core_mask = ~0ULL;
    tr->ns_mask = ~0u;
    return tr;
}

void tempo_trace_free(tempo_trace_t *tr)
{
    if (!tr)
        return;
    free(tr->records);
    free(tr->participants);
    free(tr);
}

void tempo_trace_set_enabled(tempo_trace_t *tr, bool enabled)
{
    if (tr)
        tr->enabled = enabled;
}

void tempo_trace_set_masks(tempo_trace_t *tr, uint64_t core_mask, uint32_t ns_mask)
{
    if (!tr)
        return;
    tr->core_mask = core_mask;
    tr->ns_mask = ns_mask;
}

void tempo_trace_set_participant_filter(tempo_trace_t *tr, const uint8_t *enabled, uint32_t n)
{
    if (!tr)
        return;
    free(tr->participants);
    tr->participants = NULL;
    tr->n_participants = 0;
    if (!enabled || n == 0)
        return;
    tr->participants = (uint8_t *)malloc(n);
    if (!tr->participants)
        return;
    memcpy(tr->participants, enabled, n);
    tr->n_participants = n;
}

bool tempo_trace_wants(const tempo_trace_t *tr, uint16_t type, uint32_t participant_id)
{
    uint16_t ns;

    if (!tr || !tr->enabled)
        return false;
    if (tr->participants && participant_id < tr->n_participants &&
        !tr->participants[participant_id])
        return false;

    if (type < 64)
        return (tr->core_mask >> type) & 1ULL;
    ns = (uint16_t)(TEMPO_EV_NAMESPACE(type) >> 8);
    return (tr->ns_mask >> (ns & 31u)) & 1u;
}

void tempo_trace_emit(tempo_trace_t *tr,
                      tempo_time_t logical_time,
                      uint32_t     microstep,
                      uint32_t     participant_id,
                      uint16_t     type,
                      uint16_t     flags,
                      uint32_t     arg0,
                      uint64_t     arg1,
                      uint64_t     cause)
{
    tempo_event_t *e;

    if (!tr)
        return;
    if (tr->held == tr->capacity) {
        if (!tr->overwrite) {
            tr->dropped++;
            return;
        }
        /* Drop the oldest record. */
        tr->base_seq++;
        tr->held--;
        tr->dropped++;
    }

    e = &tr->records[(tr->base_seq + tr->held) & tr->mask];
    e->hdr.logical_time = logical_time;
    e->hdr.sequence = tr->total;
    e->hdr.participant_id = participant_id;
    e->hdr.type = type;
    e->hdr.flags = flags;
    e->microstep = microstep;
    e->arg0 = arg0;
    e->arg1 = arg1;
    e->cause = cause;

    tr->total++;
    tr->held++;
}

uint64_t tempo_trace_count(const tempo_trace_t *tr)   { return tr ? tr->held : 0; }
uint64_t tempo_trace_total(const tempo_trace_t *tr)   { return tr ? tr->total : 0; }
uint64_t tempo_trace_dropped(const tempo_trace_t *tr) { return tr ? tr->dropped : 0; }

const tempo_event_t *tempo_trace_at(const tempo_trace_t *tr, uint64_t i)
{
    if (!tr || i >= tr->held)
        return NULL;
    return &tr->records[(tr->base_seq + i) & tr->mask];
}

void tempo_trace_clear(tempo_trace_t *tr)
{
    if (!tr)
        return;
    tr->held = 0;
    tr->base_seq = tr->total;
}

int tempo_trace_write_file(const tempo_trace_t *tr, const char *path)
{
    tempo_trace_file_header_t hdr;
    FILE *f;
    uint64_t i;

    if (!tr || !path)
        return TEMPO_ERR_INVALID_ARG;
    f = fopen(path, "wb");
    if (!f)
        return TEMPO_ERR_IO;

    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, TEMPO_TRACE_MAGIC, 8);
    hdr.version = TEMPO_TRACE_VERSION;
    hdr.record_size = (uint32_t)sizeof(tempo_event_t);
    hdr.record_count = tr->held;
    hdr.dropped_count = tr->dropped;
    hdr.n_participants = tr->n_participants;

    if (fwrite(&hdr, sizeof(hdr), 1, f) != 1) {
        fclose(f);
        return TEMPO_ERR_IO;
    }
    /* The ring may wrap, so write in emission order rather than memory order. */
    for (i = 0; i < tr->held; i++) {
        const tempo_event_t *e = &tr->records[(tr->base_seq + i) & tr->mask];
        if (fwrite(e, sizeof(*e), 1, f) != 1) {
            fclose(f);
            return TEMPO_ERR_IO;
        }
    }
    if (fclose(f) != 0)
        return TEMPO_ERR_IO;
    return TEMPO_OK;
}
