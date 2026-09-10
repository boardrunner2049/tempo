/* Minimal check-and-report harness.
 *
 * A test binary runs every case, reports each failure with a file and line,
 * and exits non-zero if anything failed -- so one run tells you everything
 * that is broken rather than only the first thing.
 */
#ifndef TEMPO_TEST_H
#define TEMPO_TEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tempo_test_failures;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            tempo_test_failures++;                                             \
            fprintf(stderr, "    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);\
        }                                                                      \
    } while (0)

#define CHECK_U64(actual, expected)                                            \
    do {                                                                       \
        unsigned long long _a = (unsigned long long)(actual);                  \
        unsigned long long _e = (unsigned long long)(expected);                \
        if (_a != _e) {                                                        \
            tempo_test_failures++;                                             \
            fprintf(stderr, "    FAIL %s:%d: %s == %llu, expected %llu\n",     \
                    __FILE__, __LINE__, #actual, _a, _e);                      \
        }                                                                      \
    } while (0)

#define CHECK_I(actual, expected)                                              \
    do {                                                                       \
        long long _a = (long long)(actual);                                    \
        long long _e = (long long)(expected);                                  \
        if (_a != _e) {                                                        \
            tempo_test_failures++;                                             \
            fprintf(stderr, "    FAIL %s:%d: %s == %lld, expected %lld\n",     \
                    __FILE__, __LINE__, #actual, _a, _e);                      \
        }                                                                      \
    } while (0)

#define CHECK_F(actual, expected, tol)                                         \
    do {                                                                       \
        double _a = (double)(actual);                                          \
        double _e = (double)(expected);                                        \
        double _d = _a > _e ? _a - _e : _e - _a;                               \
        if (!(_d <= (tol))) {                                                  \
            tempo_test_failures++;                                             \
            fprintf(stderr, "    FAIL %s:%d: %s == %g, expected %g\n",         \
                    __FILE__, __LINE__, #actual, _a, _e);                      \
        }                                                                      \
    } while (0)

#define CHECK_STR(actual, expected)                                            \
    do {                                                                       \
        const char *_a = (actual);                                             \
        const char *_e = (expected);                                           \
        if (!_a || !_e || strcmp(_a, _e) != 0) {                                \
            tempo_test_failures++;                                             \
            fprintf(stderr, "    FAIL %s:%d: %s == \"%s\", expected \"%s\"\n", \
                    __FILE__, __LINE__, #actual, _a ? _a : "(null)",           \
                    _e ? _e : "(null)");                                       \
        }                                                                      \
    } while (0)

#define RUN(fn)                                                                \
    do {                                                                       \
        fprintf(stderr, "  %s\n", #fn);                                        \
        fn();                                                                  \
    } while (0)

#define TEST_RESULT()                                                          \
    (tempo_test_failures                                                       \
         ? (fprintf(stderr, "%d check(s) failed\n", tempo_test_failures), 1)   \
         : (fprintf(stderr, "ok\n"), 0))

#endif /* TEMPO_TEST_H */
