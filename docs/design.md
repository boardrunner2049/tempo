# The design document

This project implements the design document

> **FastDyn-Friendly High-Performance Co-Simulation Scheduler**

Section references of the form `S1` … `S52` throughout the source and the rest
of `docs/` point at that document's numbered sections, and the seven rules in
its section 51 are cited as `Rule 1` … `Rule 7`.

The document itself is not reproduced here. Place it at this path so the
references resolve for anyone reading the code.

For quick orientation, the rules the implementation is answerable to:

1. **The scheduler core must not know what QEMU is.** Simulator-specific
   control belongs in an adapter.
2. **Python must not be in the scheduler hot path.** Python is for
   configuration and analysis.
3. **Do not synchronize simply because time passed.** Synchronize because the
   policy or a causal interaction requires it.
4. **Observable does not mean synchronizing.** Millions of events must be
   recordable without stopping a participant millions of times.
5. **Resolve everything possible before execution.** Strings, types, routing,
   dependencies, subscriptions, and topology are compiled before the run.
6. **The common path should be specialized and cheap.** Expensive generic
   logic runs only when necessary.
7. **Benchmark continuously.** An elegant scheduler that significantly slows
   FastDyn is not acceptable.

Where the implementation makes a decision the document leaves open, that
decision is written down and justified in the relevant file under `docs/` —
most of them in [`scheduling.md`](scheduling.md).
