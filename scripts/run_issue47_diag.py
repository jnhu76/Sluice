#!/usr/bin/env python3
"""ISSUE-47-DIAGNOSTIC-1 — direct-binary signal-classifying runner (DIAGNOSTIC ONLY).

Executes the built `multi_worker_coord_test` binary DIRECTLY (never via
`xmake run` / `xmake test` or any shell wrapper that collapses signal status)
so the real process exit status is preserved. This is the apparatus that lets
the next abnormal termination of case `mwcoord_serialized_backend_access` be
unambiguously classified into one of the issue-#47 categories (C1 early run
return / C2 SLUICE_CHECK + teardown fail-fast / C3 std::terminate / C4 signal /
C5 unhandled exception / C6 other).

It is a DIAGNOSTIC tool only. It performs NO Scheduler fix, adds NO sleeps, and
changes NO production behavior. The diagnostic breadcrumbs/snapshot/early-exit
seam live in the test file and activate only when SLUICE_ISSUE47_DIAG=1.

Classification (see issue #47 §6):
    returncode == 0   -> PASS
    returncode == 1   -> NORMAL_HARNESS_FAILURE (harness's own contract)
    returncode < 0    -> SIGNAL_TERMINATION (signal_number = -returncode;
                        signal_name resolved via signal.Signals)
    returncode in {90, 91, 92}
                       -> reserved diagnostic code (documented meaning below)
    other positive     -> ABNORMAL_NON_SIGNAL_EXIT

Reserved diagnostic exit codes (set by the test's diagnostic early-exit seam,
which calls std::_Exit before local destructors so teardown fail-fast cannot
overwrite the original result):
    90  EARLY_RUN_RETURN_WITH_LIVE_WORK
    91  RUN_RETURNED_WITH_NO_OUTSTANDING_BUT_INCOMPLETE_FIBERS
    92  CONCURRENT_BACKEND_ACCESS_OBSERVED

Per-iteration record (JSONL) + human-readable summary are written to
--output-dir. Exits nonzero if ANY iteration is not a clean PASS (so a CI step
or local loop stops at the first abnormal iteration).

Usage:
    scripts/run_issue47_diag.py --mode debug --count 1000 \
        --output-dir build/issue47-diag/run-1 --timeout-seconds 30
"""
from __future__ import annotations

import argparse
import json
import os
import platform
import signal
import subprocess
import sys
import time
from pathlib import Path

# Reserved diagnostic exit codes (documented above; mirror the test's
# i47_diag::early_exit codes). Kept here so the runner classifies them by name
# without depending on test internals.
RESERVED_CODES = {
    90: "EARLY_RUN_RETURN_WITH_LIVE_WORK",
    91: "RUN_RETURNED_WITH_NO_OUTSTANDING_BUT_INCOMPLETE_FIBERS",
    92: "CONCURRENT_BACKEND_ACCESS_OBSERVED",
}

# The signal classifications issue #47 §6 requires us to distinguish at minimum.
KNOWN_SIGNALS = ("SIGABRT", "SIGSEGV", "SIGILL", "SIGBUS", "SIGFPE",
                 "SIGTERM", "SIGKILL")

REQUIRED_FILTER = "mwcoord_serialized_backend_access"


def signal_name(num: int) -> str:
    """Resolve a signal number to a name, falling back to a stable literal."""
    try:
        return signal.Signals(num).name
    except (ValueError, AttributeError):
        return f"UNKNOWN_SIGNAL_{num}"


def classify(returncode: int) -> tuple[str, str]:
    """Return (classification, detail) per issue #47 §6.

    detail is a signal name for SIGNAL_TERMINATION, a reserved-code name for a
    reserved code, or "" otherwise.
    """
    if returncode == 0:
        return ("PASS", "")
    if returncode == 1:
        return ("NORMAL_HARNESS_FAILURE", "")
    if returncode < 0:
        num = -returncode
        return ("SIGNAL_TERMINATION", signal_name(num))
    if returncode in RESERVED_CODES:
        return (RESERVED_CODES[returncode], RESERVED_CODES[returncode])
    return ("ABNORMAL_NON_SIGNAL_EXIT", str(returncode))


def parse_last_phase(stderr_text: str) -> str:
    """Extract the final [I47] phase breadcrumb from captured stderr.

    The test writes stable iteration-independent phase IDs like
    '[I47] I47-P10 run-returned' to stderr. The last one observed is the
    process's last known phase. Returns 'UNAVAILABLE' if none found (e.g. a
    signal before any breadcrumb, or diagnostic mode not active).
    """
    last = ""
    for line in stderr_text.splitlines():
        s = line.strip()
        if s.startswith("[I47] I47-P") or s.startswith("[I47] I47-T") \
                or s.startswith("[I47] I47-CLASS") or s.startswith("[I47] I47-SNAPSHOT"):
            last = s
    return last or "UNAVAILABLE"


