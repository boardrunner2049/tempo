/* Scriptable mock participant.
 *
 * Phase 1 integrates no real simulator on purpose (S37): the scheduler's
 * semantics have to be pinned down by tests before QEMU or FMI can be
 * blamed for anything. A mock is driven by a list of timed actions, which is
 * enough to express every behaviour the scheduler must handle -- reaching a
 * deadline, trapping out early, producing a value, finishing.
 *
 * It also serves as the reference for what an adapter looks like: it reads
 * and writes its slots directly and never calls back into the scheduler.
 */
#include "tempo_mock.h"

#include "tempo/trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOCK_MAX_STEPS 256

struct tempo_mock {
    tempo_mock_step_t steps[MOCK_MAX_STEPS];
    uint32_t          n_steps;
    uint32_t          cursor;
    tempo_time_t      max_step;
    tempo_time_t      time;
    uint32_t          advance_count;
    uint32_t          extra_caps;
    bool              echo_inputs;
    double            echo_gain;

    const tempo_participant_context_t *ctx;
    uint64_t *seen_generation;   /* per input, for echo change detection */
    double   *last_input;
};

static int mock_initialize(void *self, const tempo_participant_context_t *ctx)
{
    tempo_mock_t *m = (tempo_mock_t *)self;

    m->ctx = ctx;
    free(m->seen_generation);
    free(m->last_input);
    m->seen_generation = NULL;
    m->last_input = NULL;
    if (ctx->n_inputs) {
        m->seen_generation = (uint64_t *)calloc(ctx->n_inputs, sizeof(uint64_t));
        m->last_input = (double *)calloc(ctx->n_inputs, sizeof(double));
        if (!m->seen_generation || !m->last_input)
            return TEMPO_ERR_NO_MEMORY;
    }
    m->time = 0;
    m->cursor = 0;
    return TEMPO_OK;
}

static void mock_write_output(tempo_mock_t *m, uint32_t local, double value)
{
    const tempo_participant_context_t *ctx = m->ctx;
    const tempo_port_desc_t *d;

    if (!ctx || local >= ctx->n_outputs)
        return;
    d = &ctx->output_desc[local];
    switch (d->type) {
    case TEMPO_TYPE_BOOL:    ctx->outputs[local].scalar.b = value != 0.0; break;
    case TEMPO_TYPE_INT64:   ctx->outputs[local].scalar.i64 = (int64_t)value; break;
    case TEMPO_TYPE_FLOAT32: ctx->outputs[local].scalar.f32 = (float)value; break;
    case TEMPO_TYPE_FLOAT64: ctx->outputs[local].scalar.f64 = value; break;
    case TEMPO_TYPE_BYTES: {
        /* Just enough to exercise a byte port: the value's bits as a payload. */
        double v = value;
        tempo_slot_set_bytes(&ctx->outputs[local], &v, (uint32_t)sizeof(v));
        break;
    }
    default: break;
    }
}

static double mock_read_input(const tempo_mock_t *m, uint32_t local)
{
    const tempo_participant_context_t *ctx = m->ctx;
    const tempo_port_desc_t *d;

    if (!ctx || local >= ctx->n_inputs)
        return 0.0;
    d = &ctx->input_desc[local];
    switch (d->type) {
    case TEMPO_TYPE_BOOL:    return ctx->inputs[local].scalar.b ? 1.0 : 0.0;
    case TEMPO_TYPE_INT64:   return (double)ctx->inputs[local].scalar.i64;
    case TEMPO_TYPE_FLOAT32: return (double)ctx->inputs[local].scalar.f32;
    case TEMPO_TYPE_FLOAT64: return ctx->inputs[local].scalar.f64;
    case TEMPO_TYPE_BYTES: {
        double v = 0.0;
        if (ctx->inputs[local].len >= sizeof(v))
            memcpy(&v, ctx->inputs[local].bytes, sizeof(v));
        return v;
    }
    default: return 0.0;
    }
}

/* Copy each changed input onto the output of the same local index. The
 * generation compare is the whole point of the generation field: detecting a
 * new input costs one integer compare, not a callback. */
static void mock_echo(tempo_mock_t *m)
{
    const tempo_participant_context_t *ctx = m->ctx;
    uint32_t i, n;

    if (!ctx)
        return;
    n = ctx->n_inputs < ctx->n_outputs ? ctx->n_inputs : ctx->n_outputs;
    for (i = 0; i < n; i++) {
        uint64_t gen = ctx->inputs[i].generation;
        double v;
        if (gen == m->seen_generation[i])
            continue;
        m->seen_generation[i] = gen;
        v = mock_read_input(m, i);
        m->last_input[i] = v;
        mock_write_output(m, i, v * m->echo_gain);
    }
}

