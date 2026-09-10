# Architecture

tempo has two planes, and the boundary between them is the most important
line in the project.

```
              Python / TOML
                    |
            configuration + setup
                    |
                    v
     =============================
            Native Runtime
     =============================
                    |
               Scheduler
                    |
        participants + communication
```

Python is the **control plane**: it declares experiments, compiles them,
inspects plans, and reads traces. Native code is the **execution plane**:
logical time, scheduling, routing, and event recording. Python never runs
inside the scheduler's loop.

## Module map

| Path | What lives there |
| --- | --- |
| `include/tempo/` | The public C API. Nothing outside this directory is part of the ABI. |
| `src/scheduler/` | The scheduler: windows, horizons, communication points, microsteps. |
| `src/federation/` | The federation compiler: spec → validation → plan, plus dependency analysis. |
| `src/routing/` | The delivery queue: values in flight between participants. |
| `src/trace/` | The native trace ring buffer. |
| `adapters/mock/` | A scriptable mock participant, and the reference for what an adapter looks like. |
| `adapters/fmi/` | Reserved for the FMI adapter (phase 5). |
| `transport/` | Reserved for out-of-process participants: control transport, then shared memory (phase 4). |
| `python/tempo/` | The control plane: cffi bindings, `Simulation`, TOML front end, trace reader. |
| `tools/tempo-trace/` | Native trace converter: text, JSONL, CSV, Chrome trace. |
| `tests/` | C tests (`test_*.c`) and Python tests (`test_*.py`), both run by `meson test`. |
| `benchmarks/` | Scheduler overhead measurements, built by default. |

## The abstraction boundaries

The scheduler understands logical time, participants, dependencies,
communication points, events, ordering, and synchronization. It does **not**
understand QEMU, firmware, FMI, physical modelling, or Modelica. It sees only

```c
participant->advance_to(target_time)
```

An **adapter** understands how to control one kind of simulator, and translates
`advance_to(T)` into whatever that simulator requires. Adapters may define
their own event types in their own namespace, so the trace infrastructure
carries FastDyn- or FMI-specific detail without the scheduler learning about
it.

Dependency direction is one-way:

```
FastDyn ---> tempo
tempo -X-> FastDyn
```

## What the compiler removes from the hot path

Configuration is compiled once. A connection written as

```toml
from = "controller.motor"
to   = "world.motor"
```

becomes a pair of indices, so routing at runtime is

```c
input_slots[3] = output_slots[17];
```

with no string lookup, no hash table, and no type dispatch. Everything that
can be wrong about a configuration — a missing participant or port, a type
mismatch, two writers into one input, an unknown policy — is rejected before
the run, with a location the front end can point at.

## Slots are plain memory

A participant reads and writes `tempo_slot_t` arrays directly. There is no
function call per value. The layout is deliberately ABI-stable and free of
pointers into scheduler internals, so the same code works for an in-process
participant today and for a shared-memory segment mapped into another process
later.

`tempo_slot_t::generation` is the participant-visible signal that an input
carries a new value: detecting a changed input costs one integer compare, not
a callback.

## Further reading

- [`scheduling.md`](scheduling.md) — the conservative policy, and why it is sound
- [`observability.md`](observability.md) — events, subscriptions, traces
- [`adapters.md`](adapters.md) — writing a participant
- [`configuration.md`](configuration.md) — the Python and TOML surfaces
- [`roadmap.md`](roadmap.md) — what is built and what is next
