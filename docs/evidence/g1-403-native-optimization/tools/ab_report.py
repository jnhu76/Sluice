#!/usr/bin/env python3
"""Aggregate an ab_run.sh directory into a comparison table.

Per case and side: median ops_per_sec, median cpu_s per op, median lat_p99.
Then the paired effect: median over repetitions of arm/b0 ops_per_sec ratio,
the min/max paired ratio, and the worst p99 regression ratio.
"""

from __future__ import annotations

import json
import statistics
import sys
from pathlib import Path


def load(d: Path, case: str, side: str, reps: int) -> list[dict]:
    rows = []
    for rep in range(reps):
        p = d / f"{case}.{side}.r{rep}.jsonl"
        rec = None
        with p.open() as fh:
            for line in fh:
                r = json.loads(line)
                if r.get("case_id") == case and r.get("measured") is True:
                    if rec is not None:
                        raise SystemExit(f"{p}: more than one measured record")
                    rec = r
        if rec is None:
            raise SystemExit(f"{p}: no measured record")
        rows.append(rec)
    return rows


def med(rows: list[dict], key: str) -> float:
    return statistics.median(r[key] for r in rows)


def main() -> int:
    d = Path(sys.argv[1])
    spec = Path(sys.argv[2])
    lines = [
        "# A/B comparison",
        "",
        "| case | reps | B0 ops/s | arm ops/s | paired ratio (median) | paired min..max | B0 cpu/op | arm cpu/op | B0 p99 ns | arm p99 ns | p99 worst |",
        "|---|---|---|---|---|---|---|---|---|---|---|",
    ]
    for spec_line in spec.read_text().splitlines():
        parts = spec_line.split()
        if not parts or parts[0].startswith("#"):
            continue
        case, _profile, _target, reps = parts[0], parts[1], parts[2], int(parts[3])
        b0 = load(d, case, "b0", reps)
        arm = load(d, case, "arm", reps)
        ratios = [a["ops_per_sec"] / b["ops_per_sec"] for a, b in zip(arm, b0)]
        p99 = [
            (a["lat_p99_ns"] / b["lat_p99_ns"]) if b["lat_p99_ns"] > 0 else float("nan")
            for a, b in zip(arm, b0)
        ]
        finite_p99 = [x for x in p99 if x == x]
        worst_p99 = max(finite_p99) if finite_p99 else float("nan")
        p99_cell = f"{worst_p99:.4f}" if finite_p99 else "n/a"
        lines.append(
            f"| {case} | {reps} | {med(b0, 'ops_per_sec'):.0f} "
            f"| {med(arm, 'ops_per_sec'):.0f} "
            f"| {statistics.median(ratios):.4f} "
            f"| {min(ratios):.4f}..{max(ratios):.4f} "
            f"| {med(b0, 'cpu_s') / med(b0, 'ops') * 1e9:.0f}ns "
            f"| {med(arm, 'cpu_s') / med(arm, 'ops') * 1e9:.0f}ns "
            f"| {med(b0, 'lat_p99_ns'):.0f} | {med(arm, 'lat_p99_ns'):.0f} "
            f"| {p99_cell} |"
        )
    out = d / "AB.md"
    out.write_text("\n".join(lines) + "\n")
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
