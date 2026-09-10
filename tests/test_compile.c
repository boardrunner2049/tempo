/* Federation compiler tests.
 *
 * The point of these is that every configuration mistake is caught here,
 * with a location, rather than surfacing as odd behaviour during a run (S14).
 */
#include "tempo/federation.h"
#include "tempo_test.h"

/* A minimal two-participant federation: controller.motor -> world.motor. */
static tempo_federation_spec_t *basic_spec(void)
{
    tempo_federation_spec_t *spec = tempo_spec_new();
    uint32_t controller, world;

    tempo_spec_set_end_time(spec, 10 * TEMPO_NS_PER_MS);
    tempo_spec_set_quantum(spec, 100 * TEMPO_NS_PER_US);
    tempo_spec_add_participant(spec, "controller", "mock", &controller);
    tempo_spec_add_participant(spec, "world", "mock", &world);
    tempo_spec_add_port(spec, controller, "motor", TEMPO_PORT_OUTPUT,
                        TEMPO_TYPE_FLOAT64, 0, NULL);
    tempo_spec_add_port(spec, controller, "gyro", TEMPO_PORT_INPUT,
                        TEMPO_TYPE_FLOAT64, 0, NULL);
    tempo_spec_add_port(spec, world, "motor", TEMPO_PORT_INPUT,
                        TEMPO_TYPE_FLOAT64, 0, NULL);
    tempo_spec_add_port(spec, world, "gyro", TEMPO_PORT_OUTPUT,
                        TEMPO_TYPE_FLOAT64, 0, NULL);
    tempo_spec_add_connection(spec, "controller.motor", "world.motor",
                              TEMPO_TIME_NEVER, NULL);
    tempo_spec_add_connection(spec, "world.gyro", "controller.gyro",
                              TEMPO_TIME_NEVER, NULL);
    return spec;
}

static void test_defaults(void)
{
    tempo_federation_spec_t *spec = tempo_spec_new();

    CHECK(spec != NULL);
    /* An empty federation is a configuration error, not an empty run. */
    CHECK(tempo_compile(spec, NULL) == NULL);
    tempo_spec_free(spec);
}

static void test_compiles_and_resolves(void)
{
    tempo_federation_spec_t *spec = basic_spec();
    tempo_diagnostics_t diag;
    tempo_plan_t *plan = tempo_compile(spec, &diag);
    tempo_route_t r;
    tempo_port_desc_t d;

    CHECK(plan != NULL);
    if (!plan) {
        fprintf(stderr, "    compile failed: %s (%s)\n", diag.message, diag.location);
        tempo_spec_free(spec);
        return;
    }

    CHECK_U64(tempo_plan_n_participants(plan), 2);
    CHECK_STR(tempo_plan_participant_name(plan, 0), "controller");
    CHECK_STR(tempo_plan_participant_type(plan, 0), "mock");
    CHECK_U64(tempo_plan_n_routes(plan), 2);
    CHECK_U64(tempo_plan_n_input_slots(plan), 2);
    CHECK_U64(tempo_plan_n_output_slots(plan), 2);
    CHECK_U64(tempo_plan_n_inputs(plan, 0), 1);
    CHECK_U64(tempo_plan_n_outputs(plan, 0), 1);

    /* A connection has become a pair of indices; no string survives into the
     * runtime record (S13). */
    CHECK_I(tempo_plan_route(plan, 0, &r), TEMPO_OK);
    CHECK_U64(r.connection_id, 0);
    CHECK_U64(r.src_participant, 0);
    CHECK_U64(r.dst_participant, 1);
    CHECK_U64(r.type, TEMPO_TYPE_FLOAT64);
    /* An unspecified delay follows the quantum, which is what keeps the
     * fixed-window policy sound. */
    CHECK_U64(r.delay, tempo_plan_quantum(plan));

    CHECK_I(tempo_plan_find_port(plan, "world.motor", TEMPO_PORT_INPUT, &d), TEMPO_OK);
    CHECK_U64(d.participant_id, 1);
    CHECK_U64(d.global_index, r.dst_global);
    CHECK(tempo_plan_find_port(plan, "world.motor", TEMPO_PORT_OUTPUT, &d) != TEMPO_OK);
    CHECK(tempo_plan_find_port(plan, "world.nope", TEMPO_PORT_INPUT, &d) != TEMPO_OK);

    tempo_plan_free(plan);
    tempo_spec_free(spec);
}

