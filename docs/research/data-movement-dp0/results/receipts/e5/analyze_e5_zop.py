#!/usr/bin/env python3
"""E5 analyzer: paired base-vs-arm deltas from perf stat outputs.

Reads /tmp/e5/results/zop/<tag>.perf (perf stat -x,) and <tag>.bench.json,
pairs a/b runs by pair index, and reports per-op instruction/cycle deltas
in the DP-0 matrix-summary format. Research-only /tmp tool.
"""
import glob
import json
import os
import re
import statistics
import sys

RES = sys.argv[1] if len(sys.argv) > 1 else "/tmp/e5/results/zop"


def parse_perf(path):
    counts = {}
    with open(path) as f:
        for line in f:
            parts = line.rstrip("\n").split(",")
            if len(parts) >= 4 and parts[0]:
                try:
                    counts[parts[2]] = float(parts[0])
                except ValueError:
                    pass
    return counts


runs = {}
for perf in glob.glob(os.path.join(RES, "*.perf")):
    tag = os.path.basename(perf)[: -len(".perf")]
    m = re.match(r"(.+)-p(\d+)-([ab])$", tag)
    if not m:
        continue
    family, pair, pos = m.group(1), int(m.group(2)), m.group(3)
    bench_path = perf[: -len(".perf")] + ".bench.json"
    with open(bench_path) as f:
        bench = json.loads(f.read().strip().splitlines()[-1])
    runs.setdefault(family, {}).setdefault(pair, {})[pos] = {
        "perf": parse_perf(perf),
        "iters": bench["iters"],
        "wall_s": bench["wall_s"],
    }

print(f"{'family':<16} {'n':>3} {'instr/op delta':>15} {'pairs':>7} "
      f"{'cyc/op delta':>14} {'wall-arm':>9} {'wall-base':>9}")
out = {}
for family in sorted(runs):
    di, dc, dw = [], [], []
    for pair, sides in sorted(runs[family].items()):
        if len(sides) != 2:
            continue
        a, b = sides["a"], sides["b"]  # a = arm, b = base
        it = a["iters"]
        ai = a["perf"].get("instructions:u", 0) / it
        bi = b["perf"].get("instructions:u", 0) / it
        ac = a["perf"].get("cycles:u", 0) / it
        bc = b["perf"].get("cycles:u", 0) / it
        di.append(bi - ai)
        dc.append(bc - ac)
        dw.append(a["wall_s"] - b["wall_s"])
    if not di:
        continue
    arm_name = family.replace("zop-", "").replace("w01ctl", "w01ctl(threearm)")
    print(f"{family:<16} {len(di):>3} {statistics.median(di):>15.5f} "
          f"{sum(1 for x in di if x > 0)}/{len(di):<5} "
          f"{statistics.median(dc):>14.5f} "
          f"{statistics.median(dw):>9.6f} {statistics.median([0.0]):>9.6f}")
    out[family] = {
        "pairs_ok": len(di),
        "median_instr_per_op_delta": statistics.median(di),
        "positive_instr_pairs": sum(1 for x in di if x > 0),
        "median_cycles_per_op_delta": statistics.median(dc),
        "median_wall_delta_s": statistics.median(dw),
    }

with open(os.path.join(RES, "summary.json"), "w") as f:
    json.dump(out, f, indent=2)
print("\nwrote", os.path.join(RES, "summary.json"))
