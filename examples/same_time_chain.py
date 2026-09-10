#!/usr/bin/env python3
"""Same-time propagation, and why microsteps exist (S17).

Three participants in a chain, connected with zero delay. A value written by
the first must reach the third within one logical instant -- physical time
does not advance between the hops. The scheduler resolves this with
microsteps: the tag goes (3ms, 0), (3ms, 1), (3ms, 2), and no artificial
nanosecond offsets are invented to force an ordering.

Run with:  python examples/same_time_chain.py
"""

from __future__ import annotations

import tempo
from tempo import EventType, Mock, Simulation


def main() -> None:
    sim = Simulation(end_time="6ms", quantum="1ms", trace=True)

    source = sim.add(
        Mock("source", script="3ms:emit_sync 0 1.0", outputs={"o": "float64"})
    )
    middle = sim.add(
        Mock("middle", echo=True, gain=2.0,
             inputs={"i": "float64"}, outputs={"o": "float64"})
    )
    sink = sim.add(
        Mock("sink", echo=True, gain=5.0,
             inputs={"i": "float64"}, outputs={"o": "float64"})
    )

    # delay=0 makes these same-time connections. The destinations must be able
    # to re-evaluate within one instant, which the scheduler checks before the
    # run rather than discovering during it.
    sim.connect(source["o"], middle["i"], delay=0)
    sim.connect(middle["o"], sink["i"], delay=0)

    with sim:
        plan = sim.compile()
        print("execution order:", " -> ".join(plan.execution_order))
        print("components:     ", plan.components)
        print("cyclic:         ", plan.cyclic_components or "none")
        print()

        sim.run()

        print(f"source wrote  {sim.value('source.o')}")
        print(f"middle sees   {sim.value('middle.i')} and wrote {sim.value('middle.o')}")
        print(f"sink   sees   {sim.value('sink.i')} and wrote {sim.value('sink.o')}")
        print(f"\ndeepest same-time iteration: {sim.stats['max_microstep']}")

        print("\nwhat happened at 3 ms, by tag")
        print("-" * 60)
        at_3ms = tempo.parse_duration("3ms")
        names = [p.name for p in plan.participants]
        for event in sim.trace:
            if event.time != at_3ms:
                continue
            if event.type not in (
                EventType.MICROSTEP,
                EventType.PORT_WRITE,
                EventType.ROUTE_DELIVER,
            ):
                continue
            who = names[event.participant] if event.participant < len(names) else "-"
            print(f"  (3ms, {event.microstep})  {who:<8} {event.type_name}")


if __name__ == "__main__":
    main()
