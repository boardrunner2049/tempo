/* The mutable federation spec, built by the Python/TOML front end.
 *
 * Nothing here runs while the simulation runs. It exists so that the
 * compiler in compile.c has one well-defined input, whether the experiment
 * came from TOML, from the Python API, or from a C test.
 */
#include "tempo_internal.h"

#include <stdio.h>

tempo_federation_spec_t *tempo_spec_new(void)
{
    tempo_federation_spec_t *spec = (tempo_federation_spec_t *)calloc(1, sizeof(*spec));
    if (!spec)
        return NULL;
    spec->end_time = 0;
    spec->quantum = 100 * TEMPO_NS_PER_US;   /* the design doc's example default */
    spec->default_delay = TEMPO_TIME_NEVER;  /* == quantum, resolved at compile */
    spec->max_microsteps = 100;
    spec->trace_capacity = 0;                /* trace.c default */
    snprintf(spec->policy, sizeof(spec->policy), "%s", "conservative");
    return spec;
}

void tempo_spec_free(tempo_federation_spec_t *spec)
{
    uint32_t i, j;

    if (!spec)
        return;
    for (i = 0; i < spec->n_parts; i++) {
        for (j = 0; j < spec->parts[i].n_ports; j++)
            free(spec->parts[i].ports[j].name);
        free(spec->parts[i].ports);
        free(spec->parts[i].name);
        free(spec->parts[i].type);
    }
    free(spec->parts);
    for (i = 0; i < spec->n_conns; i++) {
        free(spec->conns[i].from);
        free(spec->conns[i].to);
    }
    free(spec->conns);
    free(spec->subs);
    free(spec);
}

int tempo_spec_set_end_time(tempo_federation_spec_t *spec, tempo_time_t t)
{
    if (!spec)
        return TEMPO_ERR_INVALID_ARG;
    spec->end_time = t;
    return TEMPO_OK;
}

int tempo_spec_set_policy(tempo_federation_spec_t *spec, const char *policy)
{
    if (!spec || !policy)
        return TEMPO_ERR_INVALID_ARG;
    snprintf(spec->policy, sizeof(spec->policy), "%s", policy);
    return TEMPO_OK;
}

int tempo_spec_set_quantum(tempo_federation_spec_t *spec, tempo_time_t q)
{
    if (!spec)
        return TEMPO_ERR_INVALID_ARG;
    if (q == 0)
        return TEMPO_ERR_INVALID_ARG;
    spec->quantum = q;
    return TEMPO_OK;
}

int tempo_spec_set_default_delay(tempo_federation_spec_t *spec, tempo_time_t d)
{
    if (!spec)
        return TEMPO_ERR_INVALID_ARG;
    spec->default_delay = d;
    return TEMPO_OK;
}

int tempo_spec_set_max_microsteps(tempo_federation_spec_t *spec, uint32_t n)
{
    if (!spec || n == 0)
        return TEMPO_ERR_INVALID_ARG;
    spec->max_microsteps = n;
    return TEMPO_OK;
}

int tempo_spec_set_trace_capacity(tempo_federation_spec_t *spec, size_t records)
{
    if (!spec)
        return TEMPO_ERR_INVALID_ARG;
    spec->trace_capacity = records;
    return TEMPO_OK;
}

uint32_t tempo_spec_find_participant(const tempo_federation_spec_t *spec, const char *name)
{
    uint32_t i;

    if (!spec || !name)
        return TEMPO_ID_ANY;
    for (i = 0; i < spec->n_parts; i++)
        if (!strcmp(spec->parts[i].name, name))
            return i;
    return TEMPO_ID_ANY;
}

/* A name must be usable in a dotted reference, so no '.' and no emptiness. */
static bool valid_name(const char *name)
{
    if (!name || !*name)
        return false;
    return strchr(name, '.') == NULL;
}

