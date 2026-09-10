/* The scheduler: the federation's logical-time authority.
 *
 * One iteration of tempo_scheduler_step() is one communication window:
 *
 *   1. Communication point at `now`: make every value whose delivery time has
 *      arrived logically visible, and iterate microsteps until same-time
 *      propagation is quiescent (S17).
 *   2. Choose a horizon. It is bounded by the policy quantum, by the end
 *      time, by the next pending delivery, and by any participant that can
 *      predict its own next event.
 *   3. Let every participant run toward that horizon, in the compiled
 *      execution order. A participant may trap out early (S10); that is the
 *      normal case, not an error.
 *   4. Advance `now` to the minimum live participant time. That is the
 *      conservative lower bound on logical time: no participant has run past
 *      the horizon, so nothing can observe the future (S29).
 *
 * Why step 2's clamp matters. A value produced at time t on a connection
 * with delay d becomes visible at t + d. Clamping the horizon to the earliest
 * pending delivery is what guarantees that no participant runs past a time at
 * which one of its inputs changes -- which is what makes this conservative
 * rather than optimistic. A zero-delay connection therefore forces the
 * horizon down to the producing instant and is resolved by microsteps; a
 * delayed connection lets participants run freely to the next boundary, which
 * is the case we actually want to be fast (Rule 3).
 */
#include "tempo_internal.h"

#include <stdarg.h>
#include <stdio.h>

typedef struct {
    const tempo_participant_vtable_t *vt;
    void                            *self;
    tempo_capabilities_t             caps;
    tempo_time_t                     time;
    tempo_time_t                     next_event;
    tempo_participant_context_t      ctx;
    bool                             bound;
    bool                             finished;
    bool                             blocked;
    /* Inputs made visible at the current communication point. */
    uint32_t *changed_list;
    uint8_t  *changed_flag;
    uint32_t  n_changed;
} sched_participant_t;

struct tempo_scheduler {
    const tempo_plan_t  *plan;
    sched_participant_t *parts;

    tempo_slot_t *in_slots;
    tempo_slot_t *out_slots;
    tempo_slot_t *out_shadow;   /* last value routed, for change detection */
    uint8_t      *bytes_in;
    uint8_t      *bytes_out;
    uint8_t      *bytes_shadow;

    tempo_delivery_queue_t dq;
    uint64_t               enqueue_seq;

    tempo_tag_t  now;
    tempo_time_t run_until;
    bool         initialized;
    bool         started;      /* SIM_START already emitted for this run */
    bool         finished;
    bool         ended;        /* SIM_END already emitted */
    uint32_t     stall;

    tempo_trace_t          *trace;
    tempo_scheduler_stats_t stats;
    char                    error[256];
};

/* ------------------------------------------------------------- helpers ---- */

static int sched_fail(tempo_scheduler_t *s, int rc, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(s->error, sizeof(s->error), fmt, ap);
    va_end(ap);
    return rc;
}

static size_t scalar_width(tempo_type_t t)
{
    switch (t) {
    case TEMPO_TYPE_BOOL:    return sizeof(bool);
    case TEMPO_TYPE_INT64:   return sizeof(int64_t);
    case TEMPO_TYPE_FLOAT32: return sizeof(float);
    case TEMPO_TYPE_FLOAT64: return sizeof(double);
    default:                 return 0;
    }
}

/* Bit-exact value comparison. Comparing the bytes rather than the values
 * keeps change detection deterministic, including for NaN. */
static bool scalar_equal(tempo_type_t t, const tempo_slot_t *a, const tempo_slot_t *b)
{
    return memcmp(&a->scalar, &b->scalar, scalar_width(t)) == 0;
}

static uint64_t scalar_bits(tempo_type_t t, const tempo_slot_t *s)
{
    uint64_t bits = 0;
    memcpy(&bits, &s->scalar, scalar_width(t));
    return bits;
}

static void emit_at(tempo_scheduler_t *s, tempo_time_t time, uint32_t microstep,
                    uint32_t pid, uint16_t type, uint16_t flags,
                    uint32_t arg0, uint64_t arg1, uint64_t cause)
{
    if (TEMPO_UNLIKELY(tempo_trace_wants(s->trace, type, pid)))
        tempo_trace_emit(s->trace, time, microstep, pid, type, flags, arg0, arg1, cause);
}

static void emit(tempo_scheduler_t *s, uint32_t pid, uint16_t type, uint16_t flags,
                 uint32_t arg0, uint64_t arg1)
{
    emit_at(s, s->now.time, s->now.microstep, pid, type, flags, arg0, arg1, 0);
}

/* ------------------------------------------------------- setup/teardown ---- */

/* Allocate the federation's slot arrays. All BYTES payloads live in one pool
 * per array so a participant's ports stay contiguous in memory. */
