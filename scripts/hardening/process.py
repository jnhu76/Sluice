"""Subprocess execution with group-level timeout, TERM -> KILL escalation,
sanitizer log scanning, and optional long-command heartbeat.

This is the only module that spawns OS processes.  All external commands
go through ``run_command()``.

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

Heartbeat
---------
When ``heartbeat_seconds > 0``, the initial wait is sliced into intervals
of at most ``heartbeat_seconds``.  After each slice the runner emits a
heartbeat line to stderr, the run log, and (optionally) a JSONL file.  The
heartbeat includes the command's elapsed time, remaining timeout, log size
delta, and — for fuzz commands — a best-effort libFuzzer status line.  The
heartbeat is a progress signal; it never reclassifies the command, never
changes the timeout, and never delays the TERM → KILL escalation.
"""

from __future__ import annotations

import datetime
import json
import os
import re
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

from .model import (
    Classification,
    CommandResult,
    CommandSpec,
)

# ═══════════════════════════════════════════════════════════════════════════════
# Constants
# ═══════════════════════════════════════════════════════════════════════════════

KILL_AFTER_SECONDS = 10.0

# Heartbeat: maximum bytes to read from the command log tail for libFuzzer
# status extraction.  Must be bounded; never blocks.
_TAIL_READ_BYTES = 64 * 1024

# Best-effort libFuzzer status-line parser.  Matches the standard
# ``#<N>\t<tag>  cov:... ft:... corp:... exec/s:... rss:...`` line.
# The tag is INITED, NEW, REDUCE, pulse, or DONE.  Captures the run count,
# tag, exec/s, and RSS.  Other fields are parsed only when present.
_LIBFUZZER_STATUS_RE = re.compile(
    r"#(?P<count>\d+)\s+"
    r"(?P<tag>INITED|NEW|REDUCE|pulse|DONE)\s+"
    r".*?"
    r"exec/s:\s*(?P<exec_s>\d+)\s+"
    r".*?"
    r"rss:\s*(?P<rss>\d+)Mb",
    re.DOTALL,
)

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
# Heartbeat emitter
# ═══════════════════════════════════════════════════════════════════════════════

def _tail_read_log(log_path: Path, max_bytes: int = _TAIL_READ_BYTES) -> str:
    """Read at most *max_bytes* from the tail of *log_path*.

    Bounded, never blocks, tolerates missing/truncated/partial-UTF-8.
    """
    try:
        size = log_path.stat().st_size
    except OSError:
        return ""
    if size == 0:
        return ""
    read_size = min(max_bytes, size)
    try:
        with open(log_path, "rb") as f:
            if size > read_size:
                f.seek(size - read_size)
            return f.read(read_size).decode("utf-8", errors="replace")
    except OSError:
        return ""


def _parse_libfuzzer_status(
    log_path: Path,
) -> Optional[Dict[str, Any]]:
    """Best-effort parse the latest libFuzzer status line from *log_path*.

    Returns a dict with keys count, tag, exec_s, rss, and optionally cov, ft,
    corpus_count, corpus_size, lim, or None if parsing fails.
    """
    tail = _tail_read_log(log_path)
    if not tail:
        return None
    m = _LIBFUZZER_STATUS_RE.search(tail)
    if not m:
        return None
    d: Dict[str, Any] = {
        "count": int(m.group("count")),
        "tag": m.group("tag"),
        "exec_s": int(m.group("exec_s")),
        "rss": int(m.group("rss")),
    }
    # Optional fields: cov, ft, corpus, lim
    for opt_field, opt_re in [
        ("cov", r"cov:\s*(\d+)"),
        ("ft", r"ft:\s*(\d+)"),
        ("lim", r"lim:\s*(\d+)"),
    ]:
        om = re.search(opt_re, tail)
        if om:
            d[opt_field] = int(om.group(1))
    # corpus: "count/size" — e.g. "corp: 171/19Kb"
    cm = re.search(r"corp:\s*(\d+)/(\d+)([Kk]?[Bb]?)", tail)
    if cm:
        d["corpus_count"] = int(cm.group(1))
        d["corpus_size"] = int(cm.group(2))
        if cm.group(3).lower().startswith("k"):
            d["corpus_size"] *= 1024
    return d


