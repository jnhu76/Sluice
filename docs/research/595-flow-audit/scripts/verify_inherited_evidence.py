#!/usr/bin/env python3
"""Offline verification of inherited evidence (no re-timing).

1. Primary-metric reproduction: per-arm mean cpu ns/op and p99 (us) from
   r5-repair per-rep raw jsonl; must bit-match RESULTS.md tables.
2. F5 corrected-position summary must contain the quoted conditional tail
   risks (positions 0-7 ~1.2%, 8-15 ~9.0%, occupancy-8 ~26%) and clean
   recovery validation totals.
Exit 0 = all checks pass; each failure prints a receipt line.
"""
import json, glob, sys
from collections import defaultdict

R = "/home/jnhu/sluice-595-e5/r5-repair-measure"
EXPECT = {
    "main-w03-d16": {"A0": (3208.6, 53.92), "R": (2519.1, 64.63), "C": (2499.4, 63.44)},
    "prot-w03-d4": {"A0": (5618.3, 33.40), "R": (4861.2, 27.90), "C": (4672.6, 26.98)},
    "prot-w03-d2serial": {"A0": (13264.5, 18.35), "R": (14421.7, 21.22), "C": (13777.1, 19.82)},
}
fail = 0
for cell, arms in EXPECT.items():
    per = defaultdict(list); lat = defaultdict(list)
    for f in glob.glob(f"{R}/raw/{cell}/main/jsonl/*.jsonl"):
        arm = f.split("-")[-2]
        for line in open(f):
            r = json.loads(line)
            if r.get("measured") and r.get("ops", 0) >= 20000 and r.get("cpu_s", 0) > 0:
                per[arm].append(r["cpu_s"] / r["ops"] * 1e9)
                lat[arm].append(r.get("lat_p99_ns", 0) / 1000)
    for arm, (cpu_e, p99_e) in arms.items():
        got_cpu = round(sum(per[arm]) / len(per[arm]), 1)
        got_p99 = round(sum(lat[arm]) / len(lat[arm]), 2)
        ok = abs(got_cpu - cpu_e) < 0.15 and abs(got_p99 - p99_e) < 0.03
        print(f"{cell} {arm}: cpu {got_cpu} (expect {cpu_e}) p99 {got_p99} (expect {p99_e}) -> {'PASS' if ok else 'FAIL'}")
        fail += 0 if ok else 1

f5 = json.load(open(f"{R}/analysis/f5-position.json"))
mp = f5["member_position"]
pos = {int(k.split("|pos")[1]): v["tail_gt55_pct"] for k, v in mp.items() if k.startswith("W03-uring-4k-d16|cw0|")}
front = [pos[i] for i in range(0, 8) if i in pos]
back = [pos[i] for i in range(8, 16) if i in pos]
cw8 = f5["window_occupancy"]["W03-uring-4k-d16|cw8"]["tail_gt55_pct"]
vt = f5["validation_totals"]
c1 = max(front) < 1.5 and min(front) > 0.9
c2 = min(back) > 8.5 and max(back) < 9.5
c3 = 25.5 < cw8 < 27.0
c4 = vt["anomalous_groups"] == 0 and vt["unpositioned_rows"] == 0 and vt["tags_with_seq_gaps"] == 0
print(f"F5 front max {max(front):.2f}% (<1.5) -> {'PASS' if c1 else 'FAIL'}")
print(f"F5 back min {min(back):.2f}% (>8.5) -> {'PASS' if c2 else 'FAIL'}")
print(f"F5 cw8 {cw8:.2f}% (~26) -> {'PASS' if c3 else 'FAIL'}")
print(f"F5 validation clean -> {'PASS' if c4 else 'FAIL'}")
fail += sum(1 for c in (c1, c2, c3, c4) if not c)
sys.exit(1 if fail else 0)