def locate_binary(explicit: str | None, mode: str) -> Path:
    """Locate the built test binary using repository conventions.

    Layout: build/<platform>/<arch>/<mode>/<binary>. Falls back to a couple of
    common shapes; never hard-codes one developer-specific absolute path.
    """
    if explicit:
        p = Path(explicit)
        if not p.is_file():
            sys.exit(f"error: --binary not a file: {p}")
        if not os.access(p, os.X_OK):
            sys.exit(f"error: --binary not executable: {p}")
        return p

    plat = "linux" if platform.system() == "Linux" else platform.system().lower()
    machine = platform.machine().lower()
    if machine in ("x86_64", "amd64"):
        arch = "x86_64"
    elif machine in ("aarch64", "arm64"):
        arch = "arm64"
    else:
        arch = machine

    name = "multi_worker_coord_test"
    candidates = [
        Path("build") / plat / arch / mode / name,
        Path("build") / plat / arch / name,  # mode-less fallback
    ]
    for p in candidates:
        if p.is_file() and os.access(p, os.X_OK):
            return p
    sys.exit(
        f"error: could not locate built binary '{name}' under build/{plat}/{arch}/"
        f"{{{mode},}}. Build it first (xmake build {name}) or pass --binary PATH."
    )


def env_for_run() -> dict[str, str]:
    """Build the child environment: diagnostic mode + the case filter required."""
    env = dict(os.environ)
    env["SLUICE_ISSUE47_DIAG"] = "1"
    # The required filter selects the affected case. Do not let an ambient
    # SLUICE_TEST_FILTER broaden the run and pollute evidence.
    env["SLUICE_TEST_FILTER"] = REQUIRED_FILTER
    return env


def run_one(binary: Path, iteration: int, timeout: float,
            taskset: list[str] | None, env: dict[str, str],
            out_dir: Path) -> dict:
    """Execute one iteration and return its JSONL record."""
    argv = (taskset + [str(binary)]) if taskset else [str(binary)]
    start = time.monotonic()
    stdout_f = out_dir / f"iter-{iteration:05d}.stdout"
    stderr_f = out_dir / f"iter-{iteration:05d}.stderr"
    timed_out = False
    returncode: int
    try:
        with open(stdout_f, "wb") as out, open(stderr_f, "wb") as err:
            proc = subprocess.Popen(
                argv, env=env, stdout=out, stderr=err, close_fds=True)
            try:
                returncode = proc.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
                timed_out = True
                returncode = proc.returncode if proc.returncode is not None else -9
    except OSError as e:
        # Binary vanished or failed to exec: record as a runner-level failure.
        return {
            "iteration": iteration,
            "pid": None,
            "returncode": None,
            "classification": "RUNNER_SPAWN_ERROR",
            "detail": str(e),
            "duration_ms": int((time.monotonic() - start) * 1000),
            "stdout_file": str(stdout_f),
            "stderr_file": str(stderr_f),
            "last_phase": "UNAVAILABLE",
            "timed_out": False,
        }

    duration_ms = int((time.monotonic() - start) * 1000)

    # Read captured stderr to parse the last phase breadcrumb. Bounded read so a
    # pathological run cannot OOM the runner.
    stderr_text = ""
    try:
        stderr_text = stderr_f.read_text(errors="replace")[:65536]
    except OSError:
        pass
    last_phase = parse_last_phase(stderr_text)

    if timed_out:
        classification = "TIMEOUT"
        detail = f"killed after {timeout}s"
    else:
        classification, detail = classify(returncode if returncode is not None else 0)

    return {
        "iteration": iteration,
        "pid": None,  # Popen pid is not stable post-wait; left None for honesty
        "returncode": returncode,
        "classification": classification,
        "signal_number": (-returncode) if (returncode is not None and returncode < 0) else None,
        "signal_name": detail if classification == "SIGNAL_TERMINATION" else None,
        "detail": detail,
        "duration_ms": duration_ms,
        "stdout_file": str(stdout_f),
        "stderr_file": str(stderr_f),
        "last_phase": last_phase,
        "timed_out": timed_out,
    }