int tempo_spec_add_participant(tempo_federation_spec_t *spec,
                               const char *name,
                               const char *type,
                               uint32_t   *out_id)
{
    spec_participant_t *p;
    int rc;

    if (!spec || !valid_name(name) || !type || !*type)
        return TEMPO_ERR_INVALID_ARG;
    if (tempo_spec_find_participant(spec, name) != TEMPO_ID_ANY)
        return TEMPO_ERR_DUPLICATE;

    rc = TEMPO_RESERVE(&spec->parts, &spec->cap_parts, spec->n_parts + 1,
                       spec_participant_t);
    if (rc != TEMPO_OK)
        return rc;

    p = &spec->parts[spec->n_parts];
    p->name = tempo__strdup(name);
    p->type = tempo__strdup(type);
    if (!p->name || !p->type) {
        free(p->name);
        free(p->type);
        p->name = p->type = NULL;
        return TEMPO_ERR_NO_MEMORY;
    }
    if (out_id)
        *out_id = spec->n_parts;
    spec->n_parts++;
    return TEMPO_OK;
}

int tempo_spec_add_port(tempo_federation_spec_t *spec,
                        uint32_t         participant_id,
                        const char      *name,
                        tempo_port_dir_t dir,
                        tempo_type_t     type,
                        uint32_t         max_bytes,
                        uint32_t        *out_local_index)
{
    spec_participant_t *p;
    spec_port_t *port;
    uint32_t i, local = 0;
    int rc;

    if (!spec || participant_id >= spec->n_parts || !valid_name(name))
        return TEMPO_ERR_INVALID_ARG;
    if (type >= TEMPO_TYPE__COUNT)
        return TEMPO_ERR_INVALID_ARG;
    if (dir != TEMPO_PORT_INPUT && dir != TEMPO_PORT_OUTPUT)
        return TEMPO_ERR_DIRECTION;
    if (type == TEMPO_TYPE_BYTES) {
        if (max_bytes == 0)
            max_bytes = TEMPO_PORT_MAX_BYTES;
        if (max_bytes > TEMPO_PORT_MAX_BYTES)
            return TEMPO_ERR_INVALID_ARG;
    } else {
        max_bytes = 0;
    }

    p = &spec->parts[participant_id];
    /* A name may repeat across directions -- an input and an output called
     * "gyro" are different ports -- but not within one direction. */
    for (i = 0; i < p->n_ports; i++) {
        if (p->ports[i].dir == dir) {
            if (!strcmp(p->ports[i].name, name))
                return TEMPO_ERR_DUPLICATE;
            local++;
        }
    }

    rc = TEMPO_RESERVE(&p->ports, &p->cap_ports, p->n_ports + 1, spec_port_t);
    if (rc != TEMPO_OK)
        return rc;

    port = &p->ports[p->n_ports];
    port->name = tempo__strdup(name);
    if (!port->name)
        return TEMPO_ERR_NO_MEMORY;
    port->dir = dir;
    port->type = type;
    port->max_bytes = max_bytes;
    p->n_ports++;

    if (out_local_index)
        *out_local_index = local;
    return TEMPO_OK;
}

int tempo_spec_add_connection(tempo_federation_spec_t *spec,
                              const char   *from,
                              const char   *to,
                              tempo_time_t  delay,
                              uint32_t     *out_connection_id)
{
    spec_conn_t *c;
    int rc;

    if (!spec || !from || !to)
        return TEMPO_ERR_INVALID_ARG;
    /* The reference must be dotted; the compiler reports which half is wrong. */
    if (!strchr(from, '.') || !strchr(to, '.'))
        return TEMPO_ERR_CONFIG;

    rc = TEMPO_RESERVE(&spec->conns, &spec->cap_conns, spec->n_conns + 1, spec_conn_t);
    if (rc != TEMPO_OK)
        return rc;

    c = &spec->conns[spec->n_conns];
    c->from = tempo__strdup(from);
    c->to = tempo__strdup(to);
    if (!c->from || !c->to) {
        free(c->from);
        free(c->to);
        c->from = c->to = NULL;
        return TEMPO_ERR_NO_MEMORY;
    }
    c->delay = delay;
    if (out_connection_id)
        *out_connection_id = spec->n_conns;
    spec->n_conns++;
    return TEMPO_OK;
}

int tempo_spec_subscribe(tempo_federation_spec_t *spec, const tempo_subscription_t *sub)
{
    int rc;

    if (!spec || !sub)
        return TEMPO_ERR_INVALID_ARG;
    rc = TEMPO_RESERVE(&spec->subs, &spec->cap_subs, spec->n_subs + 1,
                       tempo_subscription_t);
    if (rc != TEMPO_OK)
        return rc;
    spec->subs[spec->n_subs++] = *sub;
    return TEMPO_OK;
}
