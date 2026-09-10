"""Read-only view of a compiled federation plan.

The plan is what the scheduler actually runs: every name has already become
an index (S13). This wrapper exists so an experiment can inspect what the
compiler decided -- routing, components, execution order -- before or instead
of running anything.
"""

from __future__ import annotations

from dataclasses import dataclass

from ._ffi import ffi, format_duration, lib
from .types import PortDir, PortType


@dataclass(frozen=True)
class PlanPort:
    name: str
    participant: str
    participant_id: int
    local_index: int
    global_index: int
    type: PortType
    max_bytes: int
    dir: PortDir

    @property
    def path(self) -> str:
        return f"{self.participant}.{self.name}"


@dataclass(frozen=True)
class PlanRoute:
    connection_id: int
    source: str
    dest: str
    src_global: int
    dst_global: int
    src_participant: int
    dst_participant: int
    type: PortType
    delay_ns: int

    @property
    def delay(self) -> str:
        return format_duration(self.delay_ns)

    @property
    def same_time(self) -> bool:
        """True for a zero-delay connection, which needs microsteps (S17)."""
        return self.delay_ns == 0

    def __str__(self) -> str:
        return f"{self.source} -> {self.dest} (delay {self.delay})"


@dataclass(frozen=True)
class PlanParticipant:
    id: int
    name: str
    type: str
    n_inputs: int
    n_outputs: int
    component: int
    cyclic: bool


class Plan:
    """A compiled plan. Owns the native handle."""

    def __init__(self, handle) -> None:
        self._plan = handle

    def close(self) -> None:
        if self._plan is not None:
            lib.tempo_plan_free(self._plan)
            self._plan = None

    def __del__(self) -> None:  # pragma: no cover - best effort
        self.close()

    def _check(self):
        if self._plan is None:
            raise RuntimeError("this plan has been freed")
        return self._plan

    # --- settings --------------------------------------------------------

    @property
    def policy(self) -> str:
        return ffi.string(lib.tempo_plan_policy(self._check())).decode()

    @property
    def end_time(self) -> int:
        return int(lib.tempo_plan_end_time(self._check()))

    @property
    def quantum(self) -> int:
        return int(lib.tempo_plan_quantum(self._check()))

    @property
    def max_microsteps(self) -> int:
        return int(lib.tempo_plan_max_microsteps(self._check()))

    # --- structure -------------------------------------------------------

    @property
    def participants(self) -> list[PlanParticipant]:
        plan = self._check()
        out = []
        for pid in range(lib.tempo_plan_n_participants(plan)):
            comp = int(lib.tempo_plan_component_of(plan, pid))
            out.append(
                PlanParticipant(
                    id=pid,
                    name=ffi.string(lib.tempo_plan_participant_name(plan, pid)).decode(),
                    type=ffi.string(lib.tempo_plan_participant_type(plan, pid)).decode(),
                    n_inputs=int(lib.tempo_plan_n_inputs(plan, pid)),
                    n_outputs=int(lib.tempo_plan_n_outputs(plan, pid)),
                    component=comp,
                    cyclic=bool(lib.tempo_plan_component_is_cyclic(plan, comp)),
                )
            )
        return out

    def _port(self, dir: PortDir, index: int) -> PlanPort:
        plan = self._check()
        desc = lib.tempo_plan_port(plan, int(dir), index)
        if desc == ffi.NULL:
            raise IndexError(f"no {dir.name.lower()} slot {index}")
        return self._port_from_desc(desc)

    def _port_from_desc(self, desc) -> PlanPort:
        plan = self._check()
        name = ffi.string(
            lib.tempo_plan_participant_name(plan, desc.participant_id)
        ).decode()
        return PlanPort(
            name=ffi.string(desc.name).decode(),
            participant=name,
            participant_id=int(desc.participant_id),
            local_index=int(desc.local_index),
            global_index=int(desc.global_index),
            type=PortType(int(desc.type)),
            max_bytes=int(desc.max_bytes),
            dir=PortDir(int(desc.dir)),
        )

    def find_port(self, dotted: str, dir: PortDir) -> PlanPort:
        """Resolve ``"participant.port"`` to its compiled descriptor."""
        plan = self._check()
        out = ffi.new("tempo_port_desc_t *")
        rc = lib.tempo_plan_find_port(plan, dotted.encode(), int(dir), out)
        if rc != 0:
            raise KeyError(f"no {dir.name.lower()} port named {dotted!r}")
        return self._port_from_desc(out)

    @property
    def inputs(self) -> list[PlanPort]:
        n = lib.tempo_plan_n_input_slots(self._check())
        return [self._port(PortDir.INPUT, i) for i in range(n)]

    @property
    def outputs(self) -> list[PlanPort]:
        n = lib.tempo_plan_n_output_slots(self._check())
        return [self._port(PortDir.OUTPUT, i) for i in range(n)]

    @property
    def routes(self) -> list[PlanRoute]:
        plan = self._check()
        out = ffi.new("tempo_route_t *")
        routes = []
        for i in range(lib.tempo_plan_n_routes(plan)):
            if lib.tempo_plan_route(plan, i, out) != 0:
                continue
            src = self._port(PortDir.OUTPUT, int(out.src_global))
            dst = self._port(PortDir.INPUT, int(out.dst_global))
            routes.append(
                PlanRoute(
                    connection_id=int(out.connection_id),
                    source=src.path,
                    dest=dst.path,
                    src_global=int(out.src_global),
                    dst_global=int(out.dst_global),
                    src_participant=int(out.src_participant),
                    dst_participant=int(out.dst_participant),
                    type=PortType(int(out.type)),
                    delay_ns=int(out.delay),
                )
            )
        return routes

    @property
    def execution_order(self) -> list[str]:
        """Participant names in the order the scheduler advances them."""
        plan = self._check()
        n = lib.tempo_plan_n_participants(plan)
        return [
            ffi.string(
                lib.tempo_plan_participant_name(plan, lib.tempo_plan_order_at(plan, i))
            ).decode()
            for i in range(n)
        ]

    @property
    def components(self) -> list[list[str]]:
        """Strongly connected components of the same-time graph (S16)."""
        plan = self._check()
        n_comp = lib.tempo_plan_n_components(plan)
        groups: list[list[str]] = [[] for _ in range(n_comp)]
        for p in self.participants:
            groups[p.component].append(p.name)
        return groups

    @property
    def cyclic_components(self) -> list[list[str]]:
        """Only the components that actually need same-time iteration."""
        plan = self._check()
        return [
            group
            for comp, group in enumerate(self.components)
            if lib.tempo_plan_component_is_cyclic(plan, comp)
        ]

    def dependents(self, participant_id: int) -> list[int]:
        """Participants this one can affect, over every connection (S15)."""
        plan = self._check()
        n = lib.tempo_plan_n_successors(plan, participant_id)
        return [int(lib.tempo_plan_successor(plan, participant_id, i)) for i in range(n)]

    def describe(self) -> str:
        """The compiler's own account of what it produced."""
        plan = self._check()
        size = 4096
        while size <= 1 << 20:
            buf = ffi.new("char[]", size)
            rc = lib.tempo_plan_describe(plan, buf, size)
            text = ffi.string(buf).decode()
            if rc == 0:
                return text
            size *= 4
        return text  # pragma: no cover - truncated but still useful

    def __repr__(self) -> str:
        if self._plan is None:
            return "<Plan (freed)>"
        return (
            f"<Plan {len(self.participants)} participants, "
            f"{lib.tempo_plan_n_routes(self._plan)} connections, "
            f"{lib.tempo_plan_n_components(self._plan)} components>"
        )
