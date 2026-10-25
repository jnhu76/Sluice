#!/usr/bin/env python3
"""#593 K2 E18 discriminating experiment — pair analysis incl. p50/p99."""
import glob, json, os, statistics

ROOT = os.path.dirname(os.path.abspath(__file__))

def metric(path):
    w, c, p50, p99 = [], [], [], []
    for line in open(path):
        r = json.loads(line)
        if not r.get("measured") or not r.get("ok", True):
            continue
        w.append(r["wall_s"] / r["ops"] * 1e6)
        c.append(r["cpu_s"] / r["ops"] * 1e6)
        if r.get("lat_n"):
            p50.append(r["lat_p50_ns"]); p99.append(r["lat_p99_ns"])
    med = lambda v: statistics.median(v) if v else float("nan")
    return med(w), med(c), med(p50), med(p99)

def analyze(tag, mode):
    d = os.path.join(ROOT, f"pairs-{tag}-{mode}")
    second = "full2" if mode == "aa-full" else "elide"
    rows = []
    for i in range(15):
        a = metric(f"{d}/full-{i}.jsonl")
        b = metric(f"{d}/{second}-{i}.jsonl")
        rows.append((a, b))
    def delta(idx):
        return [(b[idx]-a[idx])/a[idx]*100 for a, b in rows]
    dw, dc, d50, d99 = delta(0), delta(1), delta(2), delta(3)
    fmt = lambda v: f"{statistics.median(v):+.2f}% range[{min(v):+.2f},{max(v):+.2f}]"
    print(f"{tag:20s} {mode:14s} pairs={len(rows)}")
    print(f"   wall: {fmt(dw)}")
    print(f"   cpu : {fmt(dc)}  neg={sum(1 for x in dc if x<0)}/{len(dc)}")
    print(f"   p50 : {fmt(d50)}")
    print(f"   p99 : {fmt(d99)}")
    absmed = lambda idx, a_or_b: statistics.median([r[a_or_b][idx] for r in rows])
    print(f"   medians full: wall={absmed(0,0):.4f} cpu={absmed(1,0):.4f} p50={absmed(2,0):.0f} p99={absmed(3,0):.0f}")
    print(f"   medians elide: wall={absmed(0,1):.4f} cpu={absmed(1,1):.4f} p50={absmed(2,1):.0f} p99={absmed(3,1):.0f}")

for tag in ("w02_uring_4k_d16", "w02_uring_4k_d64", "w02_uring_4k_d1", "w03_uring_4k_d16"):
    analyze(tag, "elide-vs-full")
for tag in ("w02_uring_4k_d16", "w02_uring_4k_d64"):
    analyze(tag, "aa-full")
