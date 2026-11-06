#!/usr/bin/env python3
"""#595 R5 Phase A tail-trace analyzer (diagnostic-protocol.md).

Reads the binary dumps written by SLUICE_595_R5_TAIL_TRACE=1 runs plus the
bench JSONL summaries, and emits the protocol's per-interval percentile
tables, attribution rule (>=50% of the total-tail gap), tail-op phase
membership, burst spreads, and coverage accounting.

Record layout (88 bytes, little endian):
  u32 seq, u32 slot, u32 generation, u32 presentation,
  u64 cookie, u32 flags, u32 pad, i64 t0,t1,t2,twake,t3,t4,t5
flags: stop kind bits 0..2 (0 plain single, 1 input_exhausted, 2 rejected,
3 cap, 4 sq_budget, 5 control, 6 recovery, 7 ineligible), caller_window
bits 3..10.

Counts header order:
  t0, t1, t2_keys, t2_single, cookie, cqe, published, taken, wake,
  lost_overflow, join_fail_t1, join_fail_t2, join_fail_cookie,
  join_fail_cqe, join_fail_published, join_fail_taken, seq_out_of_range,
  presentations
"""
import json
import struct
import sys
from collections import Counter

MAGIC = 0x5441494C54524143
REC = struct.Struct("<IIIIQIIqqqqqqq")
assert REC.size == 88, REC.size
HDR = struct.Struct("<QIIII")
COUNT_NAMES = [
    "t0", "t1", "t2_keys", "t2_single", "cookie", "cqe", "published", "taken",
    "wake", "lost_overflow", "join_fail_t1", "join_fail_t2", "join_fail_cookie",
    "join_fail_cqe", "join_fail_published", "join_fail_taken",
    "seq_out_of_range", "presentations",
]
STOP_NAMES = {
    0: "plain_single", 1: "input_exhausted", 2: "rejected", 3: "cap",
    4: "sq_budget", 5: "control", 6: "recovery", 7: "ineligible",
}
INTERVALS = [
    ("prep_T1-T0", lambda r: r["t1"] - r["t0"]),
    ("submit_T2-T1", lambda r: r["t2"] - r["t1"]),
    ("kernel_wake_T3-T2", lambda r: r["t3"] - r["t2"]),
    ("reap_pub_T4-T3", lambda r: r["t4"] - r["t3"]),
    ("observe_T5-T4", lambda r: r["t5"] - r["t4"]),
    ("total_T5-T0", lambda r: r["t5"] - r["t0"]),
    ("wake_part_Tw-T2", lambda r: r["twake"] - r["t2"]),
    ("reap_part_T3-Tw", lambda r: r["t3"] - r["twake"]),
]


def load(path):
    with open(path, "rb") as f:
        blob = f.read()
    magic, version, rbytes, nrec, clen = HDR.unpack_from(blob, 0)
    if magic != MAGIC:
        raise SystemExit(f"{path}: bad magic {magic:#x}")
    if version != 1 or rbytes != 88:
        raise SystemExit(f"{path}: version/record mismatch")
    off = HDR.size
    counts = struct.unpack_from(f"<{clen}Q", blob, off)
    off += 8 * clen
    recs = []
    for i in range(nrec):
        vals = REC.unpack_from(blob, off + i * 88)
        recs.append({
            "seq": vals[0], "slot": vals[1], "gen": vals[2], "pres": vals[3],
            "cookie": vals[4], "flags": vals[5],
            "t0": vals[7], "t1": vals[8], "t2": vals[9], "twake": vals[10],
            "t3": vals[11], "t4": vals[12], "t5": vals[13],
        })
    return dict(zip(COUNT_NAMES, counts)), recs


def quantile(sorted_vals, q):
    if not sorted_vals:
        return None
    raw = q * (len(sorted_vals) - 1)
    import math
    idx = min(int(math.floor(raw + 0.5)), len(sorted_vals) - 1)
    return sorted_vals[idx]


