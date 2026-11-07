#!/usr/bin/env python3
"""Extract per-arm p50/p95/p99 tables from R4 raw matrix blocks.

Reads every valid invocation in the timed cells, aggregates invocation-level
metrics (the same level the preregistered gates use), and computes paired
per-block percent differences vs the A0 invocation of the same block.

Output: analysis/tail-latency-p50-p95-p99.csv
Reproduce: python3 analysis/extract_tail_table.py
"""
import csv
import glob
import json
import os
import statistics as st

R4 = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

CELLS = [
    ("MAIN-W03-D16", "raw/matrix/MAIN-W03-D16"),
    ("CORE-W03-D4", "raw/matrix/CORE-W03-D4"),
    ("CORE-W03-D64", "raw/matrix/CORE-W03-D64"),
    ("CORE-W03-D2SERIAL", "raw/matrix/CORE-W03-D2SERIAL"),
    ("CORE-W02-D1", "raw/matrix/CORE-W02-D1"),
    ("CORE-W02-D2", "raw/matrix/CORE-W02-D2"),
    ("CORE-W02-D16", "raw/matrix/CORE-W02-D16"),
    ("B-SUPP-W03-D16", "raw/bsupp/B-SUPP-W03-D16"),
    ("PROT-64K-D16", "raw/extension/PROT-64K-D16"),
    ("PROT-NONTMPFS", "raw/extension/PROT-NONTMPFS"),
]

FIELDS = ["p50_ns", "p95_ns", "p99_ns", "cpu_ns_per_op", "ops_per_sec"]


def collect(cell_dir):
    by_block = {}
    for path in sorted(glob.glob(os.path.join(R4, cell_dir, "block-*.json"))):
        block = json.load(open(path))
        idx = block["block"]
        for inv in block["invocations"]:
            if inv.get("valid"):
                by_block.setdefault(idx, {})[inv["arm"]] = inv["metrics"]
    return by_block


def paired_pct(by_block, arm, field):
    deltas = []
    for idx, arms in sorted(by_block.items()):
        a0 = arms.get("A0")
        cand = arms.get(arm)
        if a0 and cand and a0[field] > 0:
            deltas.append((cand[field] / a0[field] - 1.0) * 100.0)
    return deltas


def main():
    rows = []
    for cell, rel in CELLS:
        by_block = collect(rel)
        arms = sorted({a for arms in by_block.values() for a in arms})
        for arm in arms:
            metrics = [m for arms in by_block.values() for a, m in arms.items() if a == arm]
            row = {
                "cell": cell,
                "arm": arm,
                "n_blocks": len(metrics),
            }
            for f in FIELDS:
                row[f + "_mean"] = round(st.mean(m[f] for m in metrics), 2)
            for f in ("p50_ns", "p95_ns", "p99_ns", "cpu_ns_per_op", "ops_per_sec"):
                d = paired_pct(by_block, arm, f)
                if arm == "A0" or not d:
                    row["d_" + f + "_pct_mean"] = 0.0 if arm == "A0" else ""
                    row["d_" + f + "_pct_min"] = ""
                    row["d_" + f + "_pct_max"] = ""
                else:
                    row["d_" + f + "_pct_mean"] = round(st.mean(d), 2)
                    row["d_" + f + "_pct_min"] = round(min(d), 2)
                    row["d_" + f + "_pct_max"] = round(max(d), 2)
            rows.append(row)
    out = os.path.join(R4, "analysis", "tail-latency-p50-p95-p99.csv")
    cols = list(rows[0].keys())
    with open(out, "w", newline="") as fh:
        w = csv.DictWriter(fh, fieldnames=cols)
        w.writeheader()
        w.writerows(rows)
    print(f"wrote {out} rows={len(rows)}")
    for r in rows:
        if r["cell"].startswith(("MAIN", "B-SUPP")) or r["arm"] in ("A0", "R", "S"):
            print(
                f"{r['cell']:>18} {r['arm']:>6} n={r['n_blocks']:>2} "
                f"p50={r['p50_ns_mean']:>9.1f} d50={r['d_p50_ns_pct_mean']:>7}% "
                f"p95={r['p95_ns_mean']:>9.1f} d95={r['d_p95_ns_pct_mean']:>7}% "
                f"p99={r['p99_ns_mean']:>9.1f} d99={r['d_p99_ns_pct_mean']:>7}% "
                f"[{r['d_p99_ns_pct_min']}..{r['d_p99_ns_pct_max']}] "
                f"cpu={r['d_cpu_ns_per_op_pct_mean']:>7}% tput={r['d_ops_per_sec_pct_mean']:>7}%"
            )


if __name__ == "__main__":
    main()
