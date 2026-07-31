"""Phase implementations for the hardening test runner.

Each phase is a function that takes a ``PhaseContext`` and returns a
``PhaseOutcome``.  Phases are self-contained and may reconfigure xmake
as needed.
"""

from __future__ import annotations

import datetime
import os
import re
import shlex
import shutil
import subprocess
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Set

from .model import (
    Classification,
    CommandResult,
    CommandSpec,
    Config,
    FuzzCorpusSnapshot,
    PhaseStats,
    Verdict,
)
from .preflight import PreflightResult
from .process import run_command


def _human_dur(s: int) -> str:
    """Format a duration in seconds as a human-readable string."""
    m, s = divmod(s, 60)
    h, m = divmod(m, 60)
    if h:
        return f"{h}h{m:02d}m{s:02d}s"
    elif m:
        return f"{m}m{s:02d}s"
    return f"{s}s"


# ═══════════════════════════════════════════════════════════════════════════════
# Target constants  (mirror the Bash definitions)
# ═══════════════════════════════════════════════════════════════════════════════

TSAN_HOT_SET: List[str] = [
    "multi_worker_test",
    "multi_worker_coord_test",
    "scheduler_worker_topology_race_test",
    "application_runtime_worker_topology_test",
    "runnable_dup_publication_test",
    "runnable_steal_test",
    "external_wake_test",
    "wake_handle_lifetime_test",
    "scheduler_wait_test",
    "wait_queue_test",
    "wait_queue_external_wake_test",
    "wait_queue_unlink_topology_test",
    "select_multi_worker_test",
    "runtime_wait_test",
    "backend_conformance_test",
    "threadpool_backend_test",
    "uring_backend_test",
    "sluice_copy_integration_test",
]

ASAN_HOT_SET: List[str] = [
    "select_registration_rollback_test",
    "group_evented_admission_exception_safety_test",
    "application_runtime_resource_test",
    "async_io_context_test",
    "batch_reap_order_test",
    "async_queue_primitive_test",
    "runtime_wait_test",
    "sluice_copy_integration_test",
]

FUZZ_TARGETS: List[str] = [
    "wal_read_record_fuzz",
    "wal_roundtrip_fuzz",
    "copy_all_fault_fuzz",
]

FUZZ_MAXLEN: Dict[str, int] = {
    "wal_read_record_fuzz": 1048576,
    "wal_roundtrip_fuzz": 262144,
    "copy_all_fault_fuzz": 8192,
}

# Per-target libFuzzer dictionary.  None means no dictionary.
# wal_read_record_fuzz:  MATCH — input is a WAL frame (magic u32 + length u32
#   + payload + checksum u32, LE); the WAL dict tokens build those fields.
# wal_roundtrip_fuzz:    PARTIAL_MATCH — input is bare payload; dict tokens
#   are generic bytes (useful for boundary payloads), but WAL semantics are
#   inert here since the harness produces the frame, not the input.
# copy_all_fault_fuzz:   MISMATCH — input is a CopyConfig header + opaque
#   payload (fuzz/support/copy_model.hpp); no WAL field exists, so the WAL
#   dict is wasteful and semantically misleading.
FUZZ_DICTS: Dict[str, Optional[str]] = {
    "wal_read_record_fuzz": "wal_record.dict",
    "wal_roundtrip_fuzz": "wal_record.dict",
    "copy_all_fault_fuzz": None,
}

ACCEPTANCE_CONSUMERS: List[str] = [
    "public_api_acceptance",
    "async_foundation_quickstart",
]

NEG_COMPILE_SCRIPTS: List[str] = [
    "scripts/verify-async-api-negative-compile.sh",
    "scripts/verify-async-identity-negative-compile.sh",
]

# ═══════════════════════════════════════════════════════════════════════════════
# Phase context
# ═══════════════════════════════════════════════════════════════════════════════

@dataclass
class PhaseContext:
    """Shared state available to every phase."""

    config: Config
    project_root: Path
    run_dir: Path
    head_sha: str
    head_short: str
    worktree_dirty: bool
    nproc: int
    global_deadline: float  # monotonic
    final_debug_reserved: float  # seconds
    sticky_hold: bool
    baseline_ok: bool
    final_debug_ok: bool
    # Per-phase stats
    stats: Dict[str, PhaseStats] = field(default_factory=dict)
    # All results
    results: List[CommandResult] = field(default_factory=list)
    # Failure records (for failures.jsonl)
    failures: List[CommandResult] = field(default_factory=list)
    # Fuzz results
    fuzz_results: List[FuzzCorpusSnapshot] = field(default_factory=list)
    # Target cache: mode -> set of target names
    target_cache: Dict[str, Set[str]] = field(default_factory=dict)
    _current_cache_mode: str = ""

    def get_or_create_stats(self, phase: str) -> PhaseStats:
        if phase not in self.stats:
            self.stats[phase] = PhaseStats()
        return self.stats[phase]

    def remaining_seconds(self) -> float:
        return max(0.0, self.global_deadline - time.monotonic())

    def phase_remaining(self, phase_timeout: float) -> float:
        """Return the per-phase timeout cap (min of global remaining and
        configured)."""
        return min(phase_timeout, self.remaining_seconds())


# ═══════════════════════════════════════════════════════════════════════════════
# Phase outcome
# ═══════════════════════════════════════════════════════════════════════════════

@dataclass
class PhaseOutcome:
    phase: str
    passed: bool


