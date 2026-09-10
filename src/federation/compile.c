/* The federation compiler (S13, S14).
 *
 * Input:  a mutable spec, however it was built.
 * Output: an immutable plan in which every name has become an index.
 *
 * Everything that can be wrong about a configuration is rejected here, with a
 * location the front end can point at -- a missing participant or port, a
 * type mismatch, two writers into one input, an unknown policy. The runtime
 * therefore never validates and never looks anything up by name (Rule 5).
 */
#include "tempo_internal.h"

#include <stdarg.h>
#include <stdio.h>

/* Default trace filter when the experiment declares no subscriptions: the
 * scheduler's own decisions, which are low-volume and almost always what you
 * want to see first. High-volume adapter namespaces stay off until something
 * asks for them (S21). */
#define TEMPO_TRACE_DEFAULT_CORE_MASK                                          \
    ((1ULL << TEMPO_EV_SIM_START) | (1ULL << TEMPO_EV_SIM_END) |               \
     (1ULL << TEMPO_EV_PARTICIPANT_RUN) | (1ULL << TEMPO_EV_PARTICIPANT_STOP) |\
     (1ULL << TEMPO_EV_TIME_GRANT) | (1ULL << TEMPO_EV_SYNC_POINT) |           \
     (1ULL << TEMPO_EV_MICROSTEP) | (1ULL << TEMPO_EV_PORT_WRITE) |            \
     (1ULL << TEMPO_EV_ROUTE_DELIVER) | (1ULL << TEMPO_EV_RESET))

static void diag_set(tempo_diagnostics_t *d, int status,
                     const char *location, const char *fmt, ...)
{
    va_list ap;

    if (!d)
        return;
    d->status = status;
    snprintf(d->location, sizeof(d->location), "%s", location ? location : "");
    va_start(ap, fmt);
    vsnprintf(d->message, sizeof(d->message), fmt, ap);
    va_end(ap);
}

/* Split "participant.port". Names contain no dots, so the first dot is the
 * separator and a second dot is a configuration error. */
static int split_ref(const char *ref, char *pname, size_t pn, char *portname, size_t on)
{
    const char *dot = strchr(ref, '.');
    size_t len;

    if (!dot || dot == ref || dot[1] == '\0')
        return TEMPO_ERR_CONFIG;
    if (strchr(dot + 1, '.'))
        return TEMPO_ERR_CONFIG;
    len = (size_t)(dot - ref);
    if (len >= pn || strlen(dot + 1) >= on)
        return TEMPO_ERR_CONFIG;
    memcpy(pname, ref, len);
    pname[len] = '\0';
    snprintf(portname, on, "%s", dot + 1);
    return TEMPO_OK;
}

/* Find a port by name and direction, returning its local index. */
static int find_spec_port(const spec_participant_t *p, const char *name,
                          tempo_port_dir_t dir, uint32_t *out_local,
                          const spec_port_t **out_port)
{
    uint32_t i, local = 0;

    for (i = 0; i < p->n_ports; i++) {
        if (p->ports[i].dir != dir)
            continue;
        if (!strcmp(p->ports[i].name, name)) {
            *out_local = local;
            *out_port = &p->ports[i];
            return TEMPO_OK;
        }
        local++;
    }
    return TEMPO_ERR_NOT_FOUND;
}

