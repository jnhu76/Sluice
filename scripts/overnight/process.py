"""Subprocess execution with group-level timeout, TERM -> KILL escalation,
and sanitizer log scanning.

This is the only module that spawns OS processes.  All external commands
go through `run_command()`.
"""

from __future__ import annotations

import datetime
import os
import re
import signal
import subprocess
import time
from pathlib import Path
from typing import Dict, List, Optional

from .model import (
    Classification,
    CommandResult,
    CommandSpec,
)

# ═══════════════════════════════════════════════════════════════════════════════
# Constants
# ═══════════════════════════════════════════════════════════════════════════════

KILL_AFTER_SECONDS = 10.0

# Sanitizer signature patterns (must not match runner log header).
_TSAN_PATTERNS = [
    re.compile(r"WARNING: ThreadSanitizer"),
    re.compile(r"WARNING: data race"),
    re.compile(r"WARNING: deadlock"),
]
_ASAN_PATTERNS = [
    re.compile(r"ERROR: AddressSanitizer"),
    re.compile(r"AddressSanitizer:"),
    re.compile(r"LeakSanitizer"),
]
_UBSAN_PATTERNS = [
    re.compile(r"runtime error:"),
    re.compile(r"SUMMARY: UndefinedBehaviorSanitizer"),
]


# ═══════════════════════════════════════════════════════════════════════════════
# Sanitizer scanning
# =============================================================================

def scan_sanitizer(log_text: str, kind: Optional[str]) -> Optional[str]:
    """Scan *log_text* for known sanitizer signatures.

    Returns the first matching line, or *None*.
    Only scans the part after the ``----- output -----`` marker to avoid
    matching runner header text.
    """
    if not log_text:
        return None

    # Only scan the real output portion (after the marker).
    marker = "----- output -----\n"
    idx = log_text.find(marker)
    if idx >= 0:
        output = log_text[idx + len(marker):]
    else:
        output = log_text

    patterns: List[re.Pattern] = []
    if kind == "tsan":
        patterns = _TSAN_PATTERNS
    elif kind == "asan":
        patterns = _ASAN_PATTERNS + _UBSAN_PATTERNS
    else:
        # Unknown kind - scan all.
        patterns = _TSAN_PATTERNS + _ASAN_PATTERNS + _UBSAN_PATTERNS

    for pat in patterns:
        m = pat.search(output)
        if m:
            return m.group(0)
    return None


def classify_with_sanitizer(
    exit_code: Optional[int],
    log_text: str,
    sanitizer_kind: Optional[str],
    timed_out: bool,
) -> tuple[Classification, Optional[str]]:
    """Determine the classification for a completed command.

    Sanitizer signatures take priority over exit code:  an exit-zero log
    that contains a sanitizer warning is still SANITIZER_FAIL.
    """
    sig = scan_sanitizer(log_text, sanitizer_kind) if log_text else None

    if timed_out:
        return Classification.TIMEOUT, sig

    if exit_code == 0:
        if sig:
            return Classification.SANITIZER_FAIL, sig
        return Classification.PASS, None

    if exit_code == 77:
        return Classification.SKIP, None

    if sig:
        return Classification.SANITIZER_FAIL, sig

    return Classification.FAIL, sig


# ═══════════════════════════════════════════════════════════════════════════════
# Core runner
# =============================================================================