def pct_table(recs):
    """Per-interval percentiles over fully-joined records."""
    full = [r for r in recs if r["t0"] and r["t1"] and r["t2"] and r["t3"]
            and r["t4"] and r["t5"]]
    out = {"n_full": len(full), "n_total": len(recs), "p": {}}
    for name, fn in INTERVALS:
        vals = sorted(fn(r) for r in full)
        out["p"][name] = {
            "p50": quantile(vals, 0.50), "p95": quantile(vals, 0.95),
            "p99": quantile(vals, 0.99), "max": vals[-1] if vals else None,
            "mean": (sum(vals) / len(vals)) if vals else None,
            "neg": sum(1 for v in vals if v < 0),
        }
    return out, full


def burst_spreads(full):
    """max(T3)-min(T3) per presentation (>=2 ops)."""
    by_pres = {}
    for r in full:
        by_pres.setdefault(r["pres"], []).append(r["t3"])
    spreads = {}
    for pid, t3s in by_pres.items():
        if len(t3s) >= 2:
            spreads[pid] = max(t3s) - min(t3s)
    size_hist = Counter(len(v) for v in by_pres.values())
    return spreads, size_hist, by_pres


def tail_membership(full, top_frac=0.01):
    total = sorted(full, key=lambda r: r["t5"] - r["t0"])
    k = max(1, int(len(total) * top_frac))
    tail = total[-k:]
    body = total[:-k]
    def summarize(rs):
        stops = Counter(STOP_NAMES[r["flags"] & 7] for r in rs)
        cw = Counter((r["flags"] >> 3) & 0xFF for r in rs)
        in_batch = sum(1 for r in rs if (r["flags"] & 7) != 0)
        return {
            "n": len(rs), "stop_hist": dict(stops),
            "caller_window_hist": dict(sorted(cw.items())),
            "frac_in_kernel_presentation": round(in_batch / len(rs), 4) if rs else None,
            "total_p50_ns": quantile(sorted(r["t5"] - r["t0"] for r in rs), 0.5),
            "kernel_T3_T2_p99_ns": quantile(sorted(r["t3"] - r["t2"] for r in rs), 0.99),
            "observe_T5_T4_p99_ns": quantile(sorted(r["t5"] - r["t4"] for r in rs), 0.99),
            "reap_T4_T3_p99_ns": quantile(sorted(r["t4"] - r["t3"] for r in rs), 0.99),
        }
    return {"tail": summarize(tail), "body": summarize(body),
            "tail_threshold_ns": tail[0]["t5"] - tail[0]["t0"]}


def main():
    result = {"files": {}}
    for path in sys.argv[1:]:
        counts, recs = load(path)
        tbl, full = pct_table(recs)
        spreads, size_hist, by_pres = burst_spreads(full)
        sv = sorted(spreads.values())
        result["files"][path] = {
            "counts": counts,
            "coverage": {
                "t0": sum(1 for r in recs if r["t0"]),
                "t1": sum(1 for r in recs if r["t1"]),
                "t2": sum(1 for r in recs if r["t2"]),
                "t3": sum(1 for r in recs if r["t3"]),
                "t4": sum(1 for r in recs if r["t4"]),
                "t5": sum(1 for r in recs if r["t5"]),
                "twake_set": sum(1 for r in recs if r["twake"]),
                "cookie_set": sum(1 for r in recs if r["cookie"]),
            },
            "join_fail_total": sum(counts[k] for k in counts if k.startswith("join_fail")),
            "percentiles": tbl["p"],
            "n_full": tbl["n_full"],
            "n_total": tbl["n_total"],
            "presentation_size_hist": dict(size_hist),
            "burst_spread_ns": {
                "n_presentations_ge2": len(sv),
                "p50": quantile(sv, 0.5), "p95": quantile(sv, 0.95),
                "p99": quantile(sv, 0.99), "max": sv[-1] if sv else None,
            },
            "tail_membership": tail_membership(full),
        }
    print(json.dumps(result, indent=1))


if __name__ == "__main__":
    main()
