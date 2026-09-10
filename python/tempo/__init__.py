"""tempo -- a co-simulation scheduler.

tempo coordinates independently executing simulators under one notion of
logical time: firmware under QEMU, physical models, FMI components, network
simulators, or anything else whose execution can be advanced.

The runtime is native and the control plane is Python. Experiments are
declared here, compiled once into an execution plan, and then run by the C
scheduler; Python is never in the scheduler's loop.

    from tempo import Simulation, Mock

    sim = Simulation(end_time="10ms", quantum="1ms", trace=True)
    a = sim.add(Mock("controller", script="3ms:emit_sync 0 0.7",
                     outputs={"motor": "float64"}))
    b = sim.add(Mock("world", echo=True, gain=0.5,
                     inputs={"motor": "float64"},
                     outputs={"gyro": "float64"}))
    sim.connect(a["motor"], b["motor"])
    sim.run()
    print(sim.value("world.gyro"), sim.stats)
"""

from __future__ import annotations

from ._ffi import format_duration, parse_duration, version
from .config import ConfigError, register_participant_type
from .errors import CompileError, SchedulerError, TempoError
from .participants import Mock, Participant, Port, PortRef
from .plan import Plan, PlanParticipant, PlanPort, PlanRoute
from .simulation import Simulation
from .trace import Event, Trace, TraceFile
from .types import (
    Capability,
    EventFlag,
    EventNamespace,
    EventType,
    PortDir,
    PortType,
    StopReason,
)

__version__ = version()

__all__ = [
    "Capability",
    "CompileError",
    "ConfigError",
    "Event",
    "EventFlag",
    "EventNamespace",
    "EventType",
    "Mock",
    "Participant",
    "Plan",
    "PlanParticipant",
    "PlanPort",
    "PlanRoute",
    "Port",
    "PortDir",
    "PortRef",
    "PortType",
    "SchedulerError",
    "Simulation",
    "StopReason",
    "TempoError",
    "Trace",
    "TraceFile",
    "__version__",
    "format_duration",
    "parse_duration",
    "register_participant_type",
    "version",
]
