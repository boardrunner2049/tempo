/* Scheduler semantics (S37).
 *
 * These are the tests that have to pass before any real simulator is
 * attached: logical time, early return, deterministic ordering,
 * communication points, same-time propagation, and reset.
 */
#include "tempo/scheduler.h"
#include "tempo_mock.h"
#include "tempo_test.h"

#define MAX_PARTS 8

typedef struct {
    tempo_federation_spec_t *spec;
    tempo_plan_t            *plan;
    tempo_scheduler_t       *sched;
    tempo_trace_t           *trace;
    tempo_mock_t            *mocks[MAX_PARTS];
    uint32_t                 n_mocks;
} fixture_t;

static void fx_init(fixture_t *fx, tempo_time_t end, tempo_time_t quantum)
{
    memset(fx, 0, sizeof(*fx));
    fx->spec = tempo_spec_new();
    tempo_spec_set_end_time(fx->spec, end);
    tempo_spec_set_quantum(fx->spec, quantum);
}

/* Adds a mock participant and remembers it so it can be bound later.
 * Capabilities beyond the mock's defaults are applied in fx_start(). */
static uint32_t fx_add(fixture_t *fx, const char *name, const char *script,
                       bool echo, double gain)
{
    const char *err = NULL;
    uint32_t id = TEMPO_ID_ANY;
    tempo_mock_config_t cfg;
    tempo_mock_t *m;

    if (script) {
        m = tempo_mock_from_script(script, 0, echo, gain, &err);
        if (!m)
            fprintf(stderr, "    bad script \"%s\": %s\n", script, err ? err : "?");
    } else {
        memset(&cfg, 0, sizeof(cfg));
        cfg.echo_inputs = echo;
        cfg.echo_gain = gain;
        m = tempo_mock_new(&cfg);
    }
    CHECK(m != NULL);
    if (!m)
        return id;
    tempo_spec_add_participant(fx->spec, name, "mock", &id);
    fx->mocks[id] = m;
    if (id + 1 > fx->n_mocks)
        fx->n_mocks = id + 1;
    return id;
}

static int fx_start(fixture_t *fx, size_t trace_cap, uint32_t *extra_caps)
{
    tempo_diagnostics_t diag;
    tempo_trace_config_t tc;
    uint32_t i;
    int rc;

    fx->plan = tempo_compile(fx->spec, &diag);
    if (!fx->plan) {
        fprintf(stderr, "    compile failed at %s: %s\n", diag.location, diag.message);
        return diag.status;
    }
    fx->sched = tempo_scheduler_new(fx->plan);
    CHECK(fx->sched != NULL);
    if (!fx->sched)
        return TEMPO_ERR_NO_MEMORY;

    if (trace_cap) {
        memset(&tc, 0, sizeof(tc));
        tc.capacity = trace_cap;
        fx->trace = tempo_trace_new(&tc);
        tempo_scheduler_set_trace(fx->sched, fx->trace);
    }

    for (i = 0; i < tempo_plan_n_participants(fx->plan); i++) {
        tempo_capabilities_t caps = tempo_mock_capabilities(fx->mocks[i]);
        if (extra_caps)
            caps.flags |= extra_caps[i];
        rc = tempo_scheduler_bind(fx->sched, i, tempo_mock_vtable(),
                                  fx->mocks[i], &caps);
        if (rc != TEMPO_OK)
            return rc;
    }
    return tempo_scheduler_init(fx->sched);
}

static void fx_free(fixture_t *fx)
{
    uint32_t i;

    tempo_scheduler_free(fx->sched);
    tempo_trace_free(fx->trace);
    tempo_plan_free(fx->plan);
    tempo_spec_free(fx->spec);
    for (i = 0; i < fx->n_mocks; i++)
        tempo_mock_free(fx->mocks[i]);
    memset(fx, 0, sizeof(*fx));
}

static void add_ports(fixture_t *fx, uint32_t id, const char *in, const char *out,
                      tempo_type_t type, uint32_t max_bytes)
{
    if (in)
        tempo_spec_add_port(fx->spec, id, in, TEMPO_PORT_INPUT, type, max_bytes, NULL);
    if (out)
        tempo_spec_add_port(fx->spec, id, out, TEMPO_PORT_OUTPUT, type, max_bytes, NULL);
}

