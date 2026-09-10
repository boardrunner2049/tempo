/* Dependency-graph and component tests (S15, S16).
 *
 * The distinction being tested: a delayed connection is a dependency but not
 * a same-time dependency, so it cannot create a cycle that needs microstep
 * iteration.
 */
#include "tempo/federation.h"
#include "tempo_test.h"

static tempo_federation_spec_t *spec_with(uint32_t n_parts)
{
    tempo_federation_spec_t *spec = tempo_spec_new();
    uint32_t i;

    tempo_spec_set_end_time(spec, TEMPO_NS_PER_MS);
    tempo_spec_set_quantum(spec, 100 * TEMPO_NS_PER_US);
    for (i = 0; i < n_parts; i++) {
        char name[8];
        uint32_t id;
        name[0] = (char)('a' + i);
        name[1] = '\0';
        tempo_spec_add_participant(spec, name, "mock", &id);
        tempo_spec_add_port(spec, id, "in", TEMPO_PORT_INPUT, TEMPO_TYPE_FLOAT64, 0, NULL);
        tempo_spec_add_port(spec, id, "in2", TEMPO_PORT_INPUT, TEMPO_TYPE_FLOAT64, 0, NULL);
        tempo_spec_add_port(spec, id, "out", TEMPO_PORT_OUTPUT, TEMPO_TYPE_FLOAT64, 0, NULL);
        tempo_spec_add_port(spec, id, "out2", TEMPO_PORT_OUTPUT, TEMPO_TYPE_FLOAT64, 0, NULL);
    }
    return spec;
}

static uint32_t index_in_order(const tempo_plan_t *plan, const char *name)
{
    uint32_t i;

    for (i = 0; i < tempo_plan_n_participants(plan); i++) {
        uint32_t id = tempo_plan_order_at(plan, i);
        if (!strcmp(tempo_plan_participant_name(plan, id), name))
            return i;
    }
    return TEMPO_ID_ANY;
}

static void test_acyclic_chain_order(void)
{
    tempo_federation_spec_t *spec = spec_with(3);
    tempo_plan_t *plan;

    /* a -> b -> c, all same-time. */
    tempo_spec_add_connection(spec, "a.out", "b.in", 0, NULL);
    tempo_spec_add_connection(spec, "b.out", "c.in", 0, NULL);
    plan = tempo_compile(spec, NULL);
    CHECK(plan != NULL);
    if (plan) {
        CHECK_U64(tempo_plan_n_components(plan), 3);
        CHECK(!tempo_plan_component_is_cyclic(plan, tempo_plan_component_of(plan, 0)));
        /* Producers must run before consumers, so a value can propagate
         * through the whole chain within one instant. */
        CHECK(index_in_order(plan, "a") < index_in_order(plan, "b"));
        CHECK(index_in_order(plan, "b") < index_in_order(plan, "c"));
        tempo_plan_free(plan);
    }
    tempo_spec_free(spec);
}

static void test_zero_delay_cycle_is_one_component(void)
{
    tempo_federation_spec_t *spec = spec_with(2);
    tempo_plan_t *plan;

    tempo_spec_add_connection(spec, "a.out", "b.in", 0, NULL);
    tempo_spec_add_connection(spec, "b.out", "a.in", 0, NULL);
    plan = tempo_compile(spec, NULL);
    CHECK(plan != NULL);
    if (plan) {
        uint32_t ca = tempo_plan_component_of(plan, 0);
        CHECK_U64(tempo_plan_n_components(plan), 1);
        CHECK_U64(ca, tempo_plan_component_of(plan, 1));
        CHECK_U64(tempo_plan_component_size(plan, ca), 2);
        /* A cyclic component is the flag the scheduler needs to know that
         * same-time iteration is required here. */
        CHECK(tempo_plan_component_is_cyclic(plan, ca));
        tempo_plan_free(plan);
    }
    tempo_spec_free(spec);
}

static void test_delayed_cycle_is_not_a_same_time_cycle(void)
{
    tempo_federation_spec_t *spec = spec_with(2);
    tempo_plan_t *plan;

    /* The same topology, but each hop takes time. */
    tempo_spec_add_connection(spec, "a.out", "b.in", 50 * TEMPO_NS_PER_US, NULL);
    tempo_spec_add_connection(spec, "b.out", "a.in", 50 * TEMPO_NS_PER_US, NULL);
    plan = tempo_compile(spec, NULL);
    CHECK(plan != NULL);
    if (plan) {
        CHECK_U64(tempo_plan_n_components(plan), 2);
        CHECK(!tempo_plan_component_is_cyclic(plan, tempo_plan_component_of(plan, 0)));
        CHECK(!tempo_plan_component_is_cyclic(plan, tempo_plan_component_of(plan, 1)));
        /* But it is still a dependency in both directions, which is what a
         * future causal-slicing pass needs to see (S28). */
        CHECK_U64(tempo_plan_n_successors(plan, 0), 1);
        CHECK_U64(tempo_plan_successor(plan, 0, 0), 1);
        CHECK_U64(tempo_plan_n_successors(plan, 1), 1);
        CHECK_U64(tempo_plan_successor(plan, 1, 0), 0);
        tempo_plan_free(plan);
    }
    tempo_spec_free(spec);
}

