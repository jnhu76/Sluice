"""Structured output (JSON / JSONL / text summaries) for the hardening runner.

All output is written to the artifact directory for the current run.
"""

from __future__ import annotations

import datetime
import json
import os
import time
from pathlib import Path
from typing import Dict, List, Optional

from .model import (
    Classification,
    CommandResult,
    Config,
    FuzzCorpusSnapshot,
    PhaseStats,
    Verdict,
    VERDICT_EXIT,
)
from .failure_detail import aggregate_failures
from .preflight import PreflightResult


# ═══════════════════════════════════════════════════════════════════════════════
# Events writer
# ═══════════════════════════════════════════════════════════════════════════════

def write_events_jsonl(run_dir: Path, results: List[CommandResult]) -> None:
    """Append all results to *events.jsonl* (one JSON object per line)."""
    path = run_dir / "events.jsonl"
    with open(path, "w") as f:
        for r in results:
            f.write(json.dumps(r.to_json_dict(), sort_keys=True) + "\n")


def write_failures_jsonl(run_dir: Path, failures: List[CommandResult]) -> None:
    """Write only real failures to *failures.jsonl*."""
    path = run_dir / "failures.jsonl"
    failure_kinds = {
        Classification.FAIL,
        Classification.TIMEOUT,
        Classification.SANITIZER_FAIL,
        Classification.FUZZ_CRASH,
        Classification.BUILD_FAIL,
        Classification.RUNNER_ERROR,
    }
    with open(path, "w") as f:
        for r in failures:
            if r.classification in failure_kinds:
                f.write(json.dumps(r.to_json_dict(), sort_keys=True) + "\n")


# ═══════════════════════════════════════════════════════════════════════════════
# Preflight output
# ═══════════════════════════════════════════════════════════════════════════════

def write_preflight_txt(run_dir: Path, preflight: PreflightResult) -> None:
    """Write human-readable preflight report."""
    path = run_dir / "preflight.txt"
    lines: List[str] = []
    lines.append("## Sluice hardening preflight report")
    lines.append(f"HEAD: {preflight.head_short} ({preflight.head_sha})")
    lines.append(f"Worktree dirty: {'yes' if preflight.worktree_dirty else 'no'}")
    lines.append(f"CPU cores: {preflight.nproc}")
    lines.append(f"Disk free: {preflight.disk_gib:.1f} GiB")
    lines.append("")
    lines.append("Checks:")
    for c in preflight.checks:
        status = "PASS" if c.passed else "FAIL"
        fatal = " [FATAL]" if c.is_fatal else ""
        lines.append(f"  {status}{fatal}: {c.name} - {c.message}")
    lines.append("")
    lines.append("Tool versions:")
    for tool, ver in sorted(preflight.tool_versions.items()):
        lines.append(f"  {tool}: {ver}")
    lines.append("")
    lines.append("Compiler probes:")
    for name, ok in sorted(preflight.compiler_probes.items()):
        lines.append(f"  {name}: {'OK' if ok else 'FAIL'}")
    lines.append("")
    if preflight.fatal_failures:
        lines.append("FATAL FAILURES:")
        for c in preflight.fatal_failures:
            lines.append(f"  {c.name}: {c.message}")
    if preflight.warnings:
        lines.append("WARNINGS:")
        for c in preflight.warnings:
            lines.append(f"  {c.name}: {c.message}")
    path.write_text("\n".join(lines) + "\n")


def write_preflight_json(run_dir: Path, preflight: PreflightResult) -> None:
    """Write structured preflight JSON."""
    path = run_dir / "preflight.json"
    checks = [
        {
            "name": c.name,
            "passed": c.passed,
            "message": c.message,
            "is_fatal": c.is_fatal,
            "detail": c.detail,
        }
        for c in preflight.checks
    ]
    data = {
        "schema_version": 1,
        "head_sha": preflight.head_sha,
        "head_short": preflight.head_short,
        "worktree_dirty": preflight.worktree_dirty,
        "nproc": preflight.nproc,
        "disk_gib": round(preflight.disk_gib, 1),
        "checks": checks,
        "tool_versions": preflight.tool_versions,
        "compiler_probes": preflight.compiler_probes,
        "passed": preflight.passed,
    }
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")


# ═══════════════════════════════════════════════════════════════════════════════
# Environment record
# ═══════════════════════════════════════════════════════════════════════════════