def env_record() -> dict:
    """Capture CI-like environment identifiers for the result file header."""
    import subprocess as sp
    def git(args: list[str]) -> str:
        try:
            return sp.run(["git"] + args, capture_output=True, text=True,
                          check=False).stdout.strip()
        except OSError:
            return "UNAVAILABLE"

    def sh(cmd: str) -> str:
        try:
            return sp.run(cmd, shell=True, capture_output=True, text=True,
                          check=False).stdout.strip() or "UNAVAILABLE"
        except OSError:
            return "UNAVAILABLE"

    return {
        "commit": git(["rev-parse", "HEAD"]),
        "branch": git(["rev-parse", "--abbrev-ref", "HEAD"]),
        "mode": "<from --mode>",
        "platform": platform.platform(),
        "python": sys.version.split()[0],
        "cpu_count": os.cpu_count(),
        "uname": sh("uname -a"),
        "clang": sh("clang --version | head -1"),
        "xmake": sh("xmake --version 2>/dev/null | head -1"),
    }


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--binary", default=None,
                   help="explicit path to the built multi_worker_coord_test")
    p.add_argument("--mode", default="debug", choices=["debug", "release"],
                   help="build mode used to locate the binary (default: debug)")
    p.add_argument("--count", type=int, required=True,
                   help="number of iterations (must be > 0)")
    p.add_argument("--output-dir", required=True,
                   help="directory for JSONL + per-iter stdout/stderr + summary")
    p.add_argument("--timeout-seconds", type=float, default=30.0,
                   help="per-iteration timeout in seconds (must be > 0)")
    p.add_argument("--taskset", default=None,
                   help="optional comma-separated CPU list, e.g. '0,1'")
    args = p.parse_args(argv)

    # Reject invalid inputs (issue #47 §5): count <= 0, invalid timeout.
    # Missing/non-executable binary is handled in locate_binary.
    if args.count <= 0:
        sys.exit("error: --count must be > 0")
    if args.timeout_seconds <= 0:
        sys.exit("error: --timeout-seconds must be > 0")

    binary = locate_binary(args.binary, args.mode)
    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    taskset: list[str] | None = None
    if args.taskset:
        taskset = ["taskset", "-c", args.taskset]

    env = env_for_run()
    env_info = env_record()
    env_info["mode"] = args.mode
    env_info["binary"] = str(binary)
    env_info["taskset"] = args.taskset or "(none)"
    env_info["timeout_seconds"] = args.timeout_seconds

    jsonl_path = out_dir / "iterations.jsonl"
    summary_path = out_dir / "summary.txt"

    # Counters across all iterations.
    counts = {
        "PASS": 0, "NORMAL_HARNESS_FAILURE": 0, "SIGNAL_TERMINATION": 0,
        "TIMEOUT": 0, "RUNNER_SPAWN_ERROR": 0, "ABNORMAL_NON_SIGNAL_EXIT": 0,
        **{name: 0 for name in RESERVED_CODES.values()},
    }
    first_abnormal: dict | None = None
    sig_tally: dict[str, int] = {}

    print(f"[issue47-diag] binary={binary} mode={args.mode} count={args.count} "
          f"taskset={args.taskset or '(none)'}", file=sys.stderr)

    with open(jsonl_path, "w") as jsonl:
        # Header line with environment capture (not an iteration record).
        jsonl.write(json.dumps({"_env": env_info}) + "\n")
        for i in range(1, args.count + 1):
            rec = run_one(binary, i, args.timeout_seconds, taskset, env, out_dir)
            jsonl.write(json.dumps(rec) + "\n")
            jsonl.flush()
            cls = rec["classification"]
            counts[cls] = counts.get(cls, 0) + 1
            if cls == "SIGNAL_TERMINATION":
                sig_tally[rec["signal_name"]] = sig_tally.get(rec["signal_name"], 0) + 1
            # Per-iteration one-line trace to stderr.
            tag = rec["returncode"] if rec["returncode"] is not None else "??"
            print(f"  iter {i:5d}: rc={tag} {cls}"
                  f"{' ' + rec['signal_name'] if cls == 'SIGNAL_TERMINATION' else ''}"
                  f"{' ' + rec['detail'] if cls in ('TIMEOUT','RUNNER_SPAWN_ERROR') else ''}"
                  f"  last_phase={rec['last_phase']}", file=sys.stderr)
            # Stop after the first abnormal iteration (issue #47 §12 shard rule).
            if cls != "PASS" and first_abnormal is None:
                first_abnormal = rec
                break

    # Human-readable summary.
    total = sum(v for k, v in counts.items())
    lines = []
    lines.append("ISSUE-47-DIAGNOSTIC-1 summary")
    lines.append(f"  binary: {binary}")
    lines.append(f"  mode:   {args.mode}")
    lines.append(f"  taskset: {args.taskset or '(none)'}")
    lines.append(f"  commit: {env_info['commit']}")
    lines.append(f"  iterations run: {total} / {args.count} requested "
                 f"(stopped at first abnormal: {first_abnormal is not None})")
    lines.append("  classification counts:")
    for cls in ["PASS", "NORMAL_HARNESS_FAILURE", *RESERVED_CODES.values(),
                "SIGNAL_TERMINATION", "TIMEOUT", "ABNORMAL_NON_SIGNAL_EXIT",
                "RUNNER_SPAWN_ERROR"]:
        if counts.get(cls, 0):
            extra = ""
            if cls == "SIGNAL_TERMINATION" and sig_tally:
                extra = "  " + ", ".join(f"{k}={v}" for k, v in sorted(sig_tally.items()))
            lines.append(f"    {cls:32s} {counts[cls]}{extra}")
    if first_abnormal is not None:
        lines.append("  FIRST ABNORMAL ITERATION:")
        lines.append(json.dumps(first_abnormal, indent=2))
    else:
        lines.append("  no abnormal iteration observed (NOT-REPRODUCED)")
    summary_text = "\n".join(lines) + "\n"
    summary_path.write_text(summary_text)
    print(summary_text, file=sys.stderr)

    # Exit nonzero if any iteration was not a clean PASS (issue #47 §6).
    any_non_pass = total - counts.get("PASS", 0)
    return 0 if any_non_pass == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
