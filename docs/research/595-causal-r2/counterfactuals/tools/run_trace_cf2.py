#!/usr/bin/env python3
"""CF-2 deferoff single-instrumentation trace companion (user 7-point ring only).

Reuses the validated E2 protocol (2000 ops, warmup=0, measured=1) with NO
bpftrace, NO research counters: one instrumentation per run. For each build
(frozen / sigdrop) runs arm B once and dumps the ring; analysis reports defer
ratio and flush-predecessor composition per phase.
"""
import json
import os
import struct
import subprocess
import sys

REC = struct.Struct("<QQQQQIiIIBBH4x")
EVT = {1: "accept", 2: "sqe_prep", 3: "defer", 4: "immediate", 5: "flush",
       6: "reap", 7: "published", 8: "take"}


def run_once(binary, out_dir, label):
    env = os.environ.copy()
    env.update({
        "SLUICE_595_TRACE": "1",
        "SLUICE_595_TRACE_OUT": os.path.join(out_dir, f"{label}.bin"),
        "SLUICE_595_ARM": "B",
        "SLUICE_BENCH_WARMUP": "0",
        "SLUICE_BENCH_MEASURED": "1",
        "SLUICE_BENCH_595_DEPTHS": "1",
        "SLUICE_BENCH_CASE_FILTER": "W02-uring-4k-d2",
        "SLUICE_BENCH_595_OPS": "2000",
        "SLUICE_BENCH_REUSE_FIXTURE": "1",
        "SLUICE_BENCH_TMP_DIR": "/tmp/sluice-595-cost",
        "SLUICE_BENCH_STATS": "1",
    })
    p = subprocess.run(["/usr/bin/taskset", "-c", "2", binary],
                       env=env, capture_output=True, text=True, timeout=120)
    dump_line = [l for l in p.stderr.splitlines() if "S595_TRACE_DUMP" in l]
    return p.returncode, (dump_line[-1] if dump_line else "")


def load_user(path):
    data = open(path, "rb").read()
    rows = []
    for i in range(len(data) // REC.size):
        (seq, ts, cookie, gen, aux, tid, fd, slot, phase, evt, cpu, flags) = \
            REC.unpack_from(data, i * REC.size)
        rows.append(dict(seq=seq, ts=ts, phase=phase, evt=EVT.get(evt, evt)))
    return rows


def analyze(label, rows):
    out = {"label": label, "phases": {}}
    phases = sorted(set(r["phase"] for r in rows))
    for ph in phases:
        ev = [r for r in rows if r["phase"] == ph]
        n_defer = sum(1 for r in ev if r["evt"] == "defer")
        n_imm = sum(1 for r in ev if r["evt"] == "immediate")
        n_flush = sum(1 for r in ev if r["evt"] == "flush")
        # flush predecessor: nearest earlier defer/immediate decision event
        pred = {"defer": 0, "immediate": 0, "other": 0}
        last_decision = None
        for r in ev:
            if r["evt"] in ("defer", "immediate"):
                last_decision = r["evt"]
            elif r["evt"] == "flush":
                pred[last_decision or "other"] += 1
        out["phases"][str(ph)] = {
            "defer": n_defer, "immediate": n_imm, "flush": n_flush,
            "defer_ratio": round(n_defer / (n_defer + n_imm), 4) if n_defer + n_imm else None,
            "flush_pred": pred,
        }
    return out


def main():
    out_dir = sys.argv[1]
    binaries = [("frozen", sys.argv[2]), ("deferoff", sys.argv[3])]
    os.makedirs(out_dir, exist_ok=True)
    results = []
    for label, binary in binaries:
        rc, dump = run_once(binary, out_dir, label)
        rows = load_user(os.path.join(out_dir, f"{label}.bin"))
        res = analyze(label, rows)
        res["exit"] = rc
        res["dump_line"] = dump
        results.append(res)
        print(json.dumps(res))
    with open(os.path.join(out_dir, "analysis.json"), "w") as f:
        json.dump(results, f, indent=1)


if __name__ == "__main__":
    main()
