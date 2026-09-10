#include "tempo/base.h"
#include "tempo/event.h"
#include "tempo/participant.h"
#include "tempo/port.h"

#include <string.h>

const char *tempo_version(void)
{
    return "0.1.0";
}

const char *tempo_status_str(int status)
{
    switch (status) {
    case TEMPO_OK:                  return "ok";
    case TEMPO_ERR_INVALID_ARG:     return "invalid argument";
    case TEMPO_ERR_NO_MEMORY:       return "out of memory";
    case TEMPO_ERR_NOT_FOUND:       return "not found";
    case TEMPO_ERR_DUPLICATE:       return "duplicate name";
    case TEMPO_ERR_TYPE_MISMATCH:   return "port type mismatch";
    case TEMPO_ERR_DIRECTION:       return "port direction mismatch";
    case TEMPO_ERR_UNSUPPORTED:     return "unsupported capability";
    case TEMPO_ERR_NOT_BOUND:       return "participant not bound";
    case TEMPO_ERR_STATE:           return "invalid state";
    case TEMPO_ERR_TIME_REGRESSION: return "logical time regression";
    case TEMPO_ERR_NON_CONVERGENT:  return "same-time iteration did not converge";
    case TEMPO_ERR_PARTICIPANT:     return "participant reported an error";
    case TEMPO_ERR_CYCLE:           return "unsupported dependency cycle";
    case TEMPO_ERR_CONFIG:          return "invalid configuration";
    case TEMPO_ERR_OVERFLOW:        return "overflow";
    case TEMPO_ERR_IO:              return "i/o error";
    default:                        return "unknown error";
    }
}

const char *tempo_stop_reason_str(tempo_stop_reason_t r)
{
    switch (r) {
    case TEMPO_TARGET_REACHED: return "target_reached";
    case TEMPO_EVENT:          return "event";
    case TEMPO_BLOCKED:        return "blocked";
    case TEMPO_FINISHED:       return "finished";
    case TEMPO_ERROR:          return "error";
    default:                   return "unknown";
    }
}

const char *tempo_type_str(tempo_type_t t)
{
    switch (t) {
    case TEMPO_TYPE_BOOL:    return "bool";
    case TEMPO_TYPE_INT64:   return "int64";
    case TEMPO_TYPE_FLOAT32: return "float32";
    case TEMPO_TYPE_FLOAT64: return "float64";
    case TEMPO_TYPE_BYTES:   return "bytes";
    default:                 return "invalid";
    }
}

int tempo_type_parse(const char *name, tempo_type_t *out)
{
    if (!name || !out)
        return TEMPO_ERR_INVALID_ARG;
    if (!strcmp(name, "bool"))                            *out = TEMPO_TYPE_BOOL;
    else if (!strcmp(name, "int64") || !strcmp(name, "int")) *out = TEMPO_TYPE_INT64;
    else if (!strcmp(name, "float32"))                    *out = TEMPO_TYPE_FLOAT32;
    else if (!strcmp(name, "float64") || !strcmp(name, "float")) *out = TEMPO_TYPE_FLOAT64;
    else if (!strcmp(name, "bytes"))                      *out = TEMPO_TYPE_BYTES;
    else return TEMPO_ERR_NOT_FOUND;
    return TEMPO_OK;
}

uint32_t tempo_slot_set_bytes(tempo_slot_t *s, const void *data, uint32_t len)
{
    if (!s || !s->bytes)
        return 0;
    if (len > s->capacity)
        len = s->capacity;
    if (len)
        memcpy(s->bytes, data, len);
    s->len = len;
    return len;
}

static const char *const core_event_names[TEMPO_EV__CORE_COUNT] = {
    "sim_start", "sim_end", "participant_run", "participant_stop",
    "time_grant", "sync_point", "microstep", "port_write", "port_read",
    "route_deliver", "message_send", "message_receive", "mmio_read",
    "mmio_write", "interrupt", "world_get", "world_set", "reset",
    "user_event"
};

const char *tempo_event_type_str(uint16_t type)
{
    if (type < TEMPO_EV__CORE_COUNT)
        return core_event_names[type];
    switch (TEMPO_EV_NAMESPACE(type)) {
    case TEMPO_EV_NS_FASTDYN: return "fastdyn";
    case TEMPO_EV_NS_FMI:     return "fmi";
    case TEMPO_EV_NS_WORLD:   return "world";
    default:
        return (type >= TEMPO_EV_NS_USER) ? "user" : "unknown";
    }
}
