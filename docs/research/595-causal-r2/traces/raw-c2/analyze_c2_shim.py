#!/usr/bin/env python3
"""Analyze C2 liburing shim event streams (.bin/.meta) + jsonl overhead."""
import json
import struct
import sys
from pathlib import Path

RAW = Path("/home/jnhu/sluice-595-e5/causal-r2/evidence/traces/raw-c2")


def load_events(base):
    meta = {}
    for kv in open(str(base) + ".shim.meta").read().split():
        k, _, v = kv.partition("=")
        meta[k] = v
    hz = float(meta["tsc_hz"])
    n = int(meta["events"])
    data = open(str(base) + ".shim.bin", "rb").read()
    evs = []
    for i in range(n):
        tsc, payload = struct.unpack_from("<QQ", data, i * 16)
        evs.append((tsc, payload & 0xFF, (payload >> 8) - 1024))
    return evs, hz, meta


def segments(evs):
    segs, cur = [], None
    for tsc, kind, val in evs:
        if kind == 5:
            cur = {"entries": val, "t0": tsc, "ev": []}
            segs.append(cur)
            continue
        if cur is not None:
            cur["ev"].append((tsc, kind, val))
    return segs


def q(v, p):
    if not v:
        return None
    v = sorted(v)
    k = (len(v) - 1) * p
    lo, hi = int(k), min(int(k) + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (k - lo)


def seg_stats(seg, hz):
    ev = seg["ev"]
    submits, rets, peeks, peek_rets = [], [], [], []
    gaps_sub_peek, gaps_peek_sub = [], []
    i = 0
    while i < len(ev):
        tsc, kind, val = ev[i]
        if kind == 1 and i + 1 < len(ev) and ev[i + 1][1] == 2:
            submits.append((ev[i + 1][0] - tsc) / hz * 1e9)
            rets.append(ev[i + 1][2])
            # gap to next peek begin
            for j in range(i + 2, min(i + 6, len(ev))):
                if ev[j][1] == 3:
                    gaps_sub_peek.append((ev[j][0] - ev[i + 1][0]) / hz * 1e9)
                    break
                if ev[j][1] == 1:
                    break
            i += 2
            continue
        if kind == 3 and i + 1 < len(ev) and ev[i + 1][1] == 4:
            peeks.append((ev[i + 1][0] - tsc) / hz * 1e9)
            peek_rets.append(ev[i + 1][2])
            for j in range(i + 2, min(i + 6, len(ev))):
                if ev[j][1] == 1:
                    gaps_peek_sub.append((ev[j][0] - ev[i + 1][0]) / hz * 1e9)
                    break
                if ev[j][1] == 3:
                    break
            i += 2
            continue
        i += 1
    return {
        "entries": seg["entries"],
        "n_events": len(ev),
        "submits": {
            "n": len(submits), "ret_hist": {str(r): rets.count(r) for r in set(rets)},
            "mean": sum(submits) / len(submits) if submits else None,
            "p50": q(submits, .5), "p90": q(submits, .9), "p95": q(submits, .95),
            "p99": q(submits, .99), "max": max(submits) if submits else None,
        },
        "peeks": {
            "n": len(peeks),
            "ret_hist": {str(r): peek_rets.count(r) for r in set(peek_rets)},
        },
        "gap_submit_to_peek_ns": {
            "n": len(gaps_sub_peek), "mean": sum(gaps_sub_peek) / len(gaps_sub_peek) if gaps_sub_peek else None,
            "p10": q(gaps_sub_peek, .1), "p50": q(gaps_sub_peek, .5),
            "p90": q(gaps_sub_peek, .9), "p99": q(gaps_sub_peek, .99),
        },
        "gap_peek_to_submit_ns": {
            "n": len(gaps_peek_sub), "mean": sum(gaps_peek_sub) / len(gaps_peek_sub) if gaps_peek_sub else None,
            "p10": q(gaps_peek_sub, .1), "p50": q(gaps_peek_sub, .5),
            "p90": q(gaps_peek_sub, .9), "p99": q(gaps_peek_sub, .99),
        },
    }


def main():
    out = {}
    for arm in ("A", "B"):
        for idx in (1, 2):
            tag = f"c2shim-shim-{arm}-{idx}"
            base = RAW / tag
            if not Path(str(base) + ".shim.bin").exists():
                continue
            evs, hz, meta = load_events(base)
            segs = [s for s in segments(evs) if s["entries"] == 2]
            recs = [seg_stats(s, hz) for s in segs[-3:]]
            jsonl = [json.loads(x) for x in open(str(base) + ".jsonl") if x.strip()]
            out[tag] = {"tsc_hz": hz, "events_total": len(evs), "segments": recs,
                        "jsonl": [{k: r.get(k) for k in ("measured", "ops", "cpu_s", "wall_s", "ops_per_sec")}
                                  for r in jsonl]}
    json.dump(out, sys.stdout, indent=1)


if __name__ == "__main__":
    main()