static int build_ports(struct tempo_plan *plan, const tempo_federation_spec_t *spec,
                       tempo_diagnostics_t *diag)
{
    uint32_t i, j, in_base = 0, out_base = 0;

    /* Pass 1: count and assign contiguous slot ranges per participant, so a
     * participant's ports are cache-adjacent. */
    for (i = 0; i < spec->n_parts; i++) {
        const spec_participant_t *sp = &spec->parts[i];
        uint32_t n_in = 0, n_out = 0;
        for (j = 0; j < sp->n_ports; j++) {
            if (sp->ports[j].dir == TEMPO_PORT_INPUT)
                n_in++;
            else
                n_out++;
        }
        plan->parts[i].input_base = in_base;
        plan->parts[i].n_inputs = n_in;
        plan->parts[i].output_base = out_base;
        plan->parts[i].n_outputs = n_out;
        in_base += n_in;
        out_base += n_out;
    }
    plan->n_input_slots = in_base;
    plan->n_output_slots = out_base;

    if (in_base) {
        plan->inputs = (tempo_port_desc_t *)calloc(in_base, sizeof(tempo_port_desc_t));
        plan->input_writer = (uint32_t *)malloc((size_t)in_base * sizeof(uint32_t));
        if (!plan->inputs || !plan->input_writer)
            goto oom;
        for (i = 0; i < in_base; i++)
            plan->input_writer[i] = TEMPO_ID_ANY;
    }
    if (out_base) {
        plan->outputs = (tempo_port_desc_t *)calloc(out_base, sizeof(tempo_port_desc_t));
        if (!plan->outputs)
            goto oom;
    }

    /* Pass 2: fill descriptors. `name` points into the plan's own copy of the
     * spec name, which the plan owns and frees. */
    for (i = 0; i < spec->n_parts; i++) {
        const spec_participant_t *sp = &spec->parts[i];
        uint32_t li = 0, lo = 0;
        for (j = 0; j < sp->n_ports; j++) {
            const spec_port_t *port = &sp->ports[j];
            tempo_port_desc_t *d;
            uint32_t local;

            if (port->dir == TEMPO_PORT_INPUT) {
                local = li++;
                d = &plan->inputs[plan->parts[i].input_base + local];
            } else {
                local = lo++;
                d = &plan->outputs[plan->parts[i].output_base + local];
            }
            d->name = tempo__strdup(port->name);
            if (!d->name)
                goto oom;
            d->participant_id = i;
            d->local_index = local;
            d->global_index = (uint32_t)(d - (port->dir == TEMPO_PORT_INPUT
                                                  ? plan->inputs : plan->outputs));
            d->type = port->type;
            d->max_bytes = port->max_bytes;
            d->dir = port->dir;
        }
    }
    return TEMPO_OK;

oom:
    diag_set(diag, TEMPO_ERR_NO_MEMORY, "ports", "out of memory building port tables");
    return TEMPO_ERR_NO_MEMORY;
}

