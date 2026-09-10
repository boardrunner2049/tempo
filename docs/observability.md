# Observability

tempo is meant for dynamic analysis, not only for running simulations. The
relationship worth seeing is a causal chain that crosses participants:

```
Firmware A writes an actuator command
        |
        v
the physical model changes
        |
        v
the sensor value changes
        |
        v
Firmware B observes the new sensor value
```

Making that visible is an architectural requirement, so logical time,
participant identity, communication, and synchronization are all exposed
through one mechanism rather than through logging added afterwards.

## Observing is not synchronizing

This is the distinction the whole design turns on. Every event is one of two
kinds:

- **OBSERVE** — written to a trace buffer; execution continues.
- **SYNCHRONIZE** — control returns to the scheduler.

```
Firmware reads an internal UART register        -> OBSERVE
Firmware transmits a packet Firmware B consumes -> SYNCHRONIZE
```

Millions of events can be recorded without stopping a participant millions of
times. Stopping QEMU for every function entry would destroy the throughput
that made integrating with it worthwhile in the first place.

In a trace, `tempo-trace dump` marks these `[obs]` and `[sync]`, and in Python
`Event.synchronizing` tells them apart.

## The event record

One fixed-size 48-byte record, for everything:

```c
struct tempo_event_header {
    uint64_t logical_time;
    uint64_t sequence;
    uint32_t participant_id;
    uint16_t type;
    uint16_t flags;
};

struct tempo_event {
    tempo_event_header_t hdr;
    uint32_t             microstep;
    uint32_t             arg0;    /* slot / connection / port index   */
    uint64_t             arg1;    /* address, value bits, target time */
    uint64_t             cause;   /* parent sequence, 0 if unknown    */
};
```

Fixed size keeps ring-buffer indexing to a mask and keeps the writer
allocation-free. `sequence` is the global deterministic ordering.  `cause`
carries the sequence number of the causing event where an adapter can supply
one, which is what makes cross-participant causal chains reconstructable
offline.

## Type namespaces

The core reserves only the low range, so adapters add their own types without
the scheduler learning about them:

| Range | Owner |
| --- | --- |
| `0x0000`–`0x00FF` | scheduler core |
| `0x0100`–`0x01FF` | FastDyn adapter |
| `0x0200`–`0x02FF` | FMI adapter |
| `0x0300`–`0x03FF` | world / physics |
| `0x8000`–`0xFFFF` | user analyses |

Core types: `sim_start`, `sim_end`, `participant_run`, `participant_stop`,
`time_grant`, `sync_point`, `microstep`, `port_write`, `port_read`,
`route_deliver`, `message_send`, `message_receive`, `mmio_read`, `mmio_write`,
`interrupt`, `world_get`, `world_set`, `reset`, `user_event`.

## Selective tracing

Tracing is selective by construction. Asking for MMIO writes from one
participant at one address must not mean generating and discarding everything
else.

```python
sim.subscribe(participant="flight", event="mmio_write", address=0x40012000)
```

```toml
[[subscriptions]]
participant = "flight"
event = "mmio_write"
address = 0x40012000
```

Subscriptions are compiled into native filters before the run, so the
no-observer path at every emit site is a single predictable branch:

```c
if (TEMPO_UNLIKELY(tempo_trace_wants(tr, type, pid)))
    tempo_trace_emit(tr, ...);
```

With no subscriptions at all, the compiler records the scheduler's own
decisions — low volume, and almost always what you want to see first — and
leaves high-volume adapter namespaces off.

Post-hoc filtering in Python (`trace.events(type=...)`) is a different thing:
it selects among records that were already produced. Use `subscribe` to avoid
producing them.

## Observe versus intervene

Some analyses need to stop execution. That is explicitly the slow path, and it
is spelled differently:

```python
sim.subscribe(participant="flight", event="mmio_write",
              address=0x40012000, break_on=True)
```

The difference between watching and interfering stays visible in the source.

## Trace buffers and files

Events go into a preallocated ring buffer. No JSON is produced on the
execution path. A full buffer either drops records and counts them, or
overwrites the oldest — the choice is explicit, and `trace.dropped` says what
happened, so a truncated trace is never mistaken for a complete one.

The file format is a 64-byte header (`TEMPOTR1`) followed by packed records.
Conversion happens in a separate tool:

```console
$ tempo-trace info run.trace
$ tempo-trace dump run.trace
$ tempo-trace dump --format=jsonl run.trace | jq .
$ tempo-trace dump --format=csv   run.trace > run.csv
$ tempo-trace dump --format=chrome run.trace > run.json   # chrome://tracing
```

Or in Python:

```python
from tempo import TraceFile
trace = TraceFile.load("run.trace")
for event in trace.filter(type="mmio_write"):
    print(event)
```

## Reading a chain out of a trace

Because every record carries a logical time, a stable participant id, and a
global sequence number, a causal chain is recoverable after the fact:

```console
       3ms  firmware_a   wrote        # actuator command
    3500us  world        received
    4500us  world        wrote        # angular velocity changed
       5ms  firmware_b   received     # sensor value observed
```

That output comes from `examples/motor_and_sensor.py`, built from
`port_write` and `route_deliver` records alone. A richer cross-participant
analysis — following one value through the federation and reporting where it
surfaced — is phase 8; the information it needs is already in the records.