static void test_slot_bases_are_contiguous(void)
{
    tempo_federation_spec_t *spec = tempo_spec_new();
    tempo_plan_t *plan;
    uint32_t a, b, i;

    tempo_spec_set_end_time(spec, TEMPO_NS_PER_MS);
    tempo_spec_add_participant(spec, "a", "mock", &a);
    tempo_spec_add_participant(spec, "b", "mock", &b);
    for (i = 0; i < 3; i++) {
        char name[16];
        snprintf(name, sizeof(name), "o%u", i);
        tempo_spec_add_port(spec, a, name, TEMPO_PORT_OUTPUT, TEMPO_TYPE_INT64, 0, NULL);
        snprintf(name, sizeof(name), "i%u", i);
        tempo_spec_add_port(spec, b, name, TEMPO_PORT_INPUT, TEMPO_TYPE_INT64, 0, NULL);
    }
    plan = tempo_compile(spec, NULL);
    CHECK(plan != NULL);
    if (plan) {
        /* Each participant's ports occupy one contiguous range, so an adapter
         * gets a base pointer and an index rather than a lookup. */
        for (i = 0; i < 3; i++) {
            const tempo_port_desc_t *o = tempo_plan_port(plan, TEMPO_PORT_OUTPUT, i);
            const tempo_port_desc_t *in = tempo_plan_port(plan, TEMPO_PORT_INPUT, i);
            CHECK(o && o->participant_id == a && o->local_index == i);
            CHECK(in && in->participant_id == b && in->local_index == i);
        }
        tempo_plan_free(plan);
    }
    tempo_spec_free(spec);
}

static void test_port_name_scoping(void)
{
    tempo_federation_spec_t *spec = tempo_spec_new();
    uint32_t a;

    tempo_spec_set_end_time(spec, TEMPO_NS_PER_MS);
    tempo_spec_add_participant(spec, "a", "mock", &a);
    /* The same name in two directions is two different ports. */
    CHECK_I(tempo_spec_add_port(spec, a, "gyro", TEMPO_PORT_INPUT,
                                TEMPO_TYPE_FLOAT64, 0, NULL), TEMPO_OK);
    CHECK_I(tempo_spec_add_port(spec, a, "gyro", TEMPO_PORT_OUTPUT,
                                TEMPO_TYPE_FLOAT64, 0, NULL), TEMPO_OK);
    /* Repeating it within one direction is not. */
    CHECK_I(tempo_spec_add_port(spec, a, "gyro", TEMPO_PORT_INPUT,
                                TEMPO_TYPE_FLOAT64, 0, NULL), TEMPO_ERR_DUPLICATE);
    /* A name containing a dot would make dotted references ambiguous. */
    CHECK(tempo_spec_add_port(spec, a, "a.b", TEMPO_PORT_INPUT,
                              TEMPO_TYPE_FLOAT64, 0, NULL) != TEMPO_OK);
    CHECK_I(tempo_spec_add_participant(spec, "a", "mock", NULL), TEMPO_ERR_DUPLICATE);
    tempo_spec_free(spec);
}

/* Every rejection below must name a location the front end can point at. */
static void expect_fail(tempo_federation_spec_t *spec, int status, const char *location)
{
    tempo_diagnostics_t diag;
    tempo_plan_t *plan = tempo_compile(spec, &diag);

    CHECK(plan == NULL);
    if (plan) {
        tempo_plan_free(plan);
        return;
    }
    CHECK_I(diag.status, status);
    CHECK_STR(diag.location, location);
    CHECK(diag.message[0] != '\0');
}

static void test_missing_participant(void)
{
    tempo_federation_spec_t *spec = basic_spec();

    tempo_spec_add_connection(spec, "ghost.out", "world.motor", TEMPO_TIME_NEVER, NULL);
    expect_fail(spec, TEMPO_ERR_NOT_FOUND, "connections[2].from");
    tempo_spec_free(spec);
}

static void test_missing_port(void)
{
    tempo_federation_spec_t *spec = basic_spec();

    tempo_spec_add_connection(spec, "controller.nope", "world.motor",
                              TEMPO_TIME_NEVER, NULL);
    expect_fail(spec, TEMPO_ERR_NOT_FOUND, "connections[2].from");
    tempo_spec_free(spec);
}

static void test_direction_is_enforced(void)
{
    tempo_federation_spec_t *spec = basic_spec();

    /* "world.motor" is an input, so it cannot be a source. */
    tempo_spec_add_connection(spec, "world.motor", "controller.gyro",
                              TEMPO_TIME_NEVER, NULL);
    expect_fail(spec, TEMPO_ERR_NOT_FOUND, "connections[2].from");
    tempo_spec_free(spec);
}

static void test_type_mismatch(void)
{
    tempo_federation_spec_t *spec = tempo_spec_new();
    uint32_t a, b;

    tempo_spec_set_end_time(spec, TEMPO_NS_PER_MS);
    tempo_spec_add_participant(spec, "a", "mock", &a);
    tempo_spec_add_participant(spec, "b", "mock", &b);
    tempo_spec_add_port(spec, a, "out", TEMPO_PORT_OUTPUT, TEMPO_TYPE_FLOAT64, 0, NULL);
    tempo_spec_add_port(spec, b, "in", TEMPO_PORT_INPUT, TEMPO_TYPE_INT64, 0, NULL);
    tempo_spec_add_connection(spec, "a.out", "b.in", TEMPO_TIME_NEVER, NULL);
    expect_fail(spec, TEMPO_ERR_TYPE_MISMATCH, "connections[0]");
    tempo_spec_free(spec);
}

