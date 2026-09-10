"""Participant declarations for the Python control plane.

A ``Participant`` here is a *description*: a name, a type, and its ports. The
thing that actually executes is native, created when the simulation starts.
Python declares and connects; it never steps anything (Rule 2).
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Iterable, Mapping

from ._ffi import ffi, lib, parse_duration
from .errors import TempoError
from .types import Capability, PortDir, PortType

#: Accepted shapes for a port declaration:
#:   "float64"                     -- a scalar type
#:   ("bytes", 64)                 -- a byte port with a capacity
#:   {"type": "bytes", "max_bytes": 64}
PortSpec = "str | int | PortType | tuple | Mapping"


@dataclass(frozen=True)
class Port:
    name: str
    type: PortType
    dir: PortDir
    max_bytes: int = 0

    def __str__(self) -> str:
        return self.name


@dataclass(frozen=True)
class PortRef:
    """A ``participant.port`` reference, as used by :meth:`Simulation.connect`."""

    participant_name: str
    port: str

    def __str__(self) -> str:
        return f"{self.participant_name}.{self.port}"


def _parse_port(name: str, spec: PortSpec, dir: PortDir) -> Port:
    max_bytes = 0
    if isinstance(spec, Mapping):
        type_value = spec.get("type", "float64")
        max_bytes = int(spec.get("max_bytes", 0))
    elif isinstance(spec, (tuple, list)):
        if not spec:
            raise ValueError(f"port {name!r} has an empty declaration")
        type_value = spec[0]
        if len(spec) > 1:
            max_bytes = int(spec[1])
    else:
        type_value = spec
    port_type = PortType.parse(type_value)
    if port_type is not PortType.BYTES and max_bytes:
        raise ValueError(
            f"port {name!r} is {port_type.name.lower()}, so max_bytes does not apply"
        )
    return Port(name=name, type=port_type, dir=dir, max_bytes=max_bytes)


class Participant:
    """Base class for a participant declaration.

    Subclasses supply :meth:`instantiate`, which creates the native object and
    returns what the scheduler needs to bind it.
    """

    #: Value written into the plan's ``type`` field, for tooling and traces.
    kind = "participant"

    def __init__(
        self,
        name: str,
        *,
        inputs: Mapping[str, PortSpec] | Iterable[str] | None = None,
        outputs: Mapping[str, PortSpec] | Iterable[str] | None = None,
        kind: str | None = None,
    ) -> None:
        if not name or "." in name:
            raise ValueError(
                f"participant name {name!r} must be non-empty and contain no '.', "
                "because names appear in dotted port references"
            )
        self.name = name
        if kind is not None:
            self.kind = kind
        self.inputs: dict[str, Port] = {}
        self.outputs: dict[str, Port] = {}
        for port_name, spec in _as_mapping(inputs).items():
            self.inputs[port_name] = _parse_port(port_name, spec, PortDir.INPUT)
        for port_name, spec in _as_mapping(outputs).items():
            self.outputs[port_name] = _parse_port(port_name, spec, PortDir.OUTPUT)

    def __getitem__(self, port: str) -> PortRef:
        if port not in self.inputs and port not in self.outputs:
            known = sorted(set(self.inputs) | set(self.outputs))
            raise KeyError(
                f"{self.name!r} has no port {port!r}"
                + (f"; it has {', '.join(known)}" if known else " (it has none)")
            )
        return PortRef(self.name, port)

    def __repr__(self) -> str:
        return (
            f"<{type(self).__name__} {self.name!r} "
            f"in={list(self.inputs)} out={list(self.outputs)}>"
        )

    # --- native side -----------------------------------------------------

    def instantiate(self):
        """Create the native participant.

        Returns ``(vtable, handle, capabilities)`` where *vtable* and *handle*
        are the pointers passed to ``tempo_scheduler_bind`` and *capabilities*
        is a ``tempo_capabilities_t *`` (or ``None`` for no optional
        capability).
        """
        raise NotImplementedError(
            f"{type(self).__name__} does not know how to create a native "
            "participant yet. Participants execute in C, so an adapter has to "
            "provide one; see docs/adapters.md."
        )

    def release(self) -> None:
        """Free the native participant, if the subclass allocated one."""


def _as_mapping(value) -> Mapping[str, PortSpec]:
    if value is None:
        return {}
    if isinstance(value, Mapping):
        return dict(value)
    # A bare iterable of names defaults to float64, which is the common case.
    return {str(name): "float64" for name in value}


class Mock(Participant):
    """A scriptable native mock (S37).

    The script is a list of ``<time>:<action>`` steps, separated by ``;``::

        Mock("flight", script="3.5ms:trap; 6ms:emit_sync 0 0.7; 9ms:finish")

    Actions:

    ``trap``
        Stop and hand control back to the scheduler, producing nothing.
    ``emit_sync <slot> <value>``
        Write an output and stop -- a synchronizing event.
    ``emit_observe <slot> <value>``
        Write an output and keep running -- an observable event.
    ``mark <slot> <value>``
        Record a trace event without stopping.
    ``finish``
        Stop for good.

    Parameters
    ----------
    echo:
        Copy each changed input onto the output of the same index, scaled by
        *gain*. This is what lets a mock stand in for a model with feedback,
        and it is what makes the mock capable of same-time iteration.
    max_step:
        Never advance further than this in one call, modelling a simulator
        with a coarse internal step.
    predict:
        Advertise ``PREDICT_NEXT``, so the scheduler can set its horizon at
        the mock's next scripted event instead of trapping out early. Off by
        default, because a QEMU-backed participant generally cannot predict.
    """

    kind = "mock"

    def __init__(
        self,
        name: str,
        *,
        script: str = "",
        inputs=None,
        outputs=None,
        echo: bool = False,
        gain: float = 1.0,
        max_step: "int | str" = 0,
        predict: bool = False,
        extra_capabilities: Capability | int = 0,
    ) -> None:
        super().__init__(name, inputs=inputs, outputs=outputs)
        self.script = script
        self.echo = bool(echo)
        self.gain = float(gain)
        self.max_step = parse_duration(max_step) if max_step else 0
        self.predict = bool(predict)
        self.extra_capabilities = int(extra_capabilities)
        self._handle = None

    def instantiate(self):
        err = ffi.new("const char **")
        handle = lib.tempo_mock_from_script(
            self.script.encode(), self.max_step, self.echo, self.gain, err
        )
        if handle == ffi.NULL:
            detail = ffi.string(err[0]).decode() if err[0] != ffi.NULL else "unknown"
            raise TempoError(f"{self.name}: bad mock script: {detail}")
        self._handle = handle

        caps = ffi.new("tempo_capabilities_t *")
        caps[0] = lib.tempo_mock_capabilities(handle)
        extra = self.extra_capabilities
        if self.predict:
            extra |= int(Capability.PREDICT_NEXT)
        caps.flags = caps.flags | extra
        return lib.tempo_mock_vtable(), handle, caps

    def release(self) -> None:
        if self._handle is not None:
            lib.tempo_mock_free(self._handle)
            self._handle = None

    # --- introspection, for tests and examples ---------------------------

    @property
    def time(self) -> int:
        """The mock's own logical time, in nanoseconds."""
        if self._handle is None:
            return 0
        return int(lib.tempo_mock_time(self._handle))

    @property
    def advance_count(self) -> int:
        """How many times the scheduler called ``advance_to`` on it."""
        if self._handle is None:
            return 0
        return int(lib.tempo_mock_advance_count(self._handle))

    def last_input(self, port: "str | int") -> float:
        """The most recent value seen on an input (requires ``echo=True``)."""
        if self._handle is None:
            return 0.0
        index = port if isinstance(port, int) else list(self.inputs).index(port)
        return float(lib.tempo_mock_last_input(self._handle, index))