# ═══════════════════════════════════════════════════════════════════════════════
# Target cache helpers
# ═══════════════════════════════════════════════════════════════════════════════

class TargetCacheError(RuntimeError):
    """Raised when the target snapshot (``xmake show -l targets``) cannot be
    obtained or parsed.

    This is an infrastructure failure.  It MUST surface as ``RUNNER_ERROR``
    rather than being silently turned into an empty target cache: an empty
    cache would make every TSan/ASan/fuzz target appear absent, fabricating
    bogus SKIPs and a false INCOMPLETE verdict.
    """


_ANSI_RE = re.compile(r"\x1b\[[0-9;<=>?]*[a-zA-Z]")
_TARGET_TOKEN_RE = re.compile(r"^[A-Za-z0-9_.+-]+$")


def _strip_ansi(s: str) -> str:
    return _ANSI_RE.sub("", s)


def _is_valid_target_token(s: str) -> bool:
    return bool(_TARGET_TOKEN_RE.match(s))


def parse_target_list(output: str) -> Set[str]:
    """Parse ``xmake show -l targets`` output into a set of target names.

    Real xmake output may place several targets on one line separated by
    whitespace (multi-column layout) and may embed ANSI color codes.  Each
    line is first stripped of ANSI escapes, then split on whitespace, and
    each token is validated independently; invalid tokens are ignored and
    duplicates are collapsed by the set.
    """
    targets: Set[str] = set()
    for line in output.splitlines():
        clean_line = _strip_ansi(line)
        for token in clean_line.split():
            if _is_valid_target_token(token):
                targets.add(token)
    return targets


def _truncate_for_log(s: object, limit: int = 2000) -> str:
    """Render *s* (str or bytes, possibly None) as a bounded log string."""
    if s is None:
        return "<none>"
    if isinstance(s, bytes):
        s = s.decode("utf-8", errors="replace")
    if not isinstance(s, str):
        s = str(s)
    if len(s) <= limit:
        return s
    return s[:limit] + f"\n...<truncated {len(s) - limit} chars>"


def _target_cache_failure(
    out_file: Path,
    mode: str,
    command: List[str],
    returncode: Optional[int],
    stdout: object,
    stderr: object,
    exception: Optional[BaseException],
    note: Optional[str] = None,
) -> TargetCacheError:
    """Build a ``TargetCacheError``, persisting full detail to *out_file*.

    The message carries everything needed to diagnose the failure: command,
    return code, stdout, stderr, the exception (if any), and the mode.
    """
    lines = [
        f"target snapshot failed (mode={mode})",
        f"command={shlex.join(command)}",
        f"returncode={returncode}",
    ]
    if note:
        lines.append(f"note={note}")
    if exception is not None:
        lines.append(f"exception={type(exception).__name__}: {exception}")
    lines.append("----- stdout -----")
    lines.append(_truncate_for_log(stdout))
    lines.append("----- stderr -----")
    lines.append(_truncate_for_log(stderr))
    message = "\n".join(lines)
    try:
        out_file.write_text(message + "\n")
    except OSError:
        pass
    return TargetCacheError(message)


def refresh_target_cache(ctx: PhaseContext, mode: str) -> Set[str]:
    """Run ``xmake show -l targets`` and parse the output into the cache.

    Invalidates any previous cache for this mode.

    Raises ``TargetCacheError`` on any infrastructure failure: non-zero exit,
    timeout, spawn/OS error, or zero parsed targets.  Never silently returns
    an empty set (see ``TargetCacheError``).
    """
    ctx._current_cache_mode = mode
    out_file = ctx.run_dir / f"{mode}-targets.txt"
    command = ["xmake", "show", "-l", "targets"]

    # Clear any stale cache entry for this mode before the refresh, so a
    # failure leaves no misleading cached state.
    ctx.target_cache.pop(mode, None)

    try:
        r = subprocess.run(command, capture_output=True, text=True, timeout=60)
    except subprocess.TimeoutExpired as e:
        raise _target_cache_failure(
            out_file, mode, command, returncode=None,
            stdout=getattr(e, "stdout", None), stderr=getattr(e, "stderr", None),
            exception=e, note="xmake show timed out",
        ) from e
    except (FileNotFoundError, OSError) as e:
        raise _target_cache_failure(
            out_file, mode, command, returncode=None, stdout=None, stderr=None,
            exception=e, note="failed to execute xmake",
        ) from e

    if r.returncode != 0:
        raise _target_cache_failure(
            out_file, mode, command, returncode=r.returncode,
            stdout=r.stdout, stderr=r.stderr, exception=None,
            note="xmake show exited non-zero",
        )

    targets = parse_target_list(r.stdout)
    if not targets:
        raise _target_cache_failure(
            out_file, mode, command, returncode=r.returncode,
            stdout=r.stdout, stderr=r.stderr, exception=None,
            note="parsed zero targets (expected many in this repository)",
        )

    sorted_targets = sorted(targets)
    try:
        out_file.write_text("\n".join(sorted_targets) + "\n")
    except OSError as e:
        raise _target_cache_failure(
            out_file, mode, command, returncode=r.returncode,
            stdout=r.stdout, stderr=r.stderr, exception=e,
            note=f"failed to write target snapshot: {e}",
        ) from e
    ctx.target_cache[mode] = targets
    return targets


def target_exists(ctx: PhaseContext, name: str) -> bool:
    """Check if a target exists in the current cached mode."""
    mode = ctx._current_cache_mode
    cache = ctx.target_cache.get(mode)
    if cache is None:
        return False
    return name in cache


