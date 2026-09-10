/* tempo/event.h -- the common native event record.
 *
 * Observability is part of the architecture, not logging bolted on afterwards
 * (S2.2, S20). Every observable thing in the system -- scheduler decisions,
 * port traffic, adapter-specific activity -- becomes one fixed-size record.
 *
 * Crucially, "observable" does not mean "synchronizing" (S11, Rule 4). An
 * OBSERVE event is written to a ring buffer and execution continues. Only a
 * SYNCHRONIZE event returns control to the scheduler.
 */
#ifndef TEMPO_EVENT_H
#define TEMPO_EVENT_H

#include "tempo/base.h"
#include "tempo/time.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Type namespaces. The scheduler core reserves only the low range, so
 * adapters can define their own types without the core learning about them
 * (S20). */
#define TEMPO_EV_NS_CORE     0x0000u  /* 0x0000 - 0x00FF: scheduler core   */
#define TEMPO_EV_NS_FASTDYN  0x0100u  /* 0x0100 - 0x01FF: FastDyn adapter  */
#define TEMPO_EV_NS_FMI      0x0200u  /* 0x0200 - 0x02FF: FMI adapter      */
#define TEMPO_EV_NS_WORLD    0x0300u  /* 0x0300 - 0x03FF: world/physics    */
#define TEMPO_EV_NS_USER     0x8000u  /* 0x8000 - 0xFFFF: user analyses    */

#define TEMPO_EV_NAMESPACE(type) ((uint16_t)((type) & 0xFF00u))

/* Core event types. Values are stable; append only. */
typedef enum tempo_event_type {
    TEMPO_EV_SIM_START        = 0,
    TEMPO_EV_SIM_END          = 1,
    TEMPO_EV_PARTICIPANT_RUN  = 2,
    TEMPO_EV_PARTICIPANT_STOP = 3,
    TEMPO_EV_TIME_GRANT       = 4,
    TEMPO_EV_SYNC_POINT       = 5,
    TEMPO_EV_MICROSTEP        = 6,
    TEMPO_EV_PORT_WRITE       = 7,
    TEMPO_EV_PORT_READ        = 8,
    TEMPO_EV_ROUTE_DELIVER    = 9,
    TEMPO_EV_MESSAGE_SEND     = 10,
    TEMPO_EV_MESSAGE_RECEIVE  = 11,
    TEMPO_EV_MMIO_READ        = 12,
    TEMPO_EV_MMIO_WRITE       = 13,
    TEMPO_EV_INTERRUPT        = 14,
    TEMPO_EV_WORLD_GET        = 15,
    TEMPO_EV_WORLD_SET        = 16,
    TEMPO_EV_RESET            = 17,
    TEMPO_EV_USER_EVENT       = 18,
    TEMPO_EV__CORE_COUNT      = 19
} tempo_event_type_t;

/* Flags. */
#define TEMPO_EVF_OBSERVE     (1u << 0)  /* recorded, execution continues     */
#define TEMPO_EVF_SYNCHRONIZE (1u << 1)  /* returned control to the scheduler */
#define TEMPO_EVF_HAS_CAUSE   (1u << 2)  /* `cause` names a parent sequence   */

/* Wire header, exactly as in the design doc (S20). */
typedef struct tempo_event_header {
    uint64_t logical_time;
    uint64_t sequence;
    uint32_t participant_id;
    uint16_t type;
    uint16_t flags;
} tempo_event_header_t;

/* One trace record: a 24-byte header plus a fixed 24-byte payload, giving a
 * 48-byte record. Fixed size keeps the ring buffer index arithmetic free of
 * division and keeps the writer allocation-free (S22).
 *
 * `cause` carries the sequence number of the causing event where an adapter
 * can supply one, which is what makes cross-participant causal chains
 * reconstructable offline (S23). */
typedef struct tempo_event {
    tempo_event_header_t hdr;
    uint32_t             microstep;
    uint32_t             arg0;   /* slot / connection / port index    */
    uint64_t             arg1;   /* address, value bits, target time  */
    uint64_t             cause;  /* parent sequence, 0 if unknown     */
} tempo_event_t;

TEMPO_API const char *tempo_event_type_str(uint16_t type);

#ifdef __cplusplus
}
#endif
#endif /* TEMPO_EVENT_H */
