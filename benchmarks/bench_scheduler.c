/* clock_gettime needs POSIX, which -std=c11 alone does not expose. */
#define _POSIX_C_SOURCE 200809L

/* Scheduler overhead benchmarks.
 *
 * The requirement these exist to defend is S46: with the scheduler attached
 * but no meaningful cross-participant synchronization, the overhead must be
 * small. Measuring that continuously is Rule 7, so this binary is built by
 * default and prints a table you can diff between commits.
 *
 * A "null" participant does nothing but move its own clock, so the numbers
 * below are the scheduler's own cost with the simulator's cost removed.
 */
#include "tempo/scheduler.h"
#include "tempo_mock.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* ------------------------------------------------- the null participant --- */

typedef struct {
    const tempo_participant_context_t *ctx;
    tempo_time_t time;
    double counter;
} null_participant_t;

static int null_initialize(void *self, const tempo_participant_context_t *ctx)
{
    null_participant_t *p = (null_participant_t *)self;
    p->ctx = ctx;
    p->time = 0;
    return TEMPO_OK;
}

static int null_advance(void *self, tempo_time_t target, tempo_advance_result_t *out)
{
    null_participant_t *p = (null_participant_t *)self;
    p->time = target;
    out->reached_time = target;
    out->reason = TEMPO_TARGET_REACHED;
    return TEMPO_OK;
}

/* Writes a fresh value every call, so every window has real routing work. */
static int writer_advance(void *self, tempo_time_t target, tempo_advance_result_t *out)
{
    null_participant_t *p = (null_participant_t *)self;
    p->time = target;
    p->counter += 1.0;
    if (p->ctx->n_outputs)
        p->ctx->outputs[0].scalar.f64 = p->counter;
    out->reached_time = target;
    out->reason = TEMPO_TARGET_REACHED;
    return TEMPO_OK;
}

static void null_destroy(void *self) { (void)self; }

static const tempo_participant_vtable_t null_vt = {
    "null", null_initialize, null_advance, null_destroy,
    NULL, NULL, NULL, NULL, NULL
};
static const tempo_participant_vtable_t writer_vt = {
    "writer", null_initialize, writer_advance, null_destroy,
    NULL, NULL, NULL, NULL, NULL
};

/* ------------------------------------------------------------- scenarios --- */

typedef struct {
    const char *name;
    uint32_t    n_parts;
    bool        connected;   /* chain the participants together */
    bool        writing;     /* produce a new value every window */
    int         trace;       /* 0 = off, 1 = selective, 2 = everything */
} scenario_t;

static void report(const char *name, double secs, tempo_scheduler_stats_t st,
                   uint64_t traced)
{
    double windows = (double)st.windows;
    printf("%-34s %9.1f ms  %10.2f Mwin/s  %8.1f ns/win  %10.2f Madv/s  trace=%llu\n",
           name, secs * 1e3,
           windows / secs / 1e6,
           secs / (windows > 0 ? windows : 1) * 1e9,
           (double)st.advance_calls / secs / 1e6,
           (unsigned long long)traced);
}

