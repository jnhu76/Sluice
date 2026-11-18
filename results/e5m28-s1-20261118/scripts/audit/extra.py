import json, random, math
S = "/home/jnhu/sluice-e5-elim/results/e5m28-s1-20261118"
obs = []
for line in open(S + "/observations.jsonl"):
    o = json.loads(line)
    if o["tag"].startswith("smoke-"): continue
    if not o.get("warm_ok") or not o.get("bench_ok"): continue
    obs.append(o)
def pairs_for(prefix):
    byp = {}
    for o in obs:
        if not o["tag"].startswith(prefix): continue
        pi, pk, arm = o["tag"][len(prefix):].split("-")
        byp.setdefault(int(pi), []).append((int(pk[0]), int(pk[1]), arm, o))
    out = {}
    for i, e in byp.items():
        e.sort(key=lambda x: (x[0], x[1]))
        sides = []
        for pos, k, arm, o in e:
            if not sides or sides[-1][0] != pos: sides.append([pos, arm, {}])
            sides[-1][2][o["pass"]] = o
        out[i] = sides
    return out
def boot_ci(xs, n=10000, seed=20261118):
    rng = random.Random(seed); m = len(xs); means = []
    for _ in range(n):
        s = 0.0
        for _ in range(m): s += xs[rng.randrange(m)]
        means.append(s/m)
    means.sort()
    return means[250], means[9750]
def ab_diffs(P, fn, pt):
    ds = []
    for i in sorted(P):
        sides = [s for s in P[i] if pt in s[2]]
        if len(sides) == 2:
            a, b = (sides[0], sides[1]) if sides[0][1] == "A" else (sides[1], sides[0])
            ds.append(fn(b[2][pt]) - fn(a[2][pt]))
    return ds
ab = pairs_for("abm-p")
instr2 = lambda o: o["instr_per_op"]
d = ab_diffs(ab, instr2, "P2")
m = sum(d)/len(d); lo, hi = boot_ci(d)
print(f"A/B P2 (u+k) instr/op B-A: mean={m:.4g} CI=[{lo:.4g},{hi:.4g}] n={len(d)}")
# uring abu A-arm level
abu = pairs_for("abu-p")
aobs = [s[2]["P2"] for i in abu for s in abu[i] if "P2" in s[2] and s[1]=="A"]
print(f"uring A-arm level mean = {sum(o['cyc_per_op'] for o in aobs)/len(aobs):.1f} -> 1% = {sum(o['cyc_per_op'] for o in aobs)/len(aobs)/100:.2f}")
# perf CSV consistency sample
import os, csv, io
tags = ["main-p001-11-A","main-p060-22-A","abm-p007-21-B","abu-p012-21-A","abk-p018-11-B","main-p120-22-A"]
obsm = {o["tag"]: o for o in obs}
import json as J
allobs = [J.loads(l) for l in open(S + "/observations.jsonl")]
allobsm = {o["tag"]: o for o in allobs}
ok = True
for t in tags:
    p = f"{S}/perf/{t}.csv"
    vals = {}
    for line in open(p):
        if line.startswith("#") or not line.strip(): continue
        parts = line.rstrip("\n").split(",")
        try: vals[parts[2]] = float(parts[0])
        except ValueError: pass
    o = allobsm[t]
    for k, v in vals.items():
        okk = abs(o.get("perf_"+k, -1) - v) < 1e-6*max(1, abs(v))
        if not okk: ok = False; print("MISMATCH", t, k, o.get("perf_"+k), v)
print("perf CSV <-> observations consistency (6 samples):", "OK" if ok else "FAILED")
# warm-fail / bench_ok and NOTWARM tags confirm
nf = [o["tag"] for o in allobs if not o.get("warm_ok")]
print("NOTWARM obs tags:", nf)
