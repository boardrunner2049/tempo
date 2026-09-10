"""The Python control plane.

An experiment is declared here, compiled once into a native plan, and then
handed to the native scheduler. Python does setup, inspection, and analysis;
it is not in the scheduler's loop (Rule 2).

    from tempo import Simulation, Mock

    sim = Simulation(end_time="10ms", quantum="1ms")
    flight = sim.add(Mock("flight", script="3ms:emit_sync 0 0.7",
                          outputs={"motor_command": "float64"}))
    world = sim.add(Mock("world", echo=True, gain=0.5,
                         inputs={"motor_command": "float64"},
                         outputs={"gyro": "float64"}))
    sim.connect(flight["motor_command"], world["motor_command"])
    sim.run()
"""

from __future__ import annotations

from pathlib import Path
from typing import Iterable, Sequence

from ._ffi import ffi, format_duration, lib, parse_duration
from .errors import CompileError, SchedulerError, TempoError, check
from .participants import Participant, PortRef
from .plan import Plan
from .trace import Trace
from .types import ID_ANY, SUB_BREAK, SUB_OBSERVE, EventType, PortDir, PortType


class Simulation:
    """A federation: participants, connections, and a scheduling policy.

    Parameters
    ----------
    end_time:
        When the run stops. Required -- a federation with no end is a
        configuration error, not an infinite run.
    quantum:
        The conservative policy's communication window.
    default_delay:
        Delay applied to a connection that does not declare one. Defaults to
        the quantum, which is what makes fixed-window conservative scheduling
        sound; see ``docs/scheduling.md``.
    max_microsteps:
        Bound on same-time iteration, used to catch a zero-delay cycle that
        never settles (S17).
    trace:
        ``False`` for no trace, ``True`` for a default-sized buffer, or a
        record count.
    """

    def __init__(
        self,
        end_time: "int | str",
        *,
        quantum: "int | str" = "100us",
        policy: str = "conservative",
        default_delay: "int | str | None" = None,
        max_microsteps: int = 100,
        trace: "bool | int" = False,
    ) -> None:
        self.end_time = parse_duration(end_time)
        if self.end_time == 0:
            raise ValueError("end_time must be greater than zero")
        self.quantum = parse_duration(quantum)
        if self.quantum == 0:
            raise ValueError("quantum must be greater than zero")
        self.policy = policy
        self.default_delay = (
            None if default_delay is None else parse_duration(default_delay)
        )
        self.max_microsteps = int(max_microsteps)

        self._participants: list[Participant] = []
        self._by_name: dict[str, Participant] = {}
        self._connections: list[tuple[str, str, int | None]] = []
        self._subscriptions: list[dict] = []

        self._plan: Plan | None = None
        self._sched = None
        self._trace: Trace | None = None
        self._trace_request = trace
        self._instantiated: list[Participant] = []
        self._caps_keepalive: list = []

    # --- declaration -----------------------------------------------------

    def add(self, participant: Participant) -> Participant:
        """Add a participant and return it, so it can be used inline."""
        self._require_unstarted("add a participant")
        if not isinstance(participant, Participant):
            raise TypeError(
                f"expected a Participant, got {type(participant).__name__}"
            )
        if participant.name in self._by_name:
            raise ValueError(f"there is already a participant called {participant.name!r}")
        self._participants.append(participant)
        self._by_name[participant.name] = participant
        return participant

    def __getitem__(self, name: str) -> Participant:
        try:
            return self._by_name[name]
        except KeyError:
            known = ", ".join(sorted(self._by_name)) or "none"
            raise KeyError(f"no participant {name!r}; there are: {known}") from None

    def connect(
        self,
        source: "PortRef | str",
        dest: "PortRef | str",
        *,
        delay: "int | str | None" = None,
    ) -> None:
        """Connect an output to an input.

        A *delay* of ``0`` makes this a same-time connection, which requires
        the destination to be able to iterate within one logical instant and
        is resolved by microsteps. Anything else lets both ends run freely
        between communication points, which is the case worth being fast.
        """
        self._require_unstarted("add a connection")
        src = self._resolve(source, PortDir.OUTPUT)
        dst = self._resolve(dest, PortDir.INPUT)
        delay_ns = None if delay is None else parse_duration(delay)
        self._connections.append((src, dst, delay_ns))

    def subscribe(
        self,
        *,
        participant: "str | int | None" = None,
        event: "EventType | str | int | Iterable | None" = None,
        arg_mask: int = 0,
        arg_value: int = 0,
        address: int | None = None,
        break_on: bool = False,
    ) -> None:
        """Say what an analysis wants to see (S24).

        Subscriptions are compiled into native filters, so asking only for
        MMIO writes to one address does not mean generating and discarding
        everything else (S21).

        ``break_on=True`` makes the event *intervene* rather than merely be
        observed: it stops the participant and goes through the slow path
        (S25). The two are kept distinct on purpose.
        """
        self._require_unstarted("add a subscription")
        if address is not None:
            if arg_mask or arg_value:
                raise ValueError("pass either address= or arg_mask/arg_value, not both")
            arg_mask, arg_value = 0xFFFFFFFFFFFFFFFF, address

        events: Sequence = [None] if event is None else list(_as_sequence(event))
        for one in events:
            self._subscriptions.append(
                {
                    "participant": participant,
                    "event": None if one is None else EventType.parse(one),
                    "arg_mask": arg_mask,
                    "arg_value": arg_value,
                    "flags": SUB_OBSERVE | (SUB_BREAK if break_on else 0),
                }
            )

    def _resolve(self, ref: "PortRef | str", dir: PortDir) -> str:
        if isinstance(ref, PortRef):
            name, port = ref.participant_name, ref.port
        else:
            text = str(ref)
            if "." not in text:
                raise ValueError(
                    f"{text!r} is not a port reference; use 'participant.port' "
                    "or participant['port']"
                )
            name, port = text.split(".", 1)

        if name not in self._by_name:
            known = ", ".join(sorted(self._by_name)) or "none"
            raise ValueError(f"no participant {name!r}; there are: {known}")
        owner = self._by_name[name]
        table = owner.outputs if dir is PortDir.OUTPUT else owner.inputs
        if port not in table:
            which = "output" if dir is PortDir.OUTPUT else "input"
            available = ", ".join(sorted(table)) or "none"
            raise ValueError(
                f"{name!r} has no {which} port {port!r}; its {which}s are: {available}"
            )
        return f"{name}.{port}"

    # --- compilation -----------------------------------------------------

    def compile(self) -> Plan:
        """Compile the federation, resolving every name into an index.

        Safe to call before running: a configuration mistake is reported here
        with the location that caused it (S14).
        """
        if self._plan is not None:
            return self._plan
        if not self._participants:
            raise CompileErrorLike("federation has no participants", "participants")

        spec = lib.tempo_spec_new()
        if spec == ffi.NULL:
            raise MemoryError("could not allocate a federation spec")
        try:
            check(lib.tempo_spec_set_end_time(spec, self.end_time), "end_time")
            check(lib.tempo_spec_set_quantum(spec, self.quantum), "quantum")
            check(lib.tempo_spec_set_policy(spec, self.policy.encode()), "policy")
            check(
                lib.tempo_spec_set_max_microsteps(spec, self.max_microsteps),
                "max_microsteps",
            )
            if self.default_delay is not None:
                check(
                    lib.tempo_spec_set_default_delay(spec, self.default_delay),
                    "default_delay",
                )
            capacity = self._trace_capacity()
            if capacity:
                check(lib.tempo_spec_set_trace_capacity(spec, capacity), "trace capacity")

            ids: dict[str, int] = {}
            for participant in self._participants:
                out_id = ffi.new("uint32_t *")
                check(
                    lib.tempo_spec_add_participant(
                        spec,
                        participant.name.encode(),
                        participant.kind.encode(),
                        out_id,
                    ),
                    f"adding participant {participant.name!r}",
                )
                pid = int(out_id[0])
                ids[participant.name] = pid
                for port in list(participant.inputs.values()) + list(
                    participant.outputs.values()
                ):
                    check(
                        lib.tempo_spec_add_port(
                            spec,
                            pid,
                            port.name.encode(),
                            int(port.dir),
                            int(port.type),
                            port.max_bytes,
                            ffi.NULL,
                        ),
                        f"adding port {participant.name}.{port.name}",
                    )

            never = 0xFFFFFFFFFFFFFFFF
            for src, dst, delay in self._connections:
                check(
                    lib.tempo_spec_add_connection(
                        spec,
                        src.encode(),
                        dst.encode(),
                        never if delay is None else delay,
                        ffi.NULL,
                    ),
                    f"connecting {src} -> {dst}",
                )

            for sub in self._subscriptions:
                cdata = ffi.new("tempo_subscription_t *")
                who = sub["participant"]
                if who is None:
                    cdata.participant_id = ID_ANY
                elif isinstance(who, int):
                    cdata.participant_id = who
                else:
                    if who not in ids:
                        raise ValueError(f"no participant {who!r} to subscribe to")
                    cdata.participant_id = ids[who]
                cdata.type = ID_ANY if sub["event"] is None else sub["event"]
                cdata.arg_mask = sub["arg_mask"]
                cdata.arg_value = sub["arg_value"]
                cdata.flags = sub["flags"]
                check(lib.tempo_spec_subscribe(spec, cdata), "adding a subscription")

            diag = ffi.new("tempo_diagnostics_t *")
            handle = lib.tempo_compile(spec, diag)
            if handle == ffi.NULL:
                raise CompileError(diag)
            self._plan = Plan(handle)
            return self._plan
        finally:
            lib.tempo_spec_free(spec)

    def _trace_capacity(self) -> int:
        if self._trace_request is False or self._trace_request is None:
            return 0
        if self._trace_request is True:
            return 1 << 16
        return int(self._trace_request)

    def explain(self) -> str:
        """The compiler's account of the plan: routing, components, order."""
        return self.compile().describe()

    @property
    def plan(self) -> Plan:
        return self.compile()

    # --- running ---------------------------------------------------------

    def start(self) -> None:
        """Compile, create the participants, bind them, and initialize."""
        if self._sched is not None:
            return
        plan = self.compile()
        sched = lib.tempo_scheduler_new(plan._plan)
        if sched == ffi.NULL:
            raise MemoryError("could not allocate a scheduler")
        self._sched = sched

        capacity = self._trace_capacity()
        if capacity:
            self._trace = Trace(capacity=capacity)
            check(
                lib.tempo_scheduler_set_trace(sched, self._trace.handle),
                "attaching the trace",
            )

        try:
            for pid, participant in enumerate(self._participants):
                vtable, handle, caps = participant.instantiate()
                self._instantiated.append(participant)
                if caps is not None:
                    self._caps_keepalive.append(caps)
                rc = lib.tempo_scheduler_bind(
                    sched, pid, vtable, handle, ffi.NULL if caps is None else caps
                )
                if rc < 0:
                    raise SchedulerError(self._error_text(f"binding {participant.name!r}"), rc)
            rc = lib.tempo_scheduler_init(sched)
            if rc < 0:
                raise SchedulerError(self._error_text("initializing"), rc)
        except Exception:
            self.close()
            raise

    def _error_text(self, what: str) -> str:
        detail = ffi.string(lib.tempo_scheduler_error(self._sched)).decode()
        return f"{what}: {detail}" if detail else what

    def run(self, until: "int | str | None" = None) -> None:
        """Run to *until*, or to the federation's end time."""
        self.start()
        if until is None:
            rc = lib.tempo_scheduler_run(self._sched)
        else:
            rc = lib.tempo_scheduler_run_until(self._sched, parse_duration(until))
        if rc < 0:
            raise SchedulerError(self._error_text("run failed"), rc)

    def step(self) -> bool:
        """Execute one communication window.

        Returns ``True`` while there is more to do, ``False`` when the run is
        complete. Useful for stepping an experiment by hand.
        """
        self.start()
        rc = lib.tempo_scheduler_step(self._sched)
        if rc < 0:
            raise SchedulerError(self._error_text("step failed"), rc)
        return rc == 0

    def reset(self) -> None:
        """Return every participant to its initial state and time to zero.

        Requires every participant to advertise ``RESET``.
        """
        if self._sched is None:
            return
        rc = lib.tempo_scheduler_reset(self._sched)
        if rc < 0:
            raise SchedulerError(self._error_text("reset failed"), rc)

    # --- state -----------------------------------------------------------

    @property
    def now(self) -> int:
        """Current logical time in nanoseconds."""
        if self._sched is None:
            return 0
        return int(lib.tempo_scheduler_now(self._sched).time)

    @property
    def tag(self) -> tuple[int, int]:
        """Current ``(time, microstep)`` tag (S17)."""
        if self._sched is None:
            return (0, 0)
        now = lib.tempo_scheduler_now(self._sched)
        return (int(now.time), int(now.microstep))

    @property
    def finished(self) -> bool:
        if self._sched is None:
            return False
        return bool(lib.tempo_scheduler_finished(self._sched))

    @property
    def trace(self) -> Trace | None:
        return self._trace

    def participant_time(self, participant: "str | int") -> int:
        """One participant's own logical time, which may be ahead of ``now``."""
        if self._sched is None:
            return 0
        pid = participant if isinstance(participant, int) else self._index(participant)
        return int(lib.tempo_scheduler_participant_time(self._sched, pid))

    def _index(self, name: str) -> int:
        for i, participant in enumerate(self._participants):
            if participant.name == name:
                return i
        raise KeyError(f"no participant {name!r}")

    @property
    def stats(self) -> dict:
        """What the scheduler actually did."""
        if self._sched is None:
            return {}
        s = lib.tempo_scheduler_stats(self._sched)
        return {
            "windows": int(s.windows),
            "advance_calls": int(s.advance_calls),
            "skipped_advances": int(s.skipped_advances),
            "early_returns": int(s.early_returns),
            "deliveries": int(s.deliveries),
            "microsteps": int(s.microsteps),
            "max_microstep": int(s.max_microstep),
        }

    def value(self, ref: "PortRef | str") -> object:
        """Read the current value of a port."""
        if self._sched is None:
            raise RuntimeError("the simulation has not started")
        text = str(ref)
        plan = self.compile()
        for dir, slots in (
            (PortDir.OUTPUT, lib.tempo_scheduler_output_slots(self._sched)),
            (PortDir.INPUT, lib.tempo_scheduler_input_slots(self._sched)),
        ):
            try:
                desc = plan.find_port(text, dir)
            except KeyError:
                continue
            return _read_slot(slots[desc.global_index], desc.type)
        raise KeyError(f"no port named {text!r}")

    # --- lifetime --------------------------------------------------------

    def close(self) -> None:
        if self._sched is not None:
            lib.tempo_scheduler_free(self._sched)
            self._sched = None
        for participant in self._instantiated:
            participant.release()
        self._instantiated.clear()
        self._caps_keepalive.clear()
        if self._trace is not None:
            self._trace.close()
            self._trace = None
        if self._plan is not None:
            self._plan.close()
            self._plan = None

    def __enter__(self) -> "Simulation":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def __del__(self) -> None:  # pragma: no cover - best effort
        try:
            self.close()
        except Exception:
            pass

    def _require_unstarted(self, what: str) -> None:
        if self._sched is not None:
            raise RuntimeError(f"cannot {what} after the simulation has started")
        if self._plan is not None:
            # The plan is immutable, so drop it and recompile on demand.
            self._plan.close()
            self._plan = None

    def __repr__(self) -> str:
        return (
            f"<Simulation {len(self._participants)} participants, "
            f"{len(self._connections)} connections, "
            f"end={format_duration(self.end_time)} quantum={format_duration(self.quantum)}>"
        )

    # --- configuration files ---------------------------------------------

    @classmethod
    def from_toml(cls, path: "str | Path") -> "Simulation":
        """Build a simulation from a TOML experiment file (S33)."""
        from .config import load_toml

        return load_toml(path)


def _read_slot(slot, port_type: PortType):
    if port_type is PortType.BOOL:
        return bool(slot.scalar.b)
    if port_type is PortType.INT64:
        return int(slot.scalar.i64)
    if port_type is PortType.FLOAT32:
        return float(slot.scalar.f32)
    if port_type is PortType.FLOAT64:
        return float(slot.scalar.f64)
    if slot.bytes == ffi.NULL or slot.len == 0:
        return b""
    return bytes(ffi.buffer(slot.bytes, slot.len))


def _as_sequence(value):
    if isinstance(value, (str, int)) or not hasattr(value, "__iter__"):
        return [value]
    return value


class CompileErrorLike(TempoError):
    """A configuration problem caught in Python before reaching the compiler."""

    def __init__(self, message: str, location: str) -> None:
        self.location = location
        self.detail = message
        super().__init__(f"{message} at {location}")