# ═══════════════════════════════════════════════════════════════════════════════
# Command helpers
# ═══════════════════════════════════════════════════════════════════════════════

def _cmd(
    ctx: PhaseContext,
    phase: str,
    iteration: str,
    target: str,
    mode: str,
    command: List[str],
    timeout_s: Optional[float] = None,
    sanitizer_kind: Optional[str] = None,
    env: Optional[Dict[str, str]] = None,
    log_subdir: str = "",
    synthetic: bool = False,
) -> CommandResult:
    """Build a CommandSpec, run it, and record the result.

    Returns the CommandResult.
    """
    if timeout_s is None:
        timeout_s = float(ctx.config.phase_timeout_seconds)
    timeout = min(timeout_s, ctx.remaining_seconds())

    # Build log path.
    if log_subdir:
        log_dir = ctx.run_dir / log_subdir
    else:
        log_dir = ctx.run_dir / "logs"
    log_dir.mkdir(parents=True, exist_ok=True)
    log_path = log_dir / f"{phase}-{iteration}-{target}.log"

    child_env: Dict[str, str] = {}
    if env:
        child_env.update(env)

    spec = CommandSpec(
        phase=phase,
        iteration=iteration,
        target=target,
        mode=mode,
        command=command,
        timeout_seconds=timeout,
        log_path=log_path,
        environment=child_env,
        sanitizer_kind=sanitizer_kind,
        synthetic=synthetic,
        heartbeat_seconds=ctx.config.heartbeat_seconds,
        heartbeats_path=ctx.run_dir / "heartbeats.jsonl",
        run_log_path=ctx.run_dir / "run.log",
    )

    result = run_command(spec, ctx.head_sha, ctx.worktree_dirty,
                         global_remaining_fn=ctx.remaining_seconds)
    ctx.results.append(result)

    # Update stats.
    stats = ctx.get_or_create_stats(phase)
    stats.iteration += 1
    stats.executed += 1
    if result.classification == Classification.PASS:
        stats.passed += 1
    elif result.classification == Classification.FAIL:
        stats.failed += 1
    elif result.classification == Classification.TIMEOUT:
        stats.timed_out += 1
    elif result.classification == Classification.SKIP:
        stats.skipped += 1
    elif result.classification == Classification.SANITIZER_FAIL:
        stats.sanitizer_fail += 1
    elif result.classification == Classification.FUZZ_CRASH:
        stats.fuzz_crash += 1
    elif result.classification == Classification.BUILD_FAIL:
        stats.build_fail += 1

    # Track failures.
    if not synthetic and result.classification in (
        Classification.FAIL,
        Classification.TIMEOUT,
        Classification.SANITIZER_FAIL,
        Classification.FUZZ_CRASH,
        Classification.BUILD_FAIL,
        Classification.RUNNER_ERROR,
    ):
        ctx.failures.append(result)

    return result


def _log(ctx: PhaseContext, msg: str) -> None:
    """Log a message to the run log and stderr."""
    import datetime
    ts = datetime.datetime.now(datetime.timezone.utc).isoformat()
    line = f"{ts} {msg}"
    print(line, file=__import__("sys").stderr)

    # Also write to run.log.
    run_log = ctx.run_dir / "run.log"
    try:
        with open(run_log, "a") as f:
            f.write(line + "\n")
    except OSError:
        pass


# ═══════════════════════════════════════════════════════════════════════════════
# Phase A -- Debug baseline
# ═══════════════════════════════════════════════════════════════════════════════

def phase_baseline(ctx: PhaseContext) -> PhaseOutcome:
    """Configure Debug, build production libs, run full test suite,
    negative-compile scripts, and acceptance consumers."""
    _log(ctx, "[baseline] configuring clang debug")

    # Configure.
    r = _cmd(ctx, "baseline", "0", "configure", "debug",
             ["xmake", "f", "-c", "-m", "debug", "--toolchain=clang", "-y"])
    if r.classification != Classification.PASS:
        _log(ctx, "[baseline][FAIL] configure")
        ctx.sticky_hold = True
        return PhaseOutcome("baseline", passed=False)

    # Refresh target cache after configure.
    refresh_target_cache(ctx, "debug")

    # Build sluice_core.
    r = _cmd(ctx, "baseline", "0", "sluice_core", "debug",
             ["xmake", "build", "sluice_core"])
    if r.classification != Classification.PASS:
        _log(ctx, "[baseline][FAIL] sluice_core build")
        ctx.sticky_hold = True
        return PhaseOutcome("baseline", passed=False)

    # Build sluice_async.
    r = _cmd(ctx, "baseline", "0", "sluice_async", "debug",
             ["xmake", "build", "sluice_async"])
    if r.classification != Classification.PASS:
        _log(ctx, "[baseline][FAIL] sluice_async build")
        ctx.sticky_hold = True
        return PhaseOutcome("baseline", passed=False)

    # Build test group.
    r = _cmd(ctx, "baseline", "0", "build-test-group", "debug",
             ["xmake", "build", "-g", "test"])
    if r.classification != Classification.PASS:
        _log(ctx, "[baseline][FAIL] build -g test")
        ctx.sticky_hold = True
        return PhaseOutcome("baseline", passed=False)

    # Run full test suite.
    r = _cmd(ctx, "baseline", "0", "full-suite", "debug",
             ["xmake", "test", "-v"])
    if r.classification == Classification.PASS:
        ctx.baseline_ok = True
        _log(ctx, "[baseline] full suite PASS")
    else:
        _log(ctx, "[baseline][FAIL] full suite")
        ctx.sticky_hold = True

    # Negative-compile scripts.
    for script_rel in NEG_COMPILE_SCRIPTS:
        script_path = ctx.project_root / script_rel
        name = os.path.basename(script_rel).replace(".sh", "")
        if script_path.is_file():
            r = _cmd(ctx, "baseline", "0", name, "debug",
                     ["bash", str(script_path)])
            if r.classification != Classification.PASS:
                _log(ctx, f"[baseline][FAIL] {name}")
                ctx.sticky_hold = True
        else:
            _log(ctx, f"[baseline][SKIP] {script_rel} (missing)")

    # Acceptance consumers.
    for consumer in ACCEPTANCE_CONSUMERS:
        if target_exists(ctx, consumer):
            r = _cmd(ctx, "baseline", "0", f"build:{consumer}", "debug",
                     ["xmake", "build", consumer])
            if r.classification != Classification.PASS:
                _log(ctx, f"[baseline][FAIL] build {consumer}")
                ctx.sticky_hold = True
            r = _cmd(ctx, "baseline", "0", f"run:{consumer}", "debug",
                     ["xmake", "run", consumer])
            if r.classification != Classification.PASS:
                _log(ctx, f"[baseline][FAIL] run {consumer}")
                ctx.sticky_hold = True
        else:
            _log(ctx, f"[baseline][SKIP] {consumer} (target absent)")

    return PhaseOutcome("baseline", passed=ctx.baseline_ok)


