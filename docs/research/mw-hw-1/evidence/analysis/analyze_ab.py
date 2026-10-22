#!/usr/bin/env python3
"""MW-HW-1 A/B analysis: rep-index paired wall ratios from raw rows.

Input: ab/rows.jsonl as written by ab/run_ab_mwhw1.sh (one JSON object per
run, file order = run order). The runner visits each config once per
repetition and emits one timed row per arm with arm order rotated by
repetition, so for each config the timed rows chunk in file order into
consecutive one-row-per-arm groups; each group is one paired repetition.
Ratios are arm/base within a group. Reproduces ab/analysis-rep-paired.txt.
"""

import json
import statistics
import sys

ARMS = ("e1", "e2", "e3")


def main() -> None:
    path = sys.argv[1] if len(sys.argv) > 1 else "ab/rows.jsonl"
    timed = []
    with open(path) as fh:
        for line in fh:
            row = json.loads(line)
            if row.get("phase") == "timed" and row.get("ok"):
                timed.append(row)

    by_config = {}
    for row in timed:
        key = (row["block"], row["depth"], row["capacity"])
        by_config.setdefault(key, []).append(row)

    print("rep-index pairing (true per-repetition pairs):")
    for blk, dep, cap in sorted(by_config):
        rows = by_config[(blk, dep, cap)]
        chunk_len = 1 + len(ARMS)
        ratios = {arm: [] for arm in ARMS}
        for i in range(0, len(rows) - chunk_len + 1, chunk_len):
            chunk = rows[i : i + chunk_len]
            base = next(r for r in chunk if r["arm"] == "base")
            base_us = base["wall_s"] / base["ops"] * 1e6
            for arm in ARMS:
                row = next(r for r in chunk if r["arm"] == arm)
                ratios[arm].append(row["wall_s"] / row["ops"] * 1e6 / base_us)
        print(f"block={blk:7d} d={dep:<4d}C={cap:<5d}")
        for arm in ARMS:
            vals = ratios[arm]
            below = sum(1 for v in vals if v < 1.0)
            print(
                f"    {arm}: med={statistics.median(vals):.4f} "
                f"[{min(vals):.4f},{max(vals):.4f}] reps<1.0: {below}/{len(vals)}"
            )


if __name__ == "__main__":
    main()
