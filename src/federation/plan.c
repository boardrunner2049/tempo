/* Read-only accessors for a compiled plan.
 *
 * The plan layout is private, so Python and external tools depend only on
 * these functions and stay ABI-stable across changes to the internals.
 */
#include "tempo_internal.h"

#include <stdio.h>

uint32_t tempo_plan_n_participants(const tempo_plan_t *plan)
{
    return plan ? plan->n_parts : 0;
}

const char *tempo_plan_participant_name(const tempo_plan_t *plan, uint32_t id)
{
    if (!plan || id >= plan->n_parts)
        return NULL;
    return plan->parts[id].name;
}

const char *tempo_plan_participant_type(const tempo_plan_t *plan, uint32_t id)
{
    if (!plan || id >= plan->n_parts)
        return NULL;
    return plan->parts[id].type;
}

uint32_t tempo_plan_n_inputs(const tempo_plan_t *plan, uint32_t id)
{
    return (plan && id < plan->n_parts) ? plan->parts[id].n_inputs : 0;
}

uint32_t tempo_plan_n_outputs(const tempo_plan_t *plan, uint32_t id)
{
    return (plan && id < plan->n_parts) ? plan->parts[id].n_outputs : 0;
}

uint32_t tempo_plan_n_input_slots(const tempo_plan_t *plan)
{
    return plan ? plan->n_input_slots : 0;
}

uint32_t tempo_plan_n_output_slots(const tempo_plan_t *plan)
{
    return plan ? plan->n_output_slots : 0;
}

uint32_t tempo_plan_n_routes(const tempo_plan_t *plan)
{
    return plan ? plan->n_routes : 0;
}

int tempo_plan_route(const tempo_plan_t *plan, uint32_t i, tempo_route_t *out)
{
    if (!plan || !out || i >= plan->n_routes)
        return TEMPO_ERR_INVALID_ARG;
    *out = plan->routes[i];
    return TEMPO_OK;
}

tempo_time_t tempo_plan_end_time(const tempo_plan_t *plan)
{
    return plan ? plan->end_time : 0;
}

tempo_time_t tempo_plan_quantum(const tempo_plan_t *plan)
{
    return plan ? plan->quantum : 0;
}

uint32_t tempo_plan_max_microsteps(const tempo_plan_t *plan)
{
    return plan ? plan->max_microsteps : 0;
}

const char *tempo_plan_policy(const tempo_plan_t *plan)
{
    return plan ? plan->policy : NULL;
}

size_t tempo_plan_trace_capacity(const tempo_plan_t *plan)
{
    return plan ? plan->trace_capacity : 0;
}

int tempo_plan_find_port(const tempo_plan_t *plan, const char *dotted,
                         tempo_port_dir_t dir, tempo_port_desc_t *out)
{
    const tempo_port_desc_t *table;
    uint32_t n, i;
    const char *dot;
    size_t plen;

    if (!plan || !dotted || !out)
        return TEMPO_ERR_INVALID_ARG;
    dot = strchr(dotted, '.');
    if (!dot)
        return TEMPO_ERR_CONFIG;
    plen = (size_t)(dot - dotted);

    table = (dir == TEMPO_PORT_INPUT) ? plan->inputs : plan->outputs;
    n = (dir == TEMPO_PORT_INPUT) ? plan->n_input_slots : plan->n_output_slots;
    for (i = 0; i < n; i++) {
        const char *pname = plan->parts[table[i].participant_id].name;
        if (strlen(pname) == plen && !memcmp(pname, dotted, plen) &&
            !strcmp(table[i].name, dot + 1)) {
            *out = table[i];
            return TEMPO_OK;
        }
    }
    return TEMPO_ERR_NOT_FOUND;
}

const tempo_port_desc_t *tempo_plan_port(const tempo_plan_t *plan,
                                         tempo_port_dir_t dir,
                                         uint32_t global_index)
{
    if (!plan)
        return NULL;
    if (dir == TEMPO_PORT_INPUT)
        return global_index < plan->n_input_slots ? &plan->inputs[global_index] : NULL;
    return global_index < plan->n_output_slots ? &plan->outputs[global_index] : NULL;
}

uint32_t tempo_plan_n_successors(const tempo_plan_t *plan, uint32_t id)
{
    return (plan && id < plan->n_parts) ? plan->parts[id].n_succ : 0;
}

uint32_t tempo_plan_successor(const tempo_plan_t *plan, uint32_t id, uint32_t i)
{
    if (!plan || id >= plan->n_parts || i >= plan->parts[id].n_succ)
        return TEMPO_ID_ANY;
    return plan->succ[plan->parts[id].succ_off + i];
}

uint32_t tempo_plan_n_components(const tempo_plan_t *plan)
{
    return plan ? plan->n_components : 0;
}