# ═══════════════════════════════════════════════════════════════════════════════
# Phase B -- Debug soak
# ═══════════════════════════════════════════════════════════════════════════════

# Classifications that count as a reproduced soak failure when they survive
# (are not recovered by) the reproduction retries.
_SOAK_FAILURE_KINDS = (
    Classification.FAIL,
    Classification.TIMEOUT,
    Classification.SANITIZER_FAIL,
)


def soak_next_consec_fail(
    initial: Classification,
    retries: List[Classification],
    prev: int,
) -> int:
    """Compute the next "consecutive unrecovered failures" count for debug-soak.

    This deliberately separates two concepts:

    *   A *sticky* test failure — recorded via ``sticky_hold`` by the caller and
        never cleared here.  A failure that happened once is a HOLD forever,
        even if every retry passes.
    *   A *consecutive unrecovered failure* — counted here, and used only to
        decide whether to stop repeating the soak loop early.

    Rules:

    *   initial ``PASS`` → reset to 0.
    *   initial failure reproduced by at least one retry (a retry also failed),
        or with no retries at all → unrecovered → ``prev + 1``.
    *   initial failure whose retries all passed → not reproduced → reset to 0.
        The original failure remains sticky; this does not wash it away.
    *   initial classification that is not a soak failure kind (e.g. SKIP) →
        leave the count unchanged.
    """
    if initial == Classification.PASS:
        return 0
    if initial not in _SOAK_FAILURE_KINDS:
        return prev
    if retries and all(c == Classification.PASS for c in retries):
        return 0
    return prev + 1


def phase_debug_soak(ctx: PhaseContext, budget_seconds: float) -> PhaseOutcome:
    """Repeat the full ``xmake test -v`` suite for up to *budget_seconds*."""
    if not ctx.baseline_ok:
        _log(ctx, "[debug-soak][SKIP] baseline build failed")
        return PhaseOutcome("debug-soak", passed=True)

    per_cmd = float(ctx.config.phase_timeout_seconds)
    iter_count = 0
    consec_fail = 0
    start_mono = time.monotonic()

    _log(ctx, f"[debug-soak] budget={budget_seconds:.0f}s per-cmd={per_cmd:.0f}s")

    while time.monotonic() - start_mono < budget_seconds:
        if ctx.remaining_seconds() < per_cmd:
            _log(ctx, "[debug-soak] insufficient remaining time; stopping")
            break
        if not ctx.config.keep_going and iter_count > 0:
            _log(ctx, "[debug-soak] KEEP_GOING=0; stopping")
            break

        iter_count += 1
        _log(ctx, f"[debug-soak] iteration {iter_count}")

        r = _cmd(ctx, "debug-soak", str(iter_count), "full-suite", "debug",
                 ["xmake", "test", "-v"],
                 log_subdir="debug-soak")

        # Any real failure is sticky (a HOLD forever). Retries are recorded as
        # reproduction evidence; they determine only whether the failure counts
        # as "unrecovered" for the early-stop heuristic, never whether it stays
        # sticky.
        retries: List[Classification] = []
        if r.classification in _SOAK_FAILURE_KINDS:
            ctx.sticky_hold = True
            for retry in range(1, 3):
                rr = _cmd(ctx, "debug-soak", f"{iter_count}-retry{retry}",
                          "full-suite", "debug", ["xmake", "test", "-v"],
                          log_subdir="debug-soak")
                retries.append(rr.classification)
                _log(ctx, f"[debug-soak] iteration {iter_count} retry {retry}: "
                     f"{rr.classification.value}")

        consec_fail = soak_next_consec_fail(r.classification, retries, consec_fail)

        if consec_fail >= 3:
            # Stop only this repeating soak loop. TSan, ASan, fuzz, and Final
            # Debug are independent phases and still run.
            _log(ctx, "[debug-soak] 3 consecutive unrecovered failures; "
                 "stopping soak loop")
            break

    stats = ctx.get_or_create_stats("debug-soak")
    _log(ctx, f"[debug-soak] done (iter={stats.iteration} pass={stats.passed} "
         f"fail={stats.failed} timeout={stats.timed_out})")
    return PhaseOutcome("debug-soak", passed=True)


