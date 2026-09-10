"""Exceptions raised by the tempo control plane."""

from __future__ import annotations

from ._ffi import ffi, status_str


class TempoError(Exception):
    """Base class for every tempo failure."""

    def __init__(self, message: str, status: int | None = None) -> None:
        super().__init__(message)
        self.status = status


class CompileError(TempoError):
    """The federation configuration is invalid.

    Carries the location the compiler blamed, so a front end can point at the
    offending line of TOML rather than at the whole file.
    """

    def __init__(self, diag) -> None:
        self.location = ffi.string(diag.location).decode()
        message = ffi.string(diag.message).decode()
        self.detail = message
        where = f" at {self.location}" if self.location else ""
        super().__init__(f"{message}{where}", int(diag.status))


class SchedulerError(TempoError):
    """The run failed."""


def check(status: int, what: str) -> None:
    """Raise if a C call returned a negative status."""
    if status < 0:
        raise TempoError(f"{what}: {status_str(status)}", status)