static void test_exclusive_input(void)
{
    tempo_federation_spec_t *spec = tempo_spec_new();
    uint32_t a, b, c;

    tempo_spec_set_end_time(spec, TEMPO_NS_PER_MS);
    tempo_spec_add_participant(spec, "a", "mock", &a);
    tempo_spec_add_participant(spec, "b", "mock", &b);
    tempo_spec_add_participant(spec, "c", "mock", &c);
    tempo_spec_add_port(spec, a, "out", TEMPO_PORT_OUTPUT, TEMPO_TYPE_FLOAT64, 0, NULL);
    tempo_spec_add_port(spec, b, "out", TEMPO_PORT_OUTPUT, TEMPO_TYPE_FLOAT64, 0, NULL);
    tempo_spec_add_port(spec, c, "in", TEMPO_PORT_INPUT, TEMPO_TYPE_FLOAT64, 0, NULL);
    tempo_spec_add_connection(spec, "a.out", "c.in", TEMPO_TIME_NEVER, NULL);
    /* Two writers into one input would make the result depend on evaluation
     * order, which is exactly what determinism forbids (S26). */
    tempo_spec_add_connection(spec, "b.out", "c.in", TEMPO_TIME_NEVER, NULL);
    expect_fail(spec, TEMPO_ERR_DUPLICATE, "connections[1].to");
    tempo_spec_free(spec);
}

static void test_bad_reference(void)
{
    tempo_federation_spec_t *spec = basic_spec();

    /* A reference with no dot is rejected at add time. */
    CHECK(tempo_spec_add_connection(spec, "controller", "world.motor",
                                    TEMPO_TIME_NEVER, NULL) != TEMPO_OK);
    /* A reference with two dots is ambiguous and is rejected at compile. */
    tempo_spec_add_connection(spec, "controller.motor.x", "world.motor",
                              TEMPO_TIME_NEVER, NULL);
    expect_fail(spec, TEMPO_ERR_CONFIG, "connections[2].from");
    tempo_spec_free(spec);
}

static void test_bad_settings(void)
{
    tempo_federation_spec_t *spec = basic_spec();

    tempo_spec_set_end_time(spec, 0);
    expect_fail(spec, TEMPO_ERR_CONFIG, "simulation.end_time");

    tempo_spec_set_end_time(spec, TEMPO_NS_PER_MS);
    tempo_spec_set_policy(spec, "time_warp");
    expect_fail(spec, TEMPO_ERR_CONFIG, "scheduler.policy");

    /* A zero quantum is refused up front rather than compiled into a
     * scheduler that cannot advance. */
    CHECK(tempo_spec_set_quantum(spec, 0) != TEMPO_OK);
    tempo_spec_free(spec);
}

static void test_bytes_capacity(void)
{
    tempo_federation_spec_t *spec = tempo_spec_new();
    uint32_t a, b;

    tempo_spec_set_end_time(spec, TEMPO_NS_PER_MS);
    tempo_spec_add_participant(spec, "a", "mock", &a);
    tempo_spec_add_participant(spec, "b", "mock", &b);
    tempo_spec_add_port(spec, a, "frame", TEMPO_PORT_OUTPUT, TEMPO_TYPE_BYTES, 64, NULL);
    tempo_spec_add_port(spec, b, "frame", TEMPO_PORT_INPUT, TEMPO_TYPE_BYTES, 8, NULL);
    /* Silently truncating a message would be worse than refusing to run. */
    tempo_spec_add_connection(spec, "a.frame", "b.frame", TEMPO_TIME_NEVER, NULL);
    expect_fail(spec, TEMPO_ERR_CONFIG, "connections[0]");
    tempo_spec_free(spec);
}

static void test_describe(void)
{
    tempo_federation_spec_t *spec = basic_spec();
    tempo_plan_t *plan = tempo_compile(spec, NULL);
    char buf[4096];
    char small[16];

    CHECK(plan != NULL);
    if (plan) {
        CHECK_I(tempo_plan_describe(plan, buf, sizeof(buf)), TEMPO_OK);
        CHECK(strstr(buf, "controller") != NULL);
        CHECK(strstr(buf, "out[") != NULL);
        /* A short buffer truncates and says so rather than overflowing. */
        CHECK_I(tempo_plan_describe(plan, small, sizeof(small)), TEMPO_ERR_OVERFLOW);
        CHECK(strlen(small) < sizeof(small));
        tempo_plan_free(plan);
    }
    tempo_spec_free(spec);
}

int main(void)
{
    RUN(test_defaults);
    RUN(test_compiles_and_resolves);
    RUN(test_slot_bases_are_contiguous);
    RUN(test_port_name_scoping);
    RUN(test_missing_participant);
    RUN(test_missing_port);
    RUN(test_direction_is_enforced);
    RUN(test_type_mismatch);
    RUN(test_exclusive_input);
    RUN(test_bad_reference);
    RUN(test_bad_settings);
    RUN(test_bytes_capacity);
    RUN(test_describe);
    return TEST_RESULT();
}