static int alloc_slots(tempo_scheduler_t *s)
{
    const tempo_plan_t *plan = s->plan;
    size_t in_bytes = 0, out_bytes = 0, off;
    uint32_t i;

    for (i = 0; i < plan->n_input_slots; i++)
        in_bytes += plan->inputs[i].max_bytes;
    for (i = 0; i < plan->n_output_slots; i++)
        out_bytes += plan->outputs[i].max_bytes;

    if (plan->n_input_slots) {
        s->in_slots = (tempo_slot_t *)calloc(plan->n_input_slots, sizeof(tempo_slot_t));
        if (!s->in_slots)
            return TEMPO_ERR_NO_MEMORY;
    }
    if (plan->n_output_slots) {
        s->out_slots = (tempo_slot_t *)calloc(plan->n_output_slots, sizeof(tempo_slot_t));
        s->out_shadow = (tempo_slot_t *)calloc(plan->n_output_slots, sizeof(tempo_slot_t));
        if (!s->out_slots || !s->out_shadow)
            return TEMPO_ERR_NO_MEMORY;
    }
    if (in_bytes) {
        s->bytes_in = (uint8_t *)calloc(in_bytes, 1);
        if (!s->bytes_in)
            return TEMPO_ERR_NO_MEMORY;
    }
    if (out_bytes) {
        s->bytes_out = (uint8_t *)calloc(out_bytes, 1);
        s->bytes_shadow = (uint8_t *)calloc(out_bytes, 1);
        if (!s->bytes_out || !s->bytes_shadow)
            return TEMPO_ERR_NO_MEMORY;
    }

    off = 0;
    for (i = 0; i < plan->n_input_slots; i++) {
        uint32_t mb = plan->inputs[i].max_bytes;
        if (mb) {
            s->in_slots[i].bytes = s->bytes_in + off;
            s->in_slots[i].capacity = mb;
            off += mb;
        }
    }
    off = 0;
    for (i = 0; i < plan->n_output_slots; i++) {
        uint32_t mb = plan->outputs[i].max_bytes;
        if (mb) {
            s->out_slots[i].bytes = s->bytes_out + off;
            s->out_slots[i].capacity = mb;
            s->out_shadow[i].bytes = s->bytes_shadow + off;
            s->out_shadow[i].capacity = mb;
            off += mb;
        }
    }
    return TEMPO_OK;
}

tempo_scheduler_t *tempo_scheduler_new(const tempo_plan_t *plan)
{
    tempo_scheduler_t *s;
    uint32_t i;

    if (!plan)
        return NULL;
    s = (tempo_scheduler_t *)calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->plan = plan;
    s->run_until = plan->end_time;
    s->now = tempo_tag(0, 0);

    s->parts = (sched_participant_t *)calloc(plan->n_parts, sizeof(sched_participant_t));
    if (!s->parts)
        goto fail;
    for (i = 0; i < plan->n_parts; i++) {
        uint32_t n_in = plan->parts[i].n_inputs;
        s->parts[i].next_event = TEMPO_TIME_NEVER;
        if (n_in) {
            s->parts[i].changed_list = (uint32_t *)calloc(n_in, sizeof(uint32_t));
            s->parts[i].changed_flag = (uint8_t *)calloc(n_in, 1);
            if (!s->parts[i].changed_list || !s->parts[i].changed_flag)
                goto fail;
        }
    }
    if (alloc_slots(s) != TEMPO_OK)
        goto fail;
    tempo__dq_init(&s->dq, plan->routes);
    return s;

fail:
    tempo_scheduler_free(s);
    return NULL;
}

void tempo_scheduler_free(tempo_scheduler_t *s)
{
    uint32_t i;

    if (!s)
        return;
    if (s->parts) {
        for (i = 0; i < s->plan->n_parts; i++) {
            free(s->parts[i].changed_list);
            free(s->parts[i].changed_flag);
        }
        free(s->parts);
    }
    tempo__dq_free(&s->dq);
    free(s->in_slots);
    free(s->out_slots);
    free(s->out_shadow);
    free(s->bytes_in);
    free(s->bytes_out);
    free(s->bytes_shadow);
    free(s);
}

