"""The Python bindings hand-maintain the C declarations, so check them.

ABI mode means cffi trusts what ``_ffi.py`` says about struct layout. A silent
mismatch would corrupt values rather than raise, so these are the tests that
keep that from happening quietly.
"""

from __future__ import annotations

from tempo._ffi import ffi, lib
from tempo.trace import RECORD_SIZE, _FILE_HEADER


def test_struct_sizes_match_the_wire_format():
    # These two are part of the trace file format, not implementation detail.
    assert ffi.sizeof("tempo_event_header_t") == 24
    assert ffi.sizeof("tempo_event_t") == 48
    assert ffi.sizeof("tempo_event_t") == RECORD_SIZE
    assert _FILE_HEADER.size == 64


def test_field_offsets():
    off = ffi.offsetof
    assert off("tempo_event_t", "hdr") == 0
    assert off("tempo_event_t", "microstep") == 24
    assert off("tempo_event_t", "arg0") == 28
    assert off("tempo_event_t", "arg1") == 32
    assert off("tempo_event_t", "cause") == 40

    assert off("tempo_slot_t", "generation") == 0
    assert off("tempo_slot_t", "len") == 8
    assert off("tempo_slot_t", "capacity") == 12
    assert off("tempo_slot_t", "scalar") == 16
    assert off("tempo_slot_t", "bytes") == 24

    assert off("tempo_route_t", "delay") == 8
    assert off("tempo_tag_t", "microstep") == 8


def test_scalar_union_is_eight_bytes():
    assert ffi.sizeof("tempo_scalar_t") == 8


def test_duration_parsing_agrees_with_c():
    from tempo import format_duration, parse_duration

    for text, ns in [
        ("100ns", 100),
        ("100us", 100_000),
        ("3.5ms", 3_500_000),
        ("10s", 10_000_000_000),
        ("250", 250),
    ]:
        assert parse_duration(text) == ns

    assert format_duration(0) == "0s"
    assert format_duration(100_000) == "100us"
    assert format_duration(10**9) == "1s"


def test_struct_by_value_calls_work():
    """cffi ABI mode routes these through libffi; make sure that holds."""
    err = ffi.new("const char **")
    mock = lib.tempo_mock_from_script(b"1ms:trap", 0, False, 1.0, err)
    assert mock != ffi.NULL
    try:
        caps = lib.tempo_mock_capabilities(mock)
        # RESET | EARLY_RETURN, per the mock's documented defaults.
        assert caps.flags & 0x01
        assert caps.flags & 0x08
    finally:
        lib.tempo_mock_free(mock)


def test_status_strings_are_available():
    from tempo._ffi import status_str

    assert status_str(0) == "ok"
    assert "memory" in status_str(-2)
