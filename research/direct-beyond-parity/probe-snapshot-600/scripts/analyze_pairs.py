#!/usr/bin/env python3
"""#600 paired-series analyzer: reads ladder 'arm=... ns_per_op=...' lines,
pairs consecutive A/B samples in given order, prints paired deltas."""
import re
import statistics
import sys

lines = sys.stdin.read().splitlines()
pat = re.compile(r"arm=(\S+) size=(\d+) ops=(\d+) ns_per_op=([\d.]+)")
samples = []
for ln in lines:
    m = pat.search(ln)
    if m:
        samples.append((m.group(1), int(m.group(2)), int(m.group(3)), float(m.group(4))))

arm_a, arm_b = sys.argv[1], sys.argv[2]
seq = [(a, ns) for (a, s, o, ns) in samples]
pairs = []
i = 0
while i + 1 < len(seq):
    x, y = seq[i], seq[i + 1]
    if {x[0], y[0]} == {arm_a, arm_b}:
        if x[0] == arm_a:
            pairs.append((x[1], y[1]))
        else:
            pairs.append((y[1], x[1]))
        i += 2
    else:
        i += 1

if not pairs:
    print("no pairs")
    sys.exit(1)
deltas = [a - b for a, b in pairs]
a_vals = [a for a, _ in pairs]
b_vals = [b for _, b in pairs]
med_delta = statistics.median(deltas)
mean_delta = statistics.mean(deltas)
sd = statistics.stdev(deltas) if len(deltas) > 1 else 0.0
n_pos = sum(1 for d in deltas if d > 0)
sd_a = statistics.stdev(a_vals) if len(a_vals) > 1 else 0.0
sd_b = statistics.stdev(b_vals) if len(b_vals) > 1 else 0.0
mean_a, mean_b = statistics.mean(a_vals), statistics.mean(b_vals)
rel = med_delta / mean_b * 100 if mean_b else 0
print(f"pairs={len(pairs)}")
print(f"{arm_a}: mean={mean_a:.3f}ns sd={sd_a:.3f} median={statistics.median(a_vals):.3f}")
print(f"{arm_b}: mean={mean_b:.3f}ns sd={sd_b:.3f} median={statistics.median(b_vals):.3f}")
print(f"delta({arm_a}-{arm_b}): median={med_delta:+.3f}ns mean={mean_delta:+.3f}ns "
      f"sd={sd:.3f} rel={rel:+.3f}% pos={n_pos}/{len(pairs)}")
se = sd / len(deltas) ** 0.5 if deltas else 0
if se > 0:
    t = mean_delta / se
    print(f"paired t={t:+.2f} (df={len(deltas) - 1})")