# ═══════════════════════════════════════════════════════════════════════════════
# Phase C -- TSan hot set
# ═══════════════════════════════════════════════════════════════════════════════

def phase_tsan(ctx: PhaseContext, budget_seconds: float) -> PhaseOutcome:
    """Configure TSan, build test group, and run the hot set."""
    _log(ctx, "[tsan] configuring clang tsan")

    r = _cmd(ctx, "tsan", "0", "configure", "tsan",
             ["xmake", "f", "-c", "-m", "tsan", "--toolchain=clang", "-y"])
    if r.classification != Classification.PASS:
        _log(ctx, "[tsan][FAIL] configure; skipping phase")
        ctx.sticky_hold = True
        return PhaseOutcome("tsan", passed=False)

    refresh_target_cache(ctx, "tsan")

    r = _cmd(ctx, "tsan", "0", "build-test-group", "tsan",
             ["xmake", "build", "-g", "test"])
    if r.classification != Classification.PASS:
        _log(ctx, "[tsan][FAIL] build -g test; skipping hot set")
        ctx.sticky_hold = True
        return PhaseOutcome("tsan", passed=False)

    per_cmd = float(ctx.config.phase_timeout_seconds)
    tsan_env = {"TSAN_OPTIONS": "halt_on_error=1:second_deadlock_stack=1"}
    set_iter = 0
    critical_exec = 0
    start_mono = time.monotonic()

    _log(ctx, f"[tsan] budget={budget_seconds:.0f}s per-cmd={per_cmd:.0f}s "
         f"targets={len(TSAN_HOT_SET)}")

    while time.monotonic() - start_mono < budget_seconds:
        if ctx.remaining_seconds() < per_cmd:
            break
        if not ctx.config.keep_going and set_iter > 0:
            _log(ctx, "[tsan] KEEP_GOING=0; stopping")
            break

        set_iter += 1
        for tgt in TSAN_HOT_SET:
            if ctx.remaining_seconds() < per_cmd:
                break
            if time.monotonic() - start_mono >= budget_seconds:
                break

            if not target_exists(ctx, tgt):
                _log(ctx, f"[tsan][SKIP] iteration {set_iter}: {tgt} (absent)")
                continue

            _log(ctx, f"[tsan] iteration {set_iter}: {tgt}")
            r = _cmd(ctx, "tsan", str(set_iter), tgt, "tsan",
                     ["xmake", "run", tgt],
                     sanitizer_kind="tsan",
                     env=tsan_env,
                     log_subdir="tsan")

            if r.classification == Classification.PASS:
                critical_exec += 1
            elif r.classification == Classification.SANITIZER_FAIL:
                critical_exec += 1
                ctx.sticky_hold = True
            elif r.classification == Classification.FAIL:
                critical_exec += 1
                ctx.sticky_hold = True
            elif r.classification == Classification.TIMEOUT:
                ctx.sticky_hold = True

            if not ctx.config.keep_going:
                break

    stats = ctx.get_or_create_stats("tsan")
    _log(ctx, f"[tsan] done (set_iters={set_iter} exec={stats.executed} "
         f"pass={stats.passed} fail={stats.failed} timeout={stats.timed_out} "
         f"skip={stats.skipped} san={stats.sanitizer_fail} "
         f"critical={critical_exec})")
    return PhaseOutcome("tsan", passed=True)


# ═══════════════════════════════════════════════════════════════════════════════
# Phase D -- ASan + UBSan
# ═══════════════════════════════════════════════════════════════════════════════

def phase_asanubsan(ctx: PhaseContext, budget_seconds: float) -> PhaseOutcome:
    """Configure ASan+UBSan, run full suite, then hot set."""
    _log(ctx, "[asanubsan] configuring clang asanubsan")

    r = _cmd(ctx, "asanubsan", "0", "configure", "asanubsan",
             ["xmake", "f", "-c", "-m", "asanubsan", "--toolchain=clang", "-y"])
    if r.classification != Classification.PASS:
        _log(ctx, "[asanubsan][FAIL] configure; skipping phase")
        ctx.sticky_hold = True
        return PhaseOutcome("asanubsan", passed=False)

    refresh_target_cache(ctx, "asanubsan")

    r = _cmd(ctx, "asanubsan", "0", "build-test-group", "asanubsan",
             ["xmake", "build", "-g", "test"])
    if r.classification != Classification.PASS:
        _log(ctx, "[asanubsan][FAIL] build -g test; skipping hot set")
        ctx.sticky_hold = True
        return PhaseOutcome("asanubsan", passed=False)

    asan_env = {
        "ASAN_OPTIONS": "halt_on_error=1:detect_leaks=1",
        "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
    }

    # Full suite first.
    _log(ctx, "[asanubsan] full suite")
    full = _cmd(ctx, "asanubsan", "0", "full-suite", "asanubsan",
                ["xmake", "test", "-v"],
                sanitizer_kind="asan",
                env=asan_env,
                log_subdir="asanubsan")
    if full.classification in (Classification.SANITIZER_FAIL, Classification.FAIL,
                                Classification.TIMEOUT):
        ctx.sticky_hold = True
    if full.classification != Classification.PASS:
        _log(ctx, "[asanubsan] full suite: " + full.classification.value)

    # Hot set.
    per_cmd = float(ctx.config.phase_timeout_seconds)
    start_mono = time.monotonic()

    for tgt in ASAN_HOT_SET:
        if ctx.remaining_seconds() < per_cmd:
            break
        if time.monotonic() - start_mono >= budget_seconds:
            break

        if not target_exists(ctx, tgt):
            _log(ctx, f"[asanubsan][SKIP] {tgt} (absent)")
            continue

        _log(ctx, f"[asanubsan] {tgt}")
        r = _cmd(ctx, "asanubsan", "0", tgt, "asanubsan",
                 ["xmake", "run", tgt],
                 sanitizer_kind="asan",
                 env=asan_env,
                 log_subdir="asanubsan")

        if r.classification in (Classification.SANITIZER_FAIL, Classification.FAIL,
                                Classification.TIMEOUT):
            ctx.sticky_hold = True

        if not ctx.config.keep_going:
            _log(ctx, "[asanubsan] KEEP_GOING=0; stopping")
            break

    stats = ctx.get_or_create_stats("asanubsan")
    _log(ctx, f"[asanubsan] done (exec={stats.executed} pass={stats.passed} "
         f"skip={stats.skipped} san={stats.sanitizer_fail})")
    return PhaseOutcome("asanubsan", passed=True)


