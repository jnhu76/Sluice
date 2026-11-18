#!/usr/bin/env python3
"""Statistics for the e5 M-28 ablation (protocol §5).

Per-pair paired differences -> sign test (direction counts) + bootstrap CI
(percentile, 10000 resamples) + Holm correction across the family
{cycles/op, rusage CPU/op, ops/s, p99}. Only cycles/op decides.
"""
import json
import math
import random
import sys

SESSION = "/home/jnhu/sluice-e5-elim/results/e5m28-s1-20261118"


def load_observations():
    obs = []
    for line in open(SESSION + "/observations.jsonl"):
        line = line.strip()
        if line:
            o = json.loads(line)
            if o["tag"].startswith("smoke-"):
                continue
            if not o.get("warm_ok") or not o.get("bench_ok"):
                print(f"  [excluded invalid obs] {o['tag']} warm_ok={o.get('warm_ok')} bench_ok={o.get('bench_ok')}")
                continue
            obs.append(o)
    return obs


def pairs_from_tags(obs, cell, tag_prefix):
    """pairs: list of {'i', 'sides': [(pos, arm, {pass: obs}), ...]} sorted by pos."""
    bypair = {}
    for o in obs:
        if o["cell"] != cell or not o["tag"].startswith(tag_prefix):
            continue
        # tag: <cell>-pNNN-<pos><k>-<arm>
        _, pi, pk, arm = o["tag"].split("-")
        pos, k = int(pk[0]), int(pk[1])
        bypair.setdefault(int(pi[1:]), []).append((pos, k, arm, o))
    pairs = []
    for i in sorted(bypair):
        entries = sorted(bypair[i], key=lambda e: (e[0], e[1]))
        sides = []
        cur = None
        for pos, k, arm, o in entries:
            if cur is None or cur[0] != pos:
                if cur is not None:
                    sides.append(cur)
                cur = (pos, arm, {})
            cur[2][o["pass"]] = o
        if cur is not None:
            sides.append(cur)
        pairs.append({"i": i, "sides": sides})
    return pairs


def get(pairs, pass_type="P2"):
    """yield (pair_i, obs_side1, obs_side2) with sides at the given pass."""
    for p in pairs:
        s1 = [s for s in p["sides"] if pass_type in s[2]]
        if len(s1) == 2:
            yield p["i"], s1[0], s1[1]


def diffs_ab(pairs, fn, pass_type="P2"):
    out = []
    for i, s1, s2 in get(pairs, pass_type):
        a = s1 if s1[1] == "A" else s2
        b = s2 if s1[1] == "A" else s1
        assert (s1[1], s2[1]) in (("A", "B"), ("B", "A"))
        out.append(fn(b[2][pass_type]) - fn(a[2][pass_type]))
    return out


def diffs_aa(pairs, fn, pass_type="P2"):
    out = []
    for i, s1, s2 in get(pairs, pass_type):
        assert s1[1] == s2[1] == "A"
        out.append(fn(s2[2][pass_type]) - fn(s1[2][pass_type]))
    return out


def bootstrap_ci(xs, n=10000, alpha=0.05, seed=20261118):
    rng = random.Random(seed)
    m = len(xs)
    means = []
    for _ in range(n):
        s = 0.0
        for _ in range(m):
            s += xs[rng.randrange(m)]
        means.append(s / m)
    means.sort()
    return means[int(alpha / 2 * n)], means[int((1 - alpha / 2) * n)]


def sign_test(xs):
    pos = sum(1 for x in xs if x > 0)
    neg = sum(1 for x in xs if x < 0)
    n = pos + neg
    if n == 0:
        return pos, neg, 1.0
    k = max(pos, neg)

    def binom_sf(k, n):
        return sum(math.comb(n, i) * 0.5 ** n for i in range(k, n + 1))

    return pos, neg, min(1.0, 2.0 * binom_sf(k, n))


def mean(xs):
    return sum(xs) / len(xs)


def std(xs):
    m = mean(xs)
    if len(xs) < 2:
        return 0.0
    return math.sqrt(sum((x - m) ** 2 for x in xs) / (len(xs) - 1))


def holm(pvals):
    order = sorted(range(len(pvals)), key=lambda i: pvals[i])
    adj = [0.0] * len(pvals)
    cur = 0.0
    m = len(pvals)
    for rank, idx in enumerate(order):
        cur = max(cur, min(1.0, (m - rank) * pvals[idx]))
        adj[idx] = cur
    return adj


def summarize(name, ds, level=None):
    m = mean(ds)
    s = std(ds)
    lo, hi = bootstrap_ci(ds)
    hw = (hi - lo) / 2.0
    pos, neg, p = sign_test(ds)
    rel = ""
    if level:
        rel = f" halfwidth_pp={hw/level*100:.3f}pp_of_level(level={level:.1f})"
    print(f"  {name}: n={len(ds)} mean={m:.6g} std={s:.4g} CI=[{lo:.6g},{hi:.6g}] "
          f"halfwidth={hw:.4g}{rel} sign(+/-)={pos}/{neg} p={p:.4g}")
    return dict(n=len(ds), mean=m, std=s, lo=lo, hi=hi, hw=hw, pos=pos, neg=neg, p=p,
                rel_pp=(hw / level * 100 if level else None))


