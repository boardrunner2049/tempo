/* tempo/base.h -- common types, status codes, and hot-path hints.
 *
 * Every tempo header includes this one. It intentionally contains no
 * scheduler policy and no simulator-specific concepts.
 */
#ifndef TEMPO_BASE_H
#define TEMPO_BASE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TEMPO_VERSION_MAJOR 0
#define TEMPO_VERSION_MINOR 1
#define TEMPO_VERSION_PATCH 0

#if defined(__GNUC__) || defined(__clang__)
#  define TEMPO_LIKELY(x)   __builtin_expect(!!(x), 1)
#  define TEMPO_UNLIKELY(x) __builtin_expect(!!(x), 0)
#  define TEMPO_API         __attribute__((visibility("default")))
#  define TEMPO_INLINE      static inline __attribute__((always_inline))
#else
#  define TEMPO_LIKELY(x)   (x)
#  define TEMPO_UNLIKELY(x) (x)
#  define TEMPO_API
#  define TEMPO_INLINE      static inline
#endif

/* Sentinel participant / connection id meaning "any", used by subscriptions. */
#define TEMPO_ID_ANY UINT32_MAX

/* Return codes. Zero is success; every failure is negative so that
 * `if (rc < 0)` is the single hot-path check. */
typedef enum tempo_status {
    TEMPO_OK                  = 0,
    TEMPO_ERR_INVALID_ARG     = -1,
    TEMPO_ERR_NO_MEMORY       = -2,
    TEMPO_ERR_NOT_FOUND       = -3,
    TEMPO_ERR_DUPLICATE       = -4,
    TEMPO_ERR_TYPE_MISMATCH   = -5,
    TEMPO_ERR_DIRECTION       = -6,
    TEMPO_ERR_UNSUPPORTED     = -7,
    TEMPO_ERR_NOT_BOUND       = -8,
    TEMPO_ERR_STATE           = -9,
    TEMPO_ERR_TIME_REGRESSION = -10,
    TEMPO_ERR_NON_CONVERGENT  = -11,
    TEMPO_ERR_PARTICIPANT     = -12,
    TEMPO_ERR_CYCLE           = -13,
    TEMPO_ERR_CONFIG          = -14,
    TEMPO_ERR_OVERFLOW        = -15,
    TEMPO_ERR_IO              = -16
} tempo_status_t;

/* Human-readable name for a status code. Never returns NULL. */
TEMPO_API const char *tempo_status_str(int status);

/* Library version string, e.g. "0.1.0". */
TEMPO_API const char *tempo_version(void);

#ifdef __cplusplus
}
#endif
#endif /* TEMPO_BASE_H */
