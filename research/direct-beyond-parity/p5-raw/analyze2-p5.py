#!/usr/bin/env python3
"""P5 order-aware paired analyzer for two-binary series (paired2.sh output)
and name-paired analyzer for single-binary series (paired.sh output).
Closes the P4-disclosed analyze2.py-not-archived gap (P5 plan §5 item 6).

Two-binary mode (P5_ORDER=1 env or --order): samples alternate
ladderA,ladderB (pair even) / ladderB,ladderA (pair odd); the per-pair
statistic is always ladderA - ladderB (odd pairs flipped).

Single-binary mode (default): pairs consecutive armA/armB samples by name,
either order, statistic armA - armB.

Output: pairs, per-side mean/sd/median, delta median/mean/sd, sign count,
paired t, CI95 of the paired mean (t(0.975,15)=2.131 for 16 pairs), AB vs BA
subgroup medians, and the raw deltas (P5 plan §5 item 5)."""
import re
import statistics
import sys

LINES = sys.stdin.read().splitlines()
PAT = re.compile(r"arm=(\S+) size=(\d+) ops=(\d+) ns_per_op=([\d.]+)")
SAMPLES = []
for ln in LINES:
    m = PAT.search(ln)
    if m:
        SAMPLES.append((m.group(1), float(m.group(4))))

T975 = {1: 12.706, 2: 4.303, 3: 3.182, 4: 2.776, 5: 2.571, 6: 2.447,
        7: 2.365, 8: 2.306, 9: 2.262, 10: 2.228, 11: 2.201, 12: 2.179,
        13: 2.160, 14: 2.145, 15: 2.131, 16: 2.120, 17: 2.110, 18: 2.101,
        19: 2.093, 20: 2.086}


def report(deltas, ab, ba, a_vals, b_vals, la, lb):
    med = statistics.median(deltas)
    mean = statistics.mean(deltas)
    sd = statistics.stdev(deltas) if len(deltas) > 1 else 0.0
    n = len(deltas)
    pos = sum(1 for d in deltas if d > 0)
    se = sd / n ** 0.5 if deltas else 0.0
    t = mean / se if se > 0 else 0.0
    crit = T975.get(n - 1, 2.131)
    ci = (mean - crit * se, mean + crit * se) if se > 0 else (mean, mean)
    print(f"pairs={n}")
    print(f"{la}: mean={statistics.mean(a_vals):.3f}ns sd={statistics.stdev(a_vals) if n>1 else 0:.3f} median={statistics.median(a_vals):.3f}")
    print(f"{lb}: mean={statistics.mean(b_vals):.3f}ns sd={statistics.stdev(b_vals) if n>1 else 0:.3f} median={statistics.median(b_vals):.3f}")
    print(f"delta({la}-{lb}): median={med:+.3f}ns mean={mean:+.3f}ns sd={sd:.3f} pos={pos}/{n}")
    print(f"paired t={t:+.2f} (df={n-1})")
    print(f"CI95_mean=[{ci[0]:+.3f},{ci[1]:+.3f}]")
    if ab:
        print(f"AB_subgroup_median={statistics.median(ab):+.3f} BA_subgroup_median={statistics.median(ba):+.3f}")
    print("raw_deltas=" + ",".join(f"{d:.3f}" for d in deltas))


order_mode = len(sys.argv) > 1 and sys.argv[1] == "--order"

if order_mode:
    seq = [ns for _, ns in SAMPLES]
    if len(seq) % 2:
        seq = seq[:-1]
    deltas, ab, ba, av, bv = [], [], [], [], []
    for p in range(len(seq) // 2):
        first, second = seq[2 * p], seq[2 * p + 1]
        if p % 2 == 0:
            x, y = first, second
        else:
            x, y = second, first
        d = x - y
        deltas.append(d)
        av.append(x)
        bv.append(y)
        (ab if p % 2 == 0 else ba).append(d)
    report(deltas, ab, ba, av, bv, "ladderA", "ladderB")
else:
    arm_a, arm_b = sys.argv[-2], sys.argv[-1]
    seq = [(a, ns) for a, ns in SAMPLES]
    pairs, ab, ba = [], [], []
    i = 0
    p = 0
    while i + 1 < len(seq):
        x, y = seq[i], seq[i + 1]
        if {x[0], y[0]} == {arm_a, arm_b}:
            pair_ab = (x[0] == arm_a) if arm_a != arm_b else (p % 2 == 0)
            pairs.append((x[1], y[1]) if pair_ab else (y[1], x[1]))
            (ab if pair_ab else ba).append(pairs[-1][0] - pairs[-1][1])
            i += 2
            p += 1
        else:
            i += 1
    deltas = [a - b for a, b in pairs]
    report(deltas, ab, ba, [a for a, _ in pairs], [b for _, b in pairs], arm_a, arm_b)
