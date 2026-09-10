# Scheduling

The scheduler is the federation's logical-time authority. It decides who runs,
until when, and when values become visible.

## Logical time

Logical time is an unsigned nanosecond count. It is never derived from
wall-clock time, host thread scheduling, or sleeping. It is monotonic, and a
participant that moves backwards is a reported error, not a tolerated
approximation.

Events that fall at the same logical time are ordered by a **microstep** index
rather than by inventing artificial offsets. The pair `(time, microstep)` is a
**tag**, and it is the scheduler's real clock:

```
(10ms, 0)
(10ms, 1)
(10ms, 2)
(11ms, 0)
```

Physical time does not advance during microsteps.

## One window

`tempo_scheduler_step()` executes one communication window:

1. **Communication point at `now`.** Every value whose delivery time has
   arrived becomes logically visible. Same-time propagation then iterates
   until quiescent.
2. **Choose a horizon.** Bounded by the policy quantum, the end time, the
   earliest pending delivery, and any participant that can predict its own
   next event.
3. **Advance the frontier.** Every participant with `time < horizon` is asked
   to `advance_to(horizon)`, in the compiled execution order. Returning early
   is normal.
4. **Advance `now`** to the minimum live participant time, capped at the
   horizon.

Step 4 is why the scheduler is conservative: `now` is a lower bound on logical
time across the whole federation, so nothing can observe the future.

## Why it is sound

The load-bearing rule is in step 2: **the horizon is clamped to the earliest
pending delivery.**

A value produced at time `t` on a connection with delay `d` becomes visible at
`t + d`. Clamping the horizon to the earliest pending delivery guarantees that
no participant runs past a time at which one of its inputs changes. That is
what makes this conservative rather than optimistic, and it is why there is no
rollback machinery here.

Two consequences follow:

- A **delayed** connection lets both ends run freely to the next boundary. This
  is the case that has to be fast, and it is the default.
- A **zero-delay** connection forces the horizon down to the producing instant
  and is resolved by microsteps. It is more expensive and more constrained,
  which is the honest trade.

The scheduler does not synchronize because time passed. It synchronizes
because the policy's window closed or because a participant trapped out over a
causal interaction.

## Delays

Every connection has a delay. A connection that does not declare one gets the
spec default, which is the quantum. That default is not arbitrary: with
`delay >= quantum`, a value produced anywhere inside a window cannot be needed
inside that same window, so a participant that already ran to the boundary has
not missed anything.

```python
sim.connect(a["out"], b["in"])                 # delay = quantum
sim.connect(a["out"], b["in"], delay="50us")   # explicit
sim.connect(a["out"], b["in"], delay=0)        # same-time
```

## Early return, and what a timestamp means

`advance_to(T)` means *execute without advancing beyond T*. Reaching `T` is not
required:

```
Scheduler requests:   advance_to(10ms)
Participant stops at: 7.3ms, reason = EVENT
```

This is the scheduler trap. A firmware participant runs instructions, memory
accesses, and MMIO freely, and returns only when something happens that
another participant can observe.

That matters for timestamps. An output written during `advance_to(T)` is dated
at the participant's `reached_time`, because that is the latest instant the
scheduler can prove the value existed by. So:

- A participant that **traps out** the moment it produces a value gets an exact
  timestamp.
- A participant that runs to the end of its window gets its outputs dated at
  the window boundary, and the effective delay is `delay` plus up to one
  quantum.

If precise causal timing matters for a given interaction, the producing
participant should trap. That is a choice about which events are
*synchronizing*, which is the same choice discussed in
[`observability.md`](observability.md).

## Predicting the next event

A participant that advertises `PREDICT_NEXT` is asked for its next externally
relevant time, and the scheduler lowers the horizon to meet it. The trap then
lands exactly on a boundary instead of interrupting a window:

```python
Mock("a", script="3500us:trap")                  # early_returns == 1
Mock("a", script="3500us:trap", predict=True)    # early_returns == 0
```

Most QEMU-backed participants cannot predict, which is why the mock does not
advertise it by default.

## Same-time iteration

At a communication point, if any zero-delay connection exists, the scheduler
loops:

1. Deliver everything due at `now`.
2. Notify participants whose inputs changed.
3. Bump the microstep and let same-time-capable participants recompute at
   `now`.
4. Repeat until nothing new is delivered.

The loop is bounded by `max_microsteps`. A zero-delay cycle whose values keep
changing — feedback with gain above one, say — hits that bound and reports
`TEMPO_ERR_NON_CONVERGENT` with the time it gave up at. A hang would be worse
than an error.

Two capability requirements are checked before the run, not during it:

- the **destination** of a zero-delay connection must advertise
  `SAME_TIME_ITER`, otherwise it cannot re-evaluate within an instant;
- the **source** must advertise `EARLY_RETURN` or `PREDICT_NEXT`, otherwise it
  could run past the instant it produces the value.

## Execution order and dependency structure

The compiler builds two graphs.

The **dependency graph** contains every connection. It says who can eventually
affect whom, and it is what a future causal-slicing pass needs.

The **same-time graph** contains only zero-delay connections. A delayed
connection cannot force two participants to agree on a value at one instant,
so it cannot create a same-time cycle. Strongly connected components are
computed on the same-time graph, numbered in topological order, and the
execution order is "components in order, members by id". Where two components
are unordered relative to each other, the smaller id goes first, so the order
is a property of the compiled federation and never of the host.

```python
plan.execution_order      # ['source', 'middle', 'sink']
plan.components           # [['source'], ['middle'], ['sink']]
plan.cyclic_components    # the ones that actually need microsteps
```

## Determinism

Given identical deterministic participants and identical inputs, repeated runs
produce identical logical behaviour — the whole event stream, not just the
final values. Ordering comes from stable ids and sequence numbers:

- participants advance in the compiled execution order;
- deliveries are ordered by `(time, connection_id, enqueue sequence)`;
- change detection compares value bytes, so it is exact even for NaN.

Host thread scheduling, PID order, pointer values, and hash iteration order
are never used as semantic ordering mechanisms. `tempo_scheduler_reset()`
returns the federation to its initial state, and the trace of the next run is
byte-identical to the first — which is what makes repeated execution useful
for analysis and, eventually, fuzzing.

## Policy

`conservative` is the only policy in this version, and it is a fixed
communication window:

```toml
[scheduler]
policy = "conservative"
quantum = "100us"
```

Optimistic (Time Warp) scheduling is explicitly out of scope. A participant
must never observe information from the future. Event-driven, multi-rate, and
adaptive policies are intended to arrive later without changing the
participant API — which is why the API is `advance_to(deadline)` and not
`execute_one_quantum()`.
