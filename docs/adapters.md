# Writing a participant adapter

A **participant** is anything whose logical execution can be advanced. An
**adapter** knows how to control one kind of simulator and translates the
scheduler's request into whatever that simulator requires.

Adapters are native. Building one in Python would put Python in the
scheduler's loop, which the design forbids.

## The contract

```c
initialize(self, ctx)
advance_to(self, target, out)
destroy(self)
```

`advance_to(T)` means: **execute this participant without advancing beyond
logical time T.** Reaching `T` is not required, and returning early is normal
rather than exceptional.

```c
typedef enum {
    TEMPO_TARGET_REACHED,  /* at the requested time                       */
    TEMPO_EVENT,           /* stopped early: something externally relevant */
    TEMPO_BLOCKED,         /* cannot progress until an input arrives       */
    TEMPO_FINISHED,        /* will never progress again                    */
    TEMPO_ERROR            /* failed; the run aborts                       */
} tempo_stop_reason_t;
```

Rules the scheduler enforces, so you find out at once rather than through odd
behaviour:

- never return a time before the participant's current time;
- never return a time beyond `target`;
- report `TEMPO_ERROR` rather than guessing.

## Reading and writing values

`initialize` hands you a context with base pointers into the federation's slot
arrays:

```c
static int my_initialize(void *self, const tempo_participant_context_t *ctx)
{
    my_participant_t *p = self;
    p->ctx = ctx;          /* keep it; the scheduler owns the memory */
    return TEMPO_OK;
}
```

Then reads and writes are plain memory accesses — no call into the scheduler:

```c
double command = ctx->inputs[0].scalar.f64;
ctx->outputs[0].scalar.f64 = velocity;
```

To detect a changed input, compare the generation. That is one integer
compare, which is why the field exists:

```c
if (ctx->inputs[0].generation != p->seen[0]) {
    p->seen[0] = ctx->inputs[0].generation;
    /* recompute */
}
```

The scheduler detects changed *outputs* by comparing bytes against a shadow
copy, so writing the same value again costs nothing downstream. To force a
resend of an unchanged value, bump the output slot's `generation`.

For `TEMPO_TYPE_BYTES` ports use `tempo_slot_set_bytes()`, which truncates at
the slot's capacity and returns what it stored.

## Capabilities

Advertise what you can do. The scheduler branches on these flags, never on
which simulator is behind the participant:

| Flag | Meaning |
| --- | --- |
| `RESET` | `reset()` returns the participant to its initial state |
| `EARLY_RETURN` | may stop before the requested target |
| `PREDICT_NEXT` | `next_event_time()` is meaningful |
| `SAME_TIME_ITER` | can be re-evaluated at one logical instant; required to receive a zero-delay connection |
| `SNAPSHOT` / `RESTORE` | state can be saved and restored |

A claimed flag whose function is missing is rejected at bind time, so the hot
path can test the flag alone and never test for `NULL`.

## Optional entry points

```c
reset(self)                        /* requires RESET                        */
next_event_time(self)              /* requires PREDICT_NEXT                 */
inputs_changed(self, slots, n)     /* communication points only             */
snapshot(self, &blob, &len)        /* requires SNAPSHOT                     */
restore(self, blob, len)           /* requires RESTORE                      */
```

`inputs_changed` is for adapters that must *push* values into a foreign
simulator — an FMI `fmi3SetFloat64`, or a register write into a guest. A
participant that reads its input slots directly does not need it. It is called
only at a communication point, never on the fast path.

## Events

Record what an analysis might want, but do not stop unless the interaction
actually requires it:

```c
if (TEMPO_UNLIKELY(tempo_trace_wants(ctx->trace, MY_EV_MMIO_WRITE, ctx->participant_id)))
    tempo_trace_emit(ctx->trace, now, 0, ctx->participant_id,
                     MY_EV_MMIO_WRITE, TEMPO_EVF_OBSERVE, 0, address, 0);
```

Define your types in your own namespace (`TEMPO_EV_NS_FASTDYN + n`) so the
core stays simulator-neutral. An event that returns control to the scheduler
carries `TEMPO_EVF_SYNCHRONIZE` and shows up as an early return; an event that
does not carries `TEMPO_EVF_OBSERVE` and costs a ring-buffer write.

## Binding

```c
static const tempo_participant_vtable_t my_vtable = {
    "my_simulator",
    my_initialize, my_advance_to, my_destroy,
    my_reset, my_next_event_time,
    NULL, NULL,        /* snapshot, restore */
    NULL               /* inputs_changed    */
};

tempo_capabilities_t caps = {0};
caps.flags = TEMPO_CAP_RESET | TEMPO_CAP_EARLY_RETURN;
tempo_scheduler_bind(sched, participant_id, &my_vtable, instance, &caps);
```

`adapters/mock/mock.c` is a complete worked example and is deliberately kept
readable for that reason.

## Exposing an adapter to Python and TOML

The control plane needs a `Participant` subclass whose `instantiate()` returns
`(vtable, handle, capabilities)`:

```python
from tempo import Participant
from tempo._ffi import ffi, lib
from tempo.config import register_participant_type

class MySimulator(Participant):
    kind = "my_simulator"

    def instantiate(self):
        self._handle = lib.my_simulator_new(self.config.encode())
        caps = ffi.new("tempo_capabilities_t *")
        caps.flags = int(Capability.RESET | Capability.EARLY_RETURN)
        return lib.my_simulator_vtable(), self._handle, caps

    def release(self):
        if self._handle is not None:
            lib.my_simulator_free(self._handle)
            self._handle = None

register_participant_type("my_simulator", lambda name, entry, where:
                          MySimulator(name, config=entry["config"], ...))
```

Once registered, `type = "my_simulator"` works in a TOML experiment file. The
scheduler core still knows nothing about it.

## Out-of-process participants

Two forms are intended, and the scheduler core is independent of which one is
used:

```
in-process:      Scheduler --native call--> Participant
out-of-process:  Scheduler --IPC/shm-----> Participant Process
```

The out-of-process protocol and its shared-memory fast path are phase 4; see
[`roadmap.md`](roadmap.md). Nothing in the participant API needs to change for
it, which is the point of keeping the scheduler ignorant of transport.
