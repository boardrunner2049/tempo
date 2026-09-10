"""cffi ABI-mode bindings to libtempo.

ABI mode is deliberate. Python is the control plane and must never be in the
scheduler's hot path (Rule 2), so the binding only needs to reach a handful of
setup and query functions -- and in exchange, installing the Python side needs
no compiler and the boundary between the two languages stays a hard one.

The declarations below are hand-maintained against include/tempo/*.h. Struct
layouts are part of that contract; ``tests/test_python_layout.py`` checks the
ones that matter against the C library at run time.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

from cffi import FFI

ffi = FFI()

ffi.cdef(
    """
    /* --- base ------------------------------------------------------------ */
    typedef enum {
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

    const char *tempo_status_str(int status);
    const char *tempo_version(void);

    /* --- time ------------------------------------------------------------ */
    typedef uint64_t tempo_time_t;
    typedef uint32_t tempo_microstep_t;

    typedef struct { uint64_t time; uint32_t microstep; uint32_t _pad; } tempo_tag_t;

    int tempo_time_parse(const char *text, uint64_t *out);
    const char *tempo_time_format(uint64_t t, char *buf, size_t len);

    /* --- ports ----------------------------------------------------------- */
    typedef enum {
        TEMPO_TYPE_BOOL = 0, TEMPO_TYPE_INT64 = 1, TEMPO_TYPE_FLOAT32 = 2,
        TEMPO_TYPE_FLOAT64 = 3, TEMPO_TYPE_BYTES = 4
    } tempo_type_t;

    typedef enum { TEMPO_PORT_INPUT = 0, TEMPO_PORT_OUTPUT = 1 } tempo_port_dir_t;

    typedef union {
        _Bool    b;
        int64_t  i64;
        float    f32;
        double   f64;
        uint64_t raw;
    } tempo_scalar_t;

    typedef struct {
        uint64_t       generation;
        uint32_t       len;
        uint32_t       capacity;
        tempo_scalar_t scalar;
        uint8_t       *bytes;
    } tempo_slot_t;

    typedef struct {
        const char      *name;
        uint32_t         participant_id;
        uint32_t         local_index;
        uint32_t         global_index;
        tempo_type_t     type;
        uint32_t         max_bytes;
        tempo_port_dir_t dir;
    } tempo_port_desc_t;

    const char *tempo_type_str(tempo_type_t t);
    int tempo_type_parse(const char *name, tempo_type_t *out);
    uint32_t tempo_slot_set_bytes(tempo_slot_t *s, const void *data, uint32_t len);

    /* --- participants ---------------------------------------------------- */
    typedef enum {
        TEMPO_TARGET_REACHED = 0, TEMPO_EVENT = 1, TEMPO_BLOCKED = 2,
        TEMPO_FINISHED = 3, TEMPO_ERROR = 4
    } tempo_stop_reason_t;

    typedef struct {
        uint32_t flags;
        uint64_t preferred_step;
        uint64_t time_resolution;
    } tempo_capabilities_t;

    /* Opaque here: building a participant in Python would put Python on the
     * hot path, which the design forbids. Adapters are native. */
    typedef struct tempo_participant_vtable tempo_participant_vtable_t;

    const char *tempo_stop_reason_str(tempo_stop_reason_t r);

    /* --- events and traces ----------------------------------------------- */
    typedef struct {
        uint64_t logical_time;
        uint64_t sequence;
        uint32_t participant_id;
        uint16_t type;
        uint16_t flags;
    } tempo_event_header_t;

    typedef struct {
        tempo_event_header_t hdr;
        uint32_t             microstep;
        uint32_t             arg0;
        uint64_t             arg1;
        uint64_t             cause;
    } tempo_event_t;

    typedef struct tempo_trace tempo_trace_t;

    typedef struct { size_t capacity; _Bool overwrite; } tempo_trace_config_t;

    tempo_trace_t *tempo_trace_new(const tempo_trace_config_t *cfg);
    void tempo_trace_free(tempo_trace_t *tr);
    void tempo_trace_set_enabled(tempo_trace_t *tr, _Bool enabled);
    void tempo_trace_set_masks(tempo_trace_t *tr, uint64_t core_mask, uint32_t ns_mask);
    _Bool tempo_trace_wants(const tempo_trace_t *tr, uint16_t type, uint32_t pid);
    void tempo_trace_emit(tempo_trace_t *tr, uint64_t logical_time, uint32_t microstep,
                          uint32_t participant_id, uint16_t type, uint16_t flags,
                          uint32_t arg0, uint64_t arg1, uint64_t cause);
    uint64_t tempo_trace_count(const tempo_trace_t *tr);
    uint64_t tempo_trace_total(const tempo_trace_t *tr);
    uint64_t tempo_trace_dropped(const tempo_trace_t *tr);
    const tempo_event_t *tempo_trace_at(const tempo_trace_t *tr, uint64_t i);
    void tempo_trace_clear(tempo_trace_t *tr);
    int tempo_trace_write_file(const tempo_trace_t *tr, const char *path);

    const char *tempo_event_type_str(uint16_t type);

    /* --- analysis -------------------------------------------------------- */
    typedef struct {
        uint32_t participant_id;
        uint32_t type;
        uint64_t arg_mask;
        uint64_t arg_value;
        uint32_t flags;
        uint32_t _pad;
    } tempo_subscription_t;

    /* --- federation ------------------------------------------------------ */
    typedef struct tempo_federation_spec tempo_federation_spec_t;
    typedef struct tempo_plan tempo_plan_t;

    typedef struct {
        int  status;
        char message[256];
        char location[128];
    } tempo_diagnostics_t;

    typedef struct {
        uint32_t src_global;
        uint32_t dst_global;
        uint64_t delay;
        uint32_t connection_id;
        uint32_t src_participant;
        uint32_t dst_participant;
        uint16_t type;
        uint16_t max_bytes;
    } tempo_route_t;

    tempo_federation_spec_t *tempo_spec_new(void);
    void tempo_spec_free(tempo_federation_spec_t *spec);
    int tempo_spec_set_end_time(tempo_federation_spec_t *spec, uint64_t t);
    int tempo_spec_set_policy(tempo_federation_spec_t *spec, const char *policy);
    int tempo_spec_set_quantum(tempo_federation_spec_t *spec, uint64_t q);
    int tempo_spec_set_default_delay(tempo_federation_spec_t *spec, uint64_t d);
    int tempo_spec_set_max_microsteps(tempo_federation_spec_t *spec, uint32_t n);
    int tempo_spec_set_trace_capacity(tempo_federation_spec_t *spec, size_t records);
    int tempo_spec_add_participant(tempo_federation_spec_t *spec, const char *name,
                                   const char *type, uint32_t *out_id);
    int tempo_spec_add_port(tempo_federation_spec_t *spec, uint32_t participant_id,
                             const char *name, tempo_port_dir_t dir, tempo_type_t type,
                             uint32_t max_bytes, uint32_t *out_local_index);
    int tempo_spec_add_connection(tempo_federation_spec_t *spec, const char *from,
                                   const char *to, uint64_t delay, uint32_t *out_id);
    int tempo_spec_subscribe(tempo_federation_spec_t *spec,
                              const tempo_subscription_t *sub);
    uint32_t tempo_spec_find_participant(const tempo_federation_spec_t *spec,
                                          const char *name);

    tempo_plan_t *tempo_compile(const tempo_federation_spec_t *spec,
                                 tempo_diagnostics_t *diag);
    void tempo_plan_free(tempo_plan_t *plan);

    uint32_t tempo_plan_n_participants(const tempo_plan_t *plan);
    const char *tempo_plan_participant_name(const tempo_plan_t *plan, uint32_t id);
    const char *tempo_plan_participant_type(const tempo_plan_t *plan, uint32_t id);
    uint32_t tempo_plan_n_inputs(const tempo_plan_t *plan, uint32_t id);
    uint32_t tempo_plan_n_outputs(const tempo_plan_t *plan, uint32_t id);
    uint32_t tempo_plan_n_input_slots(const tempo_plan_t *plan);
    uint32_t tempo_plan_n_output_slots(const tempo_plan_t *plan);
    uint32_t tempo_plan_n_routes(const tempo_plan_t *plan);
    int tempo_plan_route(const tempo_plan_t *plan, uint32_t i, tempo_route_t *out);
    uint64_t tempo_plan_end_time(const tempo_plan_t *plan);
    uint64_t tempo_plan_quantum(const tempo_plan_t *plan);
    uint32_t tempo_plan_max_microsteps(const tempo_plan_t *plan);
    const char *tempo_plan_policy(const tempo_plan_t *plan);
    size_t tempo_plan_trace_capacity(const tempo_plan_t *plan);
    int tempo_plan_find_port(const tempo_plan_t *plan, const char *dotted,
                              tempo_port_dir_t dir, tempo_port_desc_t *out);
    const tempo_port_desc_t *tempo_plan_port(const tempo_plan_t *plan,
                                              tempo_port_dir_t dir, uint32_t index);
    uint32_t tempo_plan_n_successors(const tempo_plan_t *plan, uint32_t id);
    uint32_t tempo_plan_successor(const tempo_plan_t *plan, uint32_t id, uint32_t i);
    uint32_t tempo_plan_n_components(const tempo_plan_t *plan);
    uint32_t tempo_plan_component_of(const tempo_plan_t *plan, uint32_t id);
    uint32_t tempo_plan_component_size(const tempo_plan_t *plan, uint32_t comp);
    _Bool tempo_plan_component_is_cyclic(const tempo_plan_t *plan, uint32_t comp);
    uint32_t tempo_plan_order_at(const tempo_plan_t *plan, uint32_t i);
    uint32_t tempo_plan_n_subscriptions(const tempo_plan_t *plan);
    int tempo_plan_subscription(const tempo_plan_t *plan, uint32_t i,
                                 tempo_subscription_t *out);
    uint64_t tempo_plan_trace_core_mask(const tempo_plan_t *plan);
    uint32_t tempo_plan_trace_ns_mask(const tempo_plan_t *plan);
    int tempo_plan_describe(const tempo_plan_t *plan, char *buf, size_t len);

    /* --- scheduler ------------------------------------------------------- */
    typedef struct tempo_scheduler tempo_scheduler_t;

    typedef struct {
        uint64_t windows;
        uint64_t advance_calls;
        uint64_t skipped_advances;
        uint64_t early_returns;
        uint64_t deliveries;
        uint64_t microsteps;
        uint64_t max_microstep;
    } tempo_scheduler_stats_t;

    tempo_scheduler_t *tempo_scheduler_new(const tempo_plan_t *plan);
    void tempo_scheduler_free(tempo_scheduler_t *sched);
    int tempo_scheduler_bind(tempo_scheduler_t *sched, uint32_t participant_id,
                              const tempo_participant_vtable_t *vt, void *self,
                              const tempo_capabilities_t *caps);
    int tempo_scheduler_set_trace(tempo_scheduler_t *sched, tempo_trace_t *tr);
    tempo_trace_t *tempo_scheduler_trace(const tempo_scheduler_t *sched);
    int tempo_scheduler_init(tempo_scheduler_t *sched);
    int tempo_scheduler_step(tempo_scheduler_t *sched);
    int tempo_scheduler_run(tempo_scheduler_t *sched);
    int tempo_scheduler_run_until(tempo_scheduler_t *sched, uint64_t until);
    int tempo_scheduler_reset(tempo_scheduler_t *sched);
    tempo_tag_t tempo_scheduler_now(const tempo_scheduler_t *sched);
    _Bool tempo_scheduler_finished(const tempo_scheduler_t *sched);
    uint64_t tempo_scheduler_participant_time(const tempo_scheduler_t *sched, uint32_t id);
    const char *tempo_scheduler_error(const tempo_scheduler_t *sched);
    tempo_slot_t *tempo_scheduler_input_slots(tempo_scheduler_t *sched);
    tempo_slot_t *tempo_scheduler_output_slots(tempo_scheduler_t *sched);
    tempo_scheduler_stats_t tempo_scheduler_stats(const tempo_scheduler_t *sched);

    /* --- mock adapter ---------------------------------------------------- */
    typedef struct tempo_mock tempo_mock_t;

    tempo_mock_t *tempo_mock_from_script(const char *script, uint64_t max_step,
                                          _Bool echo_inputs, double echo_gain,
                                          const char **err);
    void tempo_mock_free(tempo_mock_t *m);
    const tempo_participant_vtable_t *tempo_mock_vtable(void);
    tempo_capabilities_t tempo_mock_capabilities(const tempo_mock_t *m);
    uint64_t tempo_mock_time(const tempo_mock_t *m);
    uint32_t tempo_mock_advance_count(const tempo_mock_t *m);
    double tempo_mock_last_input(const tempo_mock_t *m, uint32_t local_index);
    """
)

_LIB_NAMES = ("libtempo.so", "libtempo.so.0", "libtempo.dylib", "tempo.dll")


def _candidates() -> list[Path]:
    """Where to look for libtempo, most specific first."""
    out: list[Path] = []

    env = os.environ.get("TEMPO_LIB")
    if env:
        out.append(Path(env))

    here = Path(__file__).resolve()
    # Installed alongside the package (a wheel that vendored the library).
    roots = [here.parent]
    # Running from a source checkout: python/tempo/_ffi.py -> repo root.
    repo = here.parent.parent.parent
    roots += [repo / "build", repo / "builddir", repo]
    for root in roots:
        for name in _LIB_NAMES:
            out.append(root / name)
    return out


def _load():
    tried = []
    for path in _candidates():
        if path.is_file():
            try:
                return ffi.dlopen(str(path))
            except OSError as exc:  # pragma: no cover - platform specific
                tried.append(f"{path}: {exc}")
        else:
            tried.append(f"{path}: not found")

    # Last resort: let the dynamic loader search its usual paths.
    for name in _LIB_NAMES:
        try:
            return ffi.dlopen(name)
        except OSError as exc:
            tried.append(f"{name} (loader path): {exc}")

    detail = "\n  ".join(tried)
    raise ImportError(
        "tempo could not load libtempo.\n"
        "Build it with:\n"
        "  meson setup build && ninja -C build\n"
        "or point TEMPO_LIB at the shared library.\n"
        f"Tried:\n  {detail}"
    )


lib = _load()

__all__ = ["ffi", "lib"]


def version() -> str:
    return ffi.string(lib.tempo_version()).decode()


def status_str(code: int) -> str:
    return ffi.string(lib.tempo_status_str(code)).decode()


def parse_duration(value: "int | float | str") -> int:
    """Parse a duration into nanoseconds.

    Deliberately delegates to the C parser so that Python, TOML, and C all
    agree on what "3.5ms" means -- one implementation, not two.
    """
    if isinstance(value, bool):
        raise TypeError("a duration cannot be a bool")
    if isinstance(value, int):
        if value < 0:
            raise ValueError("a duration cannot be negative")
        return value
    if isinstance(value, float):
        if value < 0:
            raise ValueError("a duration cannot be negative")
        return int(round(value))
    out = ffi.new("uint64_t *")
    if lib.tempo_time_parse(str(value).encode(), out) != 0:
        raise ValueError(
            f"{value!r} is not a duration; expected something like "
            "'100us', '3.5ms', '10s', or a plain number of nanoseconds"
        )
    return int(out[0])


def format_duration(ns: int) -> str:
    buf = ffi.new("char[32]")
    lib.tempo_time_format(ns, buf, 32)
    return ffi.string(buf).decode()


if sys.version_info < (3, 11):  # pragma: no cover
    raise ImportError("tempo requires Python 3.11 or newer (it uses tomllib)")
