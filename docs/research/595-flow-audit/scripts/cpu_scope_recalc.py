#!/usr/bin/env python3
"""Offline recalculation: CPU accounting scope per arm from r5-repair raw.

Reads only existing raw records (no re-timing). For each cell/arm:
  - rusage cpu/op from ru_utime+ru_stime over measured phase (primary caliber)
  - /proc-sampled thread ticks: app threads vs iou-wrk-<pid> kernel workers
    (io-wq CPU is OUTSIDE process rusage; buckets are the only coverage)
Answers: how much of each arm's machine work the primary metric misses.
"""
import json, glob, os, sys
from collections import defaultdict

ROOT = "/home/jnhu/sluice-595-e5/r5-repair-measure/raw"
CELLS = {
    "main-w03-d16": ("MAIN", 20000),
    "prot-w03-d4": ("PROT", 20000),
    "prot-w03-d2serial": ("PROT", 20000),
}

def load(cell):
    recs = []
    for path in glob.glob(f"{ROOT}/{cell}/main/results.jsonl"):
        for line in open(path):
            recs.append(json.loads(line))
    return [r for r in recs if r.get("valid") and r.get("seg") == "main"]

def ticks(rec):
    b = rec.get("buckets", {})
    app = iowq = 0
    for tid, t in (b.get("threads") or {}).items():
        if str(t.get("comm", "")).startswith("iou-wrk"):
            iowq += t.get("ticks", 0)
        else:
            app += t.get("ticks", 0)
    return app, iowq

out = {}
for cell, (kind, ops) in CELLS.items():
    recs = load(cell)
    per = defaultdict(list)
    for r in recs:
        ru = (r.get("ru_utime", 0) + r.get("ru_stime", 0)) / max(r.get("measured_records", 1), 1)
        app, iowq = ticks(r)
        per[r["arm"]].append((ru, app, iowq, r.get("wall_s", 0) / max(r.get("measured_records", 1), 1)))
    cell_out = {}
    for arm, rows in sorted(per.items()):
        n = len(rows)
        mean = lambda i: sum(r[i] for r in rows) / n
        ru, app, iowq, wall = mean(0), mean(1), mean(2), mean(3)
        # one tick = 10ms; per-op ns
        iowq_ns_op = iowq * 10e6 / ops
        app_ns_op = app * 10e6 / ops
        cell_out[arm] = {
            "records": n,
            "rusage_cpu_s_per_record": round(ru, 6),
            "rusage_ns_per_op": round(ru * 1e9 / ops, 1),
            "proc_app_ticks": round(app, 2),
            "app_ticks_ns_per_op": round(app_ns_op, 1),
            "iou_wq_ticks": round(iowq, 2),
            "iou_wq_ns_per_op": round(iowq_ns_op, 1),
            "iou_wq_share_of_app_plus_wq_pct": round(100 * iowq / max(app + iowq, 1e-9), 2),
        }
    out[cell] = cell_out
json.dump(out, sys.stdout, indent=1, sort_keys=True)
print()