int tempo_scheduler_bind(tempo_scheduler_t *s, uint32_t id,
                         const tempo_participant_vtable_t *vt, void *self,
                         const tempo_capabilities_t *caps)
{
    sched_participant_t *p;

    if (!s || !vt || id >= s->plan->n_parts)
        return TEMPO_ERR_INVALID_ARG;
    if (!vt->advance_to)
        return sched_fail(s, TEMPO_ERR_INVALID_ARG,
                          "participant '%s' has no advance_to",
                          s->plan->parts[id].name);
    if (s->initialized)
        return TEMPO_ERR_STATE;

    p = &s->parts[id];
    p->vt = vt;
    p->self = self;
    p->bound = true;
    if (caps)
        p->caps = *caps;
    else
        memset(&p->caps, 0, sizeof(p->caps));

    /* A claimed capability must actually be implemented, so the hot path can
     * branch on the flag alone and never test for NULL. */
    if ((p->caps.flags & TEMPO_CAP_RESET) && !vt->reset)
        return sched_fail(s, TEMPO_ERR_UNSUPPORTED,
                          "'%s' claims RESET but has no reset()", s->plan->parts[id].name);
    if ((p->caps.flags & TEMPO_CAP_PREDICT_NEXT) && !vt->next_event_time)
        return sched_fail(s, TEMPO_ERR_UNSUPPORTED,
                          "'%s' claims PREDICT_NEXT but has no next_event_time()",
                          s->plan->parts[id].name);
    if ((p->caps.flags & TEMPO_CAP_SNAPSHOT) && !vt->snapshot)
        return sched_fail(s, TEMPO_ERR_UNSUPPORTED,
                          "'%s' claims SNAPSHOT but has no snapshot()",
                          s->plan->parts[id].name);
    return TEMPO_OK;
}

int tempo_scheduler_set_trace(tempo_scheduler_t *s, tempo_trace_t *tr)
{
    if (!s)
        return TEMPO_ERR_INVALID_ARG;
    s->trace = tr;
    if (tr) {
        /* The plan's compiled filters are the authority; a trace attached to
         * a scheduler records what the experiment subscribed to (S21). */
        tempo_trace_set_masks(tr, s->plan->trace_core_mask, s->plan->trace_ns_mask);
        tempo_trace_set_participant_filter(tr, s->plan->trace_participants,
                                           s->plan->n_parts);
    }
    return TEMPO_OK;
}

tempo_trace_t *tempo_scheduler_trace(const tempo_scheduler_t *s)
{
    return s ? s->trace : NULL;
}

/* ------------------------------------------------------------ harvesting -- */

static int enqueue_route(tempo_scheduler_t *s, uint32_t route_idx, tempo_time_t at,
                         const tempo_slot_t *src)
{
    const tempo_route_t *r = &s->plan->routes[route_idx];
    tempo_delivery_t d;
    tempo_time_t when = tempo_time_add(at, r->delay);
    const void *payload = NULL;
    uint32_t payload_len = 0;

    /* A value that would arrive after the end of the simulation can never be
     * observed, so it is not worth queueing. */
    if (when > s->plan->end_time)
        return TEMPO_OK;

    memset(&d, 0, sizeof(d));
    d.time = when;
    d.seq = s->enqueue_seq++;
    d.route = route_idx;
    if (r->type == TEMPO_TYPE_BYTES) {
        payload = src->bytes;
        payload_len = src->len < r->max_bytes ? src->len : r->max_bytes;
    } else {
        memcpy(&d.scalar, &src->scalar, scalar_width((tempo_type_t)r->type));
    }
    return tempo__dq_push(&s->dq, &d, payload, payload_len);
}

/* Look at a participant's output slots and route whatever changed.
 *
 * Change detection is a bit-exact compare against a shadow copy, so writing
 * the same value again costs nothing downstream (S12). A participant that
 * needs to force a resend of an unchanged value bumps the slot's generation.
 */
static int harvest(tempo_scheduler_t *s, uint32_t id, tempo_time_t at)
{
    const tempo_plan_t *plan = s->plan;
    const plan_participant_t *pp = &plan->parts[id];
    uint32_t j;

    for (j = 0; j < pp->n_outputs; j++) {
        uint32_t g = pp->output_base + j;
        const tempo_port_desc_t *d = &plan->outputs[g];
        tempo_slot_t *cur = &s->out_slots[g];
        tempo_slot_t *sh = &s->out_shadow[g];
        uint32_t k;
        bool changed;

        if (d->type == TEMPO_TYPE_BYTES) {
            changed = cur->generation != sh->generation || cur->len != sh->len ||
                      (cur->len && memcmp(cur->bytes, sh->bytes, cur->len) != 0);
        } else {
            changed = cur->generation != sh->generation ||
                      !scalar_equal(d->type, cur, sh);
        }
        if (!changed)
            continue;

        sh->generation = cur->generation;
        if (d->type == TEMPO_TYPE_BYTES) {
            sh->len = cur->len;
            if (cur->len)
                memcpy(sh->bytes, cur->bytes, cur->len);
        } else {
            memcpy(&sh->scalar, &cur->scalar, scalar_width(d->type));
        }

        emit_at(s, at, s->now.microstep, id, TEMPO_EV_PORT_WRITE, TEMPO_EVF_OBSERVE,
                g, d->type == TEMPO_TYPE_BYTES ? cur->len : scalar_bits(d->type, cur), 0);

        for (k = plan->out_route_off[g]; k < plan->out_route_off[g + 1]; k++) {
            int rc = enqueue_route(s, plan->out_route_idx[k], at, cur);
            if (rc != TEMPO_OK)
                return sched_fail(s, rc, "could not queue delivery: %s",
                                  tempo_status_str(rc));
        }
    }
    return TEMPO_OK;
}

