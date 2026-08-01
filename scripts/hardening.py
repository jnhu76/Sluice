#!/usr/bin/env python3
"""Sluice hardening test runner — Python standard library implementation.

Usage:
    python3 scripts/hardening.py
    python3 scripts/hardening.py --smoke
    python3 scripts/hardening.py --self-test
    python3 scripts/hardening.py --hours 6
    python3 scripts/hardening.py --version-b
    python3 scripts/hardening.py --help
"""

from __future__ import annotations

import datetime
import fcntl
import json
import os
import shutil
import signal
import subprocess
import sys
import textwrap
import time
import traceback
from pathlib import Path
from typing import Dict, List, Optional, Set

# Ensure the package is importable when run as a script.
_SCRIPT_DIR = Path(__file__).resolve().parent
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))

from hardening.model import (
    Classification,
    CommandResult,
    CommandSpec,
    Config,
    FuzzCorpusSnapshot,
    PhaseStats,
    Verdict,
    VERDICT_EXIT,
    EXIT_PASS,
    EXIT_HOLD,
    EXIT_ENVIRONMENT_ERROR,
    EXIT_RUNNER_ERROR,
    EXIT_INCOMPLETE,
)
from hardening.preflight import PreflightCheck, PreflightResult, run_preflight
from hardening.process import run_command
from hardening.phases import (
    PhaseContext,
    PhaseOutcome,
    TargetCacheError,
    calculate_verdict,
    _synthetic_cmd,
    refresh_target_cache,
    target_exists,
    phase_baseline,
    phase_debug_soak,
    phase_tsan,
    phase_asanubsan,
    phase_fuzz,
    phase_final_debug,
    phase_version_b_debug_soak,
    phase_version_b_tsan,
    phase_version_b_asanubsan,
    version_b_calculate_verdict,
    TSAN_HOT_SET,
)
from hardening.reporting import (
    write_all_outputs,
    write_preflight_txt,
    write_preflight_json,
    write_environment_json,
)
from hardening.cli import parse_args


# ═══════════════════════════════════════════════════════════════════════════════
# Single-instance lock
# ═══════════════════════════════════════════════════════════════════════════════

_LOCK_FILE = ".hardening.lock"
_lock_fd: Optional[int] = None


