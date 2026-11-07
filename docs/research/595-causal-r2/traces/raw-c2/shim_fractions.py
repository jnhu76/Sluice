#!/usr/bin/env python3
"""Refined shim stats: deferred-flush fraction (submit->peek gap < threshold),
pooled submit durations with segment bootstrap, shim overhead from jsonl."""
import json
import random
import struct
from pathlib import Path

RAW = Path("/home/jnhu/sluice-595-e5/causal-r2/evidence/traces/raw-c2")
TH_NS = 250.0


def load_seg_submits_peeks(base):
    meta = dict(kv.partition("=")[::2] for kv in open(str(base) + ".shim.meta").read().split())
    hz = float(meta["tsc_hz"])
    n = int(meta["events"])
    data = open(str(base) + ".shim.bin", "rb").read()
    evs = []
    for i in range(n):
        tsc, payload = struct.unpack_from("<QQ", data, i * 16)
        evs.append((tsc, payload & 0xFF, (payload >> 8) - 1024))
    segs, cur = [], None
    for tsc, kind, val in evs:
        if kind == 5:
            cur = {"entries": val, "ev": []}
            segs.append(cur)
            continue
        if cur is not None:
            cur["ev"].append((tsc, kind, val))
    out = []
    for s in segs:
        if s["entries"] != 2:
            continue
        out.append(s["ev"])
    return out[-3:], hz


def seg_metrics(ev, hz):
    submits, gaps, rets = [], [], []
    i = 0
    while i < len(ev):
        tsc, kind, val = ev[i]
        if kind == 1:
            if i + 1 < len(ev) and ev[i + 1][1] == 2:
                submits.append((ev[i + 1][0] - tsc) / hz * 1e9)
                rets.append(ev[i + 1][2])
                for j in range(i + 2, min(i + 6, len(ev))):
                    if ev[j][1] == 3:
                        gaps.append((ev[j][0] - ev[i + 1][0]) / hz * 1e9)
                        break
                    if ev[j][1] == 1:
                        break
                i += 2
                continue
        i += 1
    fast = sum(1 for g in gaps if g < TH_NS)
    return {
        "n_sub": len(submits), "n_gap": len(gaps),
        "ret_non1": sum(1 for r in rets if r != 1),
        "sub_mean": sum(submits) / len(submits),
        "sub_p50": sorted(submits)[len(submits) // 2],
        "defer_frac_gapfast": fast / len(submits),
        "n_gapfast": fast,
    }


data = {}
for arm in ("A", "B"):
    for idx in (1, 2):
        base = RAW / f"c2shim-shim-{arm}-{idx}"
        segs, hz = load_seg_submits_peeks(base)
        ms = [seg_metrics(s, hz) for s in segs]
        data[f"{arm}{idx}"] = ms
        print(f"{arm}-{idx}: " + " | ".join(
            f"submean={m['sub_mean']:.0f} p50={m['sub_p50']:.0f} "
            f"gapfast={m['n_gapfast']}/{m['n_sub']} ({m['defer_frac_gapfast']*100:.2f}%) "
            f"ret_non1={m['ret_non1']}" for m in ms))

# pooled B-A submit-duration diff, bootstrap over the 6 segments/arm
mA = [m["sub_mean"] for v in data.values() if v and True for m in v]  # placeholder
mA = [m["sub_mean"] for k in data if k[0] == "A" for m in data[k]]
mB = [m["sub_mean"] for k in data if k[0] == "B" for m in data[k]]
obs = sum(mB) / len(mB) - sum(mA) / len(mA)
random.seed(5950)
diffs = []
for _ in range(10000):
    a = [random.choice(mA) for _ in mA]
    b = [random.choice(mB) for _ in mB]
    diffs.append(sum(b) / len(b) - sum(a) / len(a))
diffs.sort()
print(f"submit dur B-A = {obs:+.1f} ns  bootstrap95 [{diffs[249]:+.1f}, {diffs[9749]:+.1f}]")

fracA = [m["defer_frac_gapfast"] for k in data if k[0] == "A" for m in data[k]]
fracB = [m["defer_frac_gapfast"] for k in data if k[0] == "B" for m in data[k]]
print(f"fast-follow fraction A: mean={sum(fracA)/len(fracA)*100:.2f}% (segs: "
      + ",".join(f"{x*100:.2f}" for x in fracA) + ")")
print(f"fast-follow fraction B: mean={sum(fracB)/len(fracB)*100:.2f}% (segs: "
      + ",".join(f"{x*100:.2f}" for x in fracB) + ")")

# overhead: shim vs noshim jsonl
print("=== overhead (measured reps):")
for mode in ("shim", "noshim"):
    for arm in ("A", "B"):
        rows = []
        for idx in (1, 2):
            p = RAW / f"c2shim-{mode}-{arm}-{idx}.jsonl"
            for line in open(p):
                r = json.loads(line)
                if r.get("measured"):
                    rows.append((r["cpu_s"] / r["ops"] * 1e9, r["ops_per_sec"]))
        cpu = [x for x, _ in rows]
        tp = [y for _, y in rows]
        print(f"{mode}-{arm}: n={len(rows)} cpu_ns/op mean={sum(cpu)/len(cpu):.1f} "
              f"tput mean={sum(tp)/len(tp):,.0f}")
