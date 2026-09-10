"""TOML experiment files (S33).

The format deliberately mirrors the Python API one-to-one, so that moving an
experiment between a script and a file is mechanical. Unknown keys are an
error rather than being ignored: a typo in a configuration file should fail
during setup, not quietly change what runs (S14).
"""

from __future__ import annotations

import tomllib
from pathlib import Path
from typing import Callable

from .participants import Mock, Participant
from .types import EventType

#: Factories for the ``type`` field of a ``[[participants]]`` entry.
#: Adapters register themselves here as they arrive (FMI in phase 5, FastDyn
#: in phase 6), which is why the scheduler core never learns their names.
PARTICIPANT_TYPES: dict[str, Callable[..., Participant]] = {}

_SIMULATION_KEYS = {"end_time"}
_SCHEDULER_KEYS = {"policy", "quantum", "default_delay", "max_microsteps"}
_TRACE_KEYS = {"enabled", "capacity"}
_PARTICIPANT_COMMON = {"name", "type", "inputs", "outputs"}
_MOCK_KEYS = {"script", "echo", "gain", "max_step", "predict"}
_CONNECTION_KEYS = {"from", "to", "delay"}
_SUBSCRIPTION_KEYS = {"participant", "event", "address", "arg_mask", "arg_value", "break"}


class ConfigError(ValueError):
    """A problem with the configuration file, reported with its location."""

    def __init__(self, message: str, location: str) -> None:
        self.location = location
        super().__init__(f"{message} (at {location})")


def register_participant_type(name: str, factory: Callable[..., Participant]) -> None:
    """Make a participant type available to TOML files."""
    PARTICIPANT_TYPES[name] = factory


def _reject_unknown(table: dict, allowed: set[str], location: str) -> None:
    extra = sorted(set(table) - allowed)
    if extra:
        raise ConfigError(
            f"unknown key{'s' if len(extra) > 1 else ''} "
            + ", ".join(repr(k) for k in extra)
            + "; expected "
            + ", ".join(sorted(allowed)),
            location,
        )


def _make_mock(name: str, entry: dict, location: str) -> Mock:
    _reject_unknown(entry, _PARTICIPANT_COMMON | _MOCK_KEYS, location)
    return Mock(
        name,
        script=entry.get("script", ""),
        inputs=entry.get("inputs"),
        outputs=entry.get("outputs"),
        echo=bool(entry.get("echo", False)),
        gain=float(entry.get("gain", 1.0)),
        max_step=entry.get("max_step", 0),
        predict=bool(entry.get("predict", False)),
    )


register_participant_type("mock", _make_mock)


def load_toml(path: "str | Path") -> "Simulation":  # noqa: F821
    """Read an experiment file and return an unstarted :class:`Simulation`."""
    from .simulation import Simulation

    path = Path(path)
    try:
        raw = tomllib.loads(path.read_text())
    except tomllib.TOMLDecodeError as exc:
        raise ConfigError(str(exc), str(path)) from None

    return from_dict(raw, source=str(path))


def from_dict(raw: dict, source: str = "<dict>") -> "Simulation":  # noqa: F821
    from .simulation import Simulation

    _reject_unknown(
        raw,
        {"simulation", "scheduler", "trace", "participants", "connections", "subscriptions"},
        source,
    )

    simulation = raw.get("simulation", {})
    _reject_unknown(simulation, _SIMULATION_KEYS, "simulation")
    if "end_time" not in simulation:
        raise ConfigError("end_time is required", "simulation")

    scheduler = raw.get("scheduler", {})
    _reject_unknown(scheduler, _SCHEDULER_KEYS, "scheduler")

    trace_table = raw.get("trace", {})
    _reject_unknown(trace_table, _TRACE_KEYS, "trace")
    if trace_table.get("enabled", False):
        trace: "bool | int" = int(trace_table.get("capacity", 0)) or True
    else:
        trace = False

    sim = Simulation(
        end_time=simulation["end_time"],
        quantum=scheduler.get("quantum", "100us"),
        policy=scheduler.get("policy", "conservative"),
        default_delay=scheduler.get("default_delay"),
        max_microsteps=int(scheduler.get("max_microsteps", 100)),
        trace=trace,
    )

    participants = raw.get("participants", [])
    if not participants:
        raise ConfigError("a federation needs at least one participant", "participants")
    for i, entry in enumerate(participants):
        location = f"participants[{i}]"
        if "name" not in entry:
            raise ConfigError("name is required", location)
        name = entry["name"]
        kind = entry.get("type")
        if kind is None:
            raise ConfigError(f"participant {name!r} has no type", location)
        factory = PARTICIPANT_TYPES.get(kind)
        if factory is None:
            known = ", ".join(sorted(PARTICIPANT_TYPES)) or "none"
            raise ConfigError(
                f"unknown participant type {kind!r}; registered types: {known}. "
                "Adapters register themselves with "
                "tempo.config.register_participant_type()",
                location,
            )
        sim.add(factory(name, entry, location))

    for i, entry in enumerate(raw.get("connections", [])):
        location = f"connections[{i}]"
        _reject_unknown(entry, _CONNECTION_KEYS, location)
        if "from" not in entry or "to" not in entry:
            raise ConfigError("a connection needs both 'from' and 'to'", location)
        try:
            sim.connect(entry["from"], entry["to"], delay=entry.get("delay"))
        except ValueError as exc:
            raise ConfigError(str(exc), location) from None

    for i, entry in enumerate(raw.get("subscriptions", [])):
        location = f"subscriptions[{i}]"
        _reject_unknown(entry, _SUBSCRIPTION_KEYS, location)
        try:
            sim.subscribe(
                participant=entry.get("participant"),
                event=entry.get("event"),
                address=entry.get("address"),
                arg_mask=int(entry.get("arg_mask", 0)),
                arg_value=int(entry.get("arg_value", 0)),
                break_on=bool(entry.get("break", False)),
            )
        except ValueError as exc:
            raise ConfigError(str(exc), location) from None

    return sim


def dump_event_types() -> str:
    """The event names a ``[[subscriptions]]`` entry may use."""
    return ", ".join(member.name.lower() for member in EventType)
