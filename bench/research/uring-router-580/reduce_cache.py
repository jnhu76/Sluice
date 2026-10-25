#!/usr/bin/env python3
"""#580 R9 cache-locality reducer.

Reads the run_cache_matrix JSONL. For paired stages (pairs/flush/external)
pairs rows by pair id within a shape, computes per-pair ratios
(interfered/warm, flush/warm, or slot1/slot0 for A/A) for wall/user/sys/
net_wall and per-op wall, then reports the median with a percentile-bootstrap
95% interval (B=10000, seed 7) over pairs, plus absolute per-condition
medians. The interval describes the spread of these pairs on this host and
session; it is not a population confidence guarantee.

For the probe stage it tabulates per-site lookup counts and probe
distributions from the instrumented counters lines.
"""
import argparse
import json
import random
import statistics


def load_rows(path):
    rows = []
    with open(path) as fh:
        for line in fh:
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError:
                continue
    return rows


def metrics_of(row):
    out = {}
    ops = max(row.get("completed_ops", 0), 1)
    for key in ("wall_s", "user_s", "sys_s"):
        if key in row:
            out[key] = float(row[key])
    if row.get("net_wall_s", -1) >= 0:
        out["net_wall_s"] = float(row["net_wall_s"])
        out["net_wall_per_op_us"] = float(row["net_wall_s"]) / ops * 1e6
    out["wall_per_op_us"] = float(row["wall_s"]) / ops * 1e6
    return out


def boot_ratio(pairs, B=10000, seed=7):
    if not pairs:
        return None
    rng = random.Random(seed)
    n = len(pairs)
    meds = []
    for _ in range(B):
        sample = [pairs[rng.randrange(n)] for _ in range(n)]
        meds.append(statistics.median(sample))
    meds.sort()
    return meds[int(0.025 * B)], meds[int(0.975 * B)]


def reduce_pairs(rows, stage):
    shapes = {}
    for r in rows:
        if r.get("stage") != stage or r.get("kind") != "timed":
            continue
        shapes.setdefault(r["shape"], {}).setdefault(r["pair"], []).append(r)
    report = {}
    for shape, pairs in sorted(shapes.items()):
        conds = sorted({r.get("cond") for rs in pairs.values() for r in rs})
        if not conds:
            continue
        if "warm" in conds and len(conds) == 2:
            num_cond, den_cond = [c for c in conds if c != "warm"][0], "warm"
        elif len(conds) == 1:
            num_cond = den_cond = conds[0]
        else:
            num_cond, den_cond = conds[1], conds[0]
        ratio_rows = {k: [] for k in ("wall_s", "user_s", "sys_s", "net_wall_s",
                                      "wall_per_op_us", "net_wall_per_op_us")}
        absolutes = {num_cond: [], den_cond: []}
        dropped = 0
        for pair_id, rs in sorted(pairs.items()):
            ok_rows = [r for r in rs if r.get("ok") and r.get("completed_ops", 0) > 0]
            if len(ok_rows) != 2:
                dropped += 1
                continue
            if num_cond == den_cond:
                num = next((r for r in ok_rows if r["slot"] == 1), None)
                den = next((r for r in ok_rows if r["slot"] == 0), None)
            else:
                num = next((r for r in ok_rows if r["cond"] == num_cond), None)
                den = next((r for r in ok_rows if r["cond"] == den_cond), None)
            if num is None or den is None:
                dropped += 1
                continue
            nm, dm = metrics_of(num), metrics_of(den)
            for key in ratio_rows:
                if key in nm and key in dm and dm[key] > 0:
                    ratio_rows[key].append(nm[key] / dm[key])
            absolutes[num_cond].append(nm)
            absolutes[den_cond].append(dm)
        entry = {"n_pairs": len(ratio_rows["wall_s"]), "dropped": dropped,
                 "num_cond": num_cond, "den_cond": den_cond, "ratios": {},
                 "abs": {}}
        for key, vals in ratio_rows.items():
            if len(vals) >= 3:
                lo, hi = boot_ratio(vals)
                entry["ratios"][key] = {
                    "median": round(statistics.median(vals), 4),
                    "lo": round(lo, 4), "hi": round(hi, 4)}
        for cond, rows_list in absolutes.items():
            if not rows_list:
                continue
            entry["abs"][cond] = {}
            for key in ("wall_s", "net_wall_s", "wall_per_op_us", "net_wall_per_op_us"):
                vals = [m[key] for m in rows_list if key in m]
                if vals:
                    entry["abs"][cond][key] = round(statistics.median(vals), 6)
        report[shape] = entry
    return report


def reduce_probe(rows):
    out = {}
    for r in rows:
        if r.get("kind") != "counters":
            continue
        shape = r.get("shape") or f"{r.get('scenario')}_{r.get('ws_name')}"
        e = out.setdefault(shape, {"runs": 0, "op_cqe": [], "misses": 0,
                                   "attempts": 0, "installs": 0, "avg_live": []})
        e["runs"] += 1
        sites = r.get("sites", {})
        op = sites.get("op_cqe")
        if op:
            e["op_cqe"].append(op["avg_probes"])
            e["avg_live"].append(op["avg_live"])
            e["misses"] += op["misses"]
            e["attempts"] += op["attempts"]
        e["installs"] += r.get("router", {}).get("installs", 0)
        for name in ("control_cqe", "mark_consumed_control", "cancel_by_handle",
                     "poison_class_a"):
            site = sites.get(name)
            if site and site["attempts"] > 0:
                e.setdefault(name, []).append((site["attempts"], site["avg_probes"],
                                               site["misses"]))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("jsonl")
    ap.add_argument("--json-out")
    args = ap.parse_args()
    rows = load_rows(args.jsonl)
    report = {"pairs": reduce_pairs(rows, "pairs"),
              "flush": reduce_pairs(rows, "flush"),
              "external": reduce_pairs(rows, "external"),
              "probe": reduce_probe(rows)}
    for stage in ("pairs", "flush", "external"):
        print(f"\n== {stage} ==")
        print(f"{'shape':32s} {'n':>3s} {'metric':18s} {'median':>8s} {'lo':>8s} {'hi':>8s}")
        for shape, e in report[stage].items():
            shown = 0
            for key, v in e["ratios"].items():
                print(f"{shape:32s} {e['n_pairs']:3d} {key:18s} {v['median']:8.4f} "
                      f"{v['lo']:8.4f} {v['hi']:8.4f}")
                shown += 1
                if shown >= 3 and len(e["ratios"]) > 4:
                    break
    print("\n== probe (instrumented, counts only) ==")
    print(f"{'shape':34s} {'runs':>4s} {'avg probes/op':>13s} {'misses':>7s} "
          f"{'lookups/install':>15s} {'avg live':>8s}")
    for shape, e in sorted(report["probe"].items()):
        ap_ = statistics.mean(e["op_cqe"]) if e["op_cqe"] else 0
        live = statistics.mean(e["avg_live"]) if e["avg_live"] else 0
        ratio = e["attempts"] / e["installs"] if e["installs"] else 0
        print(f"{shape:34s} {e['runs']:4d} {ap_:13.2f} {e['misses']:7d} "
              f"{ratio:15.2f} {live:8.1f}")
    if args.json_out:
        with open(args.json_out, "w") as fh:
            json.dump(report, fh, indent=1)
        print(f"\nwrote {args.json_out}")


if __name__ == "__main__":
    main()
