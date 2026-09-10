"""``tempo-run`` -- run an experiment described in TOML.

The native ``tempo-trace`` tool converts trace files; this one builds and runs
a federation. Both live outside the runtime, because configuration and
reporting are control-plane work.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from ._ffi import format_duration, version
from .config import ConfigError, dump_event_types
from .errors import CompileError, TempoError
from .simulation import Simulation


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="tempo-run",
        description="Run a tempo co-simulation experiment.",
        epilog="event names for [[subscriptions]]: " + dump_event_types(),
    )
    parser.add_argument("config", type=Path, help="experiment file (TOML)")
    parser.add_argument(
        "--explain",
        action="store_true",
        help="compile and print the plan without running anything",
    )
    parser.add_argument(
        "--until",
        metavar="TIME",
        help="stop at this logical time instead of the configured end_time",
    )
    parser.add_argument(
        "--trace",
        metavar="FILE",
        type=Path,
        help="write the trace to FILE (implies a trace buffer)",
    )
    parser.add_argument(
        "--dump-trace",
        metavar="N",
        type=int,
        nargs="?",
        const=50,
        help="print the first N trace records (default 50)",
    )
    parser.add_argument("--stats", action="store_true", help="print scheduler counters")
    parser.add_argument("--version", action="version", version=f"tempo {version()}")
    return parser


def main(argv: "list[str] | None" = None) -> int:
    args = _build_parser().parse_args(argv)

    try:
        sim = Simulation.from_toml(args.config)
    except ConfigError as exc:
        print(f"tempo-run: {args.config}: {exc}", file=sys.stderr)
        return 2
    except OSError as exc:
        print(f"tempo-run: {exc}", file=sys.stderr)
        return 2

    # Asking for trace output is enough to want a trace buffer; not making the
    # user say it twice.
    if (args.trace or args.dump_trace) and not sim._trace_request:
        sim._trace_request = 1 << 18

    with sim:
        try:
            plan = sim.compile()
        except (CompileError, TempoError) as exc:
            print(f"tempo-run: {args.config}: {exc}", file=sys.stderr)
            return 1

        if args.explain:
            print(plan.describe(), end="")
            return 0

        try:
            sim.run(args.until)
        except TempoError as exc:
            print(f"tempo-run: {exc}", file=sys.stderr)
            return 1

        print(
            f"finished at {format_duration(sim.now)}"
            + ("" if sim.finished else " (stopped early)")
        )

        if args.stats:
            print("\nscheduler:")
            for key, value in sim.stats.items():
                print(f"  {key:<18} {value}")

        trace = sim.trace
        if trace is not None:
            if trace.dropped:
                print(
                    f"\nwarning: the trace dropped {trace.dropped} record(s); "
                    "raise [trace].capacity",
                    file=sys.stderr,
                )
            if args.dump_trace:
                print(f"\ntrace ({len(trace)} records):")
                print(trace.dump(limit=args.dump_trace))
            if args.trace:
                trace.save(args.trace)
                print(f"\nwrote {len(trace)} records to {args.trace}")
        elif args.trace or args.dump_trace:
            print(
                "tempo-run: no trace was recorded; enable [trace] in the config",
                file=sys.stderr,
            )
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
