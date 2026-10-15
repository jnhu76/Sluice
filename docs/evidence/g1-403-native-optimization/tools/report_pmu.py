#!/usr/bin/env python3
"""Aggregate the G1 PMU counting campaign into PMU.md.

Reads <dir>/<case>.r<N>.perfstat.csv and the matching .jsonl (one measured
repetition each) written by measure_pmu.sh. Per-op values use the measured
record's ops field as the denominator. Fails non-zero if any repetition is
missing, has a different case, or any CSV is malformed.
"""

from __future__ import annotations

import csv
import statistics
import sys
from pathlib import Path

EVENTS = [
    "task-clock",
    "page-faults",
    "cycles",
    "instructions",
    "branches",
    "branch-misses",
    "cache-references",
    "cache-misses",
    "LLC-load-misses",
    "dTLB-load-misses",
]

CASES = [
    "W01-read_at-4k-seq",
    "W02-threadpool-4k-d16-w4",
    "W02-uring-4k-d16",
    "W03-threadpool-4k-d16-w4",
    "W03-uring-4k-d16",
]


def read_run(csv_path: Path) -> dict[str, float]:
    counts: dict[str, float] = {}
    with csv_path.open() as fh:
        for row in csv.reader(line for line in fh if not line.startswith("#")):
            if len(row) < 2 or not row[0]:
                continue
            name = row[2] if len(row) > 2 else row[1]
            base = name.split(":")[0]
            try:
                counts[base] = float(row[0].replace(",", ""))
            except ValueError:
                continue
    missing = [e for e in EVENTS if e not in counts]
    if missing:
        raise SystemExit(f"{csv_path}: missing events {missing}")
    return counts


def read_ops(jsonl_path: Path, case_id: str) -> int:
    ops = None
    with jsonl_path.open() as fh:
        for line in fh:
            r = json.loads(line)
            if r.get("case_id") == case_id and r.get("measured") is True:
                if ops is not None:
                    raise SystemExit(f"{jsonl_path}: more than one measured record")
                ops = int(r["ops"])
    if ops is None or ops <= 0:
        raise SystemExit(f"{jsonl_path}: no measured record with ops for {case_id}")
    return ops


def median_op(rows: list[tuple[dict[str, float], int]], event: str) -> float:
    return statistics.median(c[event] / ops for c, ops in rows)


def fmt(v: float) -> str:
    return f"{v:.4g}"


def main() -> int:
    d = Path(sys.argv[1] if len(sys.argv) > 1 else ".")
    per_case: dict[str, list[tuple[dict[str, float], int]]] = {}
    for case in CASES:
        rows = []
        rep = 0
        while (d / f"{case}.r{rep}.perfstat.csv").exists():
            counts = read_run(d / f"{case}.r{rep}.perfstat.csv")
            ops = read_ops(d / f"{case}.r{rep}.jsonl", case)
            rows.append((counts, ops))
            rep += 1
        if not rows:
            raise SystemExit(f"{case}: no perfstat captures found")
        per_case[case] = rows

    lines: list[str] = []
    lines.append("# G1 targeted PMU counting — results")
    lines.append("")
    lines.append("Source: `pmu/` (25 perf-stat runs: 5 cases x 5 repetitions, one")
    lines.append("measured bench repetition each, `G0_WARMUP=0 G0_MEASURED=1`,")
    lines.append("provenance and per-run status in `pmu/run.log`). All counts are")
    lines.append("**user-space only**: `perf_event_paranoid=2` forces")
    lines.append("`exclude_kernel=1` for an unprivileged counter; kernel-side")
    lines.append("cycle attribution is reported by the #535 flame-graph evidence and")
    lines.append("the root-required extensions are listed at the end.")
    lines.append("")
    lines.append("Per-operation values are medians over the 5 repetitions; ops from")
    lines.append("each run's own JSONL record. IPC, miss rates are user-space")
    lines.append("cycles/instructions only.")
    lines.append("")
    lines.append("| case | reps | ops/rep | cycles/op | instr/op | IPC | branch-miss/op | cache-miss/op | LLC-miss/op | dTLB-miss/op | task-clock ms/op | faults/op |")
    lines.append("|---|---|---|---|---|---|---|---|---|---|---|---|")
    for case, rows in per_case.items():
        ops_rep = statistics.median(ops for _, ops in rows)
        lines.append(
            f"| {case} | {len(rows)} | {ops_rep} "
            f"| {fmt(median_op(rows, 'cycles'))} "
            f"| {fmt(median_op(rows, 'instructions'))} "
            f"| {median_op(rows, 'instructions') / median_op(rows, 'cycles'):.3f} "
            f"| {fmt(median_op(rows, 'branch-misses'))} "
            f"| {fmt(median_op(rows, 'cache-misses'))} "
            f"| {fmt(median_op(rows, 'LLC-load-misses'))} "
            f"| {fmt(median_op(rows, 'dTLB-load-misses'))} "
            f"| {median_op(rows, 'task-clock') / 1e6:.4g} "
            f"| {fmt(median_op(rows, 'page-faults'))} |"
        )
    lines.append("")
    lines.append("## Reading notes")
    lines.append("")
    lines.append("- `W01-read_at-4k-seq` is the direct path reference: nearly all")
    lines.append("  user cycles are the read loop itself; every async case pays the")
    lines.append("  synchronization machinery on top of the same payload.")
    lines.append("- The async cases' user IPC and miss rates quantify the")
    lines.append("  user-space side of the 55.2% mutex share (ThreadPool) and the")
    lines.append("  submit/enter path (io_uring) measured in the #535 evidence;")
    lines.append("  kernel-side cost is deliberately out of these counts' scope.")
    lines.append("- `page-faults` counts during-run faults; the fixture and ring")
    lines.append("  setup dominate, steady-state pipeline reads are page-resident.")
    lines.append("")
    lines.append("## PERMISSION_REQUIRED (kernel-side attribution)")
    lines.append("")
    lines.append("The following need root on this host (`perf_event_paranoid=2`,")
    lines.append("bpftrace requires CAP_BPF/CAP_PERFMON). Record with the same")
    lines.append("one-repetition-per-invocation discipline via interactive")
    lines.append("`sudo -v`; do not embed credentials in any command, script or log:")
    lines.append("")
    lines.append("```")
    lines.append("# kernel-side PMU sampling per case (kernel call chains):")
    lines.append("sudo perf record -g --call-graph fp -k 1 -o <case>.perf.data -- \\")
    lines.append("    env G0_WARMUP=0 G0_MEASURED=1 G0_CASE_FILTER==<case> \\")
    lines.append("    G0_JSONL=<scratch>/<case>.jsonl <binary>")
    lines.append("# raw event counting including kernel half (cycles:k, or cycles with")
    lines.append("# :ku splitting): needs root or perf_event_paranoid<=1")
    lines.append("# futex wait/wake and io_uring_enter argument observation:")
    lines.append("sudo bpftrace docs/evidence/403-flamegraph-profiling-20261015/tools/trace_*.bt")
    lines.append("```")
    lines.append("")
    lines.append("Until captured, kernel-side PMU attribution for G1 stays")
    lines.append("`PERMISSION_REQUIRED`; the C9/C1/C2 experiments proceed on the")
    lines.append("#535 kernel evidence (sampling shares) plus these user-space")
    lines.append("counts, which is sufficient for mechanism identification but not")
    lines.append("for a kernel-cycle re-attribution claim.")
    lines.append("")

    out = d / "PMU.md"
    out.write_text("\n".join(lines))
    print(f"wrote {out}")
    return 0


import json  # noqa: E402

if __name__ == "__main__":
    sys.exit(main())
