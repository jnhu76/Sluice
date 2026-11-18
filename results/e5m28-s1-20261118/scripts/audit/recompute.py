#!/usr/bin/env python3
"""Independent audit recomputation for e5m28-s1-20261118 (own implementation, fixed)."""
import json, math, random
S = "/home/jnhu/sluice-e5-elim/results/e5m28-s1-20261118"

obs = []
for line in open(S + "/observations.jsonl"):
    o = json.loads(line)
    if o["tag"].startswith("smoke-"):
        continue
    if not o.get("warm_ok") or not o.get("bench_ok"):
        continue
    obs.append(o)

def pairs_for(prefix):
    byp = {}
    for o in obs:
        if not o["tag"].startswith(prefix):
            continue
        rest = o["tag"][len(prefix):]           # NNN-<pos><k>-<arm>
        pi, pk, arm = rest.split("-")
        byp.setdefault(int(pi), []).append((int(pk[0]), int(pk[1]), arm, o))
    out = {}
    for i, entries in byp.items():
        entries.sort(key=lambda e: (e[0], e[1]))   # stable: obs order preserved
        sides = []  # [pos, arm, {pass: obs}] last obs wins per (pos,pass)
        for pos, k, arm, o in entries:
            if not sides or sides[-1][0] != pos:
                sides.append([pos, arm, {}])
            sides[-1][2][o["pass"]] = o
        out[i] = sides
    return out

def boot_ci(xs, n=10000, seed=20261118):
    rng = random.Random(seed); m = len(xs); means = []
    for _ in range(n):
        s = 0.0
        for _ in range(m): s += xs[rng.randrange(m)]
        means.append(s / m)
    means.sort()
    return means[int(0.025 * n)], means[int(0.975 * n)]

def sign_p(xs):
    pos = sum(1 for x in xs if x > 0); neg = sum(1 for x in xs if x < 0)
    n = pos + neg
    if n == 0: return pos, neg, 1.0
    k = max(pos, neg)
    sf = sum(math.comb(n, i) * 0.5**n for i in range(k, n + 1))
    return pos, neg, min(1.0, 2 * sf)

def mstd(xs):
    m = sum(xs) / len(xs)
    v = sum((x - m)**2 for x in xs) / (len(xs) - 1) if len(xs) > 1 else 0.0
    return m, math.sqrt(v)

def line(name, xs, level=None):
    m, s = mstd(xs); lo, hi = boot_ci(xs); hw = (hi - lo) / 2
    pos, neg, p = sign_p(xs)
    rel = f"  hw_pp={hw/level*100:.4f}pp(level={level:.1f})" if level else ""
    print(f"{name}: n={len(xs)} mean={m:.6g} std={s:.4g} CI=[{lo:.6g},{hi:.6g}] hw={hw:.4g}{rel} sign={pos}/{neg} p={p:.5g}")
    return dict(n=len(xs), m=m, s=s, lo=lo, hi=hi, hw=hw, p=p)

def two_sides(P, pt):
    for i in sorted(P):
        sides = [s for s in P[i] if pt in s[2]]
        if len(sides) == 2:
            yield i, sides[0], sides[1]

def aa_diffs(P, fn, pt):
    return [fn(s2[2][pt]) - fn(s1[2][pt]) for i, s1, s2 in two_sides(P, pt)]

def ab_A_B(P, pt):
    for i, s1, s2 in two_sides(P, pt):
        a, b = (s1, s2) if s1[1] == "A" else (s2, s1)
        yield i, a, b

def ab_diffs(P, fn, pt):
    return [fn(b[2][pt]) - fn(a[2][pt]) for i, a, b in ab_A_B(P, pt)]

cyc = lambda o: o["cyc_per_op"]
instr = lambda o: o["instr_per_op"]
cpu = lambda o: o["cpu_op_us_median"]
ops = lambda o: o["ops_s_median"]
p99 = lambda o: o["p99_ns_median"]
ctx = lambda o: o.get("perf_context-switches", 0.0) / o["ops_denominator"]

print("== A/A main (P2 cyc/op) blocks:")
aa = pairs_for("main-p")
allp2 = [cyc(s[2]["P2"]) for i in aa for s in aa[i] if "P2" in s[2]]
lvl_aa = sum(allp2) / len(allp2)
ds_aa = aa_diffs(aa, cyc, "P2")
for cut, label in [(24,"24"),(48,"48"),(71,"72blk"),(94,"96blk"),(len(ds_aa),"120blk")]:
    m, s = mstd(ds_aa[:cut]); lo, hi = boot_ci(ds_aa[:cut]); hw=(hi-lo)/2
    pos, neg, _ = sign_p(ds_aa[:cut])
    print(f"  {label}: n={cut} mean={m:.4g} std={s:.4g} hw={hw:.4g} ({hw/lvl_aa*100:.4f}pp) sign={pos}/{neg}")
