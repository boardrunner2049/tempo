# tempo

A co-simulation scheduler: it coordinates independently executing systems
under one shared notion of simulated time — firmware under QEMU/FastDyn,
physical models, FMI components, network simulators, or anything else whose
execution can be advanced.

```
                   Scheduler
                       |
        +--------------+--------------+
        |              |              |
        v              v              v
    Firmware A     Firmware B       World
```

One firmware instance drives a motor; another reads a sensor influenced by
that motor through a shared physical model. tempo makes their interactions
respect logical time, and makes the resulting causal chain observable.

It is built around three requirements that are hard to add later, so none of
them are treated as optional:

- **Fast.** The common case is cheap. Configuration is compiled into an
  execution plan, so routing a value is index arithmetic rather than a lookup;
  participants read and write plain memory; nothing synchronizes merely
  because time passed.
- **Observable.** Logical time, participant identity, communication, and
  synchronization are exposed through one native event stream. Recording an
  event and stopping a participant are deliberately different things.
- **Convenient to drive.** Experiments are written in Python or TOML. The
  runtime stays native, and Python never enters the scheduler's loop.

Status: **phases 1 and 2 are implemented** — the core scheduler and the
federation compiler, tested against synthetic participants. No real simulator
is attached yet; FMI is next, then FastDyn. See
[`docs/roadmap.md`](docs/roadmap.md) for what that means precisely.

## Build

Needs a C11 compiler, Meson, Ninja, and Python 3.11+.

```console
$ python3 -m venv .venv && . .venv/bin/activate
$ pip install meson ninja cffi pytest
$ meson setup build --buildtype=debugoptimized
$ ninja -C build
$ meson test -C build            # C tests and Python tests
$ pip install -e .               # the control plane
```

## A first federation

```python
from tempo import Mock, Simulation

sim = Simulation(end_time="10ms", quantum="1ms", trace=True)

flight = sim.add(Mock("flight",
                      script="3ms:emit_sync 0 0.70",
                      outputs={"motor_command": "float64"}))
world = sim.add(Mock("world", echo=True, gain=3.2,
                     inputs={"motor_command": "float64"},
                     outputs={"angular_velocity": "float64"}))

sim.connect(flight["motor_command"], world["motor_command"], delay="500us")

with sim:
    sim.run()
    print(sim.value("world.angular_velocity"))   # 2.24
    print(sim.stats)
```

Or as a file:

```console
$ tempo-run examples/two_firmware.toml --stats
$ tempo-run examples/two_firmware.toml --explain     # what the compiler produced
$ tempo-run examples/two_firmware.toml --trace run.trace
$ ./build/tools/tempo-trace/tempo-trace dump run.trace
```

More in [`examples/`](examples/).

## The ideas worth knowing

**`advance_to(T)` means "run without passing T", not "run to T".** A
participant that stops at 3.17 ms because it sent a message another
participant will consume has done the right thing. Early return is the normal
case, and it is what lets firmware execute millions of instructions without
returning to the scheduler.

**Every connection has a delay, and the horizon is clamped to the earliest
pending delivery.** That single clamp is what makes the scheduler conservative
without any rollback machinery: nobody can run past a time at which one of
their inputs changes. A delayed connection lets both ends run freely to the
next boundary — the case worth being fast. A zero-delay connection is resolved
by microsteps within one instant, at a cost.

**Same-time events are ordered by `(time, microstep)`, not by faking time.**
No inventing `T + 1ns` to force an ordering. Physical time does not advance
during microsteps, and a zero-delay cycle that never settles is reported
rather than hung on.

**Observing is not synchronizing.** An observable event is a ring-buffer
write; a synchronizing event returns control to the scheduler. That
distinction is what allows millions of recorded events without millions of
stops.

**Configuration is compiled, not interpreted.** `from = "controller.motor"`
becomes a pair of slot indices before the run starts. Every configuration
mistake — a missing port, a type mismatch, two writers into one input — is
reported with the line that caused it, during setup.

**Runs are reproducible down to the event stream.** Ordering comes from stable
ids and sequence numbers, never from host thread scheduling or hash iteration
order. `reset()` puts the federation back and the next run's trace is
byte-identical.

## Documentation

| | |
| --- | --- |
| [`docs/architecture.md`](docs/architecture.md) | the two planes, module map, abstraction boundaries |
| [`docs/scheduling.md`](docs/scheduling.md) | the conservative policy, delays, microsteps, determinism |
| [`docs/observability.md`](docs/observability.md) | events, subscriptions, trace format |
| [`docs/adapters.md`](docs/adapters.md) | writing a participant for a real simulator |
| [`docs/configuration.md`](docs/configuration.md) | the Python and TOML surfaces |
| [`docs/benchmarks.md`](docs/benchmarks.md) | how overhead is measured, and what the numbers mean |
| [`docs/roadmap.md`](docs/roadmap.md) | phases, known gaps, non-goals |

## Layout

```
include/tempo/   public C API
src/             scheduler, federation compiler, routing, trace
adapters/        mock (built); fmi (phase 5)
transport/       out-of-process participants (phase 4)
python/tempo/    control plane: bindings, Simulation, TOML, trace reader
tools/           tempo-trace (native), tempo-run (Python entry point)
tests/           C and Python tests
benchmarks/      scheduler overhead
examples/        runnable federations
```

The scheduler core does not know what QEMU is, and it never will: that
knowledge belongs in an adapter. Dependencies run `FastDyn -> tempo`, never
the reverse.

## License

MIT.
