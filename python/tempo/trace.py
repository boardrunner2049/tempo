"""Reading trace buffers and trace files.

The runtime writes fixed-size binary records and nothing else (S22). Turning
them into something a person or a dataframe can read happens here, off the
execution path.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Iterator

from ._ffi import ffi, format_duration, lib
from .errors import check
from .types import EventFlag, EventType

#: struct tempo_event_t: header (u64 time, u64 seq, u32 pid, u16 type, u16 flags)
#: followed by u32 microstep, u32 arg0, u64 arg1, u64 cause.
_RECORD = struct.Struct("<QQIHHIIQQ")
RECORD_SIZE = _RECORD.size

_FILE_HEADER = struct.Struct("<8sIIQQII3Q")
TRACE_MAGIC = b"TEMPOTR1"
TRACE_VERSION = 1


@dataclass(frozen=True)
class Event:
    sequence: int
    time: int
    microstep: int
    participant: int
    type: int
    flags: int
    arg0: int
    arg1: int
    cause: int

    @property
    def type_name(self) -> str:
        return ffi.string(lib.tempo_event_type_str(self.type)).decode()

    @property
    def synchronizing(self) -> bool:
        """Whether this event stopped a participant, rather than only being
        recorded (S11)."""
        return bool(self.flags & int(EventFlag.SYNCHRONIZE))

    @property
    def tag(self) -> tuple[int, int]:
        """The (time, microstep) tag this event carries (S17)."""
        return (self.time, self.microstep)

    def __str__(self) -> str:
        ms = f"+{self.microstep}" if self.microstep else ""
        who = "-" if self.participant == 0xFFFFFFFF else f"p{self.participant}"
        kind = "sync" if self.synchronizing else "obs"
        return (
            f"{self.sequence:>7}  {format_duration(self.time):>9}{ms:<3} {who:<5} "
            f"{self.type_name:<18} [{kind}] arg0={self.arg0} arg1={self.arg1}"
        )


def _event_from_cdata(e) -> Event:
    return Event(
        sequence=int(e.hdr.sequence),
        time=int(e.hdr.logical_time),
        microstep=int(e.microstep),
        participant=int(e.hdr.participant_id),
        type=int(e.hdr.type),
        flags=int(e.hdr.flags),
        arg0=int(e.arg0),
        arg1=int(e.arg1),
        cause=int(e.cause),
    )


def _matches(event: Event, type=None, participant=None, synchronizing=None) -> bool:
    if type is not None:
        wanted = {EventType.parse(t) for t in _iterable(type)}
        if event.type not in wanted:
            return False
    if participant is not None and event.participant not in set(_iterable(participant)):
        return False
    if synchronizing is not None and event.synchronizing != bool(synchronizing):
        return False
    return True


def _iterable(value) -> Iterable:
    if isinstance(value, (str, int)) or not hasattr(value, "__iter__"):
        return [value]
    return value


class Trace:
    """A live trace buffer owned by a running simulation.

    Iterating reads directly out of the C ring buffer, so nothing is copied
    until a record is actually converted.
    """

    def __init__(self, capacity: int = 1 << 16, overwrite: bool = False) -> None:
        cfg = ffi.new("tempo_trace_config_t *")
        cfg.capacity = capacity
        cfg.overwrite = overwrite
        self._trace = lib.tempo_trace_new(cfg)
        if self._trace == ffi.NULL:
            raise MemoryError("could not allocate a trace buffer")

    @property
    def handle(self):
        return self._trace

    def close(self) -> None:
        if self._trace is not None:
            lib.tempo_trace_free(self._trace)
            self._trace = None

    def __del__(self) -> None:  # pragma: no cover - best effort
        self.close()

    def _check(self):
        if self._trace is None:
            raise RuntimeError("this trace has been freed")
        return self._trace

    def __len__(self) -> int:
        return int(lib.tempo_trace_count(self._check()))

    def __getitem__(self, index: int) -> Event:
        trace = self._check()
        n = int(lib.tempo_trace_count(trace))
        if index < 0:
            index += n
        if not 0 <= index < n:
            raise IndexError(index)
        return _event_from_cdata(lib.tempo_trace_at(trace, index))

    def __iter__(self) -> Iterator[Event]:
        trace = self._check()
        for i in range(int(lib.tempo_trace_count(trace))):
            yield _event_from_cdata(lib.tempo_trace_at(trace, i))

    @property
    def total(self) -> int:
        """Records emitted, which exceeds ``len()`` once the ring has wrapped."""
        return int(lib.tempo_trace_total(self._check()))

    @property
    def dropped(self) -> int:
        return int(lib.tempo_trace_dropped(self._check()))

    def clear(self) -> None:
        lib.tempo_trace_clear(self._check())

    def enable(self, enabled: bool = True) -> None:
        lib.tempo_trace_set_enabled(self._check(), bool(enabled))

    def events(self, **filters) -> list[Event]:
        """Records matching ``type=``, ``participant=``, ``synchronizing=``.

        This filters in Python, so it is for inspecting a trace after the
        fact. To avoid *producing* unwanted events in the first place, use
        ``Simulation.subscribe`` -- that compiles into a native filter (S21).
        """
        return [e for e in self if _matches(e, **filters)]

    def save(self, path: "str | Path") -> None:
        check(
            lib.tempo_trace_write_file(self._check(), str(path).encode()),
            f"writing {path}",
        )

    def dump(self, limit: int | None = None, **filters) -> str:
        lines = []
        for i, event in enumerate(self.events(**filters) if filters else self):
            if limit is not None and i >= limit:
                lines.append("...")
                break
            lines.append(str(event))
        return "\n".join(lines)

    def __repr__(self) -> str:
        if self._trace is None:
            return "<Trace (freed)>"
        return f"<Trace {len(self)} records, {self.dropped} dropped>"


@dataclass
class TraceFile:
    """A trace read back from disk."""

    version: int
    record_count: int
    dropped_count: int
    n_participants: int
    events: list[Event]

    @classmethod
    def load(cls, path: "str | Path") -> "TraceFile":
        data = Path(path).read_bytes()
        if len(data) < _FILE_HEADER.size:
            raise ValueError(f"{path}: too short to be a tempo trace")
        magic, version, record_size, count, dropped, n_parts, _flags, *_ = (
            _FILE_HEADER.unpack_from(data, 0)
        )
        if magic != TRACE_MAGIC:
            raise ValueError(f"{path}: not a tempo trace")
        if version != TRACE_VERSION:
            raise ValueError(
                f"{path}: trace version {version}, this reader understands {TRACE_VERSION}"
            )
        if record_size != RECORD_SIZE:
            raise ValueError(
                f"{path}: record size {record_size}, expected {RECORD_SIZE}"
            )

        events = []
        offset = _FILE_HEADER.size
        for _ in range(count):
            if offset + RECORD_SIZE > len(data):
                raise ValueError(f"{path}: truncated after {len(events)} records")
            (
                time,
                sequence,
                participant,
                etype,
                flags,
                microstep,
                arg0,
                arg1,
                cause,
            ) = _RECORD.unpack_from(data, offset)
            events.append(
                Event(
                    sequence=sequence,
                    time=time,
                    microstep=microstep,
                    participant=participant,
                    type=etype,
                    flags=flags,
                    arg0=arg0,
                    arg1=arg1,
                    cause=cause,
                )
            )
            offset += RECORD_SIZE

        return cls(
            version=version,
            record_count=count,
            dropped_count=dropped,
            n_participants=n_parts,
            events=events,
        )

    def filter(self, **filters) -> list[Event]:
        return [e for e in self.events if _matches(e, **filters)]

    def __len__(self) -> int:
        return len(self.events)

    def __iter__(self) -> Iterator[Event]:
        return iter(self.events)
