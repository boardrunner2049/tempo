"""Enumerations mirroring the C API.

These are IntEnums so they can be passed straight through to the library
while still printing readably in Python.
"""

from __future__ import annotations

from enum import IntEnum, IntFlag


class PortType(IntEnum):
    BOOL = 0
    INT64 = 1
    FLOAT32 = 2
    FLOAT64 = 3
    BYTES = 4

    @classmethod
    def parse(cls, value: "PortType | str | int") -> "PortType":
        if isinstance(value, PortType):
            return value
        if isinstance(value, int):
            return cls(value)
        aliases = {
            "bool": cls.BOOL,
            "int": cls.INT64,
            "int64": cls.INT64,
            "float32": cls.FLOAT32,
            "float": cls.FLOAT64,
            "float64": cls.FLOAT64,
            "double": cls.FLOAT64,
            "bytes": cls.BYTES,
        }
        try:
            return aliases[str(value).lower()]
        except KeyError:
            raise ValueError(
                f"unknown port type {value!r}; expected one of "
                + ", ".join(sorted(aliases))
            ) from None


class PortDir(IntEnum):
    INPUT = 0
    OUTPUT = 1


class StopReason(IntEnum):
    TARGET_REACHED = 0
    EVENT = 1
    BLOCKED = 2
    FINISHED = 3
    ERROR = 4


class Capability(IntFlag):
    """What a participant can do (S30).

    The scheduler branches on these rather than on which simulator is behind
    the participant.
    """

    RESET = 1 << 0
    SNAPSHOT = 1 << 1
    RESTORE = 1 << 2
    EARLY_RETURN = 1 << 3
    PREDICT_NEXT = 1 << 4
    SAME_TIME_ITER = 1 << 5


class EventType(IntEnum):
    """Core event types. Adapter namespaces start at 0x0100 (S20)."""

    SIM_START = 0
    SIM_END = 1
    PARTICIPANT_RUN = 2
    PARTICIPANT_STOP = 3
    TIME_GRANT = 4
    SYNC_POINT = 5
    MICROSTEP = 6
    PORT_WRITE = 7
    PORT_READ = 8
    ROUTE_DELIVER = 9
    MESSAGE_SEND = 10
    MESSAGE_RECEIVE = 11
    MMIO_READ = 12
    MMIO_WRITE = 13
    INTERRUPT = 14
    WORLD_GET = 15
    WORLD_SET = 16
    RESET = 17
    USER_EVENT = 18

    @classmethod
    def parse(cls, value: "EventType | str | int") -> int:
        if isinstance(value, EventType):
            return int(value)
        if isinstance(value, int):
            return value
        name = str(value).upper().replace("-", "_")
        try:
            return int(cls[name])
        except KeyError:
            raise ValueError(
                f"unknown event type {value!r}; expected one of "
                + ", ".join(m.name.lower() for m in cls)
            ) from None


class EventNamespace(IntEnum):
    CORE = 0x0000
    FASTDYN = 0x0100
    FMI = 0x0200
    WORLD = 0x0300
    USER = 0x8000


class EventFlag(IntFlag):
    """Whether an event was merely recorded or actually stopped a participant.

    This is the distinction the whole observability design turns on (S11).
    """

    OBSERVE = 1 << 0
    SYNCHRONIZE = 1 << 1
    HAS_CAUSE = 1 << 2


#: A subscription that only records.
SUB_OBSERVE = 1 << 0
#: A subscription that stops the participant -- the slow path (S25).
SUB_BREAK = 1 << 1

ID_ANY = 0xFFFFFFFF
TIME_NEVER = 0xFFFFFFFFFFFFFFFF
