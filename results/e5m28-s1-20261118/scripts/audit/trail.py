import json, math
S = "/home/jnhu/sluice-e5-elim/results/e5m28-s1-20261118"
obs = []
for line in open(S + "/observations.jsonl"):
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
meta = {}
for i in sorted(P):
    entries = sorted(P[i], key=lambda e: (e[0], e[1]))
    sides = []
    for pos, k, o in entries:
        if not sides or sides[-1][0] != pos: sides.append([pos, {}])
        sides[-1][1][o["pass"]] = o
    p2 = [s for s in sides if "P2" in s[1]]
    if len(p2) == 2:
        ds.append((i, p2[1][1]["P2"]["cyc_per_op"] - p2[0][1]["P2"]["cyc_per_op"]))
def mstd(xs):
    m = sum(xs)/len(xs)
    v = sum((x-m)**2 for x in xs)/(len(xs)-1)
    return m, math.sqrt(v)
# per-block (24-pair blocks of the valid diff list)
print("per-block stats on final data (valid diffs in blocks of 24):")
for b in range(5):
    chunk = [d for _, d in ds[b*24:(b+1)*24]]
    if chunk:
        m, s = mstd(chunk)
        print(f"  block{b+1}: n={len(chunk)} mean={m:.4g} std={s:.4g}")
# cumulative
print("cumulative:")
for cut in (24, 48, 71, 94, len(ds)):
    m, s = mstd([d for _, d in ds[:cut]])
    print(f"  n={cut}: mean={m:.4g} std={s:.4g}")
print("pair indices in each block (valid):")
for b in range(5):
    print(f"  block{b+1}:", [i for i, _ in ds[b*24:(b+1)*24]])
