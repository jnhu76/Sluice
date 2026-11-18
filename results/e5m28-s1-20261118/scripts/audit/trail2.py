import json, math, random
S = "/home/jnhu/sluice-e5-elim/results/e5m28-s1-20261118"
allobs = [json.loads(l) for l in open(S + "/observations.jsonl")]
# hypothesis: 71-point trail computed from snapshot where pairs 66-72 use attempt-1 data
# emulate by truncating observations to lines before the resume block (obs 289 onwards = resume rerun)
def build(lines):
    obs = []
    for line in lines:
        o = json.loads(line)
        if o["tag"].startswith("smoke-"): continue
        if not o.get("warm_ok") or not o.get("bench_ok"): continue
        obs.append(o)
    P = {}
    for o in obs:
        if not o["tag"].startswith("main-p"): continue
        pi, pk, arm = o["tag"][6:].split("-")
        P.setdefault(int(pi), []).append((int(pk[0]), int(pk[1]), o))
    ds = []
    for i in sorted(P):
        entries = sorted(P[i], key=lambda e: (e[0], e[1]))
        sides = []
        for pos, k, o in entries:
            if not sides or sides[-1][0] != pos: sides.append([pos, {}])
            sides[-1][1][o["pass"]] = o
        p2 = [s for s in sides if "P2" in s[1]]
        if len(p2) == 2:
            ds.append((i, p2[1][1]["P2"]["cyc_per_op"] - p2[0][1]["P2"]["cyc_per_op"]))
    return ds
def mstd(xs):
    m = sum(xs)/len(xs)
    return m, math.sqrt(sum((x-m)**2 for x in xs)/(len(xs)-1))
def boot_hw(xs, n=10000, seed=20261118):
    rng = random.Random(seed); m = len(xs); means = []
    for _ in range(n):
        s = 0.0
        for _ in range(m): s += xs[rng.randrange(m)]
        means.append(s/m)
    means.sort()
    return (means[9750]-means[250])/2
def sign(xs):
    return sum(1 for x in xs if x>0), sum(1 for x in xs if x<0)
all_lines = open(S + "/observations.jsonl").readlines()
# find line index where resume rerun starts: obs 289 (0-based) per earlier debug = line 290 (1-based) -> index 289
pre_resume = all_lines[:289]
ds = build(pre_resume)
print("pre-resume snapshot diffs:", len(ds))
for cut in (24, 48, 71):
    sub = [d for _, d in ds[:cut]]
    m, s = mstd(sub)
    print(f"  n={cut}: mean={m:.4g} std={s:.4g} hw={boot_hw(sub):.4g} sign={sign(sub)}")
