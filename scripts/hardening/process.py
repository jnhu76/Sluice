"""Subprocess execution with group-level timeout, TERM -> KILL escalation,
and sanitizer log scanning.

This is the only module that spawns OS processes.  All external commands
go through `run_command()`.

Timeout architecture
--------------------
The child's stdout/stderr are redirected *directly* into the command log
file (the file object is handed to ``Popen``).  The runner thread never
reads that pipe: it only manages the child's lifecycle with
``Popen.wait(timeout=...)``.  This is deliberate.  A blocking ``os.read()``
on the child's output pipe would stall the runner whenever a child is
silent (no output), which in turn would prevent the per-command timeout,
the global deadline, and the TERM -> KILL escalation from ever firing — a
single silent, deadlocked test could hang the whole hardening run.  With
file-directed output, ``wait(timeout=...)`` is the sole authority on
timing, and the authoritative ``timed_out`` / ``term_sent`` / ``kill_sent``
fields describe actions the runner actually took (never inferred from an
exit code such as 124 or 137).
"""

from __future__ import annotations

import datetime
import os
import re
import signal
import subprocess
from pathlib import Path
from typing import List, Optional

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

    Returns the first matching line, or *None*.  Only scans the part after
    the ``----- output -----`` marker to avoid matching runner header text.

    The set of patterns scanned is determined strictly by *kind*:

    *   ``kind == "tsan"`` → TSan patterns only.
    *   ``kind == "asan"`` → ASan + UBSan patterns.
    *   ``kind is None``    → no scanning at all (returns *None*).  A plain
        debug/baseline command must not be classified SANITIZER_FAIL just
        because its output happens to contain broad text such as
        ``runtime error:``.
    *   any other non-empty *kind* is a programming error and raises
        ``ValueError`` (fail loud rather than silently disabling scanning,
        which could mask a real sanitizer failure as a PASS).
    """
    if not log_text:
        return None

    if kind is None:
        return None

    if kind == "tsan":
        patterns: List[re.Pattern] = _TSAN_PATTERNS
    elif kind == "asan":
        patterns = _ASAN_PATTERNS + _UBSAN_PATTERNS
    else:
        raise ValueError(f"unknown sanitizer kind: {kind!r}")

    # Only scan the real output portion (after the marker).
    marker = "----- output -----\n"
    idx = log_text.find(marker)
    if idx >= 0:
        output = log_text[idx + len(marker):]
    else:
        output = log_text

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
        so that ``killpg`` reaches the whole process group (e.g. ``xmake`` and
        the test binary it spawned), not just the immediate child.
    *   stdout and stderr are redirected straight into the log file; the runner
        never reads the child's output pipe (see module docstring).
    *   Timeout is implemented purely via ``wait(timeout=...)``, followed by
        ``SIGTERM`` -> grace -> ``SIGKILL`` escalation against the process group.
    *   The log file receives a header before the command runs and a footer
        after it finishes; the command's real output lies between them.
    *   Log-write failures are escalated to ``RUNNER_ERROR``.
    """
    # Ensure log directory exists.
    try:
        spec.log_path.parent.mkdir(parents=True, exist_ok=True)
    except OSError as e:
        return _runner_error_result(spec, _now(), f"log dir create failed: {e}")

    # Build child environment.
    child_env = os.environ.copy()
    child_env.update(spec.environment)

    started_at = _now()

    # Open the log file and write + flush the header *before* the child
    # inherits the fd, so the header always precedes the child's output.
    # Binary mode keeps the raw child bytes free of text-encoding interference.
    try:
        log_file = open(spec.log_path, "ab")
    except OSError as e:
        return _runner_error_result(spec, started_at, f"log open failed: {e}")

    try:
        try:
            for line in spec.header_lines(head_sha, dirty):
                log_file.write((line + "\n").encode("utf-8", errors="replace"))
            log_file.flush()
        except OSError as e:
            return _runner_error_result(
                spec, started_at, f"log header write failed: {e}"
            )

        # Start the subprocess with output directed at the log file.
        try:
            proc = subprocess.Popen(
                spec.command,
                stdout=log_file,
                stderr=subprocess.STDOUT,
                env=child_env,
                start_new_session=True,
            )
        except OSError as e:  # FileNotFoundError, PermissionError, ...
            return _runner_error_result(spec, started_at, f"spawn failed: {e}")

        # From here the child MUST always be reaped, on every path.
        try:
            exit_code, timed_out, term_sent, kill_sent = _run_with_timeout(
                proc, spec.timeout_seconds
            )
        except BaseException:
            # KeyboardInterrupt or any runner-internal error: tear down the
            # whole process group and reap before propagating, so no orphaned
            # child survives.
            _kill_process_group(proc.pid, signal.SIGKILL)
            try:
                proc.wait(timeout=KILL_AFTER_SECONDS)
            except BaseException:
                proc.wait()
            raise
    finally:
        try:
            log_file.close()
        except OSError:
            pass

    finished_at = _now()
    end_epoch = int(finished_at.timestamp())
    duration = (finished_at - started_at).total_seconds()

    # Read back the command's real output for sanitizer classification.  The
    # footer has not been written yet, so the file is header + output.
    log_text = _read_log_text(spec.log_path)

    classification, sig = classify_with_sanitizer(
        exit_code, log_text, spec.sanitizer_kind, timed_out
    )

    # Append footer.  A failure here is an infrastructure fault: the evidence
    # chain for this command is broken, so escalate to RUNNER_ERROR.
    try:
        with open(spec.log_path, "ab") as f:
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
                f.write((line + "\n").encode("utf-8", errors="replace"))
    except OSError:
        classification = Classification.RUNNER_ERROR
        sig = None

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


