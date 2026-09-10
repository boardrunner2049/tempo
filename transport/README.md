# Transport

Reserved for phase 4: out-of-process participants.

Two participant forms are intended, and the scheduler core is independent of
which is used:

```
in-process:      Scheduler --native call--> Participant
out-of-process:  Scheduler --IPC/shm-----> Participant Process
```

Multiple QEMU instances will use the out-of-process form. The protocol is
simulator-neutral:

```
in:  INIT, ADVANCE_TO T, SET_INPUT, RESET, FINALIZE
out: READY, REACHED{time, reason}, OUTPUT, EVENT, ERROR
```

Planned contents:

- `local/`   — in-process participants, for lightweight models and testing
- `process/` — a simple reliable control transport
- `shm/`     — shared value slots and event ring buffers, added where
               benchmarking shows the control transport is the bottleneck,
               not before

Nothing in the participant API needs to change for any of this. That is the
point of keeping the scheduler ignorant of transport.