# ═══════════════════════════════════════════════════════════════════════════════
# Phase E -- Fuzz
# ═══════════════════════════════════════════════════════════════════════════════

def _is_fuzz_artifact(name: str) -> bool:
    return any(
        name.startswith(prefix)
        for prefix in ["crash-", "leak-", "timeout-", "oom-", "panic-"]
    )


def _list_fuzz_artifacts(artifact_dir: Path) -> List[str]:
    if not artifact_dir.is_dir():
        return []
    result = []
    for f in artifact_dir.iterdir():
        if f.is_file() and _is_fuzz_artifact(f.name):
            result.append(f.name)
    return sorted(result)


def _corpus_file_count(corpus_dir: Path) -> int:
    if not corpus_dir.is_dir():
        return 0
    return sum(1 for f in corpus_dir.iterdir() if f.is_file())


def _corpus_file_bytes(corpus_dir: Path) -> int:
    if not corpus_dir.is_dir():
        return 0
    total = 0
    for f in corpus_dir.iterdir():
        if f.is_file():
            try:
                total += f.stat().st_size
            except OSError:
                pass
    return total


def _committed_corpus_dir(project_root: Path, target: str) -> Path:
    mapping = {
        "wal_read_record_fuzz": "fuzz/corpus/wal_read_record",
        "wal_roundtrip_fuzz": "fuzz/corpus/wal_roundtrip",
        "copy_all_fault_fuzz": "fuzz/corpus/copy_all_fault",
    }
    rel = mapping.get(target, f"fuzz/corpus/{target}")
    return project_root / rel


def build_fuzz_argv(
    target: str,
    corpus: str,
    artifact_dir: str,
    per_target: int,
    maxlen: int,
    dict_path: Optional[str] = None,
    rss_limit_mb: int = 1024,
    timeout: int = 120,
) -> List[str]:
    """Build the libFuzzer argv for a single fuzz target.

    This is a pure function extracted so both ``phase_fuzz()`` and
    unit tests can exercise the same argv construction.
    """
    argv = [
        "xmake", "run", target, "--",
        corpus,
        f"-max_total_time={per_target}",
        f"-artifact_prefix={artifact_dir}",
        f"-rss_limit_mb={rss_limit_mb}",
        f"-max_len={maxlen}",
        f"-timeout={timeout}",
    ]
    if dict_path is not None:
        argv.append(f"-dict={dict_path}")
    return argv


