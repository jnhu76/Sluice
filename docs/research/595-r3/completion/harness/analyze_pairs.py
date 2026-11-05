#!/usr/bin/env python3
"""#595 calibration analysis: pair-relative differences + bootstrap CI.

Two-arm blocks only (A/A and A/B).  Orientation matches the frozen R1/R2
convention (harness/analyze.py): positive means the second member / B arm is
worse.

  cpu/p50/p95/p99/ru: (m2 - m1) / m1
  tput:               (m1 - m2) / m1   (positive = second member slower)

Bootstrap: 10,000 percentile resamples over pairs, fixed seed 5950; the exact
loop (rng.randrange over the pair vector) is copied from
/home/jnhu/sluice-595-e5/harness/analyze.py so results are recomputable with
that script.  Half-width = (ci_hi - ci_lo) / 2.

A/A gate (prereg 8.2): cpu half-width <= 5 pp, tput <= 3 pp, p99 <= 3 pp.
A/B negative control (prereg 5.4a / 9.5): point estimate and one-sided 95%
upper bound <= threshold (cpu/p95/p99 5 pp, tput 3 pp) and Holm-adjusted
one-sided p < 0.05 over the block's metric family.
"""
import argparse
import json
import math
import os
import random
import sys

BOOT = 10000
SEED = 5950
AA_LIMITS = {"cpu": 0.05, "tput": 0.03, "p99": 0.03}
NC_LIMITS = {"cpu": 0.05, "p95": 0.05, "p99": 0.05, "tput": 0.03}


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


def pairs_of(recs, arms):
    by_pair = {}
    for r in recs:
        by_pair.setdefault(r["pair"], {})[r["pos"]] = r
    out = []
    for pair in sorted(by_pair):
        d = by_pair[pair]
        if 0 not in d or 1 not in d:
            continue
        if arms == "AB":
            a = next((r for r in (d[0], d[1]) if r["member"] == "A"), None)
            b = next((r for r in (d[0], d[1]) if r["member"] == "B"), None)
            if a is None or b is None:
                continue
        else:
            a = next((r for r in (d[0], d[1]) if r["member"] == "m1"), None)
            b = next((r for r in (d[0], d[1]) if r["member"] == "m2"), None)
            if a is None or b is None:
                continue
        out.append((a, b))
    return out


def rel(a, b, key):
    x = a["metrics"].get(key)
    y = b["metrics"].get(key)
    if x is None:
        x = a.get(key)
    if y is None:
        y = b.get(key)
    if x is None or y is None or x == 0:
        return None
    return (y - x) / x


def rel_tput(a, b):
    x = a["metrics"].get("ops_per_sec")
    y = b["metrics"].get("ops_per_sec")
    if not x or not y:
        return None
    return (x - y) / x


def rel_ru(a, b):
    if a["ru_total"] <= 0 or b["ru_total"] <= 0:
        return None
    return (b["ru_total"] - a["ru_total"]) / a["ru_total"]


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


def noninferiority_p(stat, threshold):
    ge = sum(1 for m in stat["boot_means"] if m >= threshold)
    return (ge + 1) / (len(stat["boot_means"]) + 1)


def summarize(recs, arms):
    pairs = pairs_of(recs, arms)
    raw = {
        "cpu": [rel(a, b, "cpu_ns_per_op") for a, b in pairs],
        "p50": [rel(a, b, "p50_ns") for a, b in pairs],
        "p95": [rel(a, b, "p95_ns") for a, b in pairs],
        "p99": [rel(a, b, "p99_ns") for a, b in pairs],
        "tput": [rel_tput(a, b) for a, b in pairs],
        "ru_secondary": [rel_ru(a, b) for a, b in pairs],
        "rss": [rel(a, b, "ru_maxrss_kb") for a, b in pairs],
    }
    out = {"n_pairs": len(pairs), "metrics": {}}
    for k, vals in raw.items():
        vals = [v for v in vals if v is not None and math.isfinite(v)]
        if vals:
            out["metrics"][k] = bootstrap_ci(vals)
    return out


def public(stat):
    return {k: v for k, v in stat.items() if k != "boot_means"} if stat else None


def holm(entries, alpha=0.05):
    m = len(entries)
    order = sorted(range(m), key=lambda i: entries[i]["p"])
    stop = False
    for rank, idx in enumerate(order):
        if stop:
            entries[idx]["holm_alpha"] = None
            entries[idx]["holm_reject"] = False
            continue
        adj = alpha / (m - rank)
        entries[idx]["holm_alpha"] = adj
        if entries[idx]["p"] <= adj:
            entries[idx]["holm_reject"] = True
        else:
            entries[idx]["holm_reject"] = False
            stop = True
    return entries


def evaluate(recs, arms):
    s = summarize(recs, arms)
    result = {"n_pairs": s["n_pairs"], "metrics": {k: public(v) for k, v in s["metrics"].items()}}
    if arms == "AA":
        gates = {}
        for k, th in AA_LIMITS.items():
            m = s["metrics"].get(k)
            gates[k] = {
                "threshold_pp": th * 100,
                "half_width_pp": (m["half_width"] * 100) if m else None,
                "pass": bool(m and m["half_width"] <= th),
            }
        result["aa_gate"] = gates
        result["aa_pass"] = all(g["pass"] for g in gates.values())
        result["verdict"] = "PASS" if result["aa_pass"] else "FAIL"
        return result
    entries = []
    for k, th in NC_LIMITS.items():
        m = s["metrics"].get(k)
        if m is None:
            entries.append({"metric": k, "threshold": th, "missing": True,
                            "p": 1.0, "mean": None, "ci_hi": None})
            continue
        entries.append({
            "metric": k,
            "threshold": th,
            "mean": m["mean"],
            "ci_lo": m["ci_lo"],
            "ci_hi": m["ci_hi"],
            "ci_upper_one_sided_95": m["ci_upper_one_sided_95"],
            "p": noninferiority_p(m, th),
        })
    holm(entries)
    all_ok = True
    for e in entries:
        if e.get("missing"):
            e["pass"] = False
        else:
            e["point_in_bound"] = e["mean"] <= e["threshold"]
            e["upper_in_bound"] = e["ci_upper_one_sided_95"] <= e["threshold"]
            e["pass"] = bool(e["point_in_bound"] and e["upper_in_bound"]
                             and e.get("holm_reject"))
        all_ok = all_ok and e["pass"]
    result["negative_control"] = entries
    result["nc_pass"] = all_ok
    result["verdict"] = "PASS" if all_ok else "FAIL"
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--phase-dir", required=True)
    ap.add_argument("--arms", default="AA", choices=["AA", "AB"])
    ap.add_argument("--out", default=None)
    ap.add_argument("--label", default=None)
    args = ap.parse_args()

    results = os.path.join(args.phase_dir, "results.jsonl")
    recs = load_records(results)
    out = {
        "phase_dir": args.phase_dir,
        "label": args.label or os.path.basename(args.phase_dir.rstrip("/")),
        "arms": args.arms,
        "records_total": sum(1 for _ in open(results)) if os.path.exists(results) else 0,
        "records_valid": len(recs),
    }
    out.update(evaluate(recs, args.arms))
    text = json.dumps(out, indent=1, sort_keys=True)
    if args.out:
        with open(args.out, "w") as f:
            f.write(text + "\n")
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
