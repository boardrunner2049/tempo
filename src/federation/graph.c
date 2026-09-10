/* Dependency-graph analysis (S15, S16).
 *
 * Two graphs matter, and conflating them is a real bug source:
 *
 *   - The *dependency* graph holds every connection. It says who can
 *     eventually affect whom, which is what a future causal-slicing pass
 *     needs (S28). It is built in compile.c.
 *
 *   - The *same-time* graph holds only zero-delay connections. A connection
 *     with a delay cannot force two participants to agree on a value at the
 *     same logical instant, so it cannot create a same-time cycle. It is the
 *     same-time graph whose strongly connected components decide execution
 *     order and whose cycles require microstep iteration.
 *
 * Components are numbered in topological order, so component_of() increases
 * along a same-time chain and the execution order is simply "components in
 * order, members by id". Where two components are unordered relative to each
 * other the smaller participant id goes first, which keeps the order a
 * property of the compiled federation rather than of the host (S26).
 */
#include "tempo_internal.h"

#define UNVISITED UINT32_MAX

typedef struct {
    uint32_t v;
    uint32_t ei;
} frame_t;

int tempo__compute_components(struct tempo_plan *plan)
{
    uint32_t n = plan->n_parts;
    uint32_t *off = NULL, *adj = NULL;
    uint32_t *idx = NULL, *low = NULL, *comp_stack = NULL;
    uint8_t  *on_stack = NULL;
    frame_t  *frames = NULL;
    uint32_t  n_edges = 0, index = 0, sp = 0, depth = 0, n_comp = 0;
    uint32_t  i, s, w;
    int rc = TEMPO_ERR_NO_MEMORY;

    plan->comp_size = NULL;
    plan->comp_cyclic = NULL;
    plan->order = NULL;
    plan->n_components = 0;

    if (n == 0)
        return TEMPO_OK;

    /* --- build the zero-delay adjacency in CSR form --- */
    off = (uint32_t *)calloc((size_t)n + 1, sizeof(uint32_t));
    if (!off)
        goto done;
    for (i = 0; i < plan->n_routes; i++) {
        if (plan->routes[i].delay == 0) {
            off[plan->routes[i].src_participant + 1]++;
            n_edges++;
        }
    }
    for (i = 0; i < n; i++)
        off[i + 1] += off[i];

    if (n_edges) {
        uint32_t *cursor = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
        adj = (uint32_t *)malloc((size_t)n_edges * sizeof(uint32_t));
        if (!cursor || !adj) {
            free(cursor);
            goto done;
        }
        memcpy(cursor, off, (size_t)n * sizeof(uint32_t));
        for (i = 0; i < plan->n_routes; i++) {
            const tempo_route_t *r = &plan->routes[i];
            if (r->delay == 0)
                adj[cursor[r->src_participant]++] = r->dst_participant;
        }
        free(cursor);
    }

    /* --- Tarjan, iteratively so a deep chain cannot blow the C stack --- */
    idx = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
    low = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
    comp_stack = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
    on_stack = (uint8_t *)calloc(n, 1);
    frames = (frame_t *)malloc((size_t)n * sizeof(frame_t));
    plan->comp_size = (uint32_t *)calloc(n, sizeof(uint32_t));
    plan->comp_cyclic = (uint8_t *)calloc(n, 1);
    plan->order = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
    if (!idx || !low || !comp_stack || !on_stack || !frames ||
        !plan->comp_size || !plan->comp_cyclic || !plan->order)
        goto done;

    for (i = 0; i < n; i++) {
        idx[i] = UNVISITED;
        plan->parts[i].component = UNVISITED;
    }

    for (s = 0; s < n; s++) {
        if (idx[s] != UNVISITED)
            continue;
        idx[s] = low[s] = index++;
        comp_stack[sp++] = s;
        on_stack[s] = 1;
        frames[depth].v = s;
        frames[depth].ei = off[s];
        depth++;

        while (depth > 0) {
            frame_t *f = &frames[depth - 1];
            if (f->ei < off[f->v + 1]) {
                w = adj[f->ei++];
                if (idx[w] == UNVISITED) {
                    idx[w] = low[w] = index++;
                    comp_stack[sp++] = w;
                    on_stack[w] = 1;
                    frames[depth].v = w;
                    frames[depth].ei = off[w];
                    depth++;
                } else if (on_stack[w]) {
                    if (idx[w] < low[f->v])
                        low[f->v] = idx[w];
                }
            } else {
                uint32_t v = f->v;
                depth--;
                if (depth > 0) {
                    uint32_t u = frames[depth - 1].v;
                    if (low[v] < low[u])
                        low[u] = low[v];
                }
                if (low[v] == idx[v]) {
                    uint32_t comp = n_comp++;
                    uint32_t size = 0;
                    do {
                        w = comp_stack[--sp];
                        on_stack[w] = 0;
                        plan->parts[w].component = comp;
                        size++;
                    } while (w != v);
                    plan->comp_size[comp] = size;
                    plan->comp_cyclic[comp] = size > 1;
                }
            }
        }
    }
    plan->n_components = n_comp;

    /* A single participant forms a cyclic component if it feeds itself with
     * zero delay. */
    for (i = 0; i < plan->n_routes; i++) {
        const tempo_route_t *r = &plan->routes[i];
        if (r->delay == 0 && r->src_participant == r->dst_participant)
            plan->comp_cyclic[plan->parts[r->src_participant].component] = 1;
    }

    /* --- topological order of the condensation ---
     *
     * Tarjan numbers components in reverse topological order, so reversing it
     * would already be a valid execution order. But when two components are
     * unordered relative to each other -- two independent subsystems, or a
     * federation with no connections at all -- reversal would list them
     * backwards by id, which makes a trivial federation's trace read
     * backwards for no reason. So run Kahn's algorithm instead and break ties
     * by the smallest participant id in the component. Components are then
     * renumbered into that order, which also means component_of() increases
     * along a chain.
     *
     * This is O(C^2) in the number of components, which is setup-time work on
     * a federation of tens to hundreds of participants. If federations ever
     * get large enough for that to matter, the dense matrices here and in
     * build_dep_graph() are the things to replace. */
    {
        uint32_t C = n_comp, c, d, pos = 0;
        uint8_t *cedge = NULL;
        uint32_t *indeg = NULL, *minid = NULL, *seq = NULL, *remap = NULL;
        uint32_t *new_size = NULL;
        uint8_t *new_cyclic = NULL;

        cedge = (uint8_t *)calloc((size_t)C * C, 1);
        indeg = (uint32_t *)calloc(C, sizeof(uint32_t));
        minid = (uint32_t *)malloc((size_t)C * sizeof(uint32_t));
        seq = (uint32_t *)malloc((size_t)C * sizeof(uint32_t));
        remap = (uint32_t *)malloc((size_t)C * sizeof(uint32_t));
        new_size = (uint32_t *)calloc(C, sizeof(uint32_t));
        new_cyclic = (uint8_t *)calloc(C, 1);
        if (!cedge || !indeg || !minid || !seq || !remap || !new_size || !new_cyclic) {
            free(cedge); free(indeg); free(minid);
            free(seq); free(remap); free(new_size); free(new_cyclic);
            goto done;
        }

        for (c = 0; c < C; c++)
            minid[c] = UINT32_MAX;
        for (i = 0; i < n; i++) {
            c = plan->parts[i].component;
            if (i < minid[c])
                minid[c] = i;
        }
        for (i = 0; i < plan->n_routes; i++) {
            const tempo_route_t *r = &plan->routes[i];
            uint32_t sc, dc;
            if (r->delay != 0)
                continue;
            sc = plan->parts[r->src_participant].component;
            dc = plan->parts[r->dst_participant].component;
            if (sc == dc || cedge[(size_t)sc * C + dc])
                continue;
            cedge[(size_t)sc * C + dc] = 1;
            indeg[dc]++;
        }

        while (pos < C) {
            uint32_t best = UINT32_MAX;
            for (c = 0; c < C; c++)
                if (indeg[c] == 0 && (best == UINT32_MAX || minid[c] < minid[best]))
                    best = c;
            if (best == UINT32_MAX) {
                /* Tarjan's condensation is acyclic by construction, so this
                 * cannot happen; treat it as a bug rather than a bad graph. */
                free(cedge); free(indeg); free(minid);
                free(seq); free(remap); free(new_size); free(new_cyclic);
                rc = TEMPO_ERR_STATE;
                goto done;
            }
            seq[pos] = best;
            remap[best] = pos;
            indeg[best] = UINT32_MAX;   /* emitted */
            for (d = 0; d < C; d++)
                if (cedge[(size_t)best * C + d] && indeg[d] != UINT32_MAX)
                    indeg[d]--;
            pos++;
        }

        for (c = 0; c < C; c++) {
            new_size[remap[c]] = plan->comp_size[c];
            new_cyclic[remap[c]] = plan->comp_cyclic[c];
        }
        memcpy(plan->comp_size, new_size, (size_t)C * sizeof(uint32_t));
        memcpy(plan->comp_cyclic, new_cyclic, C);
        for (i = 0; i < n; i++)
            plan->parts[i].component = remap[plan->parts[i].component];

        pos = 0;
        for (c = 0; c < C; c++)
            for (i = 0; i < n; i++)
                if (plan->parts[i].component == c)
                    plan->order[pos++] = i;

        free(cedge); free(indeg); free(minid);
        free(seq); free(remap); free(new_size); free(new_cyclic);
        if (pos != n) {
            rc = TEMPO_ERR_STATE;
            goto done;
        }
        (void)seq;
    }
    rc = TEMPO_OK;

done:
    free(off);
    free(adj);
    free(idx);
    free(low);
    free(comp_stack);
    free(on_stack);
    free(frames);
    if (rc != TEMPO_OK) {
        free(plan->comp_size);
        free(plan->comp_cyclic);
        free(plan->order);
        plan->comp_size = NULL;
        plan->comp_cyclic = NULL;
        plan->order = NULL;
        plan->n_components = 0;
    }
    return rc;
}