static double out_value(const fixture_t *fx, const char *dotted)
{
    tempo_port_desc_t d;

    if (tempo_plan_find_port(fx->plan, dotted, TEMPO_PORT_OUTPUT, &d) != TEMPO_OK) {
        CHECK(!"output port not found");
        return 0.0;
    }
    return tempo_scheduler_output_slots(fx->sched)[d.global_index].scalar.f64;
}

static const tempo_slot_t *in_slot(const fixture_t *fx, const char *dotted)
{
    tempo_port_desc_t d;

    if (tempo_plan_find_port(fx->plan, dotted, TEMPO_PORT_INPUT, &d) != TEMPO_OK) {
        CHECK(!"input port not found");
        return NULL;
    }
    return &tempo_scheduler_input_slots(fx->sched)[d.global_index];
}

/* ------------------------------------------------------------------------- */

static void test_runs_to_end_time(void)
{
    fixture_t fx;

    fx_init(&fx, 10 * TEMPO_NS_PER_MS, TEMPO_NS_PER_MS);
    fx_add(&fx, "a", NULL, false, 1.0);
    CHECK_I(fx_start(&fx, 1024, NULL), TEMPO_OK);
    CHECK_I(tempo_scheduler_run(fx.sched), TEMPO_OK);

    CHECK_U64(tempo_scheduler_now(fx.sched).time, 10 * TEMPO_NS_PER_MS);
    CHECK(tempo_scheduler_finished(fx.sched));
    /* Ten windows of one quantum each: the policy did exactly what it said. */
    CHECK_U64(tempo_scheduler_stats(fx.sched).windows, 10);
    CHECK_U64(tempo_scheduler_stats(fx.sched).advance_calls, 10);
    CHECK_U64(tempo_scheduler_stats(fx.sched).early_returns, 0);
    CHECK_U64(tempo_mock_time(fx.mocks[0]), 10 * TEMPO_NS_PER_MS);
    fx_free(&fx);
}

static void test_logical_time_never_regresses(void)
{
    fixture_t fx;
    tempo_time_t prev = 0;
    int rc;

    fx_init(&fx, 5 * TEMPO_NS_PER_MS, 500 * TEMPO_NS_PER_US);
    fx_add(&fx, "a", "1200us:trap; 2500us:trap", false, 1.0);
    fx_add(&fx, "b", NULL, false, 1.0);
    CHECK_I(fx_start(&fx, 1024, NULL), TEMPO_OK);

    for (;;) {
        tempo_tag_t now;
        rc = tempo_scheduler_step(fx.sched);
        CHECK(rc >= 0);
        if (rc != TEMPO_OK)
            break;
        now = tempo_scheduler_now(fx.sched);
        CHECK(now.time >= prev);
        prev = now.time;
    }
    CHECK_U64(tempo_scheduler_now(fx.sched).time, 5 * TEMPO_NS_PER_MS);
    fx_free(&fx);
}

static void test_early_return(void)
{
    fixture_t fx;

    /* The participant traps out at 3.5 ms, in the middle of a window that
     * would otherwise have run to 4 ms (S10). */
    fx_init(&fx, 10 * TEMPO_NS_PER_MS, TEMPO_NS_PER_MS);
    fx_add(&fx, "a", "3500us:trap", false, 1.0);
    CHECK_I(fx_start(&fx, 1024, NULL), TEMPO_OK);
    CHECK_I(tempo_scheduler_run(fx.sched), TEMPO_OK);

    CHECK_U64(tempo_scheduler_stats(fx.sched).early_returns, 1);
    CHECK_U64(tempo_scheduler_now(fx.sched).time, 10 * TEMPO_NS_PER_MS);
    fx_free(&fx);
}

static void test_prediction_avoids_a_wasted_trap(void)
{
    fixture_t fx;
    uint32_t extra[1];

    /* Same script, but the participant can say when its next event falls, so
     * the scheduler sets the horizon there and the trap is not "early" at
     * all -- exactly the saving S9 and S10 are after. */
    fx_init(&fx, 10 * TEMPO_NS_PER_MS, TEMPO_NS_PER_MS);
    fx_add(&fx, "a", "3500us:trap", false, 1.0);
    extra[0] = TEMPO_CAP_PREDICT_NEXT;
    CHECK_I(fx_start(&fx, 1024, extra), TEMPO_OK);
    CHECK_I(tempo_scheduler_run(fx.sched), TEMPO_OK);

    CHECK_U64(tempo_scheduler_stats(fx.sched).early_returns, 0);
    CHECK_U64(tempo_scheduler_now(fx.sched).time, 10 * TEMPO_NS_PER_MS);
    fx_free(&fx);
}