/* ------------------------------------------------------ communication ----- */

static void mark_changed(tempo_scheduler_t *s, uint32_t pid, uint32_t local)
{
    sched_participant_t *p = &s->parts[pid];

    if (p->changed_flag[local])
        return;
    p->changed_flag[local] = 1;
    p->changed_list[p->n_changed++] = local;
}

static void clear_changed(tempo_scheduler_t *s)
{
    uint32_t i, j;

    for (i = 0; i < s->plan->n_parts; i++) {
        sched_participant_t *p = &s->parts[i];
        for (j = 0; j < p->n_changed; j++)
            p->changed_flag[p->changed_list[j]] = 0;
        p->n_changed = 0;
    }
}

static void apply_delivery(tempo_scheduler_t *s, const tempo_delivery_t *d)
{
    const tempo_route_t *r = &s->plan->routes[d->route];
    tempo_slot_t *dst = &s->in_slots[r->dst_global];
    uint32_t local = r->dst_global - s->plan->parts[r->dst_participant].input_base;
    uint64_t bits;

    if (r->type == TEMPO_TYPE_BYTES) {
        uint32_t len = d->len < dst->capacity ? d->len : dst->capacity;
        if (len)
            memcpy(dst->bytes, tempo__dq_payload(&s->dq, d), len);
        dst->len = len;
        bits = len;
    } else {
        memcpy(&dst->scalar, &d->scalar, scalar_width((tempo_type_t)r->type));
        bits = scalar_bits((tempo_type_t)r->type, dst);
    }
    /* The generation bump is the participant-visible signal that this input
     * carries a new value. */
    dst->generation++;
    mark_changed(s, r->dst_participant, local);

    emit(s, r->dst_participant, TEMPO_EV_ROUTE_DELIVER, TEMPO_EVF_OBSERVE,
         r->connection_id, bits);
}

static int notify_changed(tempo_scheduler_t *s)
{
    uint32_t i;

    for (i = 0; i < s->plan->n_parts; i++) {
        uint32_t id = s->plan->order[i];
        sched_participant_t *p = &s->parts[id];
        if (!p->n_changed || !p->vt->inputs_changed)
            continue;
        if (p->vt->inputs_changed(p->self, p->changed_list, p->n_changed) < 0)
            return sched_fail(s, TEMPO_ERR_PARTICIPANT,
                              "'%s' failed handling new inputs",
                              s->plan->parts[id].name);
    }
    return TEMPO_OK;
}

/* Let same-time-capable participants recompute at the current tag, so a
 * zero-delay chain settles within one logical instant (S17). */
static int react_same_time(tempo_scheduler_t *s)
{
    uint32_t i;

    for (i = 0; i < s->plan->n_parts; i++) {
        uint32_t id = s->plan->order[i];
        sched_participant_t *p = &s->parts[id];
        tempo_advance_result_t res;
        int rc;

        if (!p->n_changed || p->finished)
            continue;
        if (!(p->caps.flags & TEMPO_CAP_SAME_TIME_ITER))
            continue;
        if (p->time != s->now.time)
            continue;

        res.reached_time = p->time;
        res.reason = TEMPO_TARGET_REACHED;
        res._pad = 0;
        rc = p->vt->advance_to(p->self, s->now.time, &res);
        s->stats.advance_calls++;
        if (rc < 0 || res.reason == TEMPO_ERROR)
            return sched_fail(s, TEMPO_ERR_PARTICIPANT,
                              "'%s' failed during same-time iteration at %llu ns",
                              s->plan->parts[id].name,
                              (unsigned long long)s->now.time);
        if (res.reached_time != s->now.time)
            return sched_fail(s, TEMPO_ERR_TIME_REGRESSION,
                              "'%s' moved to %llu ns during a same-time iteration at %llu ns",
                              s->plan->parts[id].name,
                              (unsigned long long)res.reached_time,
                              (unsigned long long)s->now.time);
        rc = harvest(s, id, s->now.time);
        if (rc != TEMPO_OK)
            return rc;
    }
    return TEMPO_OK;
}

/* One communication point: deliver everything due, then iterate until
 * same-time propagation is quiescent. */
