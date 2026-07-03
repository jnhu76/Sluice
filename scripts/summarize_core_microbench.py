#!/usr/bin/env python3
"""Summarize a core microbench CSV (CPPIO-CORE-011B).

Reads the CSV emitted by scripts/run_core_microbenches.sh and prints, per
(case, bytes), each mode's elapsed_ns and a derived throughput (MB/s), plus
min/median across repeated rows when present. This is an interpretation aid, NOT
a performance claim — see docs/optimization-runbook.md.
"""
from __future__ import annotations

import argparse
import csv
import statistics
import sys
from collections import defaultdict


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("csv", help="core microbench CSV to summarize")
    p.add_argument("--mb-only", action="store_true",
                   help="print only the MB/s column per cell (one number)")
    args = p.parse_args(argv)

    # Group rows by (case, bytes, mode) -> list of (elapsed_ns, iterations, bytes).
    cells: dict[tuple[str, str, str], list[tuple[int, int, int]]] = defaultdict(list)
    bytes_by_case: dict[tuple[str, str], set[int]] = defaultdict(set)
    with open(args.csv, newline="") as f:
        reader = csv.DictReader(f)
        for row in reader:
            try:
                case = row["case"]
                mode = row["mode"]
                nbytes = int(row["bytes"])
                elapsed = int(row["elapsed_ns"])
                iters = int(row["iterations"])
            except (KeyError, ValueError):
                # Header comment or malformed row; skip.
                continue
            cells[(case, mode, nbytes)].append((elapsed, iters, nbytes))
            bytes_by_case[(case, mode)].add(nbytes)

    print("# core microbench summary (local observations, NOT performance claims)")
    print("# cell = (case, bytes): mode -> elapsed_ns [min/median if repeats]  MB/s")
    for (case, _mode), sizes in sorted(bytes_by_case.items()):
        pass  # iterate by (case, bytes) below instead

    # Iterate per (case, bytes), listing all modes.
    case_bytes: dict[str, set[int]] = defaultdict(set)
    for (case, _m, nbytes) in cells:
        case_bytes[case].add(nbytes)
    for case in sorted(case_bytes):
        for nbytes in sorted(case_bytes[case]):
            # find modes present for this (case, bytes)
            modes = sorted({m for (c, m, b) in cells if c == case and b == nbytes})
            print(f"\n[{case}, bytes={nbytes}]")
            for mode in modes:
                rows = cells[(case, mode, nbytes)]
                elapsed_vals = [r[0] for r in rows]
                total_bytes = sum(r[2] for r in rows)
                # throughput from total bytes / total elapsed
                mb_s = (total_bytes / 1e6) / (sum(elapsed_vals) / 1e9) if sum(elapsed_vals) else 0.0
                if len(elapsed_vals) > 1:
                    lo = min(elapsed_vals)
                    med = int(statistics.median(elapsed_vals))
                    print(f"  {mode:48s} {med:>12} ns (min {lo}, n={len(elapsed_vals)})  {mb_s:>9.2f} MB/s")
                else:
                    print(f"  {mode:48s} {elapsed_vals[0]:>12} ns  {mb_s:>9.2f} MB/s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