static void test_delayed_value_propagation(void)
{
    fixture_t fx;
    uint32_t a, b;
    const tempo_slot_t *slot;

    fx_init(&fx, 10 * TEMPO_NS_PER_MS, TEMPO_NS_PER_MS);
    a = fx_add(&fx, "a", "3ms:emit_sync 0 0.7", false, 1.0);
    b = fx_add(&fx, "b", NULL, true, 1.0);
    add_ports(&fx, a, NULL, "cmd", TEMPO_TYPE_FLOAT64, 0);
    add_ports(&fx, b, "cmd", "mirror", TEMPO_TYPE_FLOAT64, 0);
    tempo_spec_add_connection(fx.spec, "a.cmd", "b.cmd", TEMPO_TIME_NEVER, NULL);

    CHECK_I(fx_start(&fx, 4096, NULL), TEMPO_OK);
    CHECK_I(tempo_scheduler_run(fx.sched), TEMPO_OK);

    slot = in_slot(&fx, "b.cmd");
    CHECK(slot != NULL);
    if (slot) {
        CHECK_F(slot->scalar.f64, 0.7, 1e-12);
        /* Exactly one value was made visible, so one generation bump. */
        CHECK_U64(slot->generation, 1);
    }
    CHECK_F(out_value(&fx, "b.mirror"), 0.7, 1e-12);
    CHECK_F(tempo_mock_last_input(fx.mocks[b], 0), 0.7, 1e-12);
    CHECK_U64(tempo_scheduler_stats(fx.sched).deliveries, 1);
    fx_free(&fx);
}

static void test_delivery_lands_at_produce_time_plus_delay(void)
{
    fixture_t fx;
    uint32_t a, b;
    uint64_t i, n;
    bool found = false;

    fx_init(&fx, 10 * TEMPO_NS_PER_MS, TEMPO_NS_PER_MS);
    a = fx_add(&fx, "a", "3ms:emit_sync 0 0.7", false, 1.0);
    b = fx_add(&fx, "b", NULL, false, 1.0);
    add_ports(&fx, a, NULL, "cmd", TEMPO_TYPE_FLOAT64, 0);
    add_ports(&fx, b, "cmd", NULL, TEMPO_TYPE_FLOAT64, 0);
    /* An explicit 250 us delay, so the arrival time is not the quantum. */
    tempo_spec_add_connection(fx.spec, "a.cmd", "b.cmd", 250 * TEMPO_NS_PER_US, NULL);

    CHECK_I(fx_start(&fx, 4096, NULL), TEMPO_OK);
    CHECK_I(tempo_scheduler_run(fx.sched), TEMPO_OK);

    n = tempo_trace_count(fx.trace);
    for (i = 0; i < n; i++) {
        const tempo_event_t *e = tempo_trace_at(fx.trace, i);
        if (e->hdr.type == TEMPO_EV_ROUTE_DELIVER) {
            /* Produced at 3 ms, delayed 250 us, visible at 3.25 ms. */
            CHECK_U64(e->hdr.logical_time, 3 * TEMPO_NS_PER_MS + 250 * TEMPO_NS_PER_US);
            found = true;
        }
    }
    CHECK(found);
    fx_free(&fx);
}