static int sync_point(tempo_scheduler_t *s)
{
    uint32_t round = 0;
    int rc;

    for (;;) {
        const tempo_delivery_t *top;
        tempo_delivery_t d;
        uint32_t n = 0;

        while ((top = tempo__dq_peek(&s->dq)) != NULL && top->time <= s->now.time) {
            tempo__dq_pop(&s->dq, &d);
            apply_delivery(s, &d);
            n++;
        }
        if (n == 0)
            break;

        s->stats.deliveries += n;
        emit(s, TEMPO_ID_ANY, TEMPO_EV_SYNC_POINT, TEMPO_EVF_SYNCHRONIZE, n, s->now.time);

        rc = notify_changed(s);
        if (rc != TEMPO_OK)
            return rc;

        if (!s->plan->has_zero_delay) {
            /* Every connection has a delay, so nothing can react within this
             * instant: participants will see the new values on their next
             * advance. This is the common, cheap case. */
            clear_changed(s);
            break;
        }

        round++;
        if (round > s->plan->max_microsteps) {
            clear_changed(s);
            return sched_fail(s, TEMPO_ERR_NON_CONVERGENT,
                              "zero-delay cycle did not converge within %u microsteps at %llu ns",
                              s->plan->max_microsteps, (unsigned long long)s->now.time);
        }
        s->now.microstep = round;
        s->stats.microsteps++;
        if (round > s->stats.max_microstep)
            s->stats.max_microstep = round;
        emit(s, TEMPO_ID_ANY, TEMPO_EV_MICROSTEP, TEMPO_EVF_OBSERVE, round, s->now.time);

        rc = react_same_time(s);
        clear_changed(s);
        if (rc != TEMPO_OK)
            return rc;
    }
    return TEMPO_OK;
}

/* ------------------------------------------------------------- stepping --- */

static tempo_time_t run_limit(const tempo_scheduler_t *s)
{
    return s->plan->end_time < s->run_until ? s->plan->end_time : s->run_until;
}

static tempo_time_t compute_horizon(const tempo_scheduler_t *s)
{
    const tempo_plan_t *plan = s->plan;
    const tempo_delivery_t *top;
    tempo_time_t h = tempo_time_add(s->now.time, plan->quantum);
    tempo_time_t limit = run_limit(s);
    uint32_t i;

    if (h > limit)
        h = limit;

    /* Never let anyone run past the instant at which an input changes. This
     * clamp is what makes the policy conservative. */
    top = tempo__dq_peek(&s->dq);
    if (top && top->time > s->now.time && top->time < h)
        h = top->time;

    /* A participant that knows when its own next externally relevant event
     * falls is not asked to run past it, which saves a wasted trap. */
    for (i = 0; i < plan->n_parts; i++) {
        const sched_participant_t *p = &s->parts[i];
        if (p->finished || !(p->caps.flags & TEMPO_CAP_PREDICT_NEXT))
            continue;
        if (p->next_event > s->now.time && p->next_event < h)
            h = p->next_event;
    }
    return h;
}

static int advance_frontier(tempo_scheduler_t *s, tempo_time_t horizon)
{
    const tempo_plan_t *plan = s->plan;
    uint32_t i;

    for (i = 0; i < plan->n_parts; i++) {
        uint32_t id = plan->order[i];
        sched_participant_t *p = &s->parts[id];
        tempo_advance_result_t res;
        int rc;

        if (p->finished)
            continue;
        if (p->time >= horizon) {
            /* Already ahead of this window -- an earlier trap let it run on.
             * Not advancing it is the whole point of Rule 3. */
            s->stats.skipped_advances++;
            continue;
        }

        emit_at(s, p->time, s->now.microstep, id, TEMPO_EV_PARTICIPANT_RUN,
                TEMPO_EVF_OBSERVE, 0, horizon, 0);

        res.reached_time = p->time;
        res.reason = TEMPO_TARGET_REACHED;
        res._pad = 0;
        rc = p->vt->advance_to(p->self, horizon, &res);
        s->stats.advance_calls++;

        if (rc < 0)
            return sched_fail(s, TEMPO_ERR_PARTICIPANT,
                              "'%s' failed advancing to %llu ns: %s",
                              plan->parts[id].name, (unsigned long long)horizon,
                              tempo_status_str(rc));
        if (res.reason == TEMPO_ERROR)
            return sched_fail(s, TEMPO_ERR_PARTICIPANT,
                              "'%s' reported an error at %llu ns",
                              plan->parts[id].name,
                              (unsigned long long)res.reached_time);
        if (res.reached_time < p->time)
            return sched_fail(s, TEMPO_ERR_TIME_REGRESSION,
                              "'%s' moved backwards from %llu to %llu ns",
                              plan->parts[id].name, (unsigned long long)p->time,
                              (unsigned long long)res.reached_time);
        if (res.reached_time > horizon)
            return sched_fail(s, TEMPO_ERR_TIME_REGRESSION,
                              "'%s' overshot its target: reached %llu ns, target was %llu ns",
                              plan->parts[id].name,
                              (unsigned long long)res.reached_time,
                              (unsigned long long)horizon);

        p->time = res.reached_time;
        p->blocked = (res.reason == TEMPO_BLOCKED);
        if (res.reason == TEMPO_FINISHED)
            p->finished = true;
        if (res.reached_time < horizon)
            s->stats.early_returns++;

        emit_at(s, p->time, s->now.microstep, id, TEMPO_EV_PARTICIPANT_STOP,
                res.reason == TEMPO_EVENT ? TEMPO_EVF_SYNCHRONIZE : TEMPO_EVF_OBSERVE,
                (uint32_t)res.reason, p->time, 0);

        rc = harvest(s, id, p->time);
        if (rc != TEMPO_OK)
            return rc;

        if (p->caps.flags & TEMPO_CAP_PREDICT_NEXT)
            p->next_event = p->vt->next_event_time(p->self);
    }
    return TEMPO_OK;
}

