#include "tempo/trace.h"
#include "tempo_test.h"

#include <stdio.h>

static tempo_trace_t *make(size_t cap, bool overwrite)
{
    tempo_trace_config_t cfg;

    memset(&cfg, 0, sizeof(cfg));
    cfg.capacity = cap;
    cfg.overwrite = overwrite;
    return tempo_trace_new(&cfg);
}

static void emit_n(tempo_trace_t *tr, uint32_t n, uint16_t type)
{
    uint32_t i;

    for (i = 0; i < n; i++)
        tempo_trace_emit(tr, i * 1000, 0, 0, type, TEMPO_EVF_OBSERVE, i, i * 2, 0);
}

static void test_sequence_and_order(void)
{
    tempo_trace_t *tr = make(16, false);
    uint64_t i;

    CHECK(tr != NULL);
    emit_n(tr, 8, TEMPO_EV_USER_EVENT);
    CHECK_U64(tempo_trace_count(tr), 8);
    CHECK_U64(tempo_trace_total(tr), 8);
    CHECK_U64(tempo_trace_dropped(tr), 0);

    /* Sequence numbers are the global ordering guarantee (S26). */
    for (i = 0; i < 8; i++) {
        const tempo_event_t *e = tempo_trace_at(tr, i);
        CHECK(e != NULL);
        if (!e)
            break;
        CHECK_U64(e->hdr.sequence, i);
        CHECK_U64(e->hdr.logical_time, i * 1000);
        CHECK_U64(e->arg0, i);
        CHECK_U64(e->arg1, i * 2);
    }
    CHECK(tempo_trace_at(tr, 8) == NULL);
    tempo_trace_free(tr);
}

static void test_drop_when_full(void)
{
    tempo_trace_t *tr = make(4, false);

    emit_n(tr, 10, TEMPO_EV_USER_EVENT);
    CHECK_U64(tempo_trace_count(tr), 4);
    CHECK_U64(tempo_trace_total(tr), 4);   /* only what was actually stored */
    CHECK_U64(tempo_trace_dropped(tr), 6);
    /* The oldest records are the ones kept. */
    CHECK_U64(tempo_trace_at(tr, 0)->arg0, 0);
    CHECK_U64(tempo_trace_at(tr, 3)->arg0, 3);
    tempo_trace_free(tr);
}

static void test_overwrite_keeps_newest(void)
{
    tempo_trace_t *tr = make(4, true);

    emit_n(tr, 10, TEMPO_EV_USER_EVENT);
    CHECK_U64(tempo_trace_count(tr), 4);
    CHECK_U64(tempo_trace_total(tr), 10);
    CHECK_U64(tempo_trace_dropped(tr), 6);
    /* Reading in emission order must work across the wrap. */
    CHECK_U64(tempo_trace_at(tr, 0)->arg0, 6);
    CHECK_U64(tempo_trace_at(tr, 3)->arg0, 9);
    CHECK_U64(tempo_trace_at(tr, 0)->hdr.sequence, 6);
    tempo_trace_free(tr);
}

static void test_filters(void)
{
    tempo_trace_t *tr = make(64, false);
    uint8_t allow[3] = { 1, 0, 1 };

    /* Only MMIO writes, and only from participants 0 and 2. */
    tempo_trace_set_masks(tr, 1ULL << TEMPO_EV_MMIO_WRITE, 0);
    tempo_trace_set_participant_filter(tr, allow, 3);

    CHECK(tempo_trace_wants(tr, TEMPO_EV_MMIO_WRITE, 0));
    CHECK(!tempo_trace_wants(tr, TEMPO_EV_MMIO_READ, 0));
    CHECK(!tempo_trace_wants(tr, TEMPO_EV_MMIO_WRITE, 1));
    CHECK(tempo_trace_wants(tr, TEMPO_EV_MMIO_WRITE, 2));

    /* A namespace bit gates non-core types. */
    tempo_trace_set_masks(tr, 0, 1u << (TEMPO_EV_NS_FASTDYN >> 8));
    CHECK(tempo_trace_wants(tr, TEMPO_EV_NS_FASTDYN + 3, 0));
    CHECK(!tempo_trace_wants(tr, TEMPO_EV_NS_FMI + 3, 0));

    tempo_trace_set_enabled(tr, false);
    CHECK(!tempo_trace_wants(tr, TEMPO_EV_NS_FASTDYN + 3, 0));
    tempo_trace_free(tr);
}

static void test_wants_tolerates_null(void)
{
    /* An adapter must be able to call this unconditionally. */
    CHECK(!tempo_trace_wants(NULL, TEMPO_EV_MMIO_WRITE, 0));
    tempo_trace_emit(NULL, 0, 0, 0, TEMPO_EV_MMIO_WRITE, 0, 0, 0, 0);
}

static void test_clear(void)
{
    tempo_trace_t *tr = make(8, false);

    emit_n(tr, 5, TEMPO_EV_USER_EVENT);
    tempo_trace_clear(tr);
    CHECK_U64(tempo_trace_count(tr), 0);
    CHECK_U64(tempo_trace_total(tr), 5);
    emit_n(tr, 1, TEMPO_EV_USER_EVENT);
    /* Sequence numbers keep counting; clearing drops records, not history. */
    CHECK_U64(tempo_trace_at(tr, 0)->hdr.sequence, 5);
    tempo_trace_free(tr);
}

static void test_file_roundtrip(void)
{
    tempo_trace_t *tr = make(16, false);
    tempo_trace_file_header_t hdr;
    tempo_event_t rec;
    const char *path = "test_trace.out";
    FILE *f;

    emit_n(tr, 6, TEMPO_EV_PORT_WRITE);
    CHECK_I(tempo_trace_write_file(tr, path), TEMPO_OK);
    tempo_trace_free(tr);

    f = fopen(path, "rb");
    CHECK(f != NULL);
    if (!f)
        return;
    CHECK(fread(&hdr, sizeof(hdr), 1, f) == 1);
    CHECK(memcmp(hdr.magic, TEMPO_TRACE_MAGIC, 8) == 0);
    CHECK_U64(hdr.version, TEMPO_TRACE_VERSION);
    CHECK_U64(hdr.record_size, sizeof(tempo_event_t));
    CHECK_U64(hdr.record_count, 6);
    CHECK(fread(&rec, sizeof(rec), 1, f) == 1);
    CHECK_U64(rec.hdr.sequence, 0);
    CHECK_U64(rec.hdr.type, TEMPO_EV_PORT_WRITE);
    fclose(f);
    remove(path);
}

static void test_record_layout(void)
{
    /* The record is part of the trace file format, so its size is a
     * compatibility promise, not an implementation detail. */
    CHECK_U64(sizeof(tempo_event_header_t), 24);
    CHECK_U64(sizeof(tempo_event_t), 48);
    CHECK_STR(tempo_event_type_str(TEMPO_EV_MMIO_WRITE), "mmio_write");
    CHECK_STR(tempo_event_type_str(TEMPO_EV_NS_FASTDYN + 1), "fastdyn");
    CHECK_STR(tempo_event_type_str(TEMPO_EV_NS_USER + 1), "user");
}

int main(void)
{
    RUN(test_sequence_and_order);
    RUN(test_drop_when_full);
    RUN(test_overwrite_keeps_newest);
    RUN(test_filters);
    RUN(test_wants_tolerates_null);
    RUN(test_clear);
    RUN(test_file_roundtrip);
    RUN(test_record_layout);
    return TEST_RESULT();
}