static void test_same_time_chain(void)
{
    fixture_t fx;
    uint32_t a, b, c;

    /* a -> b -> c with zero delay: one value must reach the end of the chain
     * within a single logical instant, using microsteps (S17). */
    fx_init(&fx, 10 * TEMPO_NS_PER_MS, TEMPO_NS_PER_MS);
    a = fx_add(&fx, "a", "3ms:emit_sync 0 1.0", false, 1.0);
    b = fx_add(&fx, "b", NULL, true, 2.0);
    c = fx_add(&fx, "c", NULL, true, 3.0);
    add_ports(&fx, a, NULL, "o", TEMPO_TYPE_FLOAT64, 0);
    add_ports(&fx, b, "i", "o", TEMPO_TYPE_FLOAT64, 0);
    add_ports(&fx, c, "i", "o", TEMPO_TYPE_FLOAT64, 0);
    tempo_spec_add_connection(fx.spec, "a.o", "b.i", 0, NULL);
    tempo_spec_add_connection(fx.spec, "b.o", "c.i", 0, NULL);

    CHECK_I(fx_start(&fx, 4096, NULL), TEMPO_OK);
    CHECK_I(tempo_scheduler_run(fx.sched), TEMPO_OK);

    CHECK_F(out_value(&fx, "b.o"), 2.0, 1e-12);   /* 1.0 * gain 2 */
    CHECK_F(out_value(&fx, "c.o"), 6.0, 1e-12);   /* 2.0 * gain 3 */
    /* Two hops means two same-time iterations, and no physical time passed. */
    CHECK_U64(tempo_scheduler_stats(fx.sched).max_microstep, 2);
    fx_free(&fx);
}

static void test_same_time_cycle_converges(void)
{
    fixture_t fx;
    uint32_t a, b;

    /* Unity gain around the loop reaches a fixed point, so the microstep
     * iteration terminates on its own. */
    fx_init(&fx, 5 * TEMPO_NS_PER_MS, TEMPO_NS_PER_MS);
    a = fx_add(&fx, "a", "1ms:emit_sync 0 1.0", true, 1.0);
    b = fx_add(&fx, "b", NULL, true, 1.0);
    add_ports(&fx, a, "fb", "o", TEMPO_TYPE_FLOAT64, 0);
    add_ports(&fx, b, "i", "o", TEMPO_TYPE_FLOAT64, 0);
    tempo_spec_add_connection(fx.spec, "a.o", "b.i", 0, NULL);
    tempo_spec_add_connection(fx.spec, "b.o", "a.fb", 0, NULL);

    CHECK_I(fx_start(&fx, 4096, NULL), TEMPO_OK);
    CHECK_I(tempo_scheduler_run(fx.sched), TEMPO_OK);
    CHECK_F(out_value(&fx, "b.o"), 1.0, 1e-12);
    CHECK(tempo_scheduler_stats(fx.sched).max_microstep >= 1);
    fx_free(&fx);
}

static void test_same_time_cycle_that_diverges_is_reported(void)
{
    fixture_t fx;
    uint32_t a, b;

    /* Gain 2 around the loop never settles. The bound on same-time iteration
     * is what turns a hang into a diagnosable error (S17). */
    fx_init(&fx, 5 * TEMPO_NS_PER_MS, TEMPO_NS_PER_MS);
    tempo_spec_set_max_microsteps(fx.spec, 8);
    a = fx_add(&fx, "a", "1ms:emit_sync 0 1.0", true, 2.0);
    b = fx_add(&fx, "b", NULL, true, 2.0);
    add_ports(&fx, a, "fb", "o", TEMPO_TYPE_FLOAT64, 0);
    add_ports(&fx, b, "i", "o", TEMPO_TYPE_FLOAT64, 0);
    tempo_spec_add_connection(fx.spec, "a.o", "b.i", 0, NULL);
    tempo_spec_add_connection(fx.spec, "b.o", "a.fb", 0, NULL);

    CHECK_I(fx_start(&fx, 4096, NULL), TEMPO_OK);
    CHECK_I(tempo_scheduler_run(fx.sched), TEMPO_ERR_NON_CONVERGENT);
    CHECK(strstr(tempo_scheduler_error(fx.sched), "converge") != NULL);
    fx_free(&fx);
}

static void test_zero_delay_requires_same_time_capability(void)
{
    fixture_t fx;
    uint32_t a, b;

    /* b cannot re-evaluate at one instant, so a zero-delay connection into it
     * is refused before the run rather than silently mis-scheduled. */
    fx_init(&fx, 5 * TEMPO_NS_PER_MS, TEMPO_NS_PER_MS);
    a = fx_add(&fx, "a", "1ms:emit_sync 0 1.0", false, 1.0);
    b = fx_add(&fx, "b", NULL, false, 1.0);
    add_ports(&fx, a, NULL, "o", TEMPO_TYPE_FLOAT64, 0);
    add_ports(&fx, b, "i", NULL, TEMPO_TYPE_FLOAT64, 0);
    tempo_spec_add_connection(fx.spec, "a.o", "b.i", 0, NULL);

    CHECK_I(fx_start(&fx, 0, NULL), TEMPO_ERR_UNSUPPORTED);
    CHECK(strstr(tempo_scheduler_error(fx.sched), "same logical time") != NULL);
    fx_free(&fx);
}