static int build_routes(struct tempo_plan *plan, const tempo_federation_spec_t *spec,
                        tempo_diagnostics_t *diag)
{
    uint32_t i;
    char loc[128];
    char pname[64], portname[64];

    if (spec->n_conns == 0)
        return TEMPO_OK;

    plan->routes = (tempo_route_t *)calloc(spec->n_conns, sizeof(tempo_route_t));
    if (!plan->routes) {
        diag_set(diag, TEMPO_ERR_NO_MEMORY, "connections", "out of memory");
        return TEMPO_ERR_NO_MEMORY;
    }

    for (i = 0; i < spec->n_conns; i++) {
        const spec_conn_t *c = &spec->conns[i];
        tempo_route_t *r = &plan->routes[plan->n_routes];
        uint32_t src_id, dst_id, src_local, dst_local;
        const spec_port_t *src_port, *dst_port;
        int rc;

        snprintf(loc, sizeof(loc), "connections[%u].from", i);
        rc = split_ref(c->from, pname, sizeof(pname), portname, sizeof(portname));
        if (rc != TEMPO_OK) {
            diag_set(diag, rc, loc, "'%s' is not a \"participant.port\" reference", c->from);
            return rc;
        }
        src_id = tempo_spec_find_participant(spec, pname);
        if (src_id == TEMPO_ID_ANY) {
            diag_set(diag, TEMPO_ERR_NOT_FOUND, loc, "no participant named '%s'", pname);
            return TEMPO_ERR_NOT_FOUND;
        }
        if (find_spec_port(&spec->parts[src_id], portname, TEMPO_PORT_OUTPUT,
                           &src_local, &src_port) != TEMPO_OK) {
            diag_set(diag, TEMPO_ERR_NOT_FOUND, loc,
                     "participant '%s' has no output port '%s'", pname, portname);
            return TEMPO_ERR_NOT_FOUND;
        }

        snprintf(loc, sizeof(loc), "connections[%u].to", i);
        rc = split_ref(c->to, pname, sizeof(pname), portname, sizeof(portname));
        if (rc != TEMPO_OK) {
            diag_set(diag, rc, loc, "'%s' is not a \"participant.port\" reference", c->to);
            return rc;
        }
        dst_id = tempo_spec_find_participant(spec, pname);
        if (dst_id == TEMPO_ID_ANY) {
            diag_set(diag, TEMPO_ERR_NOT_FOUND, loc, "no participant named '%s'", pname);
            return TEMPO_ERR_NOT_FOUND;
        }
        if (find_spec_port(&spec->parts[dst_id], portname, TEMPO_PORT_INPUT,
                           &dst_local, &dst_port) != TEMPO_OK) {
            diag_set(diag, TEMPO_ERR_NOT_FOUND, loc,
                     "participant '%s' has no input port '%s'", pname, portname);
            return TEMPO_ERR_NOT_FOUND;
        }

        if (src_port->type != dst_port->type) {
            snprintf(loc, sizeof(loc), "connections[%u]", i);
            diag_set(diag, TEMPO_ERR_TYPE_MISMATCH, loc,
                     "'%s' is %s but '%s' is %s",
                     c->from, tempo_type_str(src_port->type),
                     c->to, tempo_type_str(dst_port->type));
            return TEMPO_ERR_TYPE_MISMATCH;
        }
        if (src_port->type == TEMPO_TYPE_BYTES &&
            dst_port->max_bytes < src_port->max_bytes) {
            snprintf(loc, sizeof(loc), "connections[%u]", i);
            diag_set(diag, TEMPO_ERR_CONFIG, loc,
                     "'%s' accepts %u bytes but '%s' can send %u",
                     c->to, dst_port->max_bytes, c->from, src_port->max_bytes);
            return TEMPO_ERR_CONFIG;
        }

        r->src_participant = src_id;
        r->dst_participant = dst_id;
        r->src_global = plan->parts[src_id].output_base + src_local;
        r->dst_global = plan->parts[dst_id].input_base + dst_local;
        r->connection_id = i;
        r->type = (uint16_t)src_port->type;
        r->max_bytes = (uint16_t)src_port->max_bytes;
        r->delay = (c->delay == TEMPO_TIME_NEVER) ? plan->default_delay : c->delay;

        /* An input has exactly one writer. Two connections into one input
         * would make the value order depend on evaluation order. */
        if (plan->input_writer[r->dst_global] != TEMPO_ID_ANY) {
            snprintf(loc, sizeof(loc), "connections[%u].to", i);
            diag_set(diag, TEMPO_ERR_DUPLICATE, loc,
                     "'%s' already receives connection %u", c->to,
                     plan->routes[plan->input_writer[r->dst_global]].connection_id);
            return TEMPO_ERR_DUPLICATE;
        }
        plan->input_writer[r->dst_global] = plan->n_routes;

        if (r->delay == 0)
            plan->has_zero_delay = true;
        plan->n_routes++;
    }

    /* Outgoing routes per output slot, CSR, so harvesting a changed output is
     * a contiguous scan rather than a search. */
    plan->out_route_off = (uint32_t *)calloc((size_t)plan->n_output_slots + 1,
                                             sizeof(uint32_t));
    if (!plan->out_route_off) {
        diag_set(diag, TEMPO_ERR_NO_MEMORY, "connections", "out of memory");
        return TEMPO_ERR_NO_MEMORY;
    }
    for (i = 0; i < plan->n_routes; i++)
        plan->out_route_off[plan->routes[i].src_global + 1]++;
    for (i = 0; i < plan->n_output_slots; i++)
        plan->out_route_off[i + 1] += plan->out_route_off[i];

    plan->out_route_idx = (uint32_t *)malloc((size_t)plan->n_routes * sizeof(uint32_t));
    if (!plan->out_route_idx) {
        diag_set(diag, TEMPO_ERR_NO_MEMORY, "connections", "out of memory");
        return TEMPO_ERR_NO_MEMORY;
    }
    {
        uint32_t *cursor = (uint32_t *)malloc((size_t)plan->n_output_slots * sizeof(uint32_t));
        if (!cursor) {
            diag_set(diag, TEMPO_ERR_NO_MEMORY, "connections", "out of memory");
            return TEMPO_ERR_NO_MEMORY;
        }
        memcpy(cursor, plan->out_route_off,
               (size_t)plan->n_output_slots * sizeof(uint32_t));
        /* Routes are appended in connection order, so each slot's list comes
         * out sorted by connection id. */
        for (i = 0; i < plan->n_routes; i++)
            plan->out_route_idx[cursor[plan->routes[i].src_global]++] = i;
        free(cursor);
    }
    return TEMPO_OK;
}