def cyc_per_op(o):
    return o["cyc_per_op"]


def cyc_u_per_op(o):
    return o.get("cyc_u_per_op") or 0.0


def instr_per_op(o):
    return o.get("instr_per_op") or 0.0


def cpu_op_us(o):
    return o["cpu_op_us_median"]


def ops_s(o):
    return o["ops_s_median"]


def p99_ns(o):
    return o["p99_ns_median"]


if __name__ == "__main__":
    mode = sys.argv[1]
    obs = load_observations()
    if mode == "aa":
        pairs = pairs_from_tags(obs, "main", "main-p")
        lvl = mean([cyc_per_op(s[2]["P2"]) for p in pairs for s in p["sides"] if "P2" in s[2]])
        summarize("A/A cycles/op diff", diffs_aa(pairs, cyc_per_op), level=lvl)
        summarize("A/A P1 instr/op diff", diffs_aa(pairs, instr_per_op, "P1"))
    elif mode == "ab":
        pairs = pairs_from_tags(obs, "main", "abm-p")
        lvl = mean([cyc_per_op(s[2]["P2"]) for p in pairs for s in p["sides"]
                    if s[1] == "A" and "P2" in s[2]])
        summarize("A/B cycles/op (B-A, P2)", diffs_ab(pairs, cyc_per_op), level=lvl)
        summarize("A/B P1 instr/op (B-A)", diffs_ab(pairs, instr_per_op, "P1"))
        summarize("A/B rusage CPU/op us (B-A)", diffs_ab(pairs, cpu_op_us))
        ds_rel_ops = []
        ds_rel_p99 = []
        for i, s1, s2 in get(pairs):
            a = s1 if s1[1] == "A" else s2
            b = s2 if s1[1] == "A" else s1
            ds_rel_ops.append((ops_s(b[2]["P2"]) - ops_s(a[2]["P2"])) / ops_s(a[2]["P2"]) * 100)
            ds_rel_p99.append((p99_ns(b[2]["P2"]) - p99_ns(a[2]["P2"])) / p99_ns(a[2]["P2"]) * 100)
        summarize("A/B ops/s rel% (B-A)", ds_rel_ops)
        summarize("A/B p99 rel% (B-A)", ds_rel_p99)
        fam = []
        fam.append(("cycles/op", sign_test(diffs_ab(pairs, cyc_per_op))[2]))
        fam.append(("CPU/op", sign_test(diffs_ab(pairs, cpu_op_us))[2]))
        fam.append(("ops/s", sign_test(ds_rel_ops)[2]))
        fam.append(("p99", sign_test(ds_rel_p99)[2]))
        adj = holm([p for _, p in fam])
        for (name, p), a in zip(fam, adj):
            print(f"  Holm: {name} raw_p={p:.4g} adj_p={a:.4g}")
        # p99 protection bound: bootstrap upper bound of ratio p99(B)/p99(A)
        ratios = []
        for i, s1, s2 in get(pairs):
            a = s1 if s1[1] == "A" else s2
            b = s2 if s1[1] == "A" else s1
            ratios.append(p99_ns(b[2]["P2"]) / p99_ns(a[2]["P2"]))
        lo, hi = bootstrap_ci(ratios)
        print(f"  p99(B)/p99(A) bootstrap upper bound = {hi:.4f} (SLO <= 1.05)")
    elif mode == "aa-uring":
        pairs = pairs_from_tags(obs, "uring", "au-p")
        lvl = mean([cyc_per_op(s[2]["P2"]) for p in pairs for s in p["sides"] if "P2" in s[2]])
        r = summarize("uring A/A cycles/op diff", diffs_aa(pairs, cyc_per_op), level=lvl)
        print(f"  sigma_uring (std of A/A diffs) = {r['std']:.4g} cyc/op")
    elif mode == "ab-uring":
        pairs = pairs_from_tags(obs, "uring", "abu-p")
        lvl = mean([cyc_per_op(s[2]["P2"]) for p in pairs for s in p["sides"]
                    if s[1] == "A" and "P2" in s[2]])
        summarize("uring A/B cycles/op (B-A, P2)", diffs_ab(pairs, cyc_per_op), level=lvl)
    elif mode == "64k":
        pairs = pairs_from_tags(obs, "64k", "abk-p")
        lvl = mean([cyc_per_op(s[2]["P2"]) for p in pairs for s in p["sides"]
                    if s[1] == "A" and "P2" in s[2]])
        summarize("64k rusage CPU/op us (B-A)", diffs_ab(pairs, cpu_op_us))
        summarize("64k cycles/op (B-A, P2)", diffs_ab(pairs, cyc_per_op), level=lvl)
        rel = []
        for i, s1, s2 in get(pairs):
            a = s1 if s1[1] == "A" else s2
            b = s2 if s1[1] == "A" else s1
            rel.append((cpu_op_us(b[2]["P2"]) - cpu_op_us(a[2]["P2"])) / cpu_op_us(a[2]["P2"]) * 100)
        summarize("64k CPU/op rel% (B-A)", rel)