def write_environment_json(run_dir: Path, config: Config,
                           preflight: PreflightResult) -> None:
    """Write structured environment record."""
    path = run_dir / "environment.json"
    data = {
        "schema_version": 1,
        "config": {
            "mode": config.mode,
            "hours": config.hours,
            "phase_timeout_seconds": config.phase_timeout_seconds,
            "fuzz_seconds_override": config.fuzz_seconds_override,
            "keep_going": config.keep_going,
            "hours_source": config.hours_source,
            "timeout_source": config.timeout_source,
            "fuzz_source": config.fuzz_source,
            "keep_going_source": config.keep_going_source,
        },
        "head_sha": preflight.head_sha,
        "head_short": preflight.head_short,
        "worktree_dirty": preflight.worktree_dirty,
        "nproc": preflight.nproc,
        "disk_gib": round(preflight.disk_gib, 1),
        "tool_versions": preflight.tool_versions,
    }
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")


# ═══════════════════════════════════════════════════════════════════════════════
# Summary output
# ═══════════════════════════════════════════════════════════════════════════════

def write_summary_txt(
    run_dir: Path,
    verdict: Verdict,
    config: Config,
    preflight: PreflightResult,
    started_at: float,
    finished_at: float,
    results: List[CommandResult],
    failures: List[CommandResult],
    phase_stats: Dict[str, PhaseStats],
    fuzz_results: List[FuzzCorpusSnapshot],
    interrupted: bool,
    runner_error: Optional[str] = None,
) -> None:
    """Write human-readable summary.txt."""
    dur_s = finished_at - started_at
    dur_h = int(dur_s // 3600)
    dur_m = int((dur_s % 3600) // 60)
    dur_sec = int(dur_s % 60)

    start_dt = datetime.datetime.fromtimestamp(started_at, tz=datetime.timezone.utc)
    finish_dt = datetime.datetime.fromtimestamp(finished_at, tz=datetime.timezone.utc)

    lines: List[str] = []
    lines.append(f"SLUICE HARDENING: {verdict.value}")
    lines.append("")
    lines.append(f"HEAD: {preflight.head_short}")
    lines.append(f"Started: {start_dt.isoformat()}")
    lines.append(f"Finished: {finish_dt.isoformat()}")
    lines.append(f"Duration: {dur_h}h{dur_m:02d}m{dur_sec:02d}s")
    lines.append(f"Worktree: {'dirty' if preflight.worktree_dirty else 'clean'}")
    lines.append(f"Mode: {config.mode}")
    lines.append(f"Budget hours: {config.hours}")
    if interrupted:
        lines.append("Interrupted: yes")
    if runner_error:
        lines.append("")
        lines.append("Runner error (infrastructure failure):")
        for ln in runner_error.splitlines():
            lines.append(f"  {ln}")
    lines.append("")

    # Phase stats.
    lines.append("Phase summary:")
    for phase_name in ["baseline", "debug-soak", "tsan", "asanubsan", "fuzz",
                        "final"]:
        stats = phase_stats.get(phase_name)
        if stats:
            lines.append(f"  {phase_name}: exec={stats.executed} "
                         f"pass={stats.passed} fail={stats.failed} "
                         f"timeout={stats.timed_out} skip={stats.skipped} "
                         f"san={stats.sanitizer_fail} crash={stats.fuzz_crash}")

    # Fuzz results.
    if fuzz_results:
        lines.append("")
        lines.append("Fuzz:")
        for fs in fuzz_results:
            cls = fs.classification.value if fs.classification else "N/A"
            lines.append(f"  {fs.target}: {cls} "
                         f"(corpus: {fs.before_files}->{fs.after_files} files, "
                         f"{fs.before_bytes}->{fs.after_bytes} bytes, "
                         f"new_artifacts={len(fs.new_artifacts)})")
            if fs.new_artifacts:
                for a in fs.new_artifacts:
                    lines.append(f"    artifact: {a}")

    # Failures.
    lines.append("")
    # Aggregated failure groups (best-effort diagnostic extraction). Surfaced
    # BEFORE the per-occurrence list so a reader can see the distinct failures
    # without scanning every log. The raw per-occurrence list follows. This is
    # an index layer; the raw logs remain authoritative evidence.
    groups, total_occ = aggregate_failures(failures) if failures else ([], 0)
    if groups:
        lines.append(f"Distinct failures: {len(groups)}")
        lines.append(f"Total failure occurrences: {total_occ}")
        lines.append("")
        for i, g in enumerate(groups, 1):
            loc = (f"{g.source_file}:{g.source_line}"
                   if g.source_file else "?")
            lines.append(f"{i}. {g.binary or '?'}/"
                         f"{g.case or g.abnormal_signature or '?'}")
            lines.append(f"   occurrences: {g.occurrences}")
            if g.expression:
                lines.append(f"   assertion: {loc}")
                lines.append(f"              {g.expression}")
            elif g.abnormal_signature:
                lines.append(f"   abnormal: {g.abnormal_signature}")
            if g.message:
                lines.append(f"   message: {g.message}")
            if g.exit_semantics and g.exit_semantics != "normal":
                lines.append(f"   exit: {g.exit_semantics}")
            if g.xmake_summary:
                lines.append(f"   xmake: {g.xmake_summary}")
            lines.append(f"   phases: {', '.join(g.phases)}")
            if g.iterations:
                lines.append(f"   first: iteration {g.iterations[0]}")
                lines.append(f"   last: iteration {g.iterations[-1]}")
            if g.retry_total:
                lines.append(f"   retries reproduced: {g.retry_reproduced}/"
                             f"{g.retry_total}")
            if g.sample_logs:
                lines.append("   sample logs:")
                for s in g.sample_logs:
                    lines.append(f"     {s}")
            lines.append("")
    lines.append("Failures:")
    if failures:
        for f in failures:
            lines.append(f"  {f.phase}/{f.target}: {f.classification.value} "
                         f"(exit={f.exit_code}, duration={f.duration_seconds:.1f}s)")
    else:
        lines.append("  none")

    lines.append("")
    lines.append(f"Artifacts: {run_dir}")

    path = run_dir / "summary.txt"
    path.write_text("\n".join(lines) + "\n")


def write_summary_json(
    run_dir: Path,
    verdict: Verdict,
    config: Config,
    preflight: PreflightResult,
    started_at: float,
    finished_at: float,
    results: List[CommandResult],
    failures: List[CommandResult],
    phase_stats: Dict[str, PhaseStats],
    fuzz_results: List[FuzzCorpusSnapshot],
    interrupted: bool,
    runner_error: Optional[str] = None,
) -> None:
    """Write structured summary.json."""
    dur_s = finished_at - started_at

    phase_stats_json: Dict[str, dict] = {}
    for name, st in phase_stats.items():
        phase_stats_json[name] = {
            "iteration": st.iteration,
            "executed": st.executed,
            "passed": st.passed,
            "failed": st.failed,
            "skipped": st.skipped,
            "timed_out": st.timed_out,
            "sanitizer_fail": st.sanitizer_fail,
            "fuzz_crash": st.fuzz_crash,
            "build_fail": st.build_fail,
        }

    fuzz_results_json = []
    for fs in fuzz_results:
        fuzz_results_json.append({
            "target": fs.target,
            "before_files": fs.before_files,
            "before_bytes": fs.before_bytes,
            "after_files": fs.after_files,
            "after_bytes": fs.after_bytes,
            "new_artifacts": fs.new_artifacts,
            "classification": fs.classification.value if fs.classification else None,
        })

    failures_refs = []
    for f in failures:
        failures_refs.append({
            "phase": f.phase,
            "target": f.target,
            "classification": f.classification.value,
            "exit_code": f.exit_code,
            "log_path": str(f.log_path),
        })

    # Aggregated failure groups (best-effort diagnostic extraction). Each group
    # clusters occurrences by a content fingerprint (binary + case + file:line +
    # expression), so a reader or model can consume the distinct failures
    # directly from summary.json without scanning every raw log. The raw
    # `failures` list is preserved unchanged. schema_version bumped to 3.
    groups, total_occ = aggregate_failures(failures) if failures else ([], 0)
    failure_groups_json = [g.to_dict() for g in groups]

    data = {
        "schema_version": 3,
        "verdict": verdict.value,
        "exit_code": VERDICT_EXIT.get(verdict, 1),
        "mode": config.mode,
        "head": preflight.head_sha,
        "head_short": preflight.head_short,
        "dirty": preflight.worktree_dirty,
        "started_at": datetime.datetime.fromtimestamp(
            started_at, tz=datetime.timezone.utc
        ).isoformat(),
        "finished_at": datetime.datetime.fromtimestamp(
            finished_at, tz=datetime.timezone.utc
        ).isoformat(),
        "duration_seconds": round(dur_s, 1),
        "configuration": {
            "hours": config.hours,
            "phase_timeout_seconds": config.phase_timeout_seconds,
            "fuzz_seconds_override": config.fuzz_seconds_override,
            "keep_going": config.keep_going,
        },
        "preflight": {
            "passed": preflight.passed,
            "nproc": preflight.nproc,
            "disk_gib": round(preflight.disk_gib, 1),
        },
        "phase_stats": phase_stats_json,
        "fuzz_results": fuzz_results_json,
        "distinct_failures": len(groups),
        "total_failure_occurrences": total_occ,
        "failure_groups": failure_groups_json,
        "failures": failures_refs,
        "interrupted": interrupted,
        "runner_error": runner_error,
        "artifact_directory": str(run_dir),
    }

    path = run_dir / "summary.json"
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")


# ═══════════════════════════════════════════════════════════════════════════════
# High-level writer
# ═══════════════════════════════════════════════════════════════════════════════
# Failure summary (standalone, machine + human readable)
# ═══════════════════════════════════════════════════════════════════════════════

def write_failure_summary_txt(run_dir: Path, failures: List[CommandResult]) -> None:
    """Write a standalone failure-summary.txt focused on distinct failures.

    So a human or model can read the distinct failure groups directly without
    scanning every raw log. Empty when there are no failures.
    """
    path = run_dir / "failure-summary.txt"
    groups, total_occ = aggregate_failures(failures) if failures else ([], 0)
    lines: List[str] = []
    lines.append("SLUICE HARDENING FAILURE SUMMARY")
    lines.append("=" * 60)
    lines.append(f"Distinct failures: {len(groups)}")
    lines.append(f"Total failure occurrences: {total_occ}")
    lines.append("")
    if not groups:
        lines.append("(no failures)")
        path.write_text("\n".join(lines) + "\n")
        return
    for i, g in enumerate(groups, 1):
        loc = f"{g.source_file}:{g.source_line}" if g.source_file else "?"
        lines.append(f"--- {i}. {g.binary or '?'}/"
                     f"{g.case or g.abnormal_signature or '?'} "
                     f"({g.occurrences} occurrence(s)) ---")
        if g.expression:
            lines.append(f"  assertion:  {loc}")
            lines.append(f"              {g.expression}")
        elif g.abnormal_signature:
            lines.append(f"  abnormal:   {g.abnormal_signature}")
        if g.message:
            lines.append(f"  message:    {g.message}")
        if g.exit_semantics and g.exit_semantics != "normal":
            lines.append(f"  exit:       {g.exit_semantics}")
        if g.xmake_summary:
            lines.append(f"  xmake:      {g.xmake_summary}")
        lines.append(f"  framework:  {g.framework or 'n/a'} "
                     f"(parse: {g.parse_status})")
        lines.append(f"  phases:     {', '.join(g.phases)}")
        if g.iterations:
            if len(g.iterations) <= 12:
                lines.append(f"  iterations: {', '.join(g.iterations)}")
            else:
                lines.append(f"  iterations: {', '.join(g.iterations[:6])}, ... , "
                             f"{', '.join(g.iterations[-3:])}")
        if g.retry_total:
            lines.append(f"  retries:    {g.retry_reproduced}/{g.retry_total} "
                         f"reproduced")
        if g.sample_logs:
            lines.append("  sample logs:")
            for s in g.sample_logs:
                lines.append(f"    {s}")
        lines.append("")
        lines.append(f"  FINGERPRINT: {g.fingerprint}")
        lines.append("")
    lines.append("=" * 60)
    lines.append("NOTE: clustering is content-based (binary + case + file:line + "
                 "expression).")
    lines.append("exit 255 with an xmake test-failure summary is annotated as "
                 "'xmake test failure")
    lines.append("exit'; it is never reclassified to PASS. The raw logs remain "
                 "authoritative.")
    path.write_text("\n".join(lines) + "\n")


def write_failure_summary_json(
    run_dir: Path, failures: List[CommandResult]
) -> None:
    """Write a standalone failure-summary.json (schema-stable, machine-readable)."""
    path = run_dir / "failure-summary.json"
    groups, total_occ = aggregate_failures(failures) if failures else ([], 0)
    data = {
        "schema_version": 1,
        "distinct_failures": len(groups),
        "total_failure_occurrences": total_occ,
        "failure_groups": [g.to_dict() for g in groups],
    }
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")


# ═══════════════════════════════════════════════════════════════════════════════
# High-level writer
# ═══════════════════════════════════════════════════════════════════════════════

def write_all_outputs(
    run_dir: Path,
    verdict: Verdict,
    config: Config,
    preflight: PreflightResult,
    started_at: float,
    finished_at: float,
    results: List[CommandResult],
    failures: List[CommandResult],
    phase_stats: Dict[str, PhaseStats],
    fuzz_results: List[FuzzCorpusSnapshot],
    interrupted: bool,
    runner_error: Optional[str] = None,
) -> None:
    """Write all output files (summary, events, failures, failure-summary)."""
    write_events_jsonl(run_dir, results)
    write_failures_jsonl(run_dir, failures)
    write_summary_txt(run_dir, verdict, config, preflight,
                      started_at, finished_at, results, failures,
                      phase_stats, fuzz_results, interrupted, runner_error)
    write_summary_json(run_dir, verdict, config, preflight,
                       started_at, finished_at, results, failures,
                       phase_stats, fuzz_results, interrupted, runner_error)
    write_failure_summary_txt(run_dir, failures)
    write_failure_summary_json(run_dir, failures)