uint32_t tempo_plan_component_of(const tempo_plan_t *plan, uint32_t id)
{
    return (plan && id < plan->n_parts) ? plan->parts[id].component : TEMPO_ID_ANY;
}

uint32_t tempo_plan_component_size(const tempo_plan_t *plan, uint32_t comp)
{
    return (plan && comp < plan->n_components) ? plan->comp_size[comp] : 0;
}

bool tempo_plan_component_is_cyclic(const tempo_plan_t *plan, uint32_t comp)
{
    return (plan && comp < plan->n_components) ? plan->comp_cyclic[comp] != 0 : false;
}

uint32_t tempo_plan_order_at(const tempo_plan_t *plan, uint32_t i)
{
    if (!plan || !plan->order || i >= plan->n_parts)
        return TEMPO_ID_ANY;
    return plan->order[i];
}

uint32_t tempo_plan_n_subscriptions(const tempo_plan_t *plan)
{
    return plan ? plan->n_subs : 0;
}

int tempo_plan_subscription(const tempo_plan_t *plan, uint32_t i, tempo_subscription_t *out)
{
    if (!plan || !out || i >= plan->n_subs)
        return TEMPO_ERR_INVALID_ARG;
    *out = plan->subs[i];
    return TEMPO_OK;
}

uint64_t tempo_plan_trace_core_mask(const tempo_plan_t *plan)
{
    return plan ? plan->trace_core_mask : 0;
}

uint32_t tempo_plan_trace_ns_mask(const tempo_plan_t *plan)
{
    return plan ? plan->trace_ns_mask : 0;
}

/* Appends to `buf` while tracking remaining space, so a small buffer
 * truncates instead of overflowing. */
#define APPEND(...)                                                            \
    do {                                                                       \
        int _n = snprintf(buf + used, len - used, __VA_ARGS__);                \
        if (_n < 0)                                                            \
            return TEMPO_ERR_IO;                                               \
        if ((size_t)_n >= len - used) {                                        \
            used = len - 1;                                                    \
            goto truncated;                                                    \
        }                                                                      \
        used += (size_t)_n;                                                    \
    } while (0)

int tempo_plan_describe(const tempo_plan_t *plan, char *buf, size_t len)
{
    size_t used = 0;
    char t1[32], t2[32], t3[32];
    uint32_t i;

    if (!plan || !buf || len < 2)
        return TEMPO_ERR_INVALID_ARG;
    buf[0] = '\0';

    APPEND("federation: %u participants, %u connections, %u components\n",
           plan->n_parts, plan->n_routes, plan->n_components);
    APPEND("  policy=%s end_time=%s quantum=%s default_delay=%s max_microsteps=%u\n",
           plan->policy,
           tempo_time_format(plan->end_time, t1, sizeof(t1)),
           tempo_time_format(plan->quantum, t2, sizeof(t2)),
           tempo_time_format(plan->default_delay, t3, sizeof(t3)),
           plan->max_microsteps);

    APPEND("participants:\n");
    for (i = 0; i < plan->n_parts; i++) {
        const plan_participant_t *p = &plan->parts[i];
        APPEND("  [%u] %-16s type=%-10s in=%u@%u out=%u@%u component=%u%s\n",
               i, p->name, p->type, p->n_inputs, p->input_base,
               p->n_outputs, p->output_base, p->component,
               plan->comp_cyclic && plan->comp_cyclic[p->component] ? " (cyclic)" : "");
    }

    APPEND("execution order:");
    for (i = 0; i < plan->n_parts; i++)
        APPEND(" %s", plan->parts[plan->order[i]].name);
    APPEND("\n");

    if (plan->n_routes) {
        APPEND("routes (compiled):\n");
        for (i = 0; i < plan->n_routes; i++) {
            const tempo_route_t *r = &plan->routes[i];
            APPEND("  [%u] %s.%s -> %s.%s : out[%u] -> in[%u] %s delay=%s\n",
                   r->connection_id,
                   plan->parts[r->src_participant].name,
                   plan->outputs[r->src_global].name,
                   plan->parts[r->dst_participant].name,
                   plan->inputs[r->dst_global].name,
                   r->src_global, r->dst_global,
                   tempo_type_str((tempo_type_t)r->type),
                   tempo_time_format(r->delay, t1, sizeof(t1)));
        }
    }

    APPEND("trace: core_mask=0x%016llx ns_mask=0x%08x subscriptions=%u\n",
           (unsigned long long)plan->trace_core_mask, plan->trace_ns_mask,
           plan->n_subs);
    return TEMPO_OK;

truncated:
    buf[used] = '\0';
    return TEMPO_ERR_OVERFLOW;
}
#undef APPEND
