#!/usr/bin/env python3
"""check_g1_control_c0_analysis.py — fail-closed analysis validator (#279).

Re-derives the campaign analysis from the immutable raw evidence and fails
if ANY preregistered cell is missing a valid run, if any same-work or causal
gate is violated, if the materiality/verdict computation does not match the
frozen rule, or if the verdict vocabulary drifts. Used as the mechanical gate
behind the G1-CONTROL-C0 report.

Usage: python3 check_g1_control_c0_analysis.py <session-id>
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
RESULTS = REPO / "research/g1-control-c0/results"

OPS = ["READ", "WRITE"]
SIZES = [4096, 65536, 2097152]
DEPTHS_BY_SIZE = {4096: [1, 8, 32], 65536: [1], 2097152: [1]}
FS = ["tmpfs", "btrfs"]
ARMS = ["F0", "F1", "F0-T", "F1-T"]
ROUNDS = 7
THREADED_WORKERS = 4
FILE_BYTES = {4096: 512 * 1024 * 1024, 65536: 1 << 30, 2097152: 1 << 30}
MATERIAL_RATIO = 1.03
MATERIAL_MAD_K = 1.5

PERF_VERDICTS = {
    "FIXED-FILE PERFORMANCE BENEFIT ESTABLISHED",
    "REGIME-LOCAL BENEFIT ESTABLISHED",
    "FIXED-FILE PERFORMANCE REGRESSION",
    "FIXED-FILE PERFORMANCE BENEFIT NOT ESTABLISHED",
    "BLOCKED", "INVALID",
}


def median(vals):
    if not vals:
        return 0.0
    s = sorted(vals)
    return s[len(s) // 2]


def mad(vals, med):
    if not vals:
        return 0.0
    return median([abs(v - med) for v in vals])


def main() -> None:
    if len(sys.argv) != 2:
        print("usage: check_g1_control_c0_analysis.py <session-id>",
              file=sys.stderr)
        sys.exit(2)
    sid = sys.argv[1]
    sd = RESULTS / sid
    fails: list[str] = []

    manifest = json.loads((sd / "manifest.json").read_text())
    gates = json.loads((sd / "gates.json").read_text())
    raw = sd / "raw" / "runs.jsonl"
    runs = [json.loads(l) for l in raw.read_text().splitlines() if l.strip()]

    # 1. gate errors
    errs = gates.get("errors", [])
    if errs:
        fails.append(f"{len(errs)} gate errors in gates.json")

    # 2. expected dst hashes frozen
    for size in SIZES:
        if not manifest.get(f"expected_dst_sha256_{size}"):
            fails.append(f"expected_dst_sha256_{size} not frozen")

    # 3. cell coverage: exactly ROUNDS valid runs per preregistered cell
    ok_runs = [r for r in runs if r.get("ok")]
    by_cell: dict = {}
    for r in ok_runs:
        key = (r["op"], r["size"], r["depth"], r["fs"], r["arm"])
        by_cell.setdefault(key, []).append(r)
    for op in OPS:
        for size in SIZES:
            for depth in DEPTHS_BY_SIZE[size]:
                for fs in FS:
                    for arm in ARMS:
                        cell = (op, size, depth, fs, arm)
                        vals = by_cell.get(cell, [])
                        if len(vals) != ROUNDS:
                            fails.append(
                                f"{cell}: {len(vals)} valid runs "
                                f"(expected {ROUNDS})")

    # 4. run-id uniqueness
    ids = [r["run_id"] for r in runs]
    if len(ids) != len(set(ids)):
        fails.append("duplicate run ids")

    # 5. per-run same-work + causal re-verification from raw
    for r in ok_runs:
        b = r["bench"]
        if b is None:
            fails.append(f"{r['run_id']}: bench json missing")
            continue
        if b.get("canceled", 1) != 0 or b.get("errors", 1) != 0 or \
                b.get("short_reads", 1) != 0 or b.get("short_writes", 1) != 0:
            fails.append(f"{r['run_id']}: unexpected terminal/short I/O")
        fb = FILE_BYTES[r["size"]]
        if b.get("bytes_read") != (fb if r["op"] == "READ" else 0) or \
                b.get("bytes_written") != (fb if r["op"] == "WRITE" else 0):
            fails.append(f"{r['run_id']}: byte accounting")
        if b.get("cqe_count") != b.get("chunks"):
            fails.append(f"{r['run_id']}: cqe accounting")
        if b.get("align_remainder") != 0 or b.get("slot_stride") != r["size"]:
            fails.append(f"{r['run_id']}: causal isolation storage")
        if r["arm"] in ("F1", "F1-T") and b.get("registered_files") != 1:
            fails.append(f"{r['run_id']}: registration table")
        if r["arm"] in ("F0-T", "F1-T") and (
                b.get("threads_spawned") != THREADED_WORKERS or
                b.get("threads_io_ok") != THREADED_WORKERS or
                b.get("threads_joined") != THREADED_WORKERS):
            fails.append(f"{r['run_id']}: threaded condition")
        if r["op"] == "WRITE":
            exp = manifest[f"expected_dst_sha256_{r['size']}"]
            if r.get("dst_sha256") != exp:
                fails.append(f"{r['run_id']}: dst hash mismatch")

    # 6. recompute materiality under the frozen rule and compare to
    #    summary.json
    summary = json.loads((sd / "summary.json").read_text())
    for op in OPS:
        for size in SIZES:
            for depth in DEPTHS_BY_SIZE[size]:
                for fs in FS:
                    f0 = [r["bench"]["wall_per_op_ns"] for r in
                          by_cell.get((op, size, depth, fs, "F0"), [])]
                    f1 = [r["bench"]["wall_per_op_ns"] for r in
                          by_cell.get((op, size, depth, fs, "F1"), [])]
                    m0, m1 = median(f0), median(f1)
                    mad0, mad1 = mad(f0, m0), mad(f1, m1)
                    ratio = m0 / m1 if m1 else float("inf")
                    benefit = ratio >= MATERIAL_RATIO and \
                        m1 + MATERIAL_MAD_K * mad1 < m0 - MATERIAL_MAD_K * mad0
                    regression = ratio <= 1.0 / MATERIAL_RATIO and \
                        m0 + MATERIAL_MAD_K * mad0 < m1 - MATERIAL_MAD_K * mad1
                    direction = ("F1_FASTER" if benefit else
                                 "F1_SLOWER" if regression else "NONE")
                    cell = f"{op}_{size}_{depth}_{fs}"
                    stored = summary["per_cell"][cell]["f0"]["direction"]
                    if stored != direction:
                        fails.append(
                            f"{cell}: stored direction {stored} != "
                            f"recomputed {direction}")
                    if abs(summary["per_cell"][cell]["f0"]["ratio"] -
                           ratio) > 1e-6 and size != 0:
                        fails.append(f"{cell}: ratio mismatch")

    # 7. verdict vocabulary
    for op in OPS:
        v = summary["verdicts"].get(op)
        if v not in PERF_VERDICTS:
            fails.append(f"verdict[{op}]={v} not in frozen vocabulary")

    # 8. report
    if fails:
        print(f"ANALYSIS FAIL ({len(fails)}):")
        for f in fails:
            print(f"  - {f}")
        sys.exit(1)
    print(f"ANALYSIS PASS: session {sid}, {len(ok_runs)} valid runs, "
          f"0 gate errors, all cells covered, verdicts in frozen vocabulary.")


if __name__ == "__main__":
    main()
