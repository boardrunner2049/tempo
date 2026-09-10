"""TOML experiment files."""

from __future__ import annotations

import textwrap

import pytest

import tempo
from tempo import Simulation
from tempo.config import ConfigError, from_dict

GOOD = """
[simulation]
end_time = "10ms"

[scheduler]
policy = "conservative"
quantum = "1ms"
default_delay = "1ms"
max_microsteps = 50

[trace]
enabled = true
capacity = 4096

[[participants]]
name = "flight"
type = "mock"
script = "3ms:emit_sync 0 0.7"
outputs = { motor_command = "float64" }
inputs = { gyro = "float64" }

[[participants]]
name = "world"
type = "mock"
echo = true
gain = 0.5
inputs = { motor_command = "float64" }
outputs = { gyro = "float64" }

[[connections]]
from = "flight.motor_command"
to = "world.motor_command"

[[connections]]
from = "world.gyro"
to = "flight.gyro"
delay = "500us"

[[subscriptions]]
participant = "flight"
event = "port_write"
"""


def write(tmp_path, text: str):
    path = tmp_path / "experiment.toml"
    path.write_text(textwrap.dedent(text))
    return path


def test_loads_and_runs(tmp_path):
    sim = Simulation.from_toml(write(tmp_path, GOOD))
    with sim:
        plan = sim.compile()
        assert plan.end_time == tempo.parse_duration("10ms")
        assert plan.quantum == tempo.parse_duration("1ms")
        assert plan.max_microsteps == 50
        assert [p.name for p in plan.participants] == ["flight", "world"]
        assert len(plan.routes) == 2
        assert plan.routes[1].delay_ns == tempo.parse_duration("500us")
        sim.run()
        assert sim.finished
        assert sim.value("world.motor_command") == pytest.approx(0.7)
        # A subscription was declared, so a trace buffer exists and recorded.
        assert sim.trace is not None
        assert len(sim.trace) > 0


def test_trace_disabled_by_default():
    sim = from_dict(
        {
            "simulation": {"end_time": "1ms"},
            "participants": [{"name": "a", "type": "mock"}],
        }
    )
    with sim:
        sim.run()
        assert sim.trace is None


def test_end_time_is_required():
    with pytest.raises(ConfigError) as excinfo:
        from_dict({"simulation": {}, "participants": [{"name": "a", "type": "mock"}]})
    assert excinfo.value.location == "simulation"


def test_participants_are_required():
    with pytest.raises(ConfigError, match="at least one participant"):
        from_dict({"simulation": {"end_time": "1ms"}})


def test_unknown_participant_type_lists_what_exists():
    with pytest.raises(ConfigError) as excinfo:
        from_dict(
            {
                "simulation": {"end_time": "1ms"},
                "participants": [{"name": "a", "type": "fastdyn"}],
            }
        )
    assert "registered types: mock" in str(excinfo.value)
    assert excinfo.value.location == "participants[0]"


def test_a_typo_is_an_error_not_a_silent_default():
    """A misspelled key must not quietly change what runs (S14)."""
    with pytest.raises(ConfigError) as excinfo:
        from_dict(
            {
                "simulation": {"end_time": "1ms"},
                "scheduler": {"quantom": "1ms"},
                "participants": [{"name": "a", "type": "mock"}],
            }
        )
    assert "'quantom'" in str(excinfo.value)
    assert excinfo.value.location == "scheduler"

    with pytest.raises(ConfigError, match="'scipt'"):
        from_dict(
            {
                "simulation": {"end_time": "1ms"},
                "participants": [{"name": "a", "type": "mock", "scipt": "1ms:trap"}],
            }
        )

    with pytest.raises(ConfigError, match="'dely'"):
        from_dict(
            {
                "simulation": {"end_time": "1ms"},
                "participants": [
                    {"name": "a", "type": "mock", "outputs": {"o": "float64"}},
                    {"name": "b", "type": "mock", "inputs": {"i": "float64"}},
                ],
                "connections": [{"from": "a.o", "to": "b.i", "dely": "1ms"}],
            }
        )


def test_connection_needs_both_ends():
    with pytest.raises(ConfigError, match="both 'from' and 'to'"):
        from_dict(
            {
                "simulation": {"end_time": "1ms"},
                "participants": [{"name": "a", "type": "mock"}],
                "connections": [{"from": "a.o"}],
            }
        )


def test_connection_errors_carry_their_index():
    with pytest.raises(ConfigError) as excinfo:
        from_dict(
            {
                "simulation": {"end_time": "1ms"},
                "participants": [
                    {"name": "a", "type": "mock", "outputs": {"o": "float64"}},
                    {"name": "b", "type": "mock", "inputs": {"i": "float64"}},
                ],
                "connections": [
                    {"from": "a.o", "to": "b.i"},
                    {"from": "a.missing", "to": "b.i"},
                ],
            }
        )
    assert excinfo.value.location == "connections[1]"


def test_bad_event_name_in_a_subscription():
    with pytest.raises(ConfigError) as excinfo:
        from_dict(
            {
                "simulation": {"end_time": "1ms"},
                "participants": [{"name": "a", "type": "mock"}],
                "subscriptions": [{"event": "telepathy"}],
            }
        )
    assert excinfo.value.location == "subscriptions[0]"


def test_malformed_toml_reports_the_file(tmp_path):
    path = write(tmp_path, "[simulation\nend_time = 1")
    with pytest.raises(ConfigError):
        Simulation.from_toml(path)


def test_registering_an_adapter_type():
    from tempo.config import PARTICIPANT_TYPES, register_participant_type
    from tempo.participants import Mock

    def factory(name, entry, location):
        return Mock(name, outputs={"o": "float64"})

    register_participant_type("pretend-fmi", factory)
    try:
        sim = from_dict(
            {
                "simulation": {"end_time": "1ms"},
                "participants": [{"name": "m", "type": "pretend-fmi"}],
            }
        )
        with sim:
            assert sim.compile().participants[0].type == "mock"
    finally:
        PARTICIPANT_TYPES.pop("pretend-fmi")