static void run_scenario(const scenario_t *sc, tempo_time_t end, tempo_time_t quantum)
{
    tempo_federation_spec_t *spec = tempo_spec_new();
    tempo_diagnostics_t diag;
    tempo_plan_t *plan;
    tempo_scheduler_t *sched;
    tempo_trace_t *trace = NULL;
    null_participant_t *parts;
    uint32_t i;
    double t0, t1;
    char label[96];

    tempo_spec_set_end_time(spec, end);
    tempo_spec_set_quantum(spec, quantum);
    /* Enough room that the trace never wraps and the cost stays comparable. */
    for (i = 0; i < sc->n_parts; i++) {
        char name[16];
        uint32_t id;
        snprintf(name, sizeof(name), "p%u", i);
        tempo_spec_add_participant(spec, name, "null", &id);
        tempo_spec_add_port(spec, id, "out", TEMPO_PORT_OUTPUT, TEMPO_TYPE_FLOAT64, 0, NULL);
        tempo_spec_add_port(spec, id, "in", TEMPO_PORT_INPUT, TEMPO_TYPE_FLOAT64, 0, NULL);
    }
    if (sc->connected) {
        for (i = 0; i + 1 < sc->n_parts; i++) {
            char from[24], to[24];
            snprintf(from, sizeof(from), "p%u.out", i);
            snprintf(to, sizeof(to), "p%u.in", i + 1);
            tempo_spec_add_connection(spec, from, to, TEMPO_TIME_NEVER, NULL);
        }
    }
    if (sc->trace == 2) {
        tempo_subscription_t sub = tempo_subscription_all();
        tempo_spec_subscribe(spec, &sub);
    }

    plan = tempo_compile(spec, &diag);
    if (!plan) {
        fprintf(stderr, "compile failed at %s: %s\n", diag.location, diag.message);
        tempo_spec_free(spec);
        return;
    }
    sched = tempo_scheduler_new(plan);
    parts = (null_participant_t *)calloc(sc->n_parts, sizeof(*parts));

    if (sc->trace) {
        tempo_trace_config_t tc;
        memset(&tc, 0, sizeof(tc));
        tc.capacity = 1u << 22;
        tc.overwrite = true;
        trace = tempo_trace_new(&tc);
        tempo_scheduler_set_trace(sched, trace);
    }
    for (i = 0; i < sc->n_parts; i++)
        tempo_scheduler_bind(sched, i, sc->writing ? &writer_vt : &null_vt,
                             &parts[i], NULL);
    if (tempo_scheduler_init(sched) != TEMPO_OK) {
        fprintf(stderr, "init failed: %s\n", tempo_scheduler_error(sched));
        goto done;
    }

    t0 = now_seconds();
    if (tempo_scheduler_run(sched) != TEMPO_OK)
        fprintf(stderr, "run failed: %s\n", tempo_scheduler_error(sched));
    t1 = now_seconds();

    snprintf(label, sizeof(label), "%s (%u parts)", sc->name, sc->n_parts);
    report(label, t1 - t0, tempo_scheduler_stats(sched),
           trace ? tempo_trace_total(trace) : 0);

done:
    tempo_scheduler_free(sched);
    tempo_trace_free(trace);
    tempo_plan_free(plan);
    tempo_spec_free(spec);
    free(parts);
}

int main(int argc, char **argv)
{
    /* One million windows by default: long enough to be stable, short enough
     * to run in a pre-commit loop. */
    tempo_time_t quantum = TEMPO_NS_PER_US;
    tempo_time_t end = 1000 * TEMPO_NS_PER_MS;
    static const scenario_t scenarios[] = {
        { "bare scheduler, no ports",      1,  false, false, 0 },
        { "bare scheduler, no ports",      8,  false, false, 0 },
        { "bare scheduler, no ports",      64, false, false, 0 },
        { "connected, values unchanged",   8,  true,  false, 0 },
        { "connected, value every window", 8,  true,  true,  0 },
        { "connected, selective trace",    8,  true,  true,  1 },
        { "connected, full trace",         8,  true,  true,  2 },
    };
    size_t i;

    if (argc > 1)
        end = (tempo_time_t)strtoull(argv[1], NULL, 10) * TEMPO_NS_PER_MS;
    if (argc > 2)
        quantum = (tempo_time_t)strtoull(argv[2], NULL, 10);

    printf("tempo %s -- end=%llu ns quantum=%llu ns\n\n", tempo_version(),
           (unsigned long long)end, (unsigned long long)quantum);
    printf("%-34s %12s  %18s  %11s  %14s\n",
           "scenario", "wall", "windows/s", "per window", "advances/s");
    for (i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++)
        run_scenario(&scenarios[i], end, quantum);
    printf("\nThe first three rows are the scheduler's own cost. The gap between\n"
           "\"values unchanged\" and \"value every window\" is routing; the gap to\n"
           "the trace rows is observability (S45, S46).\n");
    return 0;
}