static void end_run(tempo_scheduler_t *s)
{
    s->finished = true;
    if (!s->ended) {
        s->ended = true;
        emit(s, TEMPO_ID_ANY, TEMPO_EV_SIM_END, TEMPO_EVF_OBSERVE, 0, s->now.time);
    }
}

int tempo_scheduler_step(tempo_scheduler_t *s)
{
    tempo_time_t horizon, new_now;
    uint64_t seq_before;
    bool all_finished = true;
    uint32_t i;
    int rc;

    if (!s)
        return TEMPO_ERR_INVALID_ARG;
    if (!s->initialized)
        return sched_fail(s, TEMPO_ERR_STATE, "scheduler has not been initialized");
    if (s->finished)
        return 1;

    /* SIM_START marks the beginning of execution, not the end of setup. Doing
     * it here means the trace of a run after reset() is identical to the
     * trace of the first run, which is what makes repeated execution
     * comparable (S39). */
    if (!s->started) {
        s->started = true;
        emit(s, TEMPO_ID_ANY, TEMPO_EV_SIM_START, TEMPO_EVF_OBSERVE,
             s->plan->n_parts, s->plan->end_time);
    }

    rc = sync_point(s);
    if (rc != TEMPO_OK)
        return rc;

    for (i = 0; i < s->plan->n_parts; i++)
        if (!s->parts[i].finished)
            all_finished = false;
    if (all_finished) {
        end_run(s);
        return 1;
    }
    if (s->now.time >= run_limit(s)) {
        if (s->now.time >= s->plan->end_time)
            end_run(s);
        return 1;
    }

    horizon = compute_horizon(s);
    if (horizon <= s->now.time) {
        /* compute_horizon() only clamps to a strictly greater time, so this
         * means the run limit has been reached. */
        if (s->now.time >= s->plan->end_time)
            end_run(s);
        return 1;
    }

    seq_before = s->enqueue_seq;
    rc = advance_frontier(s, horizon);
    if (rc != TEMPO_OK)
        return rc;

    /* Global logical time is the slowest live participant, capped at the
     * horizon: the conservative lower bound on what anyone may observe. */
    new_now = TEMPO_TIME_NEVER;
    all_finished = true;
    for (i = 0; i < s->plan->n_parts; i++) {
        const sched_participant_t *p = &s->parts[i];
        if (p->finished)
            continue;
        all_finished = false;
        if (p->time < new_now)
            new_now = p->time;
    }
    s->stats.windows++;

    if (all_finished) {
        /* Logical time ends where the last participant stopped, not at the
         * start of the window in which they all finished. */
        tempo_time_t last = s->now.time;
        for (i = 0; i < s->plan->n_parts; i++)
            if (s->parts[i].time > last)
                last = s->parts[i].time;
        s->now.time = last;
        s->now.microstep = 0;
        end_run(s);
        return 1;
    }
    if (new_now > horizon)
        new_now = horizon;

    if (new_now > s->now.time) {
        s->stall = 0;
        s->now.time = new_now;
        s->now.microstep = 0;
        emit(s, TEMPO_ID_ANY, TEMPO_EV_TIME_GRANT, TEMPO_EVF_OBSERVE, 0, new_now);
    } else {
        /* Nobody moved. That is legitimate for a while: a participant may
         * trap out repeatedly at one instant, and a zero-delay value it
         * produced is delivered by the next communication point. But a
         * federation that never progresses must be reported rather than spun
         * on forever, so allow at most max_microsteps such windows. */
        const tempo_delivery_t *top = tempo__dq_peek(&s->dq);
        bool due = top && top->time <= s->now.time;
        bool queued = s->enqueue_seq != seq_before;
        if (due || queued)
            s->stall = 0;
        else if (++s->stall > s->plan->max_microsteps)
            return sched_fail(s, TEMPO_ERR_STATE,
                              "no participant made progress at %llu ns across %u "
                              "consecutive windows",
                              (unsigned long long)s->now.time, s->stall);
    }

    if (s->now.time >= run_limit(s)) {
        /* Deliver anything that lands exactly on the final instant, so the
         * trace ends with a consistent state. */
        rc = sync_point(s);
        if (rc != TEMPO_OK)
            return rc;
        if (s->now.time >= s->plan->end_time)
            end_run(s);
        return 1;
    }
    return TEMPO_OK;
}

