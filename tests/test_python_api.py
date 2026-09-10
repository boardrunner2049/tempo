"""The Python control plane, end to end."""

from __future__ import annotations

import pytest

import tempo
from tempo import CompileError, Mock, Simulation, TempoError


def basic(**kwargs) -> Simulation:
    """controller.motor -> world.motor, world echoing at half gain."""
    sim = Simulation(end_time="10ms", quantum="1ms", **kwargs)
    a = sim.add(Mock("controller", script="3ms:emit_sync 0 0.7",
                     outputs={"motor": "float64"}))
    b = sim.add(Mock("world", echo=True, gain=0.5,
                     inputs={"motor": "float64"}, outputs={"gyro": "float64"}))
    sim.connect(a["motor"], b["motor"])
    return sim


def test_runs_and_propagates():
    with basic() as sim:
        sim.run()
        assert sim.finished
        assert sim.now == tempo.parse_duration("10ms")
        assert sim.value("world.motor") == pytest.approx(0.7)
        assert sim.value("world.gyro") == pytest.approx(0.35)
        assert sim.stats["deliveries"] == 1
        assert sim.stats["windows"] == 10


def test_compile_resolves_names_to_indices():
    with basic() as sim:
        plan = sim.compile()
        assert [p.name for p in plan.participants] == ["controller", "world"]
        assert plan.execution_order == ["controller", "world"]
        (route,) = plan.routes
        assert route.source == "controller.motor"
        assert route.dest == "world.motor"
        # An unspecified delay follows the quantum.
        assert route.delay_ns == plan.quantum
        assert not route.same_time
        assert "controller.motor -> world.motor" in plan.describe()


def test_same_time_connection_uses_microsteps():
    sim = Simulation(end_time="5ms", quantum="1ms")
    a = sim.add(Mock("a", script="2ms:emit_sync 0 1.0", outputs={"o": "float64"}))
    b = sim.add(Mock("b", echo=True, gain=3.0,
                     inputs={"i": "float64"}, outputs={"o": "float64"}))
    sim.connect(a["o"], b["i"], delay=0)
    with sim:
        plan = sim.compile()
        assert plan.routes[0].same_time
        sim.run()
        # The value crossed the connection without physical time passing.
        assert sim.value("b.o") == pytest.approx(3.0)
        assert sim.stats["max_microstep"] >= 1


def test_zero_delay_needs_a_capable_destination():
    sim = Simulation(end_time="5ms", quantum="1ms")
    a = sim.add(Mock("a", script="1ms:emit_sync 0 1.0", outputs={"o": "float64"}))
    b = sim.add(Mock("b", inputs={"i": "float64"}))   # echo=False -> cannot iterate
    sim.connect(a["o"], b["i"], delay=0)
    with sim, pytest.raises(TempoError, match="same logical time"):
        sim.run()


def test_step_by_step():
    with basic() as sim:
        steps = 0
        while sim.step():
            steps += 1
            assert steps < 1000, "stepping did not terminate"
        assert sim.finished
        assert sim.now == tempo.parse_duration("10ms")


def test_run_until_then_continue():
    with basic() as sim:
        sim.run("4ms")
        assert sim.now == tempo.parse_duration("4ms")
        assert not sim.finished
        sim.run()
        assert sim.finished
        with pytest.raises(TempoError):
            sim.run("20ms")


def test_reset_reproduces_the_run():
    with basic(trace=True) as sim:
        sim.run()
        first = [str(e) for e in sim.trace]
        assert first

        sim.reset()
        assert sim.now == 0
        assert not sim.finished
        assert sim.stats["windows"] == 0
        assert sim.value("world.gyro") == pytest.approx(0.0)

        sim.trace.clear()
        sim.run()
        # Sequence numbers keep counting across a clear, so compare the parts
        # that describe behaviour rather than the record index.
        second = [str(e).split(maxsplit=1)[1] for e in sim.trace]
        assert second == [line.split(maxsplit=1)[1] for line in first]


def test_early_return_is_visible_in_the_stats():
    sim = Simulation(end_time="10ms", quantum="1ms")
    sim.add(Mock("a", script="3500us:trap"))
    with sim:
        sim.run()
        assert sim.stats["early_returns"] == 1


def test_prediction_removes_the_early_return():
    sim = Simulation(end_time="10ms", quantum="1ms")
    sim.add(Mock("a", script="3500us:trap", predict=True))
    with sim:
        sim.run()
        assert sim.stats["early_returns"] == 0