static void test_finish_ends_the_run_early(void)
{
    fixture_t fx;

    fx_init(&fx, 10 * TEMPO_NS_PER_MS, TEMPO_NS_PER_MS);
    fx_add(&fx, "a", "2ms:finish", false, 1.0);
    CHECK_I(fx_start(&fx, 1024, NULL), TEMPO_OK);
    CHECK_I(tempo_scheduler_run(fx.sched), TEMPO_OK);

    CHECK(tempo_scheduler_finished(fx.sched));
    CHECK_U64(tempo_scheduler_now(fx.sched).time, 2 * TEMPO_NS_PER_MS);
    fx_free(&fx);
}

static void test_run_until_is_resumable(void)
{
    fixture_t fx;

    fx_init(&fx, 10 * TEMPO_NS_PER_MS, TEMPO_NS_PER_MS);
    fx_add(&fx, "a", NULL, false, 1.0);
    CHECK_I(fx_start(&fx, 1024, NULL), TEMPO_OK);

    CHECK_I(tempo_scheduler_run_until(fx.sched, 4 * TEMPO_NS_PER_MS), TEMPO_OK);
    CHECK_U64(tempo_scheduler_now(fx.sched).time, 4 * TEMPO_NS_PER_MS);
    CHECK(!tempo_scheduler_finished(fx.sched));

    CHECK_I(tempo_scheduler_run(fx.sched), TEMPO_OK);
    CHECK_U64(tempo_scheduler_now(fx.sched).time, 10 * TEMPO_NS_PER_MS);
    CHECK(tempo_scheduler_finished(fx.sched));

    /* Asking to run past the federation's end time is a mistake, not a clamp. */
    CHECK(tempo_scheduler_run_until(fx.sched, 20 * TEMPO_NS_PER_MS) != TEMPO_OK);
    fx_free(&fx);
}

static void test_bytes_port(void)
{
    fixture_t fx;
    uint32_t a, b;
    const tempo_slot_t *slot;

    fx_init(&fx, 5 * TEMPO_NS_PER_MS, TEMPO_NS_PER_MS);
    a = fx_add(&fx, "a", "1ms:emit_sync 0 1.5", false, 1.0);
    b = fx_add(&fx, "b", NULL, false, 1.0);
    add_ports(&fx, a, NULL, "frame", TEMPO_TYPE_BYTES, 32);
    add_ports(&fx, b, "frame", NULL, TEMPO_TYPE_BYTES, 32);
    tempo_spec_add_connection(fx.spec, "a.frame", "b.frame", TEMPO_TIME_NEVER, NULL);

    CHECK_I(fx_start(&fx, 1024, NULL), TEMPO_OK);
    CHECK_I(tempo_scheduler_run(fx.sched), TEMPO_OK);

    slot = in_slot(&fx, "b.frame");
    CHECK(slot != NULL);
    if (slot) {
        double v = 0.0;
        CHECK_U64(slot->len, sizeof(double));
        CHECK_U64(slot->capacity, 32);
        if (slot->len == sizeof(double))
            memcpy(&v, slot->bytes, sizeof(v));
        CHECK_F(v, 1.5, 1e-12);
    }
    fx_free(&fx);
}

static void test_unchanged_values_are_not_rerouted(void)
{
    fixture_t fx;
    uint32_t a, b;

    /* Writing the same value twice must cost nothing downstream (S12). */
    fx_init(&fx, 10 * TEMPO_NS_PER_MS, TEMPO_NS_PER_MS);
    a = fx_add(&fx, "a", "1ms:emit_sync 0 0.5; 2ms:emit_sync 0 0.5; 3ms:emit_sync 0 0.9",
               false, 1.0);
    b = fx_add(&fx, "b", NULL, false, 1.0);
    add_ports(&fx, a, NULL, "o", TEMPO_TYPE_FLOAT64, 0);
    add_ports(&fx, b, "i", NULL, TEMPO_TYPE_FLOAT64, 0);
    tempo_spec_add_connection(fx.spec, "a.o", "b.i", TEMPO_TIME_NEVER, NULL);

    CHECK_I(fx_start(&fx, 4096, NULL), TEMPO_OK);
    CHECK_I(tempo_scheduler_run(fx.sched), TEMPO_OK);
    /* Three writes, two distinct values, two deliveries. */
    CHECK_U64(tempo_scheduler_stats(fx.sched).deliveries, 2);
    fx_free(&fx);
}