int tempo_scheduler_run_until(tempo_scheduler_t *s, tempo_time_t until)
{
    if (!s)
        return TEMPO_ERR_INVALID_ARG;
    if (!s->initialized)
        return sched_fail(s, TEMPO_ERR_STATE, "scheduler has not been initialized");
    if (until > s->plan->end_time)
        return sched_fail(s, TEMPO_ERR_INVALID_ARG,
                          "cannot run to %llu ns, the federation ends at %llu ns",
                          (unsigned long long)until,
                          (unsigned long long)s->plan->end_time);
    s->run_until = until;
    for (;;) {
        int rc = tempo_scheduler_step(s);
        if (rc < 0)
            return rc;
        if (rc == 1)
            break;
    }
    return TEMPO_OK;
}

int tempo_scheduler_run(tempo_scheduler_t *s)
{
    if (!s)
        return TEMPO_ERR_INVALID_ARG;
    return tempo_scheduler_run_until(s, s->plan->end_time);
}

/* ----------------------------------------------------------------- init --- */

/* A zero-delay connection asks the two ends to agree on a value within one
 * logical instant. That is only possible if the destination can be
 * re-evaluated at the same time, and only safe if the source cannot run past
 * the instant it produces the value. Both are capability questions, so they
 * are answered here -- once, before the run -- rather than in the loop. */
static int check_zero_delay_caps(tempo_scheduler_t *s)
{
    const tempo_plan_t *plan = s->plan;
    uint32_t i;

    for (i = 0; i < plan->n_routes; i++) {
        const tempo_route_t *r = &plan->routes[i];
        const sched_participant_t *src, *dst;

        if (r->delay != 0)
            continue;
        src = &s->parts[r->src_participant];
        dst = &s->parts[r->dst_participant];
        if (!(dst->caps.flags & TEMPO_CAP_SAME_TIME_ITER))
            return sched_fail(s, TEMPO_ERR_UNSUPPORTED,
                              "connection %u is zero-delay but '%s' cannot iterate at the "
                              "same logical time; give the connection a delay or add "
                              "TEMPO_CAP_SAME_TIME_ITER",
                              r->connection_id, plan->parts[r->dst_participant].name);
        if (!(src->caps.flags & (TEMPO_CAP_EARLY_RETURN | TEMPO_CAP_PREDICT_NEXT)))
            return sched_fail(s, TEMPO_ERR_UNSUPPORTED,
                              "connection %u is zero-delay but '%s' can neither return "
                              "early nor predict its next event, so it could run past the "
                              "instant it produces the value",
                              r->connection_id, plan->parts[r->src_participant].name);
    }
    return TEMPO_OK;
}

int tempo_scheduler_init(tempo_scheduler_t *s)
{
    const tempo_plan_t *plan;
    uint32_t i;
    int rc;

    if (!s)
        return TEMPO_ERR_INVALID_ARG;
    if (s->initialized)
        return sched_fail(s, TEMPO_ERR_STATE, "scheduler is already initialized");
    plan = s->plan;

    for (i = 0; i < plan->n_parts; i++)
        if (!s->parts[i].bound)
            return sched_fail(s, TEMPO_ERR_NOT_BOUND,
                              "participant '%s' has no implementation bound",
                              plan->parts[i].name);

    rc = check_zero_delay_caps(s);
    if (rc != TEMPO_OK)
        return rc;

    for (i = 0; i < plan->n_parts; i++) {
        sched_participant_t *p = &s->parts[i];
        tempo_participant_context_t *ctx = &p->ctx;

        ctx->participant_id = i;
        ctx->name = plan->parts[i].name;
        ctx->inputs = plan->parts[i].n_inputs ? &s->in_slots[plan->parts[i].input_base] : NULL;
        ctx->outputs = plan->parts[i].n_outputs ? &s->out_slots[plan->parts[i].output_base] : NULL;
        ctx->input_desc = plan->parts[i].n_inputs ? &plan->inputs[plan->parts[i].input_base] : NULL;
        ctx->output_desc = plan->parts[i].n_outputs ? &plan->outputs[plan->parts[i].output_base] : NULL;
        ctx->n_inputs = plan->parts[i].n_inputs;
        ctx->n_outputs = plan->parts[i].n_outputs;
        ctx->trace = s->trace;
        ctx->userdata = NULL;

        p->time = 0;
        p->finished = false;
        p->blocked = false;
        if (p->vt->initialize && p->vt->initialize(p->self, ctx) < 0)
            return sched_fail(s, TEMPO_ERR_PARTICIPANT,
                              "'%s' failed to initialize", plan->parts[i].name);
        p->next_event = (p->caps.flags & TEMPO_CAP_PREDICT_NEXT)
                            ? p->vt->next_event_time(p->self) : TEMPO_TIME_NEVER;
    }

    s->now = tempo_tag(0, 0);
    s->started = false;
    s->finished = false;
    s->ended = false;
    s->initialized = true;

    /* Route whatever the participants set up as initial conditions, so an
     * experiment does not have to run a window before its inputs are sane. */
    for (i = 0; i < plan->n_parts; i++) {
        rc = harvest(s, plan->order[i], 0);
        if (rc != TEMPO_OK)
            return rc;
    }
    return TEMPO_OK;
}

