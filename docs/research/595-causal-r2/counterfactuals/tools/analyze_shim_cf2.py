#!/usr/bin/env python3
"""CF-2 deferoff shim analysis: deferred-flush fraction (submit->peek gap
< 250 ns) for frozen-B vs deferoff-B. Segment loader and metrics identical
to traces/raw-c2/shim_fractions.py (validated C2 tooling); only data source
and labels differ. Prediction: deferoff-B fraction -> 0."""
import json
import struct
from pathlib import Path

RAW = Path("/home/jnhu/sluice-595-e5/causal-r2/evidence/counterfactual/raw/shim-companion-cf2")
TH_NS = 250.0


def load_seg_submits_peeks(base):
    meta = dict(kv.partition("=")[::2]
                for kv in open(str(base) + ".shim.meta").read().split())
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
        "sub_mean": sum(submits) / len(submits) if submits else None,
        "sub_p50": sorted(submits)[len(submits) // 2] if submits else None,
        "defer_frac_gapfast": fast / len(submits) if submits else None,
        "n_gapfast": fast,
    }


data = {}
for label in ("frozen", "deferoff"):
    for idx in (1, 2):
        base = RAW / f"c2shim-{label}-B-{idx}"
        segs, hz = load_seg_submits_peeks(base)
        ms = [seg_metrics(s, hz) for s in segs]
        data[f"{label}{idx}"] = ms
        print(f"{label}-B-{idx}: " + " | ".join(
            f"n_sub={m['n_sub']} gapfast={m['n_gapfast']}/{m['n_sub']} "
            f"({(m['defer_frac_gapfast'] or 0)*100:.2f}%) sub_p50={m['sub_p50']:.0f}ns "
            f"ret_non1={m['ret_non1']}" for m in ms))

for label in ("frozen", "deferoff"):
    fr = [m["defer_frac_gapfast"] for k in data if k.startswith(label)
          for m in data[k]]
    print(f"{label}-B fast-follow fraction mean = {sum(fr)/len(fr)*100:.2f}% "
          f"(segs: " + ",".join(f"{x*100:.2f}" for x in fr) + ")")

with open(RAW / "analysis.json", "w") as f:
    json.dump(data, f, indent=1)