/* Full dependency graph over every connection, deduplicated, CSR by
 * participant and sorted by destination id. */
static int build_dep_graph(struct tempo_plan *plan, tempo_diagnostics_t *diag)
{
    uint32_t n = plan->n_parts, i, j, total = 0;
    uint8_t *seen = NULL;

    plan->succ = NULL;
    plan->n_succ_total = 0;
    if (n == 0 || plan->n_routes == 0) {
        for (i = 0; i < n; i++) {
            plan->parts[i].succ_off = 0;
            plan->parts[i].n_succ = 0;
        }
        return TEMPO_OK;
    }

    seen = (uint8_t *)calloc((size_t)n * n, 1);
    if (!seen) {
        diag_set(diag, TEMPO_ERR_NO_MEMORY, "connections", "out of memory");
        return TEMPO_ERR_NO_MEMORY;
    }
    for (i = 0; i < plan->n_routes; i++) {
        const tempo_route_t *r = &plan->routes[i];
        uint8_t *cell = &seen[(size_t)r->src_participant * n + r->dst_participant];
        if (!*cell) {
            *cell = 1;
            total++;
        }
    }

    plan->succ = (uint32_t *)malloc((size_t)(total ? total : 1) * sizeof(uint32_t));
    if (!plan->succ) {
        free(seen);
        diag_set(diag, TEMPO_ERR_NO_MEMORY, "connections", "out of memory");
        return TEMPO_ERR_NO_MEMORY;
    }
    for (i = 0; i < n; i++) {
        plan->parts[i].succ_off = plan->n_succ_total;
        for (j = 0; j < n; j++)
            if (seen[(size_t)i * n + j])
                plan->succ[plan->n_succ_total++] = j;
        plan->parts[i].n_succ = plan->n_succ_total - plan->parts[i].succ_off;
    }
    free(seen);
    return TEMPO_OK;
}

static int build_subscriptions(struct tempo_plan *plan,
                               const tempo_federation_spec_t *spec,
                               tempo_diagnostics_t *diag)
{
    uint32_t i;

    plan->trace_participants = (uint8_t *)calloc(plan->n_parts ? plan->n_parts : 1, 1);
    if (!plan->trace_participants) {
        diag_set(diag, TEMPO_ERR_NO_MEMORY, "analysis", "out of memory");
        return TEMPO_ERR_NO_MEMORY;
    }

    if (spec->n_subs == 0) {
        plan->trace_core_mask = TEMPO_TRACE_DEFAULT_CORE_MASK;
        plan->trace_ns_mask = 0;
        memset(plan->trace_participants, 1, plan->n_parts);
        return TEMPO_OK;
    }

    plan->subs = (tempo_subscription_t *)malloc((size_t)spec->n_subs *
                                                sizeof(tempo_subscription_t));
    if (!plan->subs) {
        diag_set(diag, TEMPO_ERR_NO_MEMORY, "analysis", "out of memory");
        return TEMPO_ERR_NO_MEMORY;
    }

    for (i = 0; i < spec->n_subs; i++) {
        const tempo_subscription_t *s = &spec->subs[i];
        char loc[64];

        snprintf(loc, sizeof(loc), "subscriptions[%u]", i);
        if (s->participant_id != TEMPO_ID_ANY && s->participant_id >= plan->n_parts) {
            diag_set(diag, TEMPO_ERR_NOT_FOUND, loc, "no participant with id %u",
                     s->participant_id);
            return TEMPO_ERR_NOT_FOUND;
        }
        plan->subs[plan->n_subs++] = *s;

        if (s->type == TEMPO_ID_ANY) {
            plan->trace_core_mask = ~0ULL;
            plan->trace_ns_mask = ~0u;
        } else {
            uint16_t t = (uint16_t)s->type;
            if (t < 64)
                plan->trace_core_mask |= 1ULL << t;
            else
                plan->trace_ns_mask |= 1u << ((TEMPO_EV_NAMESPACE(t) >> 8) & 31u);
        }

        if (s->participant_id == TEMPO_ID_ANY)
            memset(plan->trace_participants, 1, plan->n_parts);
        else
            plan->trace_participants[s->participant_id] = 1;
    }
    return TEMPO_OK;
}

