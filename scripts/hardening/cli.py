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
        prog="hardening-local",
        description="Sluice local hardening correctness gate (Linux + Clang).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Modes:
  (default)  Full ~8h gate (Debug soak, TSan, ASan+UBSan, libFuzzer, Final Debug).
  --smoke    One pass of every phase; each fuzz target ~10s.
  --version-b  Version B pipeline overnight gate: repeated Debug rounds
               (deterministic stress with rotating seeds, depth regressions,
               real-file integration, fault-injection error drains, scripted
               controller), then TSan and ASan+UBSan passes over the Version B
               targets, then Final Debug. Default 6h budget.
  --self-test  Controlled synthetic failures; never produces an hardening HOLD.

Environment overrides (CLI wins):
  SLUICE_HARDENING_HOURS   total budget hours (default 8)
  SLUICE_VERSION_B_NIGHTLY_SECONDS  Version B budget in seconds (default 6h)
  SLUICE_HARDENING_PHASE_TIMEOUT     per-command timeout seconds (default 1200)
  SLUICE_HARDENING_FUZZ_SECONDS      override total fuzz budget seconds
  SLUICE_HARDENING_KEEP_GOING        0 = stop after current command (default 1)
  SLUICE_HARDENING_HEARTBEAT_SECONDS  long-command heartbeat interval (default 60, 0 disables)

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
        help="Total budget in hours (default 8, env SLUICE_HARDENING_HOURS).",
    )
    parser.add_argument(
        "--version-b",
        action="store_true",
        help="Version B pipeline overnight gate (default 6h; env "
             "SLUICE_VERSION_B_NIGHTLY_SECONDS overrides in seconds).",
    )
    parser.add_argument(
        "--self-test",
        action="store_true",
        dest="self_test",
        help="Controlled synthetic failures; proves runner wiring.",
    )
    parser.add_argument(
        "--heartbeat-seconds",
        type=int,
        default=None,
        help="Heartbeat interval for long-running commands in seconds "
             "(default 60, 0 disables; env SLUICE_HARDENING_HEARTBEAT_SECONDS).",
    )

    args = parser.parse_args(argv)

    # Determine mode.
    mode = "hardening"
    if args.smoke:
        mode = "smoke"
    if args.version_b:
        mode = "version-b"
    if args.self_test:
        mode = "selftest"

    # Hours: CLI > (version-b env) > generic env > mode default.
    hours: float = 8.0
    hours_source = "default"
    if mode == "version-b":
        hours = 6.0
        hours_source = "version-b-default"
    if args.hours is not None:
        hours = _validate_hours(args.hours, "--hours")
        hours_source = "cli"
    elif mode == "version-b" and "SLUICE_VERSION_B_NIGHTLY_SECONDS" in os.environ:
        hours = _validate_hours(
            float(os.environ["SLUICE_VERSION_B_NIGHTLY_SECONDS"]) / 3600.0,
            "SLUICE_VERSION_B_NIGHTLY_SECONDS",
        )
        hours_source = "env"
    elif "SLUICE_HARDENING_HOURS" in os.environ:
        hours = _validate_hours(
            float(os.environ["SLUICE_HARDENING_HOURS"]),
            "SLUICE_HARDENING_HOURS",
        )
        hours_source = "env"

    # Phase timeout: env > default.
    phase_timeout: int = 1200
    timeout_source = "default"
    if "SLUICE_HARDENING_PHASE_TIMEOUT" in os.environ:
        phase_timeout = _validate_positive_int(
            os.environ["SLUICE_HARDENING_PHASE_TIMEOUT"], "SLUICE_HARDENING_PHASE_TIMEOUT"
        )
        timeout_source = "env"

    # Fuzz seconds override: env > default.
    fuzz_override: Optional[int] = None
    fuzz_source = "default"
    if "SLUICE_HARDENING_FUZZ_SECONDS" in os.environ:
        fuzz_override = _validate_non_negative_int(
            os.environ["SLUICE_HARDENING_FUZZ_SECONDS"], "SLUICE_HARDENING_FUZZ_SECONDS"
        )
        fuzz_source = "env"

    # Heartbeat seconds: CLI > env > default 60. 0 disables heartbeats.
    heartbeat: int = 60
    heartbeat_source = "default"
    if args.heartbeat_seconds is not None:
        # argparse already coerced to int; validate range (non-negative finite).
        heartbeat = _validate_non_negative_int(
            str(args.heartbeat_seconds), "--heartbeat-seconds")
        heartbeat_source = "cli"
    elif "SLUICE_HARDENING_HEARTBEAT_SECONDS" in os.environ:
        heartbeat = _validate_non_negative_int(
            os.environ["SLUICE_HARDENING_HEARTBEAT_SECONDS"],
            "SLUICE_HARDENING_HEARTBEAT_SECONDS")
        heartbeat_source = "env"

    # Keep going: env > default.
    keep_going = True
    keep_going_source = "default"
    if "SLUICE_HARDENING_KEEP_GOING" in os.environ:
        val = os.environ["SLUICE_HARDENING_KEEP_GOING"].strip()
        if val == "0":
            keep_going = False
        elif val == "1":
            keep_going = True
        else:
            raise ValueError(
                f"SLUICE_HARDENING_KEEP_GOING must be 0 or 1, got: {val!r}"
            )
        keep_going_source = "env"

    # Smoke mode adjustments.
    if mode == "smoke":
        if args.hours is None and "SLUICE_HARDENING_HOURS" not in os.environ:
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
        heartbeat_seconds=heartbeat,
        hours_source=hours_source,
        timeout_source=timeout_source,
        fuzz_source=fuzz_source,
        keep_going_source=keep_going_source,
        heartbeat_source=heartbeat_source,
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