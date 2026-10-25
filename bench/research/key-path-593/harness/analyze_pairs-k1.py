#!/usr/bin/env python3
"""#593 K1 discriminating experiment — pair analysis (15 AB/BA pairs per arm set)."""
import json, statistics, os

ROOT = os.path.dirname(os.path.abspath(__file__))

def metric(path):
    w, c = [], []
    for line in open(path):
        r = json.loads(line)
        if not r.get("measured") or not r.get("ok", True):
            continue
        w.append(r["wall_s"] / r["ops"] * 1e6)
        c.append(r["cpu_s"] / r["ops"] * 1e6)
    return statistics.median(w), statistics.median(c)

def pairs(tag, mode):
    d = os.path.join(ROOT, f"pairs-{tag}-{mode}")
    second = "full2" if mode == "aa-full" else "elide"
    out = []
    for i in range(15):
        a = metric(f"{d}/full-{i}.jsonl")
        b = metric(f"{d}/{second}-{i}.jsonl")
        out.append(((b[0]-a[0])/a[0]*100, (b[1]-a[1])/a[1]*100))
    return out

for tag in ("w03_uring_4k_d16", "w02_uring_4k_d16"):
    for mode in ("aa-full", "elide-vs-full"):
        p = pairs(tag, mode)
        dw = statistics.median([x[0] for x in p])
        dc = statistics.median([x[1] for x in p])
        neg = sum(1 for x in p if x[1] < 0)
        print(f"{tag:22s} {mode:14s} pairs={len(p)} wall_median={dw:+.2f}% "
              f"cpu_median={dc:+.2f}% cpu_range=[{min(x[1] for x in p):+.2f},{max(x[1] for x in p):+.2f}] "
              f"neg_cpu_pairs={neg}/{len(p)}")
