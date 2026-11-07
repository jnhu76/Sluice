#!/usr/bin/env python3
"""Parse S595_TRACE_DUMP records: per-depth defer/flush sequence and submit delay.

Record layout (see include/sluice/async/detail/research_595_trace.hpp):
  seq u64, ts_ns u64, cookie u64, gen u64, aux u64, tid u32, ring_fd i32,
  slot u32, phase u32, evt u8, cpu u8, flags u16, pad 4  -> sizeof 64
Events: accept1 sqe_prep2 defer3 immediate4 flush5 reap6 published7 take8
flush.aux = ledger size at flush.
"""

import struct
import sys
from collections import Counter, defaultdict

FMT = struct.Struct("<QQQQQIiIIBBH4x")
NAMES = {1: "accept", 2: "prep", 3: "defer", 4: "immediate", 5: "flush", 6: "reap",
         7: "published", 8: "take"}


def load(path):
    data = open(path, "rb").read()
    n = len(data) // FMT.size
    out = []
    for i in range(n):
        seq, ts, cookie, gen, aux, tid, rfd, slot, phase, evt, cpu, flags = FMT.unpack_from(data, i * FMT.size)
        out.append((seq, ts, cookie, gen, aux, tid, rfd, slot, phase, evt, cpu, flags))
    return out


def analyze(path):
    recs = load(path)
    by_phase = defaultdict(list)
    for r in recs:
        by_phase[r[8]].append(r)
    result = {}
    for phase, rs in sorted(by_phase.items()):
        cnt = Counter(NAMES.get(r[9], "?") for r in rs)
        # main pipeline phase = exactly 20000 accepts
        result[phase] = {"n": len(rs), "counts": dict(cnt)}
    return result


def sequence_stats(path):
    """For the phase with 20k accepts: ledger accumulation and defer->flush delay."""
    recs = load(path)
    phases = defaultdict(list)
    for r in recs:
        phases[r[8]].append(r)
    target = None
    for phase, rs in phases.items():
        if sum(1 for r in rs if r[9] == 1) == 20000:
            target = phase
            break
    if target is None:
        return None
    rs = phases[target]
    flush_sizes = []
    defer_to_flush = []
    pending = []  # (ts, slot, gen)
    defers_between = 0
    seq_between = []
    n_defer = n_immediate = n_flush = 0
    for r in rs:
        evt, ts = r[9], r[1]
        if evt == 3:
            n_defer += 1
            pending.append((ts, r[7], r[3]))
            defers_between += 1
        elif evt == 4:
            n_immediate += 1
        elif evt == 5:
            n_flush += 1
            flush_sizes.append(r[4])
            seq_between.append(defers_between)
            for (dts, slot, gen) in pending:
                defer_to_flush.append(ts - dts)
            pending = []
            defers_between = 0
    def pct(vals, q):
        if not vals:
            return 0
        s = sorted(vals)
        return s[min(int(q * len(s)), len(s) - 1)]
    delayed = [d for d in defer_to_flush if d >= 0]
    return {
        "phase": target, "n_flush": n_flush, "n_defer": n_defer, "n_immediate": n_immediate,
        "flush_size_hist": dict(Counter(flush_sizes)),
        "defer_after_flush_hist": dict(Counter(seq_between)),
        "delay_ns_n": len(delayed),
        "delay_ns_p10": pct(delayed, 0.10), "delay_ns_p50": pct(delayed, 0.50),
        "delay_ns_p90": pct(delayed, 0.90), "delay_ns_p99": pct(delayed, 0.99),
        "delay_ns_max": max(delayed) if delayed else None,
        "delay_ns_mean": (sum(delayed) / len(delayed)) if delayed else None,
    }


if __name__ == "__main__":
    for path in sys.argv[1:]:
        print("==", path)
        for phase, info in analyze(path).items():
            print("  phase", phase, "n", info["n"], info["counts"])
        st = sequence_stats(path)
        print("  seq:", st)
