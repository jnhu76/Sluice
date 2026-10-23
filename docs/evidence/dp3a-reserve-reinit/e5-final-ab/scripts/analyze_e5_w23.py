#!/usr/bin/env python3
"""E5 analyzer stage 2: paired base-vs-threearm ratios from W-02/W-03 JSONL.

Each (case, pair) has two invocations: tag -a (threearm) and -b (base), each
a bench process whose JSONL holds 1 warmup + 3 measured reps (plus oracle and
latency passes). The analysis uses measured ok reps only, pairs invocation
medians, and reports median paired ratios (arm/base) with a bootstrap CI.
Research-only /tmp tool.
"""
import glob
import json
import os
import random
import statistics
import sys

RES = sys.argv[1] if len(sys.argv) > 1 else "/tmp/e5/results/w23"
PAIRS = 15
rng = random.Random(0xE5E5)


def inv_median(path):
    recs = []
    with open(path) as f:
        for line in f:
            r = json.loads(line)
            if r.get("measured") and r.get("ok"):
                recs.append(r)
    if not recs:
        return None
    ops = recs[0]["ops"]
    return {
        "wall_per_op": statistics.median(r["wall_s"] / r["ops"] for r in recs),
        "cpu_per_op": statistics.median(r["cpu_s"] / r["ops"] for r in recs),
        "poll_per_op": statistics.median(r["stats_poll_calls"] / r["ops"] for r in recs),
        "notif_per_op": statistics.median(r["notifications"] / r["ops"] for r in recs),
        "empty_wake_per_op": statistics.median(r["empty_wakes"] / r["ops"] for r in recs),
        "passes_per_op": statistics.median(r["passes"] / r["ops"] for r in recs),
        "waits_per_op": statistics.median(r["waits"] / r["ops"] for r in recs),
        "ops": ops,
        "n_reps": len(recs),
    }


def boot_ci(vals, stat=statistics.median, n=10000):
    if len(vals) < 3:
        return (float("nan"), float("nan"))
    meds = sorted(stat(rng.choices(vals, k=len(vals))) for _ in range(n))
    return (meds[int(0.025 * n)], meds[int(0.975 * n)])


cases = sorted({os.path.basename(p).rsplit("-p", 1)[0]
                for p in glob.glob(os.path.join(RES, "*-p0-*.jsonl"))})

print(f"{'case':<28} {'n':>3} {'wall arm/base':>14} {'95% CI':>18} "
      f"{'cpu arm/base':>13} {'95% CI':>18} {'poll/op A':>9} {'poll/op B':>9}")
summary = {}
for case in cases:
    wr, cr, polls_a, polls_b = [], [], [], []
    extras = {"notif": [], "empty": [], "passes": []}
    for p in range(PAIRS):
        a = os.path.join(RES, f"{case}-p{p}-a.jsonl")
        b = os.path.join(RES, f"{case}-p{p}-b.jsonl")
        if not (os.path.exists(a) and os.path.exists(b)):
            continue
        ra, rb = inv_median(a), inv_median(b)
        if not ra or not rb:
            continue
        wr.append(ra["wall_per_op"] / rb["wall_per_op"])
        cr.append(ra["cpu_per_op"] / rb["cpu_per_op"])
        polls_a.append(ra["poll_per_op"])
        polls_b.append(rb["poll_per_op"])
        for k, key in (("notif", "notif_per_op"), ("empty", "empty_wake_per_op"),
                       ("passes", "passes_per_op")):
            if rb[key] or ra[key]:
                extras[k].append(ra[key] - rb[key])
    if not wr:
        continue
    wci = boot_ci(wr)
    cci = boot_ci(cr)
    print(f"{case:<28} {len(wr):>3} {statistics.median(wr):>14.5f} "
          f"[{wci[0]:.5f},{wci[1]:.5f}] {statistics.median(cr):>13.5f} "
          f"[{cci[0]:.5f},{cci[1]:.5f}] {statistics.median(polls_a):>9.4f} "
          f"{statistics.median(polls_b):>9.4f}")
    summary[case] = {
        "pairs": len(wr),
        "wall_ratio_median": statistics.median(wr),
        "wall_ratio_ci95": wci,
        "cpu_ratio_median": statistics.median(cr),
        "cpu_ratio_ci95": cci,
        "poll_per_op_threearm_median": statistics.median(polls_a),
        "poll_per_op_base_median": statistics.median(polls_b),
        "delta_notif_per_op_median": statistics.median(extras["notif"]) if extras["notif"] else None,
        "delta_empty_wake_per_op_median": statistics.median(extras["empty"]) if extras["empty"] else None,
        "delta_passes_per_op_median": statistics.median(extras["passes"]) if extras["passes"] else None,
    }

with open(os.path.join(RES, "summary.json"), "w") as f:
    json.dump(summary, f, indent=2)
print("\nwrote", os.path.join(RES, "summary.json"))