static int mock_advance_to(void *self, tempo_time_t target, tempo_advance_result_t *out)
{
    tempo_mock_t *m = (tempo_mock_t *)self;

    m->advance_count++;
    if (m->echo_inputs)
        mock_echo(m);

    for (;;) {
        tempo_time_t next = (m->cursor < m->n_steps)
                                ? m->steps[m->cursor].time : TEMPO_TIME_NEVER;
        tempo_time_t limit = target;

        /* A coarse internal step models a simulator that cannot stop
         * anywhere it likes. */
        if (m->max_step) {
            tempo_time_t capped = tempo_time_add(m->time, m->max_step);
            if (capped < limit)
                limit = capped;
        }

        if (next != TEMPO_TIME_NEVER && next <= limit) {
            const tempo_mock_step_t *st = &m->steps[m->cursor++];
            if (next > m->time)
                m->time = next;
            switch (st->action) {
            case TEMPO_MOCK_EMIT_SYNC:
                mock_write_output(m, st->slot, st->value);
                out->reached_time = m->time;
                out->reason = TEMPO_EVENT;
                return TEMPO_OK;
            case TEMPO_MOCK_EMIT_OBSERVE:
                mock_write_output(m, st->slot, st->value);
                continue;
            case TEMPO_MOCK_TRAP:
                out->reached_time = m->time;
                out->reason = TEMPO_EVENT;
                return TEMPO_OK;
            case TEMPO_MOCK_MARK:
                if (m->ctx && m->ctx->trace) {
                    uint16_t type = st->event_type ? st->event_type : TEMPO_EV_USER_EVENT;
                    if (tempo_trace_wants(m->ctx->trace, type, m->ctx->participant_id))
                        tempo_trace_emit(m->ctx->trace, m->time, 0,
                                         m->ctx->participant_id, type,
                                         TEMPO_EVF_OBSERVE, st->slot,
                                         (uint64_t)st->value, 0);
                }
                continue;
            case TEMPO_MOCK_FINISH:
                out->reached_time = m->time;
                out->reason = TEMPO_FINISHED;
                return TEMPO_OK;
            }
            continue;
        }

        m->time = limit;
        out->reached_time = m->time;
        /* Stopping short of the target because of a coarse step is, from the
         * scheduler's point of view, the same request: ask me again. */
        out->reason = (m->time >= target) ? TEMPO_TARGET_REACHED : TEMPO_EVENT;
        return TEMPO_OK;
    }
}

static int mock_reset(void *self)
{
    tempo_mock_t *m = (tempo_mock_t *)self;

    m->time = 0;
    m->cursor = 0;
    m->advance_count = 0;
    if (m->ctx && m->ctx->n_inputs) {
        memset(m->seen_generation, 0, m->ctx->n_inputs * sizeof(uint64_t));
        memset(m->last_input, 0, m->ctx->n_inputs * sizeof(double));
    }
    return TEMPO_OK;
}

static tempo_time_t mock_next_event_time(void *self)
{
    tempo_mock_t *m = (tempo_mock_t *)self;

    return (m->cursor < m->n_steps) ? m->steps[m->cursor].time : TEMPO_TIME_NEVER;
}

static void mock_destroy(void *self)
{
    tempo_mock_free((tempo_mock_t *)self);
}

static const tempo_participant_vtable_t mock_vtable = {
    "mock",
    mock_initialize,
    mock_advance_to,
    mock_destroy,
    mock_reset,
    mock_next_event_time,
    NULL,   /* snapshot */
    NULL,   /* restore  */
    NULL    /* inputs_changed: the mock reads its slots directly */
};

const tempo_participant_vtable_t *tempo_mock_vtable(void)
{
    return &mock_vtable;
}

tempo_mock_t *tempo_mock_new(const tempo_mock_config_t *cfg)
{
    tempo_mock_t *m;

    if (cfg && cfg->n_steps > MOCK_MAX_STEPS)
        return NULL;
    m = (tempo_mock_t *)calloc(1, sizeof(*m));
    if (!m)
        return NULL;
    m->echo_gain = 1.0;
    if (cfg) {
        if (cfg->n_steps && cfg->steps) {
            memcpy(m->steps, cfg->steps, cfg->n_steps * sizeof(tempo_mock_step_t));
            m->n_steps = cfg->n_steps;
        }
        m->max_step = cfg->max_step;
        m->extra_caps = cfg->extra_caps;
        m->echo_inputs = cfg->echo_inputs;
        if (cfg->echo_gain != 0.0)
            m->echo_gain = cfg->echo_gain;
    }
    return m;
}

void tempo_mock_free(tempo_mock_t *m)
{
    if (!m)
        return;
    free(m->seen_generation);
    free(m->last_input);
    free(m);
}