def run_command(spec: CommandSpec, head_sha: str, dirty: bool) -> CommandResult:
    """Execute *spec* and return a fully populated ``CommandResult``.

    *   The child process is started in a new session (``start_new_session=True``)
        so that ``killpg`` works reliably.
    *   Timeout is implemented via ``wait(timeout=...)``, followed by
        ``SIGTERM`` -> grace -> ``SIGKILL`` escalation.
    *   stdout and stderr are merged into the log file.
    *   The log file is written with a header before the command runs and a
        footer after the command finishes.
    """
    # Ensure log directory exists.
    spec.log_path.parent.mkdir(parents=True, exist_ok=True)

    # Build child environment.
    child_env = os.environ.copy()
    child_env.update(spec.environment)

    started_at = datetime.datetime.now(datetime.timezone.utc)
    start_epoch = int(started_at.timestamp())
    start_mono = time.monotonic()

    # Write log header.
    with open(spec.log_path, "w") as f:
        for line in spec.header_lines(head_sha, dirty):
            f.write(line + "\n")
        f.flush()

    # Start the subprocess.
    try:
        proc = subprocess.Popen(
            spec.command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            env=child_env,
            start_new_session=True,
        )
    except FileNotFoundError:
        finished_at = datetime.datetime.now(datetime.timezone.utc)
        duration = (finished_at - started_at).total_seconds()
        return CommandResult(
            phase=spec.phase,
            iteration=spec.iteration,
            target=spec.target,
            mode=spec.mode,
            command=spec.command,
            classification=Classification.RUNNER_ERROR,
            exit_code=None,
            duration_seconds=duration,
            log_path=spec.log_path,
            sanitizer_signature=None,
            timed_out=False,
            term_sent=False,
            kill_sent=False,
            started_at=started_at.isoformat(),
            finished_at=finished_at.isoformat(),
            synthetic=spec.synthetic,
        )

    term_sent = False
    kill_sent = False
    timed_out = False
    log_chunks: List[bytes] = []

    try:
        # Read output incrementally.
        while True:
            try:
                # Wait with timeout, reading output in the meantime.
                exit_code = proc.wait(timeout=1.0)
                # Process finished - read any remaining output.
                remaining, _ = proc.communicate()
                if remaining:
                    log_chunks.append(remaining)
                break
            except subprocess.TimeoutExpired:
                # Read whatever is available.
                if proc.stdout:
                    chunk = os.read(proc.stdout.fileno(), 65536)
                    if chunk:
                        log_chunks.append(chunk)

                # Check if we've exceeded the timeout.
                elapsed = time.monotonic() - start_mono
                if elapsed >= spec.timeout_seconds and not term_sent:
                    term_sent = True
                    timed_out = True
                    _kill_process_group(proc.pid, signal.SIGTERM)

                # If we already sent TERM and grace has expired, escalate.
                if term_sent and not kill_sent:
                    grace_elapsed = time.monotonic() - (
                        start_mono + spec.timeout_seconds
                    )
                    if grace_elapsed >= KILL_AFTER_SECONDS:
                        kill_sent = True
                        _kill_process_group(proc.pid, signal.SIGKILL)

    except BaseException:
        # Ensure the process is reaped on any error.
        _kill_process_group(proc.pid, signal.SIGKILL)
        kill_sent = True
        timed_out = True
        try:
            proc.wait(timeout=5)
        except BaseException:
            pass
        raise

    finished_at = datetime.datetime.now(datetime.timezone.utc)
    end_epoch = int(finished_at.timestamp())
    duration = (finished_at - started_at).total_seconds()

    # Decode log output.
    log_text = b"".join(log_chunks).decode("utf-8", errors="replace")

    # Append output to log file.
    with open(spec.log_path, "a") as f:
        f.write(log_text)
        if log_text and not log_text.endswith("\n"):
            f.write("\n")

    # Classify.
    classification, sig = classify_with_sanitizer(
        exit_code, log_text, spec.sanitizer_kind, timed_out
    )

    # Append footer.
    with open(spec.log_path, "a") as f:
        for line in spec.footer_lines(
            end_epoch=end_epoch,
            duration_s=duration,
            raw_exit=exit_code,
            classification=classification,
            timed_out=timed_out,
            term_sent=term_sent,
            kill_sent=kill_sent,
            sanitizer_signature=sig,
        ):
            f.write(line + "\n")

    return CommandResult(
        phase=spec.phase,
        iteration=spec.iteration,
        target=spec.target,
        mode=spec.mode,
        command=spec.command,
        classification=classification,
        exit_code=exit_code,
        duration_seconds=duration,
        log_path=spec.log_path,
        sanitizer_signature=sig,
        timed_out=timed_out,
        term_sent=term_sent,
        kill_sent=kill_sent,
        started_at=started_at.isoformat(),
        finished_at=finished_at.isoformat(),
        synthetic=spec.synthetic,
    )


def _kill_process_group(pid: int, sig: int) -> None:
    """Send a signal to the process group of *pid*."""
    try:
        os.killpg(pid, sig)
    except ProcessLookupError:
        pass  # Already exited.
    except PermissionError:
        pass  # Should not happen for our own children.