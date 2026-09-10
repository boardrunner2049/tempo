# FMI adapter

Reserved for phase 5.

FMI is the right first real adapter because it exercises exactly the parts of
the participant contract that mocks cannot: variable stepping, early return,
discrete events, and physical state.

FMI semantics stay entirely inside this directory. The scheduler continues to
see only `advance_to(T)` and a set of typed slots — it must not learn what an
FMU is.

Event types belong in the `TEMPO_EV_NS_FMI` namespace (`0x0200`–`0x02FF`), so
FMI detail reaches the trace without the core reserving names for it.

See [`../../docs/adapters.md`](../../docs/adapters.md) for the contract.
