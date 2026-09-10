#!/usr/bin/env python3
"""The motivating configuration from the design document (S1).

    Firmware A writes an actuator command
            |
            v
    the physical world changes
            |
            v
    a sensor value changes
            |
            v
    Firmware B observes the new sensor value

Firmware A drives a motor. The world turns that command into an angular
velocity. Firmware B reads the resulting sensor value. Nothing here knows
about QEMU or FMI: the scheduler sees three participants and two connections,
and the mock adapter stands in for the real simulators until the FMI and
FastDyn adapters arrive (phases 5 and 6).

Run with:  python examples/motor_and_sensor.py
"""

from __future__ import annotations

import tempo
from tempo import EventType, Mock, Simulation


def build() -> Simulation:
    sim = Simulation(end_time="10ms", quantum="1ms", trace=True)

    # Firmware A: commands the motor at 3 ms and again at 7 ms. Each write is
    # a synchronizing event, so it traps out to the scheduler (S10).
    firmware_a = sim.add(
        Mock(
            "firmware_a",
            script="3ms:emit_sync 0 0.70; 7ms:emit_sync 0 0.35",
            outputs={"motor_command": "float64"},
        )
    )

    # The world: turns a motor command into an angular velocity. `echo` makes
    # it a first-order model -- output = input * gain -- which is enough to
    # show the causal chain without pretending to be Modelica.
    world = sim.add(
        Mock(
            "world",
            echo=True,
            gain=3.2,
            inputs={"motor_command": "float64"},
            outputs={"angular_velocity": "float64"},
        )
    )

    # Firmware B: reads the sensor. It does no scripted work; it just observes.
    firmware_b = sim.add(
        Mock(
            "firmware_b",
            echo=True,
            inputs={"gyro": "float64"},
            outputs={"observed": "float64"},
        )
    )

    # Each hop takes time. A delay is not a formality: it is what lets both
    # ends run freely between communication points instead of synchronizing
    # because time passed (Rule 3).
    sim.connect(firmware_a["motor_command"], world["motor_command"], delay="500us")
    sim.connect(world["angular_velocity"], firmware_b["gyro"], delay="500us")
    return sim


def main() -> None:
    with build() as sim:
        print("compiled plan")
        print("-" * 72)
        print(sim.explain(), end="")

        sim.run()

        print()
        print(f"finished at {tempo.format_duration(sim.now)}")
        print(f"  firmware_a commanded  {sim.value('firmware_a.motor_command'):.3f}")
        print(f"  world produced        {sim.value('world.angular_velocity'):.3f}")
        print(f"  firmware_b observed   {sim.value('firmware_b.observed'):.3f}")

        print("\nscheduler")
        print("-" * 72)
        for key, value in sim.stats.items():
            print(f"  {key:<18} {value}")

        # The point of the trace is that the causal chain above is visible
        # afterwards, not that it was printed while it happened (S23).
        print("\nthe command's journey through the federation")
        print("-" * 72)
        interesting = sim.trace.events(
            type=[EventType.PORT_WRITE, EventType.ROUTE_DELIVER]
        )
        names = [p.name for p in sim.plan.participants]
        for event in interesting:
            who = names[event.participant] if event.participant < len(names) else "-"
            what = "wrote" if event.type == EventType.PORT_WRITE else "received"
            print(f"  {tempo.format_duration(event.time):>8}  {who:<12} {what}")


if __name__ == "__main__":
    main()