def _acquire_lock(project_root: Path) -> None:
    """Acquire a non-blocking exclusive lock on ``.hardening.lock``.

    Raises ``RuntimeError`` if another runner is already running.
    """
    global _lock_fd
    lock_path = project_root / _LOCK_FILE
    try:
        _lock_fd = os.open(str(lock_path), os.O_CREAT | os.O_RDWR, 0o644)
        fcntl.flock(_lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except (IOError, OSError):
        if _lock_fd is not None:
            try:
                os.close(_lock_fd)
            except OSError:
                pass
            _lock_fd = None
        raise RuntimeError(
            f"Another hardening runner is running (lock: {lock_path}). "
            f"Use a different worktree or wait for it to finish."
        )


def _release_lock() -> None:
    global _lock_fd
    if _lock_fd is not None:
        try:
            fcntl.flock(_lock_fd, fcntl.LOCK_UN)
            os.close(_lock_fd)
        except OSError:
            pass
        _lock_fd = None


# ═══════════════════════════════════════════════════════════════════════════════
# Run directory setup
# ═══════════════════════════════════════════════════════════════════════════════

def _setup_run_dir(
    project_root: Path,
    head_short: str,
    preflight: PreflightResult,
    config: Config,
) -> Path:
    """Create the artifact directory for this run."""
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    run_dir = project_root / "hardening-artifacts" / f"{stamp}-{head_short}"
    run_dir.mkdir(parents=True, exist_ok=True)

    # Write preflight and environment.
    write_preflight_txt(run_dir, preflight)
    write_preflight_json(run_dir, preflight)
    write_environment_json(run_dir, config, preflight)

    # Write worktree diffs.
    if preflight.worktree_diff:
        (run_dir / "worktree.diff").write_text(preflight.worktree_diff)
    if preflight.worktree_cached_diff:
        (run_dir / "worktree-cached.diff").write_text(preflight.worktree_cached_diff)

    return run_dir


# ═══════════════════════════════════════════════════════════════════════════════
# Finalization
# ═══════════════════════════════════════════════════════════════════════════════

_FINALIZED = False


def finalize(
    verdict: Verdict,
    started_at: float,
    ctx: Optional[PhaseContext],
    config: Config,
    preflight: PreflightResult,
    interrupted: bool,
    runner_error: Optional[str] = None,
) -> int:
    """Write all outputs and return the exit code.

    Idempotent: only writes once.
    """
    global _FINALIZED
    if _FINALIZED:
        return VERDICT_EXIT.get(verdict, 1)
    _FINALIZED = True

    finished_at = time.time()

    if ctx is not None and ctx.run_dir is not None:
        write_all_outputs(
            run_dir=ctx.run_dir,
            verdict=verdict,
            config=config,
            preflight=preflight,
            started_at=started_at,
            finished_at=finished_at,
            results=ctx.results,
            failures=ctx.failures,
            phase_stats=ctx.stats,
            fuzz_results=ctx.fuzz_results,
            interrupted=interrupted,
            runner_error=runner_error,
        )

        # Print summary to stderr.
        summary_path = ctx.run_dir / "summary.txt"
        if summary_path.is_file():
            print(summary_path.read_text(), file=sys.stderr)

    exit_code = VERDICT_EXIT.get(verdict, 1)
    return exit_code


# ═══════════════════════════════════════════════════════════════════════════════
# Self-test
# ═══════════════════════════════════════════════════════════════════════════════

def self_test(config: Config, project_root: Path) -> int:
    """Run controlled synthetic tests of the runner itself.

    Does NOT run any real Sluice tests.  Uses only Python stdlib and
    temporary directories.
    """
    print("[self-test] running self-test...", file=sys.stderr)
    self_test_start = time.monotonic()

    # We need a minimal preflight to proceed.
    preflight = PreflightResult()
    preflight.head_sha = "selftest"
    preflight.head_short = "selftest"
    preflight.nproc = os.cpu_count() or 1
    preflight.checks.append(PreflightCheck(name="dummy", passed=True, is_fatal=False, message="ok"))

    # Create a temporary run directory.
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    run_dir = project_root / "hardening-artifacts" / f"selftest-{stamp}"
    run_dir.mkdir(parents=True, exist_ok=True)

    ctx = PhaseContext(
        config=config,
        project_root=project_root,
        run_dir=run_dir,
        head_sha="selftest",
        head_short="selftest",
        worktree_dirty=False,
        nproc=preflight.nproc,
        global_deadline=time.monotonic() + 300,
        final_debug_reserved=120,
        sticky_hold=False,
        baseline_ok=False,
        final_debug_ok=False,
    )

    all_pass = True

    # ── Test 1: failure does not prevent next command ──────────────────────
    print("[self-test] test 1: failure does not block next command", file=sys.stderr)
    r1 = _synthetic_cmd(ctx, "selftest", "1", "false-cmd", "debug",
                        ["false"], timeout_s=10)
    if r1.classification == Classification.FAIL:
        print("[self-test]   false command -> FAIL: OK", file=sys.stderr)
    else:
        print(f"[self-test]   FAIL: expected FAIL got {r1.classification.value}", file=sys.stderr)
        all_pass = False

    r2 = _synthetic_cmd(ctx, "selftest", "2", "true-cmd", "debug",
                        ["true"], timeout_s=10)
    if r2.classification == Classification.PASS:
        print("[self-test]   true command after failure -> PASS: OK", file=sys.stderr)
    else:
        print(f"[self-test]   FAIL: expected PASS got {r2.classification.value}", file=sys.stderr)
        all_pass = False

    # ── Test 2: silent process times out ON TIME (TERM, no KILL) ───────────
    print("[self-test] test 2: silent timeout fires on time (TERM, no KILL)",
          file=sys.stderr)
    r3 = _synthetic_cmd(ctx, "selftest", "3", "sleep-timeout", "debug",
                        ["sleep", "30"], timeout_s=2)
    t2_ok = (
        r3.classification == Classification.TIMEOUT
        and r3.timed_out is True
        and r3.term_sent is True
        and r3.kill_sent is False
        and r3.duration_seconds < 5
    )
    if t2_ok:
        print(f"[self-test]   sleep 30 @2s -> TIMEOUT, timed_out, TERM, no KILL, "
              f"dur={r3.duration_seconds:.2f}s (<5s): OK", file=sys.stderr)
    else:
        print(f"[self-test]   FAIL: cls={r3.classification.value} "
              f"timed_out={r3.timed_out} term={r3.term_sent} kill={r3.kill_sent} "
              f"dur={r3.duration_seconds:.2f}s "
              f"(expected TIMEOUT/True/True/False, dur<5s)", file=sys.stderr)
        all_pass = False

    # ── Test 3: TERM ignored → KILL after grace ────────────────────────────
    print("[self-test] test 3: TERM ignored -> KILL after grace", file=sys.stderr)
    # A Python script that ignores SIGTERM.
    kill_script = textwrap.dedent("""\
        import signal, time
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
        time.sleep(60)
    """)
    from hardening.process import KILL_AFTER_SECONDS
    t3_timeout = 2
    r4 = _synthetic_cmd(ctx, "selftest", "4", "sigterm-ignored", "debug",
                        [sys.executable, "-c", kill_script], timeout_s=t3_timeout)
    t3_expect = t3_timeout + KILL_AFTER_SECONDS
    t3_ok = (
        r4.classification == Classification.TIMEOUT
        and r4.timed_out is True
        and r4.term_sent is True
        and r4.kill_sent is True
        and (t3_expect - 1.0) <= r4.duration_seconds < (t3_expect + 8.0)
    )
    if t3_ok:
        print(f"[self-test]   SIGTERM-ignored -> TIMEOUT+TERM+KILL, "
              f"dur={r4.duration_seconds:.2f}s (~{t3_expect:.0f}s): OK",
              file=sys.stderr)
    else:
        print(f"[self-test]   FAIL: cls={r4.classification.value} "
              f"timed_out={r4.timed_out} term={r4.term_sent} kill={r4.kill_sent} "
              f"dur={r4.duration_seconds:.2f}s "
              f"(expected TIMEOUT/True/True/True, dur~{t3_expect:.0f}s)",
              file=sys.stderr)
        all_pass = False

    # ── Test 4: environment variables passed to child ──────────────────────
    print("[self-test] test 4: environment passing", file=sys.stderr)
    env_check = textwrap.dedent("""\
        import os
        val = os.environ.get('SLUICE_SELFTEST_ENV', '')
        exit(0 if val == 'expected' else 1)
    """)
    spec = CommandSpec(
        phase="selftest",
        iteration="5",
        target="env-check",
        mode="debug",
        command=[sys.executable, "-c", env_check],
        timeout_seconds=10,
        log_path=run_dir / "selftest-env.log",
        environment={"SLUICE_SELFTEST_ENV": "expected"},
        sanitizer_kind=None,
        synthetic=True,
    )
    r5 = run_command(spec, "selftest", False)
    ctx.results.append(r5)
    if r5.classification == Classification.PASS:
        print("[self-test]   env var passed to child: OK", file=sys.stderr)
    else:
        print(f"[self-test]   FAIL: expected PASS got {r5.classification.value}", file=sys.stderr)
        all_pass = False

    # ── Test 5: sanitizer exit-zero signature ──────────────────────────────
    print("[self-test] test 5: sanitizer exit-zero detection", file=sys.stderr)
    san_script = textwrap.dedent("""\
        import sys
        print("WARNING: ThreadSanitizer: data race (fake)")
        sys.exit(0)
    """)
    spec = CommandSpec(
        phase="selftest",
        iteration="6",
        target="san-exit-zero",
        mode="debug",
        command=[sys.executable, "-c", san_script],
        timeout_seconds=10,
        log_path=run_dir / "selftest-san.log",
        environment={},
        sanitizer_kind="tsan",
        synthetic=True,
    )
    r6 = run_command(spec, "selftest", False)
    ctx.results.append(r6)
    if r6.classification == Classification.SANITIZER_FAIL:
        print("[self-test]   exit-zero with TSan signature -> SANITIZER_FAIL: OK", file=sys.stderr)
    else:
        print(f"[self-test]   FAIL: expected SANITIZER_FAIL got {r6.classification.value}", file=sys.stderr)
        all_pass = False

    # ── Test 6: fuzz artifact detection ────────────────────────────────────
    print("[self-test] test 6: fuzz artifact detection", file=sys.stderr)
    artifact_dir = run_dir / "fuzz" / "selftest"
    artifact_dir.mkdir(parents=True, exist_ok=True)
    (artifact_dir / "crash-selftest").write_text("fake crash")
    before = set()
    after = set()
    for f in artifact_dir.iterdir():
        if f.is_file() and f.name.startswith("crash-"):
            after.add(f.name)
    new_artifacts = sorted(after - before)
    if "crash-selftest" in new_artifacts:
        print("[self-test]   fuzz artifact detected: OK", file=sys.stderr)
    else:
        print("[self-test]   FAIL: crash-selftest not detected", file=sys.stderr)
        all_pass = False

    # Clean up the synthetic artifact.
    shutil.rmtree(artifact_dir, ignore_errors=True)

    # ── Test 7: summary shows "none" for failures ──────────────────────────
    print("[self-test] test 7: summary zero failures", file=sys.stderr)
    # Create a clean context with no failures.
    clean_ctx = PhaseContext(
        config=config,
        project_root=project_root,
        run_dir=run_dir,
        head_sha="selftest",
        head_short="selftest",
        worktree_dirty=False,
        nproc=preflight.nproc,
        global_deadline=time.monotonic() + 300,
        final_debug_reserved=120,
        sticky_hold=False,
        baseline_ok=True,
        final_debug_ok=True,
    )
    # Add a known PASS.
    _synthetic_cmd(clean_ctx, "selftest", "7", "true-cmd", "debug",
                   ["true"], timeout_s=5)

    from hardening.reporting import write_summary_txt
    write_summary_txt(
        run_dir=run_dir,
        verdict=Verdict.PASS,
        config=config,
        preflight=preflight,
        started_at=time.time(),
        finished_at=time.time(),
        results=clean_ctx.results,
        failures=clean_ctx.failures,
        phase_stats=clean_ctx.stats,
        fuzz_results=[],
        interrupted=False,
    )
    summary_text = (run_dir / "summary.txt").read_text()
    if "Failures:\n  none" in summary_text:
        print("[self-test]   summary shows no failures: OK", file=sys.stderr)
    else:
        print("[self-test]   FAIL: summary does not show 'none' for failures", file=sys.stderr)
        all_pass = False

    # ── Test 8: target cache invalidation ──────────────────────────────────
    print("[self-test] test 8: target cache invalidation", file=sys.stderr)
    # Simulate two cache refreshes with different modes.
    from hardening.phases import refresh_target_cache
    ctx2 = PhaseContext(
        config=config,
        project_root=project_root,
        run_dir=run_dir,
        head_sha="selftest",
        head_short="selftest",
        worktree_dirty=False,
        nproc=preflight.nproc,
        global_deadline=time.monotonic() + 300,
        final_debug_reserved=120,
        sticky_hold=False,
        baseline_ok=True,
        final_debug_ok=True,
    )
    # First refresh (debug).
    try:
        t1 = refresh_target_cache(ctx2, "debug")
        # Second refresh (tsan) – should invalidate the debug cache.
        t2 = refresh_target_cache(ctx2, "tsan")
    except TargetCacheError as e:
        print(f"[self-test]   FAIL: target cache refresh raised TargetCacheError:\n{e}",
              file=sys.stderr)
        all_pass = False
        t1 = t2 = set()
    # Verify the cache mode changed AND that real targets were parsed (this
    # repository has many; zero means the snapshot silently broke).
    if ctx2._current_cache_mode == "tsan" and len(t1) > 0 and len(t2) > 0:
        print(f"[self-test]   target cache invalidated on mode change "
              f"(debug={len(t1)} tsan={len(t2)} targets): OK", file=sys.stderr)
    else:
        print(f"[self-test]   FAIL: cache mode={ctx2._current_cache_mode} "
              f"debug_targets={len(t1)} tsan_targets={len(t2)} "
              f"(expected mode=tsan, both >0)", file=sys.stderr)
        all_pass = False

    # ── Test 9: total elapsed regression check ─────────────────────────────
    # A healthy self-test completes well under 30s. The historical bug (a
    # blocking os.read on the child's output pipe) made the timeout tests wait
    # for each child's natural lifetime, pushing the total to ~90s while still
    # reporting PASS. This guard makes that regression fail loudly.
    total_elapsed = time.monotonic() - self_test_start
    if total_elapsed < 30.0:
        print(f"[self-test] test 9: total elapsed {total_elapsed:.2f}s (<30s): OK",
              file=sys.stderr)
    else:
        print(f"[self-test] test 9: FAIL: total elapsed {total_elapsed:.2f}s "
              f"(>=30s); timeout enforcement has regressed", file=sys.stderr)
        all_pass = False

    # ── Final ──────────────────────────────────────────────────────────────
    if all_pass:
        print("[self-test] ALL PASSED", file=sys.stderr)
        return EXIT_PASS
    else:
        print("[self-test] SOME TESTS FAILED", file=sys.stderr)
        return EXIT_HOLD


# ═══════════════════════════════════════════════════════════════════════════════
# Main
# ═══════════════════════════════════════════════════════════════════════════════

config: Optional[Config] = None


def main(argv: Optional[List[str]] = None) -> int:
    """Entry point for the hardening runner.

    Returns an exit code per the Verdict enum.
    """
    global config
    started_at = time.time()

    # ── Parse configuration ─────────────────────────────────────────────────
    try:
        config = parse_args(argv)
    except (ValueError, SystemExit) as e:
        print(f"Configuration error: {e}", file=sys.stderr)
        return EXIT_ENVIRONMENT_ERROR

    # ── Project root ─────────────────────────────────────────────────────────
    project_root = _SCRIPT_DIR.parent
    os.chdir(str(project_root))

    # ── Preflight ────────────────────────────────────────────────────────────
    preflight = run_preflight(project_root)

    # Print preflight summary.
    for c in preflight.checks:
        status = "PASS" if c.passed else "FAIL"
        print(f"[preflight] {status}: {c.name} – {c.message}", file=sys.stderr)

    if not preflight.passed:
        print("[preflight] FATAL: preflight checks failed", file=sys.stderr)
        # Write what we can.
        write_preflight_txt(project_root / "hardening-artifacts", preflight)
        write_preflight_json(project_root / "hardening-artifacts", preflight)
        return VERDICT_EXIT[Verdict.ENVIRONMENT_ERROR]

    # ── Self-test mode ───────────────────────────────────────────────────────
    if config.mode == "selftest":
        return self_test(config, project_root)

    # ── Single-instance lock ─────────────────────────────────────────────────
    try:
        _acquire_lock(project_root)
    except RuntimeError as e:
        print(f"[lock] {e}", file=sys.stderr)
        return VERDICT_EXIT[Verdict.ENVIRONMENT_ERROR]

    # ── Run directory ────────────────────────────────────────────────────────
    run_dir = _setup_run_dir(project_root, preflight.head_short, preflight, config)

    # ── Budget and deadline ──────────────────────────────────────────────────
    deadline_seconds = config.hours * 3600
    global_deadline = time.monotonic() + deadline_seconds
    print(f"[setup] mode={config.mode} budget_hours={config.hours} "
          f"deadline={deadline_seconds:.0f}s run={run_dir}",
          file=sys.stderr)

    # ── Phase context ────────────────────────────────────────────────────────
    ctx = PhaseContext(
        config=config,
        project_root=project_root,
        run_dir=run_dir,
        head_sha=preflight.head_sha,
        head_short=preflight.head_short,
        worktree_dirty=preflight.worktree_dirty,
        nproc=preflight.nproc,
        global_deadline=global_deadline,
        final_debug_reserved=0,  # Will be calculated after baseline.
        sticky_hold=False,
        baseline_ok=False,
        final_debug_ok=False,
    )

    # ── Signal handlers ──────────────────────────────────────────────────────
    interrupted = False

    def _handle_interrupt(signum: int, frame) -> None:
        nonlocal interrupted
        interrupted = True
        print(f"\n[runner] Caught signal {signum}; shutting down...", file=sys.stderr)

    signal.signal(signal.SIGINT, _handle_interrupt)
    signal.signal(signal.SIGTERM, _handle_interrupt)

    verdict = Verdict.RUNNER_ERROR  # Default if something goes wrong.
    runner_error: Optional[str] = None

    try:
        # ── Phase A: Baseline ───────────────────────────────────────────────
        if interrupted:
            raise KeyboardInterrupt()
        phase_baseline(ctx)

        # Calculate budget and Final Debug reserve.
        baseline_elapsed = time.time() - started_at
        ctx.final_debug_reserved = max(
            20 * 60,  # 20 minutes
            baseline_elapsed * 1.5,
        )
        remaining = max(0.0, global_deadline - time.monotonic())
        pool = max(0.0, remaining - ctx.final_debug_reserved)

        if config.mode == "smoke":
            # Smoke mode: small explicit budgets.
            soak_b = min(pool, 600)
            tsan_b = min(pool - soak_b, 600)
            asan_b = min(pool - soak_b - tsan_b, 300)
            fuzz_b = max(0.0, pool - soak_b - tsan_b - asan_b)
        elif config.mode == "version-b":
            # Version B nightly split: soak 45%, tsan 25%, asan 20%,
            # leftover stays in the soak rounds.
            soak_b = pool * 0.45
            tsan_b = pool * 0.25
            asan_b = pool * 0.20
            soak_b += max(0.0, pool - soak_b - tsan_b - asan_b)
            fuzz_b = 0.0
        else:
            # Normal split: soak 25%, tsan 25%, asan 12.5%, fuzz = rest.
            soak_b = pool * 0.25
            tsan_b = pool * 0.25
            asan_b = pool * 0.125
            fuzz_b = max(0.0, pool - soak_b - tsan_b - asan_b)

        print(f"[budget] remaining={remaining:.0f}s "
              f"final_debug_reserved={ctx.final_debug_reserved:.0f}s "
              f"pool={pool:.0f}s | "
              f"soak={soak_b:.0f}s tsan={tsan_b:.0f}s "
              f"asan={asan_b:.0f}s fuzz={fuzz_b:.0f}s",
              file=sys.stderr)

        if config.mode == "version-b":
            # ── Version B nightly gate ────────────────────────────────────────
            if not interrupted:
                phase_version_b_debug_soak(ctx, soak_b)
            if not interrupted:
                phase_version_b_tsan(ctx, tsan_b)
            if not interrupted:
                phase_version_b_asanubsan(ctx, asan_b)
            if not interrupted:
                phase_final_debug(ctx)
            verdict = version_b_calculate_verdict(ctx, preflight)
        else:
            # ── Phase B: Debug soak ────────────────────────────────────────
            if not interrupted:
                phase_debug_soak(ctx, soak_b)

            # ── Phase C: TSan ─────────────────────────────────────────────
            if not interrupted:
                phase_tsan(ctx, tsan_b)

            # ── Phase D: ASan+UBSan ───────────────────────────────────────
            if not interrupted:
                phase_asanubsan(ctx, asan_b)

            # ── Phase E: Fuzz ─────────────────────────────────────────────
            if not interrupted:
                phase_fuzz(ctx, fuzz_b)

            # ── Phase F: Final Debug ──────────────────────────────────────
            if not interrupted:
                phase_final_debug(ctx)

            # ── Verdict ───────────────────────────────────────────────────
            verdict = calculate_verdict(ctx, preflight)

    except KeyboardInterrupt:
        interrupted = True
        print("[runner] Interrupted by user", file=sys.stderr)
        # Kill any running child process group.
        verdict = Verdict.INCOMPLETE
        if ctx.sticky_hold:
            verdict = Verdict.HOLD

    except TargetCacheError as exc:
        # An infrastructure failure enumerating build targets. This must NOT be
        # disguised as "targets absent" (which would fabricate SKIP/INCOMPLETE);
        # it is a hard RUNNER_ERROR. The detail was already persisted to the
        # <mode>-targets.txt snapshot by refresh_target_cache; also record it in
        # run.log and the summary.
        runner_error = str(exc)
        print(f"[runner] TARGET CACHE ERROR (infrastructure failure):\n{exc}",
              file=sys.stderr)
        try:
            with open(run_dir / "run.log", "a") as f:
                f.write(f"[runner] TARGET CACHE ERROR:\n{exc}\n")
        except OSError:
            pass
        verdict = Verdict.RUNNER_ERROR

    except BaseException as exc:
        print(f"[runner] UNHANDLED EXCEPTION: {exc}", file=sys.stderr)
        traceback.print_exc(file=sys.stderr)
        runner_error = f"{type(exc).__name__}: {exc}"
        verdict = Verdict.RUNNER_ERROR

    finally:
        # ── Finalize ────────────────────────────────────────────────────────
        _exit_code = finalize(
            verdict=verdict,
            started_at=started_at,
            ctx=ctx,
            config=config,
            preflight=preflight,
            interrupted=interrupted,
            runner_error=runner_error,
        )
        _release_lock()

    return _exit_code


if __name__ == "__main__":
    raise SystemExit(main())