int tempo_scheduler_reset(tempo_scheduler_t *s)
{
    const tempo_plan_t *plan;
    uint32_t i;
    int rc;

    if (!s)
        return TEMPO_ERR_INVALID_ARG;
    if (!s->initialized)
        return sched_fail(s, TEMPO_ERR_STATE, "scheduler has not been initialized");
    plan = s->plan;

    for (i = 0; i < plan->n_parts; i++)
        if (!(s->parts[i].caps.flags & TEMPO_CAP_RESET))
            return sched_fail(s, TEMPO_ERR_UNSUPPORTED,
                              "'%s' cannot be reset", plan->parts[i].name);

    for (i = 0; i < plan->n_parts; i++) {
        sched_participant_t *p = &s->parts[i];
        if (p->vt->reset(p->self) < 0)
            return sched_fail(s, TEMPO_ERR_PARTICIPANT, "'%s' failed to reset",
                              plan->parts[i].name);
        p->time = 0;
        p->finished = false;
        p->blocked = false;
        p->next_event = (p->caps.flags & TEMPO_CAP_PREDICT_NEXT)
                            ? p->vt->next_event_time(p->self) : TEMPO_TIME_NEVER;
    }

    tempo__dq_clear(&s->dq);
    s->enqueue_seq = 0;
    clear_changed(s);
    for (i = 0; i < plan->n_input_slots; i++) {
        tempo_slot_t *sl = &s->in_slots[i];
        sl->generation = 0;
        sl->len = 0;
        memset(&sl->scalar, 0, sizeof(sl->scalar));
        if (sl->bytes)
            memset(sl->bytes, 0, sl->capacity);
    }
    for (i = 0; i < plan->n_output_slots; i++) {
        tempo_slot_t *a = &s->out_slots[i], *b = &s->out_shadow[i];
        a->generation = b->generation = 0;
        a->len = b->len = 0;
        memset(&a->scalar, 0, sizeof(a->scalar));
        memset(&b->scalar, 0, sizeof(b->scalar));
        if (a->bytes)
            memset(a->bytes, 0, a->capacity);
        if (b->bytes)
            memset(b->bytes, 0, b->capacity);
    }

    s->now = tempo_tag(0, 0);
    s->run_until = plan->end_time;
    s->started = false;
    s->finished = false;
    s->ended = false;
    s->stall = 0;
    memset(&s->stats, 0, sizeof(s->stats));
    s->error[0] = '\0';

    emit(s, TEMPO_ID_ANY, TEMPO_EV_RESET, TEMPO_EVF_OBSERVE, plan->n_parts, 0);

    for (i = 0; i < plan->n_parts; i++) {
        rc = harvest(s, plan->order[i], 0);
        if (rc != TEMPO_OK)
            return rc;
    }
    return TEMPO_OK;
}

/* -------------------------------------------------------------- queries --- */

tempo_tag_t tempo_scheduler_now(const tempo_scheduler_t *s)
{
    return s ? s->now : tempo_tag(0, 0);
}

bool tempo_scheduler_finished(const tempo_scheduler_t *s)
{
    return s ? s->finished : false;
}

tempo_time_t tempo_scheduler_participant_time(const tempo_scheduler_t *s, uint32_t id)
{
    if (!s || id >= s->plan->n_parts)
        return TEMPO_TIME_NEVER;
    return s->parts[id].time;
}

const char *tempo_scheduler_error(const tempo_scheduler_t *s)
{
    return s ? s->error : "";
}

tempo_slot_t *tempo_scheduler_input_slots(tempo_scheduler_t *s)
{
    return s ? s->in_slots : NULL;
}

tempo_slot_t *tempo_scheduler_output_slots(tempo_scheduler_t *s)
{
    return s ? s->out_slots : NULL;
}

tempo_scheduler_stats_t tempo_scheduler_stats(const tempo_scheduler_t *s)
{
    tempo_scheduler_stats_t empty;

    if (s)
        return s->stats;
    memset(&empty, 0, sizeof(empty));
    return empty;
}