/* Build the same federation twice and compare traces record for record. */
static void run_reference(tempo_trace_t **out_trace)
{
    fixture_t fx;
    uint32_t a, b, c;

    fx_init(&fx, 10 * TEMPO_NS_PER_MS, TEMPO_NS_PER_MS);
    a = fx_add(&fx, "flight", "2ms:emit_sync 0 0.3; 4500us:trap; 7ms:emit_sync 0 0.9",
               false, 1.0);
    b = fx_add(&fx, "world", NULL, true, 0.5);
    c = fx_add(&fx, "motor", "3ms:emit_sync 0 1.0; 6ms:trap", false, 1.0);
    add_ports(&fx, a, "gyro", "cmd", TEMPO_TYPE_FLOAT64, 0);
    add_ports(&fx, b, "cmd", "gyro", TEMPO_TYPE_FLOAT64, 0);
    tempo_spec_add_port(fx.spec, b, "aux", TEMPO_PORT_INPUT, TEMPO_TYPE_FLOAT64, 0, NULL);
    add_ports(&fx, c, NULL, "aux", TEMPO_TYPE_FLOAT64, 0);
    tempo_spec_add_connection(fx.spec, "flight.cmd", "world.cmd", TEMPO_TIME_NEVER, NULL);
    tempo_spec_add_connection(fx.spec, "world.gyro", "flight.gyro", TEMPO_TIME_NEVER, NULL);
    tempo_spec_add_connection(fx.spec, "motor.aux", "world.aux", TEMPO_TIME_NEVER, NULL);

    CHECK_I(fx_start(&fx, 1 << 14, NULL), TEMPO_OK);
    CHECK_I(tempo_scheduler_run(fx.sched), TEMPO_OK);

    /* Hand the trace back; the fixture keeps ownership of everything else. */
    *out_trace = fx.trace;
    fx.trace = NULL;
    fx_free(&fx);
}

/* Compare the first `n` records of two traces byte for byte. Sequence numbers
 * are part of the comparison: they are a determinism guarantee, not an
 * incidental counter (S26). */
static void compare_n(tempo_trace_t *x, tempo_trace_t *y, uint64_t n)
{
    uint64_t i;

    CHECK(n > 0);
    for (i = 0; i < n; i++) {
        const tempo_event_t *a = tempo_trace_at(x, i);
        const tempo_event_t *b = tempo_trace_at(y, i);
        CHECK(a != NULL && b != NULL);
        if (!a || !b)
            break;
        if (memcmp(a, b, sizeof(*a)) != 0) {
            tempo_test_failures++;
            fprintf(stderr,
                    "    FAIL record %llu differs: t=%llu/%llu type=%u/%u pid=%u/%u\n",
                    (unsigned long long)i,
                    (unsigned long long)a->hdr.logical_time,
                    (unsigned long long)b->hdr.logical_time,
                    a->hdr.type, b->hdr.type,
                    a->hdr.participant_id, b->hdr.participant_id);
            break;
        }
    }
}

static void compare_traces(tempo_trace_t *x, tempo_trace_t *y)
{
    uint64_t n = tempo_trace_count(x);

    CHECK_U64(tempo_trace_count(y), n);
    if (tempo_trace_count(y) < n)
        n = tempo_trace_count(y);
    compare_n(x, y, n);
}

static void test_repeated_runs_are_identical(void)
{
    tempo_trace_t *first = NULL, *second = NULL;

    /* Determinism is a promise about the whole event stream, not just the
     * final values (S26). */
    run_reference(&first);
    run_reference(&second);
    compare_traces(first, second);
    tempo_trace_free(first);
    tempo_trace_free(second);
}