def phase_fuzz(ctx: PhaseContext, budget_seconds: float) -> PhaseOutcome:
    """Configure Debug (fuzz build), build fuzz group, and run each fuzz target."""
    _log(ctx, "[fuzz] configuring clang debug (fuzz build)")

    r = _cmd(ctx, "fuzz", "0", "configure", "debug",
             ["xmake", "f", "-c", "-m", "debug", "--toolchain=clang", "-y"])
    if r.classification != Classification.PASS:
        _log(ctx, "[fuzz][FAIL] configure; skipping fuzz")
        ctx.sticky_hold = True
        return PhaseOutcome("fuzz", passed=False)

    refresh_target_cache(ctx, "debug")

    r = _cmd(ctx, "fuzz", "0", "build-fuzz-group", "debug",
             ["xmake", "build", "-g", "fuzz"])
    if r.classification != Classification.PASS:
        _log(ctx, "[fuzz][FAIL] build -g fuzz; skipping fuzz")
        ctx.sticky_hold = True
        return PhaseOutcome("fuzz", passed=False)

    # Determine which targets actually exist.
    existing = [t for t in FUZZ_TARGETS if target_exists(ctx, t)]
    if not existing:
        _log(ctx, "[fuzz] no fuzz targets present; skipping")
        return PhaseOutcome("fuzz", passed=True)

    n = len(existing)
    total_budget = budget_seconds
    if ctx.config.fuzz_seconds_override is not None:
        total_budget = float(ctx.config.fuzz_seconds_override)
    if total_budget <= 0:
        _log(ctx, "[fuzz] budget exhausted; skipping")
        return PhaseOutcome("fuzz", passed=True)

    per_target = max(5, int(total_budget / n))
    _log(ctx, f"[fuzz] targets={n} total_budget={total_budget:.0f}s "
         f"per-target={per_target}s")

    hardening_root = ctx.project_root / ".hardening-corpus"
    hardening_root.mkdir(parents=True, exist_ok=True)
    fuzz_subdir = ctx.run_dir / "fuzz"
    fuzz_subdir.mkdir(parents=True, exist_ok=True)

    target_index = 0
    for tgt in existing:
        target_index += 1
        if ctx.remaining_seconds() < per_target:
            _log(ctx, f"[fuzz] not enough time for {tgt}; stopping")
            break

        # Persistent corpus.
        work_corpus = hardening_root / tgt
        work_corpus.mkdir(parents=True, exist_ok=True)

        # Seed from committed corpus if empty.
        committed = _committed_corpus_dir(ctx.project_root, tgt)
        if committed.is_dir() and not any(work_corpus.iterdir()):
            shutil.copytree(committed, work_corpus, dirs_exist_ok=True)

        # Artifact directory.
        artifact_dir = fuzz_subdir / tgt
        artifact_dir.mkdir(parents=True, exist_ok=True)

        # Baseline artifact count.
        base_artifacts = set(_list_fuzz_artifacts(artifact_dir))

        # Corpus snapshot before.
        before_files = _corpus_file_count(work_corpus)
        before_bytes = _corpus_file_bytes(work_corpus)

        maxlen = FUZZ_MAXLEN.get(tgt, 1048576)
        # Per-target dictionary (see FUZZ_DICTS above).
        dict_name = FUZZ_DICTS.get(tgt)
        dict_path: Optional[str] = None
        if dict_name is not None:
            dp = ctx.project_root / "fuzz" / "dictionaries" / dict_name
            if dp.is_file():
                dict_path = str(dp)
        fuzz_args = build_fuzz_argv(
            target=tgt,
            corpus=str(work_corpus),
            artifact_dir=str(artifact_dir) + "/",
            per_target=per_target,
            maxlen=maxlen,
            dict_path=dict_path,
        )

        fuzz_env = {
            "ASAN_OPTIONS": "halt_on_error=1:detect_leaks=1",
            "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1",
        }

        # Wrapper timeout = per-target + 180s grace.
        wrapper_timeout = per_target + 180

        # Banner: phase index (5/6), expected finish, wrapper timeout, log path.
        now_ts = datetime.datetime.now(datetime.timezone.utc)
        expected_finish = now_ts + datetime.timedelta(seconds=per_target)
        hard_timeout = now_ts + datetime.timedelta(seconds=wrapper_timeout)
        _log(ctx, f"[fuzz] [phase 5/6] target {target_index}/{n}: {tgt} "
             f"(budget={_human_dur(per_target)}, "
             f"expected {expected_finish.strftime('%H:%M:%S')}Z, "
             f"wrapper={_human_dur(wrapper_timeout)}, "
             f"hard {hard_timeout.strftime('%H:%M:%S')}Z)")
        _log(ctx, f"[fuzz]   corpus={work_corpus} artifact={artifact_dir}")

        r = _cmd(ctx, "fuzz", str(target_index), tgt, "debug", fuzz_args,
                 timeout_s=float(wrapper_timeout),
                 sanitizer_kind="asan",
                 env=fuzz_env,
                 log_subdir="fuzz")

        # Corpus snapshot after.
        after_files = _corpus_file_count(work_corpus)
        after_bytes = _corpus_file_bytes(work_corpus)

        # New artifacts.
        after_artifacts = set(_list_fuzz_artifacts(artifact_dir))
        new_artifacts = sorted(after_artifacts - base_artifacts)

        # Determine verdict and build end banner FIRST.  The new-artifact
        # branch may rewrite r.classification (e.g. PASS -> FUZZ_CRASH) and
        # set sticky_hold; the corpus snapshot recorded below must capture
        # the *final* classification so ctx.fuzz_results (and the summary.json
        # derived from it) agrees with the per-target banner and verdict.
        raw_exit = r.exit_code if r.exit_code is not None else "?"
        elapsed_str = _human_dur(int(r.duration_seconds))
        corpus_summary = f"corpus: {before_files}→{after_files} files ({before_bytes}→{after_bytes} bytes)"

        if new_artifacts:
            if r.classification == Classification.PASS:
                r.classification = Classification.FUZZ_CRASH
            ctx.sticky_hold = True
            _log(ctx, f"[fuzz] {tgt}: FUZZ_CRASH ({elapsed_str}, "
                 f"exit={raw_exit}), "
                 f"{corpus_summary}, "
                 f"new artifacts={len(new_artifacts)}")
        elif r.classification == Classification.PASS:
            _log(ctx, f"[fuzz] {tgt}: PASS ({elapsed_str}, "
                 f"exit={raw_exit}), "
                 f"{corpus_summary}")
        elif r.classification == Classification.TIMEOUT:
            ctx.sticky_hold = True
            _log(ctx, f"[fuzz] {tgt}: TIMEOUT ({elapsed_str}, "
                 f"exit={raw_exit}), "
                 f"{corpus_summary}")
        elif r.classification in (Classification.SANITIZER_FAIL, Classification.FAIL):
            ctx.sticky_hold = True
            _log(ctx, f"[fuzz] {tgt}: {r.classification.value} ({elapsed_str}, "
                 f"exit={raw_exit}), "
                 f"{corpus_summary}")

        # Record the corpus snapshot with the post-verdict classification.
        fuzz_snap = FuzzCorpusSnapshot(
            target=tgt,
            before_files=before_files,
            before_bytes=before_bytes,
            after_files=after_files,
            after_bytes=after_bytes,
            new_artifacts=new_artifacts,
            classification=r.classification,
        )
        ctx.fuzz_results.append(fuzz_snap)

        # Next target / phase indicator.
        if target_index < n:
            next_tgt = existing[target_index]
            _log(ctx, f"[fuzz]   next: {next_tgt} ({target_index + 1}/{n})")
        else:
            _log(ctx, "[fuzz]   next_phase=final-debug")

        # Write corpus stats.
        stats_path = fuzz_subdir / f"{tgt}.corpus-stats.txt"
        try:
            with open(stats_path, "w") as f:
                f.write(f"target={tgt}\n")
                f.write(f"corpus_before_files={before_files}\n")
                f.write(f"corpus_before_bytes={before_bytes}\n")
                f.write(f"corpus_after_files={after_files}\n")
                f.write(f"corpus_after_bytes={after_bytes}\n")
                f.write(f"artifact_prefix={artifact_dir}\n")
                f.write(f"classification={r.classification.value}\n")
                f.write(f"new_artifacts={len(new_artifacts)}\n")
                if new_artifacts:
                    for a in new_artifacts:
                        f.write(f"artifact={a}\n")
        except OSError:
            pass

        if not ctx.config.keep_going:
            _log(ctx, "[fuzz] KEEP_GOING=0; stopping")
            break

    return PhaseOutcome("fuzz", passed=True)


