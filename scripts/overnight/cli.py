"""Command-line interface and configuration parsing.

Uses only ``argparse`` and ``os.environ`` from the standard library.
"""

from __future__ import annotations

import argparse
import math
import os
from typing import List, Optional

from .model import Config


def parse_args(argv: Optional[List[str]] = None) -> Config:
    """Parse CLI arguments and environment variables, returning a ``Config``.

    Priority: CLI > environment variable > default.
    """
    parser = argparse.ArgumentParser(
        prog="overnight-local",
        description="Sluice local overnight correctness gate (Linux + Clang).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Modes:
  (default)  Full ~8h gate (Debug soak, TSan, ASan+UBSan, libFuzzer, Final Debug).
  --smoke    One pass of every phase; each fuzz target ~10s.
  --self-test  Controlled synthetic failures; never produces an overnight HOLD.

Environment overrides (CLI wins):
  SLUICE_OVERNIGHT_HOURS   total budget hours (default 8)
  SLUICE_PHASE_TIMEOUT     per-command timeout seconds (default 1200)
  SLUICE_FUZZ_SECONDS      override total fuzz budget seconds
  SLUICE_KEEP_GOING        0 = stop after current command (default 1)

Verdicts:
  PASS       exit 0   All gates passed.
  HOLD       exit 1   Any sticky failure.
  ENVIRONMENT_ERROR  exit 2   Missing tools or bad environment.
  RUNNER_ERROR  exit 3   Internal runner error.
  INCOMPLETE exit 4   No failures but not all evidence families ran.
        """,
    )

    parser.add_argument(
        "--smoke",
        action="store_true",
        help="One pass of every phase (NOT a correctness gate).",
    )
    parser.add_argument(
        "--hours",
        type=float,
        default=None,
        help="Total budget in hours (default 8, env SLUICE_OVERNIGHT_HOURS).",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        dest="self_test",
        help="Controlled synthetic failures; proves runner wiring.",
    )

    args = parser.parse_args(argv)

    # Determine mode.
    mode = "overnight"
    if args.smoke:
        mode = "smoke"
    if args.self_test:
        mode = "selftest"

    # Hours: CLI > env > default.
    hours: float = 8.0
    hours_source = "default"
    if args.hours is not None:
        hours = _validate_hours(args.hours, "--hours")
        hours_source = "cli"
    elif "SLUICE_OVERNIGHT_HOURS" in os.environ:
        hours = _validate_hours(
            float(os.environ["SLUICE_OVERNIGHT_HOURS"]),
            "SLUICE_OVERNIGHT_HOURS",
        )
        hours_source = "env"

    # Phase timeout: env > default.
    phase_timeout: int = 1200
    timeout_source = "default"
    if "SLUICE_PHASE_TIMEOUT" in os.environ:
        phase_timeout = _validate_positive_int(
            os.environ["SLUICE_PHASE_TIMEOUT"], "SLUICE_PHASE_TIMEOUT"
        )
        timeout_source = "env"

    # Fuzz seconds override: env > default.
    fuzz_override: Optional[int] = None
    fuzz_source = "default"
    if "SLUICE_FUZZ_SECONDS" in os.environ:
        fuzz_override = _validate_non_negative_int(
            os.environ["SLUICE_FUZZ_SECONDS"], "SLUICE_FUZZ_SECONDS"
        )
        fuzz_source = "env"

    # Keep going: env > default.
    keep_going = True
    keep_going_source = "default"
    if "SLUICE_KEEP_GOING" in os.environ:
        val = os.environ["SLUICE_KEEP_GOING"].strip()
        if val == "0":
            keep_going = False
        elif val == "1":
            keep_going = True
        else:
            raise ValueError(
                f"SLUICE_KEEP_GOING must be 0 or 1, got: {val!r}"
            )
        keep_going_source = "env"

    # Smoke mode adjustments.
    if mode == "smoke":
        if args.hours is None and "SLUICE_OVERNIGHT_HOURS" not in os.environ:
            hours = 1.0
            hours_source = "smoke-default"
        if fuzz_override is None:
            fuzz_override = 30
            fuzz_source = "smoke-default"

    return Config(
        mode=mode,
        hours=hours,
        phase_timeout_seconds=phase_timeout,
        fuzz_seconds_override=fuzz_override,
        keep_going=keep_going,
        hours_source=hours_source,
        timeout_source=timeout_source,
        fuzz_source=fuzz_source,
        keep_going_source=keep_going_source,
    )


# ═══════════════════════════════════════════════════════════════════════════════
# Validation helpers
# ═══════════════════════════════════════════════════════════════════════════════

def _validate_hours(value: float, source: str) -> float:
    if value <= 0:
        raise ValueError(f"{source}: must be > 0, got {value}")
    if not _isfinite(value):
        raise ValueError(f"{source}: must be a finite number, got {value}")
    return value


def _validate_positive_int(raw: str, source: str) -> int:
    try:
        val = int(raw)
    except (ValueError, TypeError):
        raise ValueError(f"{source}: must be a positive integer, got {raw!r}")
    if val <= 0:
        raise ValueError(f"{source}: must be > 0, got {val}")
    return val


def _validate_non_negative_int(raw: str, source: str) -> int:
    try:
        val = int(raw)
    except (ValueError, TypeError):
        raise ValueError(f"{source}: must be a non-negative integer, got {raw!r}")
    if val < 0:
        raise ValueError(f"{source}: must be >= 0, got {val}")
    return val


def _isfinite(value: float) -> bool:
    return math.isfinite(value)