static void test_reset_restores_initial_state(void)
{
    fixture_t fx;
    uint32_t a, b;
    tempo_trace_t *second;
    tempo_trace_config_t tc;
    uint64_t n_first;

    fx_init(&fx, 6 * TEMPO_NS_PER_MS, TEMPO_NS_PER_MS);
    a = fx_add(&fx, "a", "2ms:emit_sync 0 0.4; 4ms:trap", false, 1.0);
    b = fx_add(&fx, "b", NULL, true, 1.0);
    add_ports(&fx, a, NULL, "o", TEMPO_TYPE_FLOAT64, 0);
    add_ports(&fx, b, "i", "o", TEMPO_TYPE_FLOAT64, 0);
    tempo_spec_add_connection(fx.spec, "a.o", "b.i", TEMPO_TIME_NEVER, NULL);

    CHECK_I(fx_start(&fx, 1 << 13, NULL), TEMPO_OK);
    CHECK_I(tempo_scheduler_run(fx.sched), TEMPO_OK);
    n_first = tempo_trace_count(fx.trace);

    CHECK_I(tempo_scheduler_reset(fx.sched), TEMPO_OK);
    CHECK_U64(tempo_scheduler_now(fx.sched).time, 0);
    CHECK(!tempo_scheduler_finished(fx.sched));
    CHECK_U64(tempo_scheduler_participant_time(fx.sched, a), 0);
    CHECK_U64(tempo_scheduler_participant_time(fx.sched, b), 0);
    CHECK_F(out_value(&fx, "b.o"), 0.0, 0.0);
    CHECK_U64(in_slot(&fx, "b.i")->generation, 0);
    CHECK_U64(tempo_scheduler_stats(fx.sched).windows, 0);
    CHECK_U64(tempo_mock_time(fx.mocks[a]), 0);

    /* Record the second run into its own buffer, so its sequence numbers
     * start from zero like the first run's did. The second run must then be
     * byte-identical -- that is what makes reset useful for repeated analysis
     * and eventual fuzzing (S31, S47). */
    memset(&tc, 0, sizeof(tc));
    tc.capacity = 1 << 13;
    second = tempo_trace_new(&tc);
    tempo_scheduler_set_trace(fx.sched, second);
    CHECK_I(tempo_scheduler_run(fx.sched), TEMPO_OK);

    CHECK_U64(tempo_trace_count(second), n_first);
    compare_n(fx.trace, second, n_first);
    tempo_trace_free(second);
    fx_free(&fx);
}

/* ------------------------------------------- participants that misbehave -- */

static int null_init(void *self, const tempo_participant_context_t *ctx)
{
    (void)self; (void)ctx;
    return TEMPO_OK;
}

static void null_destroy(void *self)
{
    (void)self;
}

static int overshoot_advance(void *self, tempo_time_t target, tempo_advance_result_t *out)
{
    (void)self;
    /* Runs past its deadline, which would let it observe the future. */
    out->reached_time = target + 1;
    out->reason = TEMPO_TARGET_REACHED;
    return TEMPO_OK;
}

static int regress_advance(void *self, tempo_time_t target, tempo_advance_result_t *out)
{
    int *calls = (int *)self;
    out->reached_time = (*calls)++ == 0 ? target : 0;
    out->reason = TEMPO_TARGET_REACHED;
    return TEMPO_OK;
}

static int failing_advance(void *self, tempo_time_t target, tempo_advance_result_t *out)
{
    (void)self; (void)target;
    out->reached_time = 0;
    out->reason = TEMPO_ERROR;
    return TEMPO_OK;
}

static const tempo_participant_vtable_t overshoot_vt = {
    "overshoot", null_init, overshoot_advance, null_destroy,
    NULL, NULL, NULL, NULL, NULL
};
static const tempo_participant_vtable_t regress_vt = {
    "regress", null_init, regress_advance, null_destroy,
    NULL, NULL, NULL, NULL, NULL
};
static const tempo_participant_vtable_t failing_vt = {
    "failing", null_init, failing_advance, null_destroy,
    NULL, NULL, NULL, NULL, NULL
};