tempo_plan_t *tempo_compile(const tempo_federation_spec_t *spec, tempo_diagnostics_t *diag)
{
    struct tempo_plan *plan;
    uint32_t i;
    int rc;

    if (diag) {
        diag->status = TEMPO_OK;
        diag->message[0] = '\0';
        diag->location[0] = '\0';
    }
    if (!spec) {
        diag_set(diag, TEMPO_ERR_INVALID_ARG, "", "no spec");
        return NULL;
    }
    if (spec->n_parts == 0) {
        diag_set(diag, TEMPO_ERR_CONFIG, "participants", "federation has no participants");
        return NULL;
    }
    if (spec->end_time == 0) {
        diag_set(diag, TEMPO_ERR_CONFIG, "simulation.end_time",
                 "end_time must be greater than zero");
        return NULL;
    }
    if (strcmp(spec->policy, "conservative") != 0) {
        diag_set(diag, TEMPO_ERR_CONFIG, "scheduler.policy",
                 "unknown policy '%s' (this version implements 'conservative')",
                 spec->policy);
        return NULL;
    }
    if (spec->quantum == 0) {
        diag_set(diag, TEMPO_ERR_CONFIG, "scheduler.quantum",
                 "quantum must be greater than zero");
        return NULL;
    }

    plan = (struct tempo_plan *)calloc(1, sizeof(*plan));
    if (!plan) {
        diag_set(diag, TEMPO_ERR_NO_MEMORY, "", "out of memory");
        return NULL;
    }
    plan->end_time = spec->end_time;
    plan->quantum = spec->quantum;
    plan->default_delay = (spec->default_delay == TEMPO_TIME_NEVER)
                              ? spec->quantum : spec->default_delay;
    plan->max_microsteps = spec->max_microsteps;
    plan->trace_capacity = spec->trace_capacity;
    snprintf(plan->policy, sizeof(plan->policy), "%s", spec->policy);

    plan->parts = (plan_participant_t *)calloc(spec->n_parts, sizeof(plan_participant_t));
    if (!plan->parts) {
        diag_set(diag, TEMPO_ERR_NO_MEMORY, "", "out of memory");
        goto fail;
    }
    plan->n_parts = spec->n_parts;
    for (i = 0; i < spec->n_parts; i++) {
        plan->parts[i].name = tempo__strdup(spec->parts[i].name);
        plan->parts[i].type = tempo__strdup(spec->parts[i].type);
        if (!plan->parts[i].name || !plan->parts[i].type) {
            diag_set(diag, TEMPO_ERR_NO_MEMORY, "participants", "out of memory");
            goto fail;
        }
    }

    rc = build_ports(plan, spec, diag);
    if (rc != TEMPO_OK)
        goto fail;
    rc = build_routes(plan, spec, diag);
    if (rc != TEMPO_OK)
        goto fail;
    rc = build_dep_graph(plan, diag);
    if (rc != TEMPO_OK)
        goto fail;
    rc = tempo__compute_components(plan);
    if (rc != TEMPO_OK) {
        diag_set(diag, rc, "connections", "dependency analysis failed: %s",
                 tempo_status_str(rc));
        goto fail;
    }
    rc = build_subscriptions(plan, spec, diag);
    if (rc != TEMPO_OK)
        goto fail;

    return plan;

fail:
    tempo_plan_free(plan);
    return NULL;
}

void tempo_plan_free(tempo_plan_t *plan)
{
    uint32_t i;

    if (!plan)
        return;
    for (i = 0; i < plan->n_parts; i++) {
        free(plan->parts[i].name);
        free(plan->parts[i].type);
    }
    for (i = 0; i < plan->n_input_slots; i++)
        free((void *)plan->inputs[i].name);
    for (i = 0; i < plan->n_output_slots; i++)
        free((void *)plan->outputs[i].name);
    free(plan->parts);
    free(plan->inputs);
    free(plan->outputs);
    free(plan->routes);
    free(plan->input_writer);
    free(plan->out_route_off);
    free(plan->out_route_idx);
    free(plan->succ);
    free(plan->comp_size);
    free(plan->comp_cyclic);
    free(plan->order);
    free(plan->subs);
    free(plan->trace_participants);
    free(plan);
}
