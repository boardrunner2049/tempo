#include "tempo/time.h"
#include "tempo_test.h"

static void test_parse_units(void)
{
    tempo_time_t t;

    CHECK_I(tempo_time_parse("0", &t), TEMPO_OK);
    CHECK_U64(t, 0);
    CHECK_I(tempo_time_parse("250", &t), TEMPO_OK);
    CHECK_U64(t, 250);                       /* bare number is nanoseconds */
    CHECK_I(tempo_time_parse("100ns", &t), TEMPO_OK);
    CHECK_U64(t, 100);
    CHECK_I(tempo_time_parse("100us", &t), TEMPO_OK);
    CHECK_U64(t, 100000);
    CHECK_I(tempo_time_parse("3.5ms", &t), TEMPO_OK);
    CHECK_U64(t, 3500000);
    CHECK_I(tempo_time_parse("10s", &t), TEMPO_OK);
    CHECK_U64(t, 10000000000ULL);
    CHECK_I(tempo_time_parse("2min", &t), TEMPO_OK);
    CHECK_U64(t, 120000000000ULL);
    CHECK_I(tempo_time_parse("1h", &t), TEMPO_OK);
    CHECK_U64(t, 3600000000000ULL);
    CHECK_I(tempo_time_parse("  5 ms", &t), TEMPO_OK);
    CHECK_U64(t, 5000000);
}

static void test_parse_rejects(void)
{
    tempo_time_t t;

    CHECK(tempo_time_parse("", &t) != TEMPO_OK);
    CHECK(tempo_time_parse("ms", &t) != TEMPO_OK);
    CHECK(tempo_time_parse("10furlongs", &t) != TEMPO_OK);
    CHECK(tempo_time_parse("-1s", &t) != TEMPO_OK);
    /* "1msx" must not be accepted as "1ms" with trailing junk. */
    CHECK(tempo_time_parse("1msx", &t) != TEMPO_OK);
    CHECK(tempo_time_parse(NULL, &t) != TEMPO_OK);
    CHECK(tempo_time_parse("1s", NULL) != TEMPO_OK);
}

static void test_format(void)
{
    char buf[32];

    CHECK_STR(tempo_time_format(0, buf, sizeof(buf)), "0s");
    CHECK_STR(tempo_time_format(100, buf, sizeof(buf)), "100ns");
    CHECK_STR(tempo_time_format(100000, buf, sizeof(buf)), "100us");
    CHECK_STR(tempo_time_format(3500000, buf, sizeof(buf)), "3500us");
    CHECK_STR(tempo_time_format(2000000000ULL, buf, sizeof(buf)), "2s");
    CHECK_STR(tempo_time_format(TEMPO_TIME_NEVER, buf, sizeof(buf)), "never");
}

static void test_roundtrip(void)
{
    static const char *const values[] = { "1ns", "100us", "7ms", "3s", "0s" };
    size_t i;

    for (i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        tempo_time_t t, back;
        char buf[32];
        CHECK_I(tempo_time_parse(values[i], &t), TEMPO_OK);
        tempo_time_format(t, buf, sizeof(buf));
        CHECK_I(tempo_time_parse(buf, &back), TEMPO_OK);
        CHECK_U64(back, t);
    }
}

static void test_tags(void)
{
    tempo_tag_t a = tempo_tag(10, 0);
    tempo_tag_t b = tempo_tag(10, 1);
    tempo_tag_t c = tempo_tag(11, 0);

    CHECK(tempo_tag_cmp(a, a) == 0);
    CHECK(tempo_tag_cmp(a, b) < 0);
    CHECK(tempo_tag_cmp(b, a) > 0);
    /* Physical time does not advance during microsteps, but the tag still
     * orders strictly (S17). */
    CHECK(tempo_tag_cmp(b, c) < 0);
    CHECK(tempo_tag_cmp(c, b) > 0);
}

static void test_saturating_add(void)
{
    CHECK_U64(tempo_time_add(5, 7), 12);
    CHECK_U64(tempo_time_add(TEMPO_TIME_NEVER, 1), TEMPO_TIME_NEVER);
    CHECK_U64(tempo_time_add(1, TEMPO_TIME_NEVER), TEMPO_TIME_NEVER);
    /* A delay must never wrap logical time back to the past. */
    CHECK_U64(tempo_time_add(TEMPO_TIME_NEVER - 2, 10), TEMPO_TIME_NEVER);
    CHECK_U64(tempo_time_min(3, 9), 3);
}

int main(void)
{
    RUN(test_parse_units);
    RUN(test_parse_rejects);
    RUN(test_format);
    RUN(test_roundtrip);
    RUN(test_tags);
    RUN(test_saturating_add);
    return TEST_RESULT();
}