static void run_one_bad(const tempo_participant_vtable_t *vt, void *self,
                        int expected, const char *expect_text)
{
    tempo_federation_spec_t *spec = tempo_spec_new();
    tempo_plan_t *plan;
    tempo_scheduler_t *sched;

    tempo_spec_set_end_time(spec, 5 * TEMPO_NS_PER_MS);
    tempo_spec_set_quantum(spec, TEMPO_NS_PER_MS);
    tempo_spec_add_participant(spec, "bad", "test", NULL);
    plan = tempo_compile(spec, NULL);
    CHECK(plan != NULL);
    if (plan) {
        sched = tempo_scheduler_new(plan);
        CHECK_I(tempo_scheduler_bind(sched, 0, vt, self, NULL), TEMPO_OK);
        CHECK_I(tempo_scheduler_init(sched), TEMPO_OK);
        CHECK_I(tempo_scheduler_run(sched), expected);
        if (expect_text)
            CHECK(strstr(tempo_scheduler_error(sched), expect_text) != NULL);
        tempo_scheduler_free(sched);
        tempo_plan_free(plan);
    }
    tempo_spec_free(spec);
}

static void test_misbehaving_participants_are_caught(void)
{
    int calls = 0;

    run_one_bad(&overshoot_vt, NULL, TEMPO_ERR_TIME_REGRESSION, "overshot");
    run_one_bad(&regress_vt, &calls, TEMPO_ERR_TIME_REGRESSION, "backwards");
    run_one_bad(&failing_vt, NULL, TEMPO_ERR_PARTICIPANT, "reported an error");
}

static void test_unbound_participant_is_refused(void)
{
    tempo_federation_spec_t *spec = tempo_spec_new();
    tempo_plan_t *plan;
    tempo_scheduler_t *sched;

    tempo_spec_set_end_time(spec, TEMPO_NS_PER_MS);
    tempo_spec_add_participant(spec, "lonely", "mock", NULL);
    plan = tempo_compile(spec, NULL);
    CHECK(plan != NULL);
    if (plan) {
        sched = tempo_scheduler_new(plan);
        CHECK_I(tempo_scheduler_init(sched), TEMPO_ERR_NOT_BOUND);
        CHECK(strstr(tempo_scheduler_error(sched), "lonely") != NULL);
        /* And stepping an uninitialized scheduler is a state error, not a
         * crash. */
        CHECK_I(tempo_scheduler_step(sched), TEMPO_ERR_STATE);
        tempo_scheduler_free(sched);
        tempo_plan_free(plan);
    }
    tempo_spec_free(spec);
}

static void test_claimed_capability_must_be_implemented(void)
{
    tempo_federation_spec_t *spec = tempo_spec_new();
    tempo_plan_t *plan;
    tempo_scheduler_t *sched;
    tempo_capabilities_t caps;

    tempo_spec_set_end_time(spec, TEMPO_NS_PER_MS);
    tempo_spec_add_participant(spec, "p", "test", NULL);
    plan = tempo_compile(spec, NULL);
    CHECK(plan != NULL);
    if (plan) {
        sched = tempo_scheduler_new(plan);
        memset(&caps, 0, sizeof(caps));
        /* The hot path branches on the flag alone, so a flag without an
         * implementation has to be rejected at bind time. */
        caps.flags = TEMPO_CAP_RESET;
        CHECK_I(tempo_scheduler_bind(sched, 0, &overshoot_vt, NULL, &caps),
                TEMPO_ERR_UNSUPPORTED);
        caps.flags = TEMPO_CAP_PREDICT_NEXT;
        CHECK_I(tempo_scheduler_bind(sched, 0, &overshoot_vt, NULL, &caps),
                TEMPO_ERR_UNSUPPORTED);
        tempo_scheduler_free(sched);
        tempo_plan_free(plan);
    }
    tempo_spec_free(spec);
}

int main(void)
{
    RUN(test_runs_to_end_time);
    RUN(test_logical_time_never_regresses);
    RUN(test_early_return);
    RUN(test_prediction_avoids_a_wasted_trap);
    RUN(test_delayed_value_propagation);
    RUN(test_delivery_lands_at_produce_time_plus_delay);
    RUN(test_same_time_chain);
    RUN(test_same_time_cycle_converges);
    RUN(test_same_time_cycle_that_diverges_is_reported);
    RUN(test_zero_delay_requires_same_time_capability);
    RUN(test_finish_ends_the_run_early);
    RUN(test_run_until_is_resumable);
    RUN(test_bytes_port);
    RUN(test_unchanged_values_are_not_rerouted);
    RUN(test_repeated_runs_are_identical);
    RUN(test_reset_restores_initial_state);
    RUN(test_misbehaving_participants_are_caught);
    RUN(test_unbound_participant_is_refused);
    RUN(test_claimed_capability_must_be_implemented);
    return TEST_RESULT();
}
