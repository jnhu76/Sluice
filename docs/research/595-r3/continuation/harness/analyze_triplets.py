#!/usr/bin/env python3
"""#595 R3 continuation triplet analysis (frozen §6.2(b)/§9.2 statistics).

Resampling unit: triplet.  Within triplet j the three paired differences are
computed together; bootstrap resamples triplet indices (10,000, seed 5950,
fresh random.Random per metric, rng.randrange loop copied from the frozen
harness analyze_pairs.py).  Orientation per §9.2: positive = arm X worse.

  d_cpu(X,Y)   = (cpu_X - cpu_Y) / cpu_Y
  d_tput(X,Y)  = (tput_Y - tput_X) / tput_Y   (positive = X slower)
  d_lat(X,Y,q) = (lat_X(q) - lat_Y(q)) / lat_Y(q)

Main gate B vs A0 (all required): mean(d_cpu) <= -10%,
upper_one_sided_95(d_cpu) <= -5%, mean&upper(d_tput) <= +3%,
mean&upper(d_p95/d_p99) <= +5%; power half_width <= gate width (§9.7);
primary/secondary CPU sign consistency (bench cpu vs wait4 rusage);
mechanism reconciliation inputs reported from bench counters.
B vs A1 and A1 vs A0 are attribution-only.
"""
import argparse
import json
import math
import os
import random
import sys

BOOT = 10000
SEED = 5950
GATE = {
    "cpu_mean": -0.10,
    "cpu_upper": -0.05,
    "cpu_power": 0.05,
    "tput": 0.03,
    "p95": 0.05,
    "p99": 0.05,
}
ARMS = ("A0", "A1", "B")


def load_records(path):
    recs = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            rec = json.loads(line)
            if rec.get("valid", True):
                recs.append(rec)
    return recs


def triplets_of(recs):
    by_t = {}
    for r in recs:
        by_t.setdefault(r["pair"], {})[r["member"]] = r
    out = []
    for t in sorted(by_t):
        d = by_t[t]
        if all(a in d for a in ARMS):
            out.append({a: d[a] for a in ARMS})
    return out


def rel(x, y, key):
    a = x["metrics"].get(key)
    b = y["metrics"].get(key)
    if a in (None, 0) or b is None:
        return None
    return (a - b) / b


def rel_tput(x, y):
    a = x["metrics"].get("ops_per_sec")
    b = y["metrics"].get("ops_per_sec")
    if not a or not b:
        return None
    return (b - a) / b


def rel_ru(x, y):
    if x["ru_total"] <= 0 or y["ru_total"] <= 0:
        return None
    return (x["ru_total"] - y["ru_total"]) / y["ru_total"]