class HeartbeatEmitter:
    """Emit periodic heartbeat lines for a single long-running command.

    One instance per command.  Tracks log size for delta reporting.
    """

    def __init__(
        self,
        phase: str,
        iteration: str,
        target: str,
        pid: int,
        start_mono: float,
        timeout_seconds: float,
        log_path: Path,
        run_log_path: Optional[Path] = None,
        heartbeats_jsonl_path: Optional[Path] = None,
        global_remaining_fn: Optional[callable] = None,  # type: ignore
        is_fuzz: bool = False,
    ):
        self._phase = phase
        self._iteration = iteration
        self._target = target
        self._pid = pid
        self._start_mono = start_mono
        self._timeout_seconds = timeout_seconds
        self._log_path = log_path
        self._run_log_path = run_log_path
        self._heartbeats_jsonl_path = heartbeats_jsonl_path
        self._global_remaining_fn = global_remaining_fn
        self._is_fuzz = is_fuzz
        self._last_log_size = 0

    def emit(self) -> None:
        """Write one heartbeat line to stderr, run.log, and heartbeats.jsonl."""
        now = time.monotonic()
        elapsed = now - self._start_mono
        remaining = max(0.0, self._timeout_seconds - elapsed)
        alive = True  # we only emit while the child is alive

        # Log size delta.
        try:
            cur_size = self._log_path.stat().st_size
        except OSError:
            cur_size = 0
        delta = max(0, cur_size - self._last_log_size)
        self._last_log_size = cur_size

        # Fuzzer status.
        fuzzer_status: Optional[Dict[str, Any]] = None
        if self._is_fuzz:
            fuzzer_status = _parse_libfuzzer_status(self._log_path)

        # Build fields.
        fields: List[str] = [
            f"phase={self._phase}",
            f"target={self._target}",
            f"iteration={self._iteration}",
            f"elapsed={elapsed:.0f}s",
            f"timeout={self._timeout_seconds:.0f}s",
            f"remaining={remaining:.0f}s",
            f"pid={self._pid}",
            "alive=yes",
            f"log_size={cur_size}",
            f"log_delta={delta}",
        ]
        if self._global_remaining_fn is not None:
            gr = self._global_remaining_fn()
            if gr is not None:
                fields.append(f"global_remaining={gr:.0f}s")
        if fuzzer_status is not None:
            fs = fuzzer_status
            fields.append(
                f"fuzzer_status=#{fs['count']} {fs['tag']} "
                f"exec/s={fs['exec_s']} rss={fs['rss']}Mb"
            )
            if "corpus_count" in fs:
                fields.append(f"corpus={fs['corpus_count']}/{fs['corpus_size']}")
        else:
            fields.append("fuzzer_status=unavailable")

        line = "[heartbeat] " + " ".join(fields)

        # stderr (console).
        print(line, file=sys.stderr)

        # run.log.
        if self._run_log_path is not None:
            try:
                with open(self._run_log_path, "a") as f:
                    f.write(line + "\n")
            except OSError:
                pass

        # heartbeats.jsonl (append-only, one JSON object per line).
        if self._heartbeats_jsonl_path is not None:
            try:
                rec: Dict[str, Any] = {
                    "phase": self._phase,
                    "iteration": self._iteration,
                    "target": self._target,
                    "pid": self._pid,
                    "elapsed": round(elapsed, 1),
                    "timeout": self._timeout_seconds,
                    "remaining": round(remaining, 1),
                    "alive": alive,
                    "log_size": cur_size,
                    "log_delta": delta,
                }
                if self._global_remaining_fn is not None:
                    gr = self._global_remaining_fn()
                    if gr is not None:
                        rec["global_remaining"] = round(gr, 1)
                if fuzzer_status is not None:
                    rec["fuzzer_status"] = fuzzer_status
                else:
                    rec["fuzzer_status"] = "unavailable"
                with open(self._heartbeats_jsonl_path, "a") as f:
                    f.write(json.dumps(rec, sort_keys=True) + "\n")
            except OSError:
                pass


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
    start_mono = time.monotonic()

    # Open the log file and write + flush the header *before* the child
    # inherits the fd, so the header always precedes the child's output.
    # Binary mode keeps the raw child bytes free of text-encoding interference.
    try:
        log_file = open(spec.log_path, "ab")
    except OSError as e:
        return _runner_error_result(
            spec, started_at, f"log open failed: {e}", start_mono=start_mono)

    try:
        try:
            for line in spec.header_lines(head_sha, dirty):
                log_file.write((line + "\n").encode("utf-8", errors="replace"))
            log_file.flush()
        except OSError as e:
            return _runner_error_result(
                spec, started_at, f"log header write failed: {e}", start_mono=start_mono)

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
            return _runner_error_result(spec, started_at, f"spawn failed: {e}", start_mono=start_mono)

        # From here the child MUST always be reaped, on every path.
        try:
            # Heartbeat: build an emitter for long-running commands.
            heartbeater: Optional[HeartbeatEmitter] = None
            if spec.heartbeat_seconds > 0:
                is_fuzz = spec.phase == "fuzz" and spec.target not in (
                    "configure", "build-fuzz-group")
                heartbeater = HeartbeatEmitter(
                    phase=spec.phase,
                    iteration=spec.iteration,
                    target=spec.target,
                    pid=proc.pid,
                    start_mono=start_mono,
                    timeout_seconds=spec.timeout_seconds,
                    log_path=spec.log_path,
                    is_fuzz=is_fuzz,
                    heartbeats_jsonl_path=spec.heartbeats_path,
                )
            exit_code, timed_out, term_sent, kill_sent = _run_with_timeout(
                proc, spec.timeout_seconds, heartbeater
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
    duration = time.monotonic() - start_mono
    end_epoch = int(finished_at.timestamp())

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
    proc: "subprocess.Popen",
    timeout_seconds: float,
    heartbeater: "Optional[HeartbeatEmitter]" = None,
) -> tuple[Optional[int], bool, bool, bool]:
    """Wait for *proc*, enforcing TERM -> KILL escalation on timeout.

    Returns ``(exit_code, timed_out, term_sent, kill_sent)``.  The boolean
    fields record the actions this runner actually took.

    When *heartbeater* is provided, the initial wait deadline is sliced into
    intervals of at most 1 second.  After each slice the emitter is called
    to signal progress.  The heartbeat processing never delays the TERM ->
    KILL escalation: once the deadline is reached the original fast path is
    used.
    """
    if heartbeater is not None:
        # Heartbeat-enabled wait: slice the deadline into short intervals.
        deadline = time.monotonic() + timeout_seconds
        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                break
            slice_t = min(1.0, remaining)
            try:
                exit_code = proc.wait(timeout=slice_t)
                return exit_code, False, False, False
            except subprocess.TimeoutExpired:
                heartbeater.emit()
        # Fall through to TERM escalation (same as original).
    else:
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
    spec: CommandSpec, started_at: datetime.datetime, message: str,
    start_mono: float = 0.0,
) -> CommandResult:
    """Build a RUNNER_ERROR result for an infrastructure failure.

    Best-effort records the error in the command log so it is visible in the
    artifact directory.
    """
    finished_at = _now()
    duration = time.monotonic() - start_mono if start_mono > 0 else 0.0
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