static void test_self_loop(void)
{
    tempo_federation_spec_t *spec = spec_with(1);
    tempo_plan_t *plan;

    tempo_spec_add_connection(spec, "a.out", "a.in", 0, NULL);
    plan = tempo_compile(spec, NULL);
    CHECK(plan != NULL);
    if (plan) {
        uint32_t c = tempo_plan_component_of(plan, 0);
        CHECK_U64(tempo_plan_component_size(plan, c), 1);
        /* A single participant feeding itself with zero delay still needs
         * microsteps. */
        CHECK(tempo_plan_component_is_cyclic(plan, c));
        tempo_plan_free(plan);
    }
    tempo_spec_free(spec);
}

static void test_independent_subsystems(void)
{
    tempo_federation_spec_t *spec = spec_with(4);
    tempo_plan_t *plan;

    /* Two disconnected pairs: a -> b and c -> d (S15). */
    tempo_spec_add_connection(spec, "a.out", "b.in", 0, NULL);
    tempo_spec_add_connection(spec, "c.out", "d.in", 0, NULL);
    plan = tempo_compile(spec, NULL);
    CHECK(plan != NULL);
    if (plan) {
        CHECK_U64(tempo_plan_n_components(plan), 4);
        CHECK(index_in_order(plan, "a") < index_in_order(plan, "b"));
        CHECK(index_in_order(plan, "c") < index_in_order(plan, "d"));
        /* Nothing links the two halves. */
        CHECK_U64(tempo_plan_n_successors(plan, 0), 1);
        CHECK_U64(tempo_plan_n_successors(plan, 1), 0);
        tempo_plan_free(plan);
    }
    tempo_spec_free(spec);
}

static void test_duplicate_edges_are_deduplicated(void)
{
    tempo_federation_spec_t *spec = spec_with(2);
    tempo_plan_t *plan;

    /* Two connections between the same pair are one dependency edge. */
    tempo_spec_add_connection(spec, "a.out", "b.in", 0, NULL);
    tempo_spec_add_connection(spec, "a.out2", "b.in2", 0, NULL);
    plan = tempo_compile(spec, NULL);
    CHECK(plan != NULL);
    if (plan) {
        CHECK_U64(tempo_plan_n_routes(plan), 2);
        CHECK_U64(tempo_plan_n_successors(plan, 0), 1);
        tempo_plan_free(plan);
    }
    tempo_spec_free(spec);
}

static void test_no_connections(void)
{
    tempo_federation_spec_t *spec = spec_with(3);
    tempo_plan_t *plan = tempo_compile(spec, NULL);

    CHECK(plan != NULL);
    if (plan) {
        uint32_t i;
        CHECK_U64(tempo_plan_n_components(plan), 3);
        /* With no edges at all, the order is simply by id, which keeps a
         * trivial federation's trace stable. */
        for (i = 0; i < 3; i++)
            CHECK_U64(tempo_plan_order_at(plan, i), i);
        tempo_plan_free(plan);
    }
    tempo_spec_free(spec);
}

static void test_order_is_a_permutation(void)
{
    tempo_federation_spec_t *spec = spec_with(5);
    tempo_plan_t *plan;

    tempo_spec_add_connection(spec, "a.out", "b.in", 0, NULL);
    tempo_spec_add_connection(spec, "b.out", "c.in", 0, NULL);
    tempo_spec_add_connection(spec, "c.out", "b.in2", 0, NULL);   /* b <-> c */
    tempo_spec_add_connection(spec, "d.out", "e.in", 10, NULL);
    plan = tempo_compile(spec, NULL);
    CHECK(plan != NULL);
    if (plan) {
        uint8_t seen[5] = { 0, 0, 0, 0, 0 };
        uint32_t i, cb, cc;
        for (i = 0; i < 5; i++) {
            uint32_t id = tempo_plan_order_at(plan, i);
            CHECK(id < 5);
            if (id < 5) {
                CHECK(!seen[id]);
                seen[id] = 1;
            }
        }
        cb = tempo_plan_component_of(plan, 1);
        cc = tempo_plan_component_of(plan, 2);
        CHECK_U64(cb, cc);
        CHECK(tempo_plan_component_is_cyclic(plan, cb));
        /* a still precedes the b/c component. */
        CHECK(index_in_order(plan, "a") < index_in_order(plan, "b"));
        tempo_plan_free(plan);
    }
    tempo_spec_free(spec);
}

int main(void)
{
    RUN(test_acyclic_chain_order);
    RUN(test_zero_delay_cycle_is_one_component);
    RUN(test_delayed_cycle_is_not_a_same_time_cycle);
    RUN(test_self_loop);
    RUN(test_independent_subsystems);
    RUN(test_duplicate_edges_are_deduplicated);
    RUN(test_no_connections);
    RUN(test_order_is_a_permutation);
    return TEST_RESULT();
}
