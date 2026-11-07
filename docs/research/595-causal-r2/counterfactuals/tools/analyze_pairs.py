#!/usr/bin/env python3
"""CF-2 deferoff pair analysis: within-pair (B-A)/A contrasts, bootstrap CIs
(10,000 resamples, seed 5950, percentile), phaseA/CF-1 conventions.

Contrast orientation cpu/p50/p95/p99/wall: (B-A)/A, positive = B worse.
tput: (A-B)/A, positive = B slower. Also merges arbitrary block dirs.
"""
import argparse
import json
import os
import random

BOOT = 10000
SEED = 5950


def load_records(path):
    recs = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                recs.append(json.loads(line))
    return recs


def pairs_of(recs):
    by = {}
    for r in recs:
        by.setdefault(r["pair"], {})[r["arm"]] = r
    out = []
    for p in sorted(by):
        d = by[p]
        if "A" in d and "B" in d:
            out.append((d["A"], d["B"]))
    return out


def rel(a, b, key):
    x, y = a["metrics"][key], b["metrics"][key]
    if not x or not y:
        return None
    return (y - x) / x


def rel_tput(a, b):
    x, y = a["metrics"]["ops_per_sec"], b["metrics"]["ops_per_sec"]
    if not x or not y:
        return None
    return (x - y) / x


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
    lo = means[int(0.025 * BOOT)]
    hi = means[min(int(0.975 * BOOT), BOOT - 1)]
    return {"n": n, "mean": sum(values) / n, "ci_lo": lo, "ci_hi": hi,
            "half_width": (hi - lo) / 2}


def summarize(recs):
    prs = pairs_of(recs)
    out = {"n_pairs": len(prs), "metrics": {}, "arm_abs": {}}
    for arm in "AB":
        xs = [p[0 if p[0]["arm"] == arm else 1]["metrics"] for p in prs]
        vals = {}
        for key in ("cpu_ns_per_op", "ops_per_sec", "p50_ns", "p95_ns",
                    "p99_ns"):
            v = [m[key] for m in xs if m.get(key)]
            if v:
                vals[key] = sum(v) / len(v)
        out["arm_abs"][arm] = vals
    metric_map = {
        "cpu": lambda a, b: rel(a, b, "cpu_ns_per_op"),
        "tput": rel_tput,
        "p50": lambda a, b: rel(a, b, "p50_ns"),
        "p95": lambda a, b: rel(a, b, "p95_ns"),
        "p99": lambda a, b: rel(a, b, "p99_ns"),
    }
    for name, fn in metric_map.items():
        v = [fn(a, b) for a, b in prs]
        v = [e for e in v if e is not None]
        if v:
            out["metrics"][name] = bootstrap_ci(v)
    # sanity: exit codes, workload equivalence
    out["all_exit0"] = all(r.get("exit") == 0 for r in recs)
    out["invocations"] = len(recs)
    ops = {r["arm"]: set() for r in recs}
    for r in recs:
        ops[r["arm"]].add(r.get("measured_records"))
    out["measured_records_by_arm"] = {k: sorted(v) for k, v in ops.items()}
    return out


def fmt_pct(s):
    if s is None:
        return "n/a"
    return (f"mean={s['mean']*100:+7.2f}% CI95=[{s['ci_lo']*100:+7.2f}%,"
            f"{s['ci_hi']*100:+7.2f}%] hw={s['half_width']*100:5.2f}pp n={s['n']}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--phase-dirs", nargs="+", required=True)
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    merged = []
    per_block = {}
    for i, d in enumerate(args.phase_dirs):
        recs = load_records(os.path.join(d, "results.jsonl"))
        per_block[os.path.basename(d)] = summarize(recs)
        for r in recs:
            r = dict(r)
            r["pair"] = i * 1000 + r["pair"]
            merged.append(r)
    combined = summarize(merged)

    for name, s in per_block.items():
        print(f"== {name}: {s['n_pairs']} pairs, all_exit0={s['all_exit0']}, "
              f"records={s['measured_records_by_arm']} ==")
        for arm in "AB":
            a = s["arm_abs"][arm]
            print(f"   arm {arm}: cpu/op={a.get('cpu_ns_per_op',0):.1f}ns "
                  f"tput={a.get('ops_per_sec',0):.0f}/s p50={a.get('p50_ns',0):.0f} "
                  f"p95={a.get('p95_ns',0):.0f} p99={a.get('p99_ns',0):.0f} ns")
        for k in ("cpu", "tput", "p50", "p95", "p99"):
            if k in s["metrics"]:
                print(f"   B-A {k:>4}: {fmt_pct(s['metrics'][k])}")
    print(f"== COMBINED ({len(args.phase_dirs)} blocks, {combined['n_pairs']} pairs) ==")
    for k in ("cpu", "tput", "p50", "p95", "p99"):
        if k in combined["metrics"]:
            print(f"   B-A {k:>4}: {fmt_pct(combined['metrics'][k])}")
    if args.out:
        with open(args.out, "w") as f:
            json.dump({"blocks": per_block, "combined": combined}, f, indent=1)
        print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
