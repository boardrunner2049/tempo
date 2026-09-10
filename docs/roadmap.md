# Roadmap

Phases follow the design document. Phases 1 and 2 are built; the rest is not
started, and this file says what "done" will mean for each so the boundary
stays honest.

## Phase 1 — Core scheduler ✅

Mock participants only, deliberately: the scheduler's semantics have to be
trustworthy before a real simulator can be blamed for anything.

- logical time as nanoseconds, with `(time, microstep)` tags
- participant registration and the `advance_to()` contract
- early return as a first-class outcome
- typed ports and slot arrays that participants read and write directly
- fixed-window conservative scheduling with horizon clamping
- deterministic ordering, verified by comparing whole event streams
- communication points and same-time iteration with a convergence bound
- `reset()`, verified to reproduce a run byte for byte
- a native trace ring buffer
- capability checks performed before the run rather than during it

## Phase 2 — Federation compiler ✅

- a mutable spec built from Python or TOML, compiled once into a plan
- participant, port, and connection ids resolved ahead of execution
- routing reduced to index arithmetic; no string lookup at runtime
- full dependency graph, plus a separate same-time graph
- strongly connected components, numbered topologically, giving a
  deterministic execution order
- validation with locations: missing participant or port, wrong direction,
  type mismatch, duplicate exclusive input, byte capacity, unknown policy
- subscriptions compiled into native trace filters
- benchmarks, so the effect of removing lookups is measurable rather than
  asserted

## Phase 3 — Observability infrastructure

Much of the mechanism exists because phase 1 needed it. What is missing:

- asynchronous trace writing, so a long run does not hold everything in memory
- `cause` populated by the scheduler itself, not only available to adapters
- subscription matching on `arg_mask`/`arg_value` enforced at emit time (the
  fields are compiled into the plan but only the coarse type and participant
  masks are consulted so far)
- richer `tempo-trace` output: per-participant timelines, and summaries

## Phase 4 — Process transport

A simulator-neutral protocol for out-of-process participants:

```
in:  INIT, ADVANCE_TO T, SET_INPUT, RESET, FINALIZE
out: READY, REACHED{time, reason}, OUTPUT, EVENT, ERROR
```

Start with a simple reliable control transport, then add shared-memory fast
paths where benchmarking shows they matter — not before. `transport/` holds
the intended shape: `local/`, `process/`, `shm/`.

## Phase 5 — FMI adapter

FMI is the right first real adapter because it exercises variable stepping,
early return, discrete events, and physical state. FMI semantics stay entirely
inside the adapter; the scheduler continues to see only participants.

## Phase 6 — FastDyn adapter

Only after the scheduler is correct with synthetic participants and one real
simulator. The adapter must expose `advance_to(T)`, `current_time()`, and
`reset()`, and must give FastDyn a way to return early on *synchronizing*
events while letting observable events flow into the trace without stopping
anything. It lives in the FastDyn repository: the dependency runs
`FastDyn -> tempo`, never the reverse.

## Phase 7 — Multi-firmware demonstration

Two firmware instances interacting through a shared physical model, with the
result deterministic even when host scheduling changes.

## Phase 8 — Dynamic analysis demonstration

Follow one actuator command from firmware A, through the physical world, to
the moment firmware B observes its effect. `examples/motor_and_sensor.py`
already prints a simple version of this from `port_write` and `route_deliver`
records; the phase is about doing it properly, with causal parents.

## Later, and deliberately not yet

**Schedule skeletons.** Many analysis and fuzzing workloads run the same
system repeatedly. Record the synchronization pattern on the first run,
compile it into a specialized schedule, and execute later runs against it with
guards that fall back to the generic scheduler when execution diverges. This
needs the generic scheduler to be correct and measured first.

**Causal federation slicing.** Use the dependency graph to work out which
participants can affect an analysis target over an interval, and avoid
synchronizing the ones outside that causal cone. The dependency information is
already preserved for this reason — `plan.dependents()` exposes it — but
nothing uses it yet.

## Non-goals for version 1

Optimistic Time Warp scheduling. Distributed cluster execution. Arbitrary
remote federation. GUI visualization. Rollback-based speculative simulation.
Full dynamic taint analysis. Python in the scheduling loop. Automatic
Modelica-to-device generation. Sophisticated real-time operation.

The first version is meant to be small, correct, fast, deterministic, and
observable.

## Known gaps in what is built

Worth knowing before relying on any of it:

- **One policy.** `conservative` with a fixed window. The participant API is
  `advance_to(deadline)` specifically so that event-driven, multi-rate, and
  adaptive policies can arrive without changing it, but none exist yet.
- **`snapshot`/`restore` are API only.** Declared, never implemented, and no
  participant advertises them. Federation-wide snapshots need every relevant
  participant to restore consistently, which is a phase in itself.
- **Subscription argument matching is not enforced at emit time.** See phase 3.
- **Single-threaded.** The scheduler runs participants in order on one thread.
  Concurrency belongs with the process transport, not before it.
- **`O(n²)` setup structures.** Fine for tens to hundreds of participants; the
  wrong shape for thousands. See [`benchmarks.md`](benchmarks.md).
- **Byte ports cap at 4096 bytes.** Larger messages are the shared-memory
  transport's job.