def test_participants_can_run_ahead_of_global_time():
    sim = Simulation(end_time="10ms", quantum="1ms")
    sim.add(Mock("fast"))
    sim.add(Mock("slow", max_step="250us"))
    with sim:
        sim.run("2ms")
        # The coarse-stepping participant is the one that holds time back.
        assert sim.participant_time("fast") >= sim.now


def test_finish_ends_the_run():
    sim = Simulation(end_time="10ms", quantum="1ms")
    sim.add(Mock("a", script="2ms:finish"))
    with sim:
        sim.run()
        assert sim.finished
        assert sim.now == tempo.parse_duration("2ms")


def test_bytes_port():
    sim = Simulation(end_time="5ms", quantum="1ms")
    a = sim.add(Mock("a", script="1ms:emit_sync 0 1.5",
                     outputs={"frame": ("bytes", 32)}))
    b = sim.add(Mock("b", inputs={"frame": {"type": "bytes", "max_bytes": 32}}))
    sim.connect(a["frame"], b["frame"])
    with sim:
        sim.run()
        payload = sim.value("b.frame")
        assert isinstance(payload, bytes)
        assert len(payload) == 8


def test_diverging_same_time_cycle_is_reported():
    sim = Simulation(end_time="5ms", quantum="1ms", max_microsteps=8)
    a = sim.add(Mock("a", script="1ms:emit_sync 0 1.0", echo=True, gain=2.0,
                     inputs={"fb": "float64"}, outputs={"o": "float64"}))
    b = sim.add(Mock("b", echo=True, gain=2.0,
                     inputs={"i": "float64"}, outputs={"o": "float64"}))
    sim.connect(a["o"], b["i"], delay=0)
    sim.connect(b["o"], a["fb"], delay=0)
    with sim, pytest.raises(TempoError, match="converge"):
        sim.run()


# --- configuration mistakes ------------------------------------------------


def test_unknown_participant_in_a_connection():
    sim = Simulation(end_time="1ms")
    sim.add(Mock("a", outputs={"o": "float64"}))
    with pytest.raises(ValueError, match="no participant 'ghost'"):
        sim.connect("ghost.o", "a.o")


def test_connecting_to_an_output_is_refused():
    sim = Simulation(end_time="1ms")
    a = sim.add(Mock("a", outputs={"o": "float64"}))
    b = sim.add(Mock("b", outputs={"o": "float64"}))
    with pytest.raises(ValueError, match="no input port"):
        sim.connect(a["o"], b["o"])


def test_type_mismatch_is_caught_at_compile_time():
    sim = Simulation(end_time="1ms")
    a = sim.add(Mock("a", outputs={"o": "float64"}))
    b = sim.add(Mock("b", inputs={"i": "int64"}))
    sim.connect(a["o"], b["i"])
    with pytest.raises(CompileError) as excinfo:
        sim.compile()
    assert excinfo.value.location == "connections[0]"
    assert "float64" in str(excinfo.value)


def test_two_writers_into_one_input():
    sim = Simulation(end_time="1ms")
    a = sim.add(Mock("a", outputs={"o": "float64"}))
    b = sim.add(Mock("b", outputs={"o": "float64"}))
    c = sim.add(Mock("c", inputs={"i": "float64"}))
    sim.connect(a["o"], c["i"])
    sim.connect(b["o"], c["i"])
    with pytest.raises(CompileError, match="already receives"):
        sim.compile()


def test_unknown_policy():
    sim = Simulation(end_time="1ms", policy="time_warp")
    sim.add(Mock("a"))
    with pytest.raises(CompileError) as excinfo:
        sim.compile()
    assert excinfo.value.location == "scheduler.policy"


def test_bad_port_name_and_missing_port():
    with pytest.raises(ValueError, match="unknown port type"):
        Mock("a", outputs={"o": "quaternion"})
    m = Mock("a", outputs={"o": "float64"})
    with pytest.raises(KeyError, match="no port 'nope'"):
        m["nope"]


def test_bad_duration():
    with pytest.raises(ValueError, match="not a duration"):
        Simulation(end_time="10 furlongs")


def test_bad_mock_script():
    sim = Simulation(end_time="1ms")
    sim.add(Mock("a", script="3ms:levitate"))
    with sim, pytest.raises(TempoError, match="unknown action"):
        sim.start()


def test_cannot_modify_after_start():
    with basic() as sim:
        sim.start()
        with pytest.raises(RuntimeError, match="after the simulation has started"):
            sim.add(Mock("late"))


def test_duplicate_participant_name():
    sim = Simulation(end_time="1ms")
    sim.add(Mock("a"))
    with pytest.raises(ValueError, match="already a participant"):
        sim.add(Mock("a"))
