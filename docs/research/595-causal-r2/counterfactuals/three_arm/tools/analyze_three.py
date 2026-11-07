#!/usr/bin/env python3
"""CF-0 three-arm analysis: within-trio contrasts, bootstrap CIs (10,000
resamples, seed 5950, percentile), phaseA conventions.

Contrast orientation for cpu/p50/p95/p99/wall: (X - Y)/Y, positive = X worse.
tput: (Y - X)/Y, positive = X slower. Pairs reported: C vs A, B vs A, B vs C.
AA mode: pos1 vs pos0 (both arm A), gates = phaseA AA limits
(cpu 5pp, tput 3pp, p99 3pp half-widths)."""
import argparse
import json
import os
import random
import sys

BOOT = 10000
SEED = 5950
AA_LIMITS = {"cpu": 0.05, "tput": 0.03, "p99": 0.03}
CONTRASTS = [("C", "A"), ("B", "A"), ("B", "C")]


def load_records(path):
    recs = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                recs.append(json.loads(line))
    return recs


def trios_of(recs):
    by = {}
    for r in recs:
        by.setdefault(r["trio"], {})[r["arm"] + str(r["pos"])] = r
    out = []
    for t in sorted(by):
        d = by[t]
        arms = {r["arm"] for r in d.values()}
        if arms == {"A", "B", "C"}:
            out.append({arm: next(r for r in d.values() if r["arm"] == arm)
                        for arm in "ABC"})
    return out


def pairs_aa(recs):
    by = {}
    for r in recs:
        by.setdefault(r["trio"], {})[r["pos"]] = r
    out = []
    for t in sorted(by):
        d = by[t]
        if 0 in d and 1 in d:
            out.append((d[0], d[1]))
    return out


def rel(x, y, key):
    a, b = x["metrics"][key], y["metrics"][key]
    if not a or not b:
        return None
    return (b - a) / a


def rel_tput(x, y):
    a, b = x["metrics"]["ops_per_sec"], y["metrics"]["ops_per_sec"]
    if not a or not b:
        return None
    return (a - b) / a


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


def summarize_trios(recs, metric_map):
    tris = trios_of(recs)
    out = {"n_trios": len(tris), "contrasts": {}, "arm_abs": {}}
    for arm in "ABC":
        vals = {}
        for key in ("cpu_ns_per_op", "ops_per_sec", "p50_ns", "p95_ns", "p99_ns"):
            xs = [t[arm]["metrics"][key] for t in tris
                  if t[arm]["metrics"].get(key)]
            if xs:
                vals[key] = sum(xs) / len(xs)
        out["arm_abs"][arm] = vals
    for x, y in CONTRASTS:
        cell = {}
        for name, fn in metric_map.items():
            v = [fn(t[x], t[y]) for t in tris]
            v = [e for e in v if e is not None]
            if v:
                cell[name] = bootstrap_ci(v)
        out["contrasts"][f"{x}_vs_{y}"] = cell
    return out


def summarize_aa(recs):
    prs = pairs_aa(recs)
    out = {"n_pairs": len(prs), "metrics": {}}
    metric_map = {
        "cpu": lambda a, b: rel(a, b, "cpu_ns_per_op"),
        "tput": rel_tput,
        "p95": lambda a, b: rel(a, b, "p95_ns"),
        "p99": lambda a, b: rel(a, b, "p99_ns"),
    }
    for name, fn in metric_map.items():
        v = [fn(a, b) for a, b in prs]
        v = [e for e in v if e is not None]
        if v:
            out["metrics"][name] = bootstrap_ci(v)
    return out


def fmt_pct(s):
    if s is None:
        return "n/a"
    return (f"mean={s['mean']*100:+7.2f}% CI95=[{s['ci_lo']*100:+7.2f}%,"
            f"{s['ci_hi']*100:+7.2f}%] hw={s['half_width']*100:5.2f}pp n={s['n']}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--phase-dir", required=True)
    ap.add_argument("--mode", default="trio", choices=["trio", "aa"])
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    recs = load_records(os.path.join(args.phase_dir, "results.jsonl"))
    if args.mode == "aa":
        s = summarize_aa(recs)
        print(f"== AA {args.phase_dir}: {s['n_pairs']} pairs ==")
        ok = True
        for k, th in AA_LIMITS.items():
            m = s["metrics"].get(k)
            good = m is not None and m["half_width"] <= th
            ok = ok and good
            print(f"  {k:>4}: {fmt_pct(m)}  gate hw<={th*100:.0f}pp -> "
                  f"{'PASS' if good else 'FAIL'}")
        print(f"  AA OVERALL: {'PASS' if ok else 'FAIL'}")
        if args.out:
            with open(args.out, "w") as f:
                json.dump({"phase_dir": args.phase_dir, "aa": s,
                           "aa_pass": ok}, f, indent=1)
        return

    metric_map = {
        "cpu": lambda x, y: rel(y, x, "cpu_ns_per_op"),
        "tput": lambda x, y: rel_tput(y, x),
        "p50": lambda x, y: rel(y, x, "p50_ns"),
        "p95": lambda x, y: rel(y, x, "p95_ns"),
        "p99": lambda x, y: rel(y, x, "p99_ns"),
    }
    s = summarize_trios(recs, metric_map)
    print(f"== TRIO {args.phase_dir}: {s['n_trios']} trios ==")
    for arm in "ABC":
        a = s["arm_abs"][arm]
        print(f"  arm {arm}: cpu/op={a.get('cpu_ns_per_op',0):.1f}ns "
              f"tput={a.get('ops_per_sec',0):.0f}/s p50={a.get('p50_ns',0):.0f} "
              f"p95={a.get('p95_ns',0):.0f} p99={a.get('p99_ns',0):.0f} ns")
    for cname, cell in s["contrasts"].items():
        print(f"  -- {cname} (positive = first letter worse; tput positive = first slower)")
        for k in ("cpu", "tput", "p50", "p95", "p99"):
            if k in cell:
                print(f"     {k:>4}: {fmt_pct(cell[k])}")
    if args.out:
        with open(args.out, "w") as f:
            json.dump({"phase_dir": args.phase_dir, "trio": s}, f, indent=1)
        print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
