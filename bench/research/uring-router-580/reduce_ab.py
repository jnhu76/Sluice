#!/usr/bin/env python3
"""#580 R6 reducer: paired ratios, median + percentile-bootstrap 95% interval.

n=15 paired ratios per shape; the interval is a small-sample bootstrap
percentile interval over the 15 pairs (resampled with replacement, B=10000).
It quantifies the spread of THESE pairs; it is not a confidence statement
about any population beyond this host/session. Oracle-failing runs are counted
and excluded from ratios (recorded, never silently dropped).
"""
import json
import sys
import random
from collections import defaultdict

# Usage: reduce_ab.py [ab_jsonl aa_jsonl]  (defaults are the WSL campaign
# files; the E5 campaign passes results/e5/ab-pairs-e5.jsonl aa-pairs-e5.jsonl)


def load(path):
    rows = []
    for line in open(path):
        line = line.strip()
        if not line:
            continue
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError:
            rows.append({"run_tag": line, "ok": False, "parse": "failed"})
    return rows


def pairs(rows):
    by_shape = defaultdict(dict)
    failures = 0
    for r in rows:
        tag = r.get("run_tag", "")
        if not r.get("ok"):
            failures += 1
            continue
        shape, rep, side = tag.rsplit("-", 2)[0], tag.rsplit("-", 2)[1], tag.rsplit("-", 2)[2]
        by_shape[shape][f"{rep}:{side}"] = r
    out = {}
    for shape, runs in by_shape.items():
        ps = []
        reps = sorted({k.split(":")[0] for k in runs})
        for rep in reps:
            a = runs.get(f"{rep}:a")
            b = runs.get(f"{rep}:b")
            if a is None or b is None:
                continue
            ps.append((a, b))
        out[shape] = ps
    return out, failures


def ratios(ps, metric):
    rs = []
    for a, b in ps:
        # ratio = b/a; 'a' and 'b' alternate arm order across reps by design
        va, vb = a[metric], b[metric]
        if va <= 0 or vb <= 0:
            continue
        rs.append(vb / va)
    return rs


def median_ci(rs, B=10000, seed=7):
    if not rs:
        return float("nan"), float("nan"), float("nan"), 0
    rng = random.Random(seed)
    n = len(rs)
    s = sorted(rs)
    med = s[n // 2] if n % 2 else 0.5 * (s[n // 2 - 1] + s[n // 2])
    meds = []
    for _ in range(B):
        sample = [rs[rng.randrange(n)] for _ in range(n)]
        sample.sort()
        meds.append(sample[n // 2] if n % 2 else 0.5 * (sample[n // 2 - 1] + sample[n // 2]))
    meds.sort()
    return med, meds[int(0.025 * B)], meds[int(0.975 * B)], n


def which_arm(r):
    # A/A rows carry arm=baseline for both binaries; the path-encoded binary
    # name is the identity that distinguishes them.
    if "binary" in r and "aa_" in str(r["binary"]):
        return r["binary"]
    return r.get("arm", r.get("binary", "?"))


def main():
    args = sys.argv[1:]
    ab_path = args[0] if len(args) > 0 else         "docs/research/uring-router-580/results/ab-pairs.jsonl"
    aa_path = args[1] if len(args) > 1 else         "docs/research/uring-router-580/results/aa-pairs.jsonl"
    print(f"== A/B source: {ab_path} (ratio = we2 / baseline, arm order alternates per rep) ==")
    ab, ab_fail = pairs(load(ab_path))
    print(f"oracle failures: {ab_fail}")
    for shape in sorted(ab):
        ps = ab[shape]
        # normalize so ratio is always we2/baseline regardless of a/b order
        norm = []
        for a, b in ps:
            if which_arm(a) == "baseline":
                norm.append((a, b))
            else:
                norm.append((b, a))
        line = f"{shape:20} n={len(norm):2d}"
        for metric in ("user_s", "wall_s", "sys_s"):
            med, lo, hi, n = median_ci(ratios(norm, metric))
            line += f"  {metric}: {med:.4f} [{lo:.4f},{hi:.4f}]"
        print(line + "   (we2/baseline; <1 favors we2)")

    print(f"\n== A/A path-name bias control source: {aa_path} "
          "(ratio = beta / alpha, byte-identical binaries, run order alternates) ==")
    aa, aa_fail = pairs(load(aa_path))
    print(f"oracle failures: {aa_fail}")
    for shape in sorted(aa):
        ps = aa[shape]
        norm = []
        for a, b in ps:
            if "alpha" in which_arm(a):
                norm.append((a, b))
            else:
                norm.append((b, a))
        line = f"{shape:20} n={len(norm):2d}"
        for metric in ("user_s", "wall_s"):
            med, lo, hi, n = median_ci(ratios(norm, metric))
            line += f"  {metric}: {med:.4f} [{lo:.4f},{hi:.4f}]"
        print(line + "   (identity expected ≈1.0)")


if __name__ == "__main__":
    main()
