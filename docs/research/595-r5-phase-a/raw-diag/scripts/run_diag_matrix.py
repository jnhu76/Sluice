#!/usr/bin/env python3
"""#595 R5 Phase A diagnostic matrix runner (diagnostic-protocol.md section 2).

Must be launched with the R5 MEASUREMENT.lock already held (fd-level flock by
the wrapper); it never takes the lock itself and never builds.

Plan (frozen):
  off-mode on/off pair, d16: 1 pair (A0,R)  — arena writes env-gated off
  on-mode d16: 3 blocks x 2 orderings (A0->R, R->A0) = 6 pairs = 12 runs
  on-mode d4:  same                                            = 12 runs
Each run: fresh bench process pinned to cpu2, WARMUP=1 MEASURED=1 STATS=0,
same binary, jsonl+log+trace dumps captured. 24 measured phase executions on
mode + 2 off mode.
"""
import hashlib
import json
import os
import re
import subprocess
import sys
import time

BIN = "/tmp/r5-build-taildiag/linux/x86_64/release/bench_w03_external_loop"
TASKSET = "/usr/sbin/taskset"
OUT = "/home/jnhu/sluice-595-e5/r5/raw-diag/matrix"
TMP = "/tmp/sluice-595-r5diag"
CPU = "2"


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def parse_jsonl(path):
    recs = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                recs.append(json.loads(line))
    return recs


def run_one(mode, case, pair, pos, arm, binary_sha):
    phase = f"{mode}-{case}"
    tag = f"p{pair:02d}-{pos}-{arm}"
    d = os.path.join(OUT, phase, tag)
    os.makedirs(d, exist_ok=True)
    dump_dir = os.path.join(d, "dump")
    os.makedirs(dump_dir, exist_ok=True)
    jpath = os.path.join(d, "bench.jsonl")
    lpath = os.path.join(d, "bench.log")
    epath = os.path.join(d, "bench.err")
    if os.path.exists(jpath):
        os.unlink(jpath)
    env = {k: v for k, v in os.environ.items() if not k.startswith("SLUICE_")}
    env.update({
        "SLUICE_BENCH_WARMUP": "1",
        "SLUICE_BENCH_MEASURED": "1",
        "SLUICE_BENCH_STATS": "0",
        "SLUICE_BENCH_CASE_FILTER": "=" + case,
        "SLUICE_BENCH_JSONL": jpath,
        "SLUICE_BENCH_TMP_DIR": TMP,
        "SLUICE_595_R3_ARM": arm,
    })
    if mode == "on":
        env["SLUICE_595_R5_TAIL_TRACE"] = "1"
        env["SLUICE_595_R5_TAIL_TRACE_DUMP_DIR"] = dump_dir
    with open(lpath, "wb") as lo, open(epath, "wb") as er:
        t0 = time.time()
        rc = subprocess.call([TASKSET, "-c", CPU, BIN], env=env, stdout=lo, stderr=er)
        wall = time.time() - t0
    recs = parse_jsonl(jpath) if os.path.exists(jpath) else []
    measured = [r for r in recs if r.get("measured") and r.get("case_id") == case]
    lat = None
    if measured:
        m = measured[0]
        lat = {k: m[k] for k in ("lat_n", "lat_p50_ns", "lat_p95_ns", "lat_p99_ns",
                                 "lat_max_ns", "lat_mean_ns")}
    arm_line = ""
    try:
        with open(lpath) as f:
            for line in f:
                if line.startswith("research arm="):
                    arm_line = line.strip()
    except OSError:
        pass
    out = {
        "phase": phase, "pair": pair, "pos": pos, "arm": arm, "case": case,
        "mode": mode, "ts_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "exit": rc, "wall_s": round(wall, 3),
        "binary_sha256": binary_sha,
        "env": {k: env[k] for k in sorted(env) if k.startswith("SLUICE_")},
        "measured_records": len(measured), "lat": lat, "readback": arm_line,
        "dumps": sorted(os.listdir(dump_dir)),
    }
    with open(os.path.join(d, "attempt.json"), "w") as f:
        json.dump(out, f, indent=1)
    print(f"{phase} {tag} exit={rc} lat={lat['lat_p99_ns'] if lat else None}", flush=True)
    return out


def main():
    binary_sha = sha256(BIN)
    manifest = {"binary": BIN, "binary_sha256": binary_sha,
                "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "note": "diagnostic-only runs; not timed gate evidence; never mixed "
                        "with clean R4/R5 gate blocks (protocol section 2)",
                "runs": []}
    failures = 0
    pair_seq = 0
    # on/off perturbation check pair first (protocol 4.2; ordering decision
    # recorded here): off-mode d16 pair
    for pos, arm in enumerate(("A0", "R")):
        r = run_one("off", "W03-uring-4k-d16", pair_seq, pos, arm, binary_sha)
        manifest["runs"].append(r)
        failures += 0 if (r["exit"] == 0 and r["lat"]) else 1
    pair_seq += 1
    for case, base in (("W03-uring-4k-d16", "on-d16"), ("W03-uring-4k-d4", "on-d4")):
        p = 0
        for block in range(3):
            for ordering in (("A0", "R"), ("R", "A0")):
                for pos, arm in enumerate(ordering):
                    r = run_one("on", case, p, pos, arm, binary_sha)
                    manifest["runs"].append(r)
                    failures += 0 if (r["exit"] == 0 and r["lat"]) else 1
                p += 1
    manifest["ended_utc"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    manifest["failed_runs"] = failures
    with open(os.path.join(OUT, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1)
    print(f"matrix done, failed_runs={failures}", flush=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