def _run_with_timeout(
    proc: "subprocess.Popen", timeout_seconds: float
) -> tuple[Optional[int], bool, bool, bool]:
    """Wait for *proc*, enforcing TERM -> KILL escalation on timeout.

    Returns ``(exit_code, timed_out, term_sent, kill_sent)``.  The boolean
    fields record the actions this runner actually took.
    """
    try:
        exit_code = proc.wait(timeout=timeout_seconds)
        return exit_code, False, False, False
    except subprocess.TimeoutExpired:
        pass

    # Per-command timeout exceeded: TERM the whole group.
    _kill_process_group(proc.pid, signal.SIGTERM)
    try:
        exit_code = proc.wait(timeout=KILL_AFTER_SECONDS)
        return exit_code, True, True, False
    except subprocess.TimeoutExpired:
        pass

    # Grace period exceeded: KILL the whole group, then reap unconditionally.
    _kill_process_group(proc.pid, signal.SIGKILL)
    exit_code = proc.wait()
    return exit_code, True, True, True


def _kill_process_group(pid: int, sig: int) -> None:
    """Send a signal to the process group of *pid*.

    The child is started with ``start_new_session=True`` so its pid equals its
    process-group id.  ``ProcessLookupError`` means the group already exited
    (nothing to signal); ``PermissionError`` should not occur for our own
    children.  Neither is an error here.
    """
    try:
        os.killpg(pid, sig)
    except ProcessLookupError:
        pass  # Already exited.
    except PermissionError:
        pass  # Should not happen for our own children.


def _read_log_text(log_path: Path) -> str:
    """Read the command log back as text (best effort)."""
    try:
        with open(log_path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def _now() -> datetime.datetime:
    return datetime.datetime.now(datetime.timezone.utc)


def _runner_error_result(
    spec: CommandSpec, started_at: datetime.datetime, message: str
) -> CommandResult:
    """Build a RUNNER_ERROR result for an infrastructure failure.

    Best-effort records the error in the command log so it is visible in the
    artifact directory.
    """
    finished_at = _now()
    duration = (finished_at - started_at).total_seconds()
    try:
        with open(spec.log_path, "ab") as f:
            f.write(
                ("----- end output -----\n" + f"runner_error={message}\n").encode(
                    "utf-8", errors="replace"
                )
            )
    except OSError:
        pass
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