# ═══════════════════════════════════════════════════════════════════════════════
# Phase F -- Final Debug
# ═══════════════════════════════════════════════════════════════════════════════

def phase_final_debug(ctx: PhaseContext) -> PhaseOutcome:
    """Reconfigure Debug and run the full suite one final time."""
    _log(ctx, "[final-debug] configuring clang debug")

    r = _cmd(ctx, "final", "0", "configure", "debug",
             ["xmake", "f", "-c", "-m", "debug", "--toolchain=clang", "-y"])
    if r.classification != Classification.PASS:
        _log(ctx, "[final-debug][FAIL] configure")
        ctx.sticky_hold = True
        return PhaseOutcome("final-debug", passed=False)

    r = _cmd(ctx, "final", "0", "build-test-group", "debug",
             ["xmake", "build", "-g", "test"])
    if r.classification != Classification.PASS:
        _log(ctx, "[final-debug][FAIL] build -g test")
        ctx.sticky_hold = True
        return PhaseOutcome("final-debug", passed=False)

    r = _cmd(ctx, "final", "0", "full-suite", "debug",
             ["xmake", "test", "-v"])
    if r.classification == Classification.PASS:
        ctx.final_debug_ok = True
        _log(ctx, "[final-debug] full suite PASS")
    else:
        _log(ctx, "[final-debug][FAIL] full suite")
        ctx.sticky_hold = True

    return PhaseOutcome("final-debug", passed=ctx.final_debug_ok)


# ═══════════════════════════════════════════════════════════════════════════════
# Verdict calculation
# =============================================================================


def calculate_verdict(
    ctx: PhaseContext,
    preflight: PreflightResult,
) -> Verdict:
    """Determine the final verdict based on all evidence."""
    if not preflight.passed:
        return Verdict.ENVIRONMENT_ERROR

    if ctx.sticky_hold:
        return Verdict.HOLD

    incomplete_reasons: List[str] = []

    if not ctx.baseline_ok:
        incomplete_reasons.append("baseline did not complete")

    tsan_critical_executed = 0
    for r in ctx.results:
        if r.phase == "tsan" and r.target in TSAN_HOT_SET:
            if r.classification != Classification.SKIP:
                tsan_critical_executed += 1
    if tsan_critical_executed == 0:
        incomplete_reasons.append("no critical TSan target executed")

    asan_stats = ctx.stats.get("asanubsan")
    if not asan_stats or asan_stats.executed == 0:
        incomplete_reasons.append("ASan+UBSan full suite not executed")

    fuzz_stats = ctx.stats.get("fuzz")
    if not fuzz_stats or fuzz_stats.executed == 0:
        incomplete_reasons.append("no fuzz target executed")

    if not ctx.final_debug_ok:
        incomplete_reasons.append("final debug not completed")

    if incomplete_reasons:
        return Verdict.INCOMPLETE

    return Verdict.PASS


# ═══════════════════════════════════════════════════════════════════════════════
# Synthetic command helper (used by self-test)
# =============================================================================


def _synthetic_cmd(
    ctx: PhaseContext,
    phase: str,
    iteration: str,
    target: str,
    mode: str,
    command: List[str],
    timeout_s: float = 10,
    env: Optional[Dict[str, str]] = None,
) -> CommandResult:
    """Run a synthetic command (no sticky hold, no failure tracking)."""
    log_dir = ctx.run_dir / "logs"
    log_dir.mkdir(parents=True, exist_ok=True)
    log_path = log_dir / f"{phase}-{iteration}-{target}.log"

    spec = CommandSpec(
        phase=phase,
        iteration=iteration,
        target=target,
        mode=mode,
        command=command,
        timeout_seconds=timeout_s,
        log_path=log_path,
        environment=env or {},
        sanitizer_kind=None,
        synthetic=True,
    )
    result = run_command(spec, ctx.head_sha, ctx.worktree_dirty)
    ctx.results.append(result)
    return result