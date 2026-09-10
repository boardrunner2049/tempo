# Benchmarks

An elegant scheduler that significantly slows the simulators it coordinates is
not acceptable, so overhead is measured continuously rather than at the end.
The benchmark binary is built by default.

```console
$ ninja -C build
$ ./build/benchmarks/bench_scheduler
$ ./build/benchmarks/bench_scheduler 200 1000    # end_time in ms, quantum in ns
```

## What is measured

A **null participant** does nothing but move its own clock, so the numbers are
the scheduler's own cost with the simulator's cost removed. Scenarios climb
from bare scheduling to full tracing, so each row isolates one thing:

| Scenario | Isolates |
| --- | --- |
| bare scheduler, no ports (1, 8, 64 participants) | window and `advance_to` overhead, and how it scales |
| connected, values unchanged | change detection: routing that has nothing to do |
| connected, value every window | the routing path: harvest, queue, deliver |
| connected, selective trace | observability with a compiled filter |
| connected, full trace | observability with everything subscribed |

Representative output on one development machine (gcc 9.5, `debugoptimized`,
1 ns quantum, 200 ms of logical time — 200,000 windows):

```
scenario                                  per window   advances/s
bare scheduler, no ports (1 parts)           26.5 ns    37.7 M/s
bare scheduler, no ports (8 parts)          106.4 ns    75.2 M/s
bare scheduler, no ports (64 parts)         756.7 ns    84.6 M/s
connected, values unchanged (8 parts)        99.1 ns    80.8 M/s
connected, value every window (8 parts)     361.3 ns    22.1 M/s
connected, selective trace (8 parts)        766.1 ns    10.4 M/s
connected, full trace (8 parts)             736.2 ns    10.9 M/s
```

Read these as ratios, not absolutes. What matters:

- **Around 13 ns per `advance_to`** once there are several participants. That
  is the floor the scheduler adds per participant per window.
- **Change detection is nearly free.** "values unchanged" costs no more than
  "no ports": a write that does not change a value costs nothing downstream.
- **Routing is the largest remaining cost** when values do change — about
  37 ns per delivered value, across harvest, queue, and delivery. It is also
  the next thing worth optimising, and the shared-memory transport is where
  that goes.
- **Trace-disabled overhead is a branch.** The gap between the untraced and
  traced rows is the cost of actually recording, not the cost of the
  possibility of recording.

## The goal

The target is relative, not an arbitrary absolute throughput: a firmware
simulator running under the scheduler, with no meaningful cross-participant
synchronization, should lose less than about 5%. Inactive scheduler
functionality must be very cheap.

That number cannot be checked until the FastDyn adapter exists (phase 6). The
comparison to make then is:

```
FastDyn standalone
FastDyn under the scheduler, no cross-participant events
FastDyn under the scheduler, periodic synchronization
FastDyn under the scheduler, selective tracing
FastDyn under the scheduler, full tracing
```

## Known costs, in the order worth attacking

1. **Delivery queue.** A binary heap with a 40-byte entry per value in flight.
   It was 288 bytes before the payload moved into an arena, which cost 2.8× on
   the routing path — worth remembering as evidence that entry size dominates
   here.
2. **Per-participant loops at every window.** `compute_horizon` scans all
   participants for predictions, and `clear_changed` scans all of them at each
   communication point. Both are `O(participants)` per window and could be
   driven by a dirty list instead.
3. **`O(n²)` setup.** The dependency graph and component condensation use dense
   matrices. This is setup-time only and fine at tens to hundreds of
   participants, but it is the wrong shape for thousands.

## Method

- Build `debugoptimized` or `release`; a `debug` build is not a measurement.
- Change one thing at a time and keep the previous table.
- Prefer a longer run over repeating short ones; 200,000+ windows is stable.
- When a change makes something faster, check whether it also made a test
  weaker.