def bootstrap_ci(values):
    n = len(values)
    if n == 0:
        return None
    rng = random.Random(SEED)
    means = []
    for _ in range(BOOT):
        s = 0.0
        for _ in range(n):
            s += values[rng.randrange(n)]
        means.append(s / n)
    means.sort()
    sv = sorted(values)
    med = sv[n // 2] if n % 2 else (sv[n // 2 - 1] + sv[n // 2]) / 2
    lo = means[int(0.025 * BOOT)]
    hi = means[min(int(0.975 * BOOT), BOOT - 1)]
    upper_one_sided = means[min(int(0.95 * BOOT), BOOT - 1)]
    return {
        "n": n,
        "mean": sum(values) / n,
        "median": med,
        "ci_lo": lo,
        "ci_hi": hi,
        "ci_upper_one_sided_95": upper_one_sided,
        "half_width": (hi - lo) / 2,
        "values": values,
        "boot_means": means,
    }


def public(stat):
    if not stat:
        return None
    return {k: ([round(v, 6) if isinstance(v, float) else v for v in val]
                if k == "values" else v)
            for k, val in stat.items() if k != "boot_means"}


def metric_series(triplets, x_arm, y_arm):
    series = {}
    for name, fn in (("cpu", lambda x, y: rel(x, y, "cpu_ns_per_op")),
                     ("p50", lambda x, y: rel(x, y, "p50_ns")),
                     ("p95", lambda x, y: rel(x, y, "p95_ns")),
                     ("p99", lambda x, y: rel(x, y, "p99_ns")),
                     ("tput", rel_tput),
                     ("ru_secondary", rel_ru),
                     ("rss", lambda x, y: rel(x, y, "ru_maxrss_kb"))):
        vals = []
        for t in triplets:
            v = fn(t[x_arm], t[y_arm])
            if v is not None and math.isfinite(v):
                vals.append(v)
        series[name] = vals
    return series


def counters_of(rec):
    keys = ("r3_entry_calls", "r3_group_calls", "r3_single_calls",
            "r3_accepted", "r3_prefix_max", "r3_suffix_single_calls",
            "r3_flush_attempts", "r3_untouched")
    reps = rec.get("reps") or []
    if not reps:
        return {k: rec.get(k) for k in keys}
    return {k: sum(int(r.get(k, 0)) for r in reps) for k in keys}


def evaluate(triplets):
    result = {"n_triplets": len(triplets), "comparisons": {}}
    for x_arm, y_arm in (("B", "A0"), ("B", "A1"), ("A1", "A0")):
        series = metric_series(triplets, x_arm, y_arm)
        comp = {"orientation": f"{x_arm} vs {y_arm}; positive = {x_arm} worse",
                "metrics": {}}
        for name, vals in series.items():
            if vals:
                comp["metrics"][name] = bootstrap_ci(vals)
        result["comparisons"][f"{x_arm}-{y_arm}"] = comp

    main = result["comparisons"]["B-A0"]["metrics"]
    entries = {}
    m = main.get("cpu")
    entries["cpu"] = {
        "mean_pct": m["mean"] * 100 if m else None,
        "upper_one_sided_95_pct": m["ci_upper_one_sided_95"] * 100 if m else None,
        "half_width_pct": m["half_width"] * 100 if m else None,
        "mean_ok": bool(m and m["mean"] <= GATE["cpu_mean"]),
        "upper_ok": bool(m and m["ci_upper_one_sided_95"] <= GATE["cpu_upper"]),
        "power_ok": bool(m and m["half_width"] <= GATE["cpu_power"]),
    }
    for key, th in (("tput", GATE["tput"]), ("p95", GATE["p95"]), ("p99", GATE["p99"])):
        m = main.get(key)
        entries[key] = {
            "mean_pct": m["mean"] * 100 if m else None,
            "upper_one_sided_95_pct": m["ci_upper_one_sided_95"] * 100 if m else None,
            "half_width_pct": m["half_width"] * 100 if m else None,
            "threshold_pct": th * 100,
            "mean_ok": bool(m and m["mean"] <= th),
            "upper_ok": bool(m and m["ci_upper_one_sided_95"] <= th),
            "power_ok": bool(m and m["half_width"] <= th),
        }
    ru = main.get("ru_secondary")
    cpu = main.get("cpu")
    sign_consistent = bool(
        ru and cpu and (ru["mean"] <= 0) == (cpu["mean"] <= 0)
        and ru["mean"] != 0 and cpu["mean"] != 0)
    entries["cpu_secondary_sign"] = {
        "primary_mean_pct": cpu["mean"] * 100 if cpu else None,
        "secondary_mean_pct": ru["mean"] * 100 if ru else None,
        "consistent": sign_consistent,
    }
    result["main_gate_B_A0"] = entries
    stats_ok = all(
        entries[k]["mean_ok"] and entries[k]["upper_ok"] and entries[k]["power_ok"]
        for k in ("cpu", "tput", "p95", "p99")
    )
    result["main_gate_stats"] = stats_ok
    result["main_gate_sign_consistency"] = sign_consistent
    result["mechanism_counters"] = {
        arm: {
            "sum_over_invocations": counters_of(triplets[0][arm]) if triplets else None,
            "note": "first-triplet per-invocation counters (summed over reps); "
                    "full per-triplet table in results.jsonl",
        }
        for arm in ARMS
    }
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--phase-dir", required=True)
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    results = os.path.join(args.phase_dir, "results.jsonl")
    recs = load_records(results)
    triplets = triplets_of(recs)
    out = {
        "phase_dir": args.phase_dir,
        "label": os.path.basename(args.phase_dir.rstrip("/")),
        "records_total": sum(1 for _ in open(results)) if os.path.exists(results) else 0,
        "records_valid": len(recs),
        "bootstrap": {"n": BOOT, "seed": SEED, "unit": "triplet",
                      "method": "percentile, fresh Random(5950) per metric, "
                                "rng.randrange loop (frozen harness parity)"},
    }
    out.update(evaluate(triplets))

    def clean(obj):
        if isinstance(obj, dict):
            return {k: clean(v) for k, v in obj.items() if k not in ("boot_means", "values")}
        return obj

    text = json.dumps(clean(out), indent=1, sort_keys=True)
    if args.out:
        with open(args.out, "w") as f:
            f.write(text + "\n")
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