m, s = mstd(ds_aa); lo, hi = boot_ci(ds_aa)
print(f"  A/A final: mean={m:.4g} std={s:.4g} CI=[{lo:.4g},{hi:.4g}] hw={(hi-lo)/2:.4g} sign={sign_p(ds_aa)[0]}/{sign_p(ds_aa)[1]}")
ds_aap1 = aa_diffs(aa, instr, "P1")
m, s = mstd(ds_aap1); lo, hi = boot_ci(ds_aap1)
print(f"  A/A P1 instr/op: n={len(ds_aap1)} mean={m:.6g} std={s:.4g} CI=[{lo:.6g},{hi:.6g}]")

print("== A/B main (abm):")
ab = pairs_for("abm-p")
aobs = [s[2]["P2"] for i in ab for s in ab[i] if "P2" in s[2] and s[1] == "A"]
lvl_abA = sum(cyc(o) for o in aobs) / len(aobs)
d_cyc = ab_diffs(ab, cyc, "P2"); d_instr = ab_diffs(ab, instr, "P1"); d_cpu = ab_diffs(ab, cpu, "P2")
d_ctx = ab_diffs(ab, ctx, "P2")
d_ops, d_p99 = [], []
for i, a, b in ab_A_B(ab, "P2"):
    d_ops.append((ops(b[2]["P2"]) - ops(a[2]["P2"])) / ops(a[2]["P2"]) * 100)
    d_p99.append((p99(b[2]["P2"]) - p99(a[2]["P2"])) / p99(a[2]["P2"]) * 100)
line("  cycles/op B-A", d_cyc, lvl_abA)
line("  P1 instr/op B-A", d_instr)
line("  rusage CPU/op us B-A", d_cpu)
line("  ops/s rel% B-A", d_ops)
line("  p99 rel% B-A", d_p99)
fam = [("cycles", sign_p(d_cyc)[2]), ("CPU", sign_p(d_cpu)[2]), ("ops/s", sign_p(d_ops)[2]), ("p99", sign_p(d_p99)[2])]
order = sorted(range(4), key=lambda i: fam[i][1]); cur = 0.0; adj = {}
for rank, idx in enumerate(order):
    cur = max(cur, min(1.0, (4 - rank) * fam[idx][1])); adj[fam[idx][0]] = cur
print("  Holm:", {k: round(v, 6) for k, v in adj.items()})
ratios = [p99(b[2]["P2"]) / p99(a[2]["P2"]) for i, a, b in ab_A_B(ab, "P2")]
lo, hi = boot_ci(ratios)
print(f"  p99 ratio boot upper = {hi:.4f} (SLO<=1.05)")
m, s = mstd(d_ctx); lo2, hi2 = boot_ci(d_ctx)
print(f"  ctx-sw/op B-A: mean={m:.6g} CI=[{lo2:.6g},{hi2:.6g}]")
dsaa_instr_p2 = aa_diffs(aa, instr, "P2")
m, s = mstd(dsaa_instr_p2); lo, hi = boot_ci(dsaa_instr_p2)
print(f"  A/A P2 instr/op noise band: mean={m:.4g} CI=[{lo:.4g},{hi:.4g}] n={len(dsaa_instr_p2)}")
dsaa_ctx = aa_diffs(aa, ctx, "P2")
m, s = mstd(dsaa_ctx); lo, hi = boot_ci(dsaa_ctx)
print(f"  A/A ctx-sw/op noise band: mean={m:.6g} CI=[{lo:.6g},{hi:.6g}] n={len(dsaa_ctx)}")

print("== uring A/A (au):")
au = pairs_for("au-p")
ds_u = aa_diffs(au, cyc, "P2")
m, s = mstd(ds_u); lo, hi = boot_ci(ds_u)
lvl_au = sum(cyc(s[2]["P2"]) for i in au for s in au[i] if "P2" in s[2]) / sum(1 for i in au for s in au[i] if "P2" in s[2])
print(f"  n={len(ds_u)} mean={m:.6g} std={s:.6g} CI=[{lo:.4g},{hi:.4g}] level={lvl_au:.1f} 1%={lvl_au/100:.2f} 2sigma={2*s:.2f}")
print("== uring A/B (abu):")
abu = pairs_for("abu-p")
ds_ub = ab_diffs(abu, cyc, "P2")
line("  uring A/B cyc/op B-A", ds_ub, lvl_au)
print("  seed sensitivity (10 seeds):")
for seed in [20261118, 1, 2, 3, 4, 5, 6, 7, 8, 9]:
    lo, hi = boot_ci(ds_ub, seed=seed)
    print(f"    seed={seed}: CI=[{lo:.4f},{hi:.4f}] {'excludes0' if lo>0 or hi<0 else 'contains0'}")
print("== 64k (abk):")
abk = pairs_for("abk-p")
d_rel64 = []
for i, a, b in ab_A_B(abk, "P2"):
    d_rel64.append((cpu(b[2]["P2"]) - cpu(a[2]["P2"])) / cpu(a[2]["P2"]) * 100)
line("  64k CPU/op rel% B-A", d_rel64)
m, s = mstd(d_rel64); lo, hi = boot_ci(d_rel64)
print(f"  64k verdict: CI lower {lo:.4f} vs tripwire +2% -> {'TRIGGER' if lo > 2.0 else 'not triggered'}")
line("  64k cyc/op B-A", ab_diffs(abk, cyc, "P2"))
