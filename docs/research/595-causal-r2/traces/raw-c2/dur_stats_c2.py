#!/usr/bin/env python3
"""Pooled submit-only io_uring_enter duration/gap stats per arm + bootstrap B-A
(bootstrap resamples per-measured-segment means: 2 invocations x 3 reps/arm)."""
import random
import re

ENTER = re.compile(
    r"^\d+ (\d+\.\d+) io_uring_enter\((\d+), (\d+), (\d+), ([^,]*?), (NULL|0x[0-9a-f]+), (\d+)\)"
    r" = (-?\d+)(?: <(\d+\.\d+)>)?$"
)
SETUP = re.compile(r"^\d+ \d+\.\d+ io_uring_setup\(")


def q(v, p):
    v = sorted(v)
    k = (len(v) - 1) * p
    lo, hi = int(k), min(int(k) + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (k - lo)


def segments(path):
    """Yield per-segment lists of (dur_ns) for submit-only enters."""
    segs, cur = [], None
    for ln in open(path, errors="replace"):
        if SETUP.match(ln):
            if cur:
                segs.append(cur)
            cur = []
            continue
        m = ENTER.match(ln)
        if m and m.group(5).strip() == "0" and m.group(9):
            cur.append(float(m.group(9)) * 1e9)
    if cur:
        segs.append(cur)
    return segs


res = {}
for arm in ("A", "B"):
    durs, seg_means = [], []
    for idx in (1, 2):
        segs = segments(f"c2d2-on-{arm}-{idx}.strace.txt")
        for s in segs[-3:]:  # measured reps only (PMU_ONLY: last 3 passes)
            durs.extend(s)
            seg_means.append(sum(s) / len(s))
    res[arm] = seg_means
    print(f"arm {arm}: n={len(durs)} dur mean={sum(durs)/len(durs):.0f} "
          f"p50={q(durs, .5):.0f} p90={q(durs, .9):.0f} p95={q(durs, .95):.0f} "
          f"p99={q(durs, .99):.0f} max={max(durs):.0f} ns | seg means: "
          + ", ".join(f"{m:.1f}" for m in seg_means))

random.seed(5950)
mA_all = res["A"]
mB_all = res["B"]
obs = sum(mB_all) / len(mB_all) - sum(mA_all) / len(mA_all)
diffs = []
for _ in range(10000):
    a = [random.choice(mA_all) for _ in mA_all]
    b = [random.choice(mB_all) for _ in mB_all]
    diffs.append(sum(b) / len(b) - sum(a) / len(a))
diffs.sort()
print(f"B-A segment-mean diff = {obs:+.1f} ns  bootstrap95 [{diffs[249]:+.1f}, {diffs[9749]:+.1f}]")
