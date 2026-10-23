#!/usr/bin/env python3
"""E5 pollmetrics: paired arm/base deltas of poll_calls, notifications,
empty_wakes and passes per operation from the stats-wired harness runs."""
import glob, json, os, statistics, sys

RES = sys.argv[1] if len(sys.argv) > 1 else "/tmp/e5/results/pollmetrics"
PAIRS = 5

def inv(path):
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
        "poll": statistics.median(r["stats_poll_calls"] / r["ops"] for r in recs),
        "notif": statistics.median(r["notifications"] / r["ops"] for r in recs),
        "empty": statistics.median(r["empty_wakes"] / r["ops"] for r in recs),
        "passes": statistics.median(r["passes"] / r["ops"] for r in recs),
        "wall": statistics.median(r["wall_s"] / r["ops"] for r in recs),
    }

cases = sorted({os.path.basename(p).rsplit("-p", 1)[0]
                for p in glob.glob(os.path.join(RES, "*-p0-a.jsonl"))})
summary = {}
print(f"{'case':<30} {'poll/op A':>10} {'B':>8} {'dA-B':>8} {'notif A':>9} {'B':>8} "
      f"{'empty A':>8} {'B':>8} {'passes A':>9} {'B':>8}")
for case in cases:
    da, db = [], []
    na, nb, ea, eb, pa, pb = [], [], [], [], [], []
    for p in range(PAIRS):
        a = os.path.join(RES, f"{case}-p{p}-a.jsonl")
        b = os.path.join(RES, f"{case}-p{p}-b.jsonl")
        if not (os.path.exists(a) and os.path.exists(b)):
            continue
        ra, rb = inv(a), inv(b)
        if not ra or not rb:
            continue
        da.append(ra["poll"]); db.append(rb["poll"])
        na.append(ra["notif"]); nb.append(rb["notif"])
        ea.append(ra["empty"]); eb.append(rb["empty"])
        pa.append(ra["passes"]); pb.append(rb["passes"])
    if not da:
        continue
    print(f"{case:<30} {statistics.median(da):>10.4f} {statistics.median(db):>8.4f} "
          f"{statistics.median(da)-statistics.median(db):>8.4f} "
          f"{statistics.median(na):>9.4f} {statistics.median(nb):>8.4f} "
          f"{statistics.median(ea):>8.4f} {statistics.median(eb):>8.4f} "
          f"{statistics.median(pa):>9.4f} {statistics.median(pb):>8.4f}")
    summary[case] = {"pairs": len(da),
                     "poll_per_op_arm_median": statistics.median(da),
                     "poll_per_op_base_median": statistics.median(db),
                     "notif_per_op_arm_median": statistics.median(na),
                     "notif_per_op_base_median": statistics.median(nb),
                     "empty_wake_per_op_arm_median": statistics.median(ea),
                     "empty_wake_per_op_base_median": statistics.median(eb),
                     "passes_per_op_arm_median": statistics.median(pa),
                     "passes_per_op_base_median": statistics.median(pb)}
with open(os.path.join(RES, "summary.json"), "w") as f:
    json.dump(summary, f, indent=2)
print("\nwrote", os.path.join(RES, "summary.json"))