tempo_capabilities_t tempo_mock_capabilities(const tempo_mock_t *m)
{
    tempo_capabilities_t caps;

    memset(&caps, 0, sizeof(caps));
    /* next_event_time() is implemented, but PREDICT_NEXT is not advertised by
     * default: a QEMU-backed participant generally cannot say when its own
     * next externally relevant event falls, and the mock should not make the
     * easy case look like the normal one. Ask for it via extra_caps. */
    caps.flags = TEMPO_CAP_RESET | TEMPO_CAP_EARLY_RETURN;
    if (m) {
        /* A mock that echoes its inputs can recompute at the same logical
         * time, which is what a zero-delay connection needs. */
        if (m->echo_inputs)
            caps.flags |= TEMPO_CAP_SAME_TIME_ITER;
        caps.flags |= m->extra_caps;
        caps.preferred_step = m->max_step;
    }
    caps.time_resolution = 1;
    return caps;
}

tempo_time_t tempo_mock_time(const tempo_mock_t *m)          { return m ? m->time : 0; }
uint32_t     tempo_mock_advance_count(const tempo_mock_t *m) { return m ? m->advance_count : 0; }

double tempo_mock_last_input(const tempo_mock_t *m, uint32_t local)
{
    if (!m || !m->last_input || !m->ctx || local >= m->ctx->n_inputs)
        return 0.0;
    return m->last_input[local];
}

/* ------------------------------------------------------------- scripts ---- */

static const struct { const char *name; tempo_mock_action_t action; } action_names[] = {
    { "emit_sync",    TEMPO_MOCK_EMIT_SYNC },
    { "emit_observe", TEMPO_MOCK_EMIT_OBSERVE },
    { "emit",         TEMPO_MOCK_EMIT_SYNC },
    { "trap",         TEMPO_MOCK_TRAP },
    { "mark",         TEMPO_MOCK_MARK },
    { "finish",       TEMPO_MOCK_FINISH },
};

/* "3.5ms:trap; 6ms:emit_sync 0 0.7; 9ms:finish" */
tempo_mock_t *tempo_mock_from_script(const char *script, tempo_time_t max_step,
                                     bool echo_inputs, double echo_gain,
                                     const char **err)
{
    tempo_mock_config_t cfg;
    tempo_mock_step_t steps[MOCK_MAX_STEPS];
    uint32_t n = 0;
    const char *p = script;

    memset(steps, 0, sizeof(steps));
    if (err)
        *err = NULL;
    if (!script) {
        if (err) *err = "no script";
        return NULL;
    }

    while (*p) {
        char timebuf[32], action[32];
        const char *colon, *semi;
        size_t len;
        size_t i;
        bool matched = false;

        while (*p == ' ' || *p == ';' || *p == '\t' || *p == '\n')
            p++;
        if (!*p)
            break;
        if (n >= MOCK_MAX_STEPS) {
            if (err) *err = "too many script steps";
            return NULL;
        }

        colon = strchr(p, ':');
        semi = strchr(p, ';');
        if (!colon || (semi && colon > semi)) {
            if (err) *err = "expected \"<time>:<action>\"";
            return NULL;
        }
        len = (size_t)(colon - p);
        if (len == 0 || len >= sizeof(timebuf)) {
            if (err) *err = "bad time field";
            return NULL;
        }
        memcpy(timebuf, p, len);
        timebuf[len] = '\0';
        if (tempo_time_parse(timebuf, &steps[n].time) != TEMPO_OK) {
            if (err) *err = "bad duration";
            return NULL;
        }

        p = colon + 1;
        while (*p == ' ')
            p++;
        len = 0;
        while (p[len] && p[len] != ' ' && p[len] != ';')
            len++;
        if (len == 0 || len >= sizeof(action)) {
            if (err) *err = "missing action";
            return NULL;
        }
        memcpy(action, p, len);
        action[len] = '\0';
        p += len;

        for (i = 0; i < sizeof(action_names) / sizeof(action_names[0]); i++) {
            if (!strcmp(action, action_names[i].name)) {
                steps[n].action = action_names[i].action;
                matched = true;
                break;
            }
        }
        if (!matched) {
            if (err) *err = "unknown action";
            return NULL;
        }

        /* Optional "<slot> <value>". */
        while (*p == ' ')
            p++;
        if (*p && *p != ';') {
            char *end;
            steps[n].slot = (uint32_t)strtoul(p, &end, 10);
            if (end == p) {
                if (err) *err = "bad slot index";
                return NULL;
            }
            p = end;
            while (*p == ' ')
                p++;
            if (*p && *p != ';') {
                steps[n].value = strtod(p, &end);
                if (end == p) {
                    if (err) *err = "bad value";
                    return NULL;
                }
                p = end;
            }
        }

        if (n > 0 && steps[n].time < steps[n - 1].time) {
            if (err) *err = "script steps must be in non-decreasing time order";
            return NULL;
        }
        n++;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.steps = steps;
    cfg.n_steps = n;
    cfg.max_step = max_step;
    cfg.echo_inputs = echo_inputs;
    cfg.echo_gain = echo_gain;
    return tempo_mock_new(&cfg);
}
