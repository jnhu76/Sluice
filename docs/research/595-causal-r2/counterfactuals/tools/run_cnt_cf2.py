#!/usr/bin/env python3
"""CF-2 deferoff mechanism companion: research-counter A/B runs on two builds.

Non-timing run (research counters enabled). Never mixed with timing blocks.
Emits one JSON record per invocation with per-rep counters and per-op rates.
"""
import json
import re
import subprocess
import sys
import os

CASE = "W02-uring-4k-d2"
OPS = 20000
TAG = re.compile(r"SLUICE595_COUNTERS tag=measured rep=(\d+) (.*)")


def run_once(binary, arm, out_dir, idx):
    env = os.environ.copy()
    env.update({
        "SLUICE_595_COUNTERS": "1",
        "SLUICE_595_ARM": arm,
        "SLUICE_BENCH_CASE_FILTER": "=" + CASE,
        "SLUICE_BENCH_595_DEPTHS": "1",
        "SLUICE_BENCH_WARMUP": "1",
        "SLUICE_BENCH_MEASURED": "3",
        "SLUICE_BENCH_STATS": "1",
        "SLUICE_BENCH_TMP_DIR": "/tmp/sluice-595-bench",
    })
    cmd = ["/usr/bin/taskset", "-c", "2", binary]
    p = subprocess.run(cmd, env=env, capture_output=True, text=True)
    reps = []
    for line in p.stderr.splitlines():
        m = TAG.match(line)
        if not m:
            continue
        vals = {}
        for kv in m.group(2).split():
            k, _, v = kv.partition("=")
            vals[k] = int(v)
        reps.append({"rep": int(m.group(1)), "counters": vals})
    return {"binary": binary, "arm": arm, "idx": idx, "exit": p.returncode,
            "reps": reps, "ops_per_rep": OPS}


def main():
    out_dir = sys.argv[1]
    binaries = [("frozen", sys.argv[2]), ("deferoff", sys.argv[3])]
    os.makedirs(out_dir, exist_ok=True)
    records = []
    for label, binary in binaries:
        for arm in ("A", "B"):
            for idx in range(3):
                rec = run_once(binary, arm, out_dir, idx)
                rec["build"] = label
                records.append(rec)
                nw = len(rec["reps"])
                wu = rec["reps"][0]["counters"].get("wakeups", -1) if nw else -1
                print(f"[{label}] arm={arm} idx={idx} exit={rec['exit']} "
                      f"reps={nw} wakeups_rep0={wu}", flush=True)
    with open(os.path.join(out_dir, "records.json"), "w") as f:
        json.dump(records, f, indent=1)


if __name__ == "__main__":
    main()
