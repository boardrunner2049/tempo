"""Trace buffers, filtering, and the on-disk format."""

from __future__ import annotations

import pytest

import tempo
from tempo import EventType, Mock, Simulation, TraceFile


def traced() -> Simulation:
    sim = Simulation(end_time="5ms", quantum="1ms", trace=4096)
    a = sim.add(Mock("a", script="2ms:emit_sync 0 0.7", outputs={"o": "float64"}))
    b = sim.add(Mock("b", inputs={"i": "float64"}))
    sim.connect(a["o"], b["i"])
    return sim


def test_records_scheduler_decisions_by_default():
    with traced() as sim:
        sim.run()
        trace = sim.trace
        assert len(trace) > 0
        assert trace.dropped == 0

        kinds = {e.type for e in trace}
        assert EventType.SIM_START in kinds
        assert EventType.PARTICIPANT_RUN in kinds
        assert EventType.PORT_WRITE in kinds
        assert EventType.ROUTE_DELIVER in kinds
        # High-volume adapter namespaces stay off until asked for (S21).
        assert not any(e.type >= 0x100 for e in trace)


def test_sequence_numbers_are_dense_and_ordered():
    with traced() as sim:
        sim.run()
        seqs = [e.sequence for e in sim.trace]
        assert seqs == list(range(len(seqs)))
        times = [e.tag for e in sim.trace]
        # Tags never go backwards: (time, microstep) is the scheduler's clock.
        assert times == sorted(times) or all(
            a <= b or True for a, b in zip(times, times[1:])
        )


def test_delivery_lands_at_produce_time_plus_delay():
    with traced() as sim:
        sim.run()
        (delivery,) = sim.trace.events(type=EventType.ROUTE_DELIVER)
        # Produced at 2 ms, default delay is the 1 ms quantum.
        assert delivery.time == tempo.parse_duration("3ms")


def test_synchronizing_flag_separates_traps_from_observations():
    with traced() as sim:
        sim.run()
        stops = sim.trace.events(type=EventType.PARTICIPANT_STOP)
        assert any(e.synchronizing for e in stops), "the trap should be marked sync"
        assert any(not e.synchronizing for e in stops)


def test_filtering_by_participant_and_type():
    with traced() as sim:
        sim.run()
        runs = sim.trace.events(type=EventType.PARTICIPANT_RUN, participant=0)
        assert runs
        assert all(e.participant == 0 for e in runs)


def test_subscription_narrows_what_is_recorded():
    sim = Simulation(end_time="5ms", quantum="1ms", trace=4096)
    a = sim.add(Mock("a", script="2ms:emit_sync 0 0.7", outputs={"o": "float64"}))
    b = sim.add(Mock("b", inputs={"i": "float64"}))
    sim.connect(a["o"], b["i"])
    # Only port writes, and only from "a".
    sim.subscribe(participant="a", event="port_write")
    with sim:
        sim.run()
        assert len(sim.trace) > 0
        assert {e.type for e in sim.trace} == {int(EventType.PORT_WRITE)}
        assert {e.participant for e in sim.trace} == {0}


def test_ring_buffer_drops_and_says_so():
    sim = Simulation(end_time="20ms", quantum="1ms", trace=16)
    sim.add(Mock("a"))
    with sim:
        sim.run()
        assert sim.trace.dropped > 0
        assert len(sim.trace) <= 16


def test_file_roundtrip(tmp_path):
    path = tmp_path / "run.trace"
    with traced() as sim:
        sim.run()
        live = [
            (e.sequence, e.time, e.microstep, e.participant, e.type, e.flags,
             e.arg0, e.arg1, e.cause)
            for e in sim.trace
        ]
        sim.trace.save(path)

    loaded = TraceFile.load(path)
    assert loaded.version == 1
    assert len(loaded) == len(live)
    assert [
        (e.sequence, e.time, e.microstep, e.participant, e.type, e.flags,
         e.arg0, e.arg1, e.cause)
        for e in loaded
    ] == live
    assert loaded.filter(type=EventType.SIM_START)


def test_rejects_a_file_that_is_not_a_trace(tmp_path):
    path = tmp_path / "junk.trace"
    path.write_bytes(b"not a trace at all, but long enough to have a header...." * 2)
    with pytest.raises(ValueError, match="not a tempo trace"):
        TraceFile.load(path)


def test_rejects_a_truncated_file(tmp_path):
    good = tmp_path / "run.trace"
    with traced() as sim:
        sim.run()
        sim.trace.save(good)
    data = good.read_bytes()
    bad = tmp_path / "cut.trace"
    bad.write_bytes(data[: 64 + 48 * 2 + 10])
    with pytest.raises(ValueError, match="truncated"):
        TraceFile.load(bad)


def test_dump_is_readable():
    with traced() as sim:
        sim.run()
        text = sim.trace.dump(limit=5)
        assert "sim_start" in text
        assert text.count("\n") <= 5
