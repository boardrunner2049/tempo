# Configuring an experiment

The same federation can be written in Python or in TOML. The two surfaces
mirror each other on purpose, so moving between them is mechanical.

## Python

```python
from tempo import Mock, Simulation

sim = Simulation(
    end_time="10ms",       # required; a federation with no end is an error
    quantum="1ms",         # the conservative policy's communication window
    policy="conservative",
    default_delay=None,    # None follows the quantum
    max_microsteps=100,    # bound on same-time iteration
    trace=True,            # False, True, or a record count
)

flight = sim.add(Mock("flight",
                      script="3ms:emit_sync 0 0.7",
                      outputs={"motor_command": "float64"},
                      inputs={"gyro": "float64"}))
world = sim.add(Mock("world", echo=True, gain=0.5,
                     inputs={"motor_command": "float64"},
                     outputs={"gyro": "float64"}))

sim.connect(flight["motor_command"], world["motor_command"])
sim.connect(world["gyro"], flight["gyro"], delay="500us")

sim.subscribe(participant="flight", event="port_write")

with sim:
    print(sim.explain())        # what the compiler produced
    sim.run()
    print(sim.value("world.motor_command"), sim.stats)
```

`Simulation` is a context manager, and closing it frees the plan, the
scheduler, the trace, and every native participant. Use `with`.

### Durations

Anywhere a duration is accepted: `"100ns"`, `"100us"`, `"3.5ms"`, `"10s"`,
`"2min"`, `"1h"`, or a plain number meaning nanoseconds. Parsing is done by
the C parser, so Python, TOML, and C agree by construction.

### Port declarations

```python
outputs={"motor": "float64"}                      # a scalar
outputs={"frame": ("bytes", 64)}                  # a byte port with capacity
outputs={"frame": {"type": "bytes", "max_bytes": 64}}
inputs=["a", "b"]                                 # names default to float64
```

Types: `bool`, `int64`, `float32`, `float64`, `bytes`. A byte port declares a
capacity, up to 4096; connecting a wider sender to a narrower receiver is a
compile error rather than a silent truncation.

An input and an output may share a name — they are different ports. Two ports
of the same direction may not.

### Running

```python
sim.run()             # to end_time
sim.run("4ms")        # to 4ms; call run() again to continue
while sim.step():     # one communication window at a time
    ...
sim.reset()           # back to t=0; needs RESET on every participant
```

### Inspecting

```python
sim.now                      # logical time in ns
sim.tag                      # (time, microstep)
sim.finished
sim.stats                    # windows, advance_calls, early_returns, ...
sim.value("world.gyro")
sim.participant_time("world")   # may be ahead of sim.now
sim.trace                    # a Trace, or None

plan = sim.compile()
plan.execution_order
plan.routes
plan.components
plan.cyclic_components
plan.describe()
```

## TOML

```toml
[simulation]
end_time = "10ms"

[scheduler]
policy = "conservative"
quantum = "1ms"
default_delay = "1ms"
max_microsteps = 100

[trace]
enabled = true
capacity = 65536

[[participants]]
name = "flight"
type = "mock"
script = "3ms:emit_sync 0 0.70"
outputs = { motor_command = "float64" }
inputs = { gyro = "float64" }

[[connections]]
from = "flight.motor_command"
to = "world.motor_command"
delay = "500us"

[[subscriptions]]
participant = "flight"
event = "mmio_write"
address = 0x40012000
break = false
```

An unrecognised key is an error, not a silently ignored line. `quantom = "1ms"`
fails during setup with the table it appeared in, rather than leaving you with
a default quantum and a puzzling result.

Run it:

```console
$ tempo-run experiment.toml --stats
$ tempo-run experiment.toml --explain
$ tempo-run experiment.toml --until 5ms
$ tempo-run experiment.toml --trace run.trace --dump-trace 20
```

## The mock participant

The mock exists so scheduler semantics can be pinned down before any real
simulator is attached. Its script is a list of `<time>:<action>` steps:

```
"3.5ms:trap; 6ms:emit_sync 0 0.7; 9ms:finish"
```

| Action | Effect |
| --- | --- |
| `trap` | stop and hand control back, producing nothing |
| `emit_sync <slot> <value>` | write an output and stop — a synchronizing event |
| `emit_observe <slot> <value>` | write an output and keep running |
| `mark <slot> <value>` | record a trace event without stopping |
| `finish` | stop for good |

Other options:

| Option | Effect |
| --- | --- |
| `echo=True`, `gain=g` | copy each changed input onto the output of the same index, scaled by `g`. Also makes the mock capable of same-time iteration. |
| `max_step="250us"` | never advance further than this per call, modelling a coarse internal step |
| `predict=True` | advertise `PREDICT_NEXT`, so the scheduler puts its horizon on the next scripted event |

## Participant types

`type` selects a factory. Only `mock` is registered in this version; adapters
register themselves as they arrive:

```python
from tempo.config import register_participant_type
register_participant_type("my_simulator", factory)
```

An unknown type lists what is registered rather than failing obscurely. See
[`adapters.md`](adapters.md).
