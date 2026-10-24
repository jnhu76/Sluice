#!/usr/bin/env python3
"""#575 paired A/B reduction with self-tests.

Reads the runner's ab.jsonl and produces, per (case, stats) group and per
requested contrast, the paired statistics used by R6-R8: per-rep ratio,
median ratio, per-pair direction, bootstrap uncertainty of the median ratio,
and median absolute difference.

Self-test: --selftest feeds synthetic fixtures (including edge cases) through
the identical reduction functions and asserts the expected outputs.
"""
import json
import math
import sys
from collections import defaultdict


def load_rows(path):
    rows = []
    with open(path) as f:
        for line in f:
            if line.strip():
                rows.append(json.loads(line))
    return rows


def group_key(row):
    return (row['case'], row['stats'])


def collect_pairs(rows, case, stats, arm_a, arm_b, metric):
    """Return list of (rep, a_value, b_value) for reps where both arms exist."""
    by_arm = defaultdict(dict)
    for r in rows:
        if group_key(r) == (case, stats) and r.get(metric) is not None:
            by_arm[r['arm']][r['rep']] = r[metric]
    pairs = []
    for rep in sorted(set(by_arm.get(arm_a, {})) & set(by_arm.get(arm_b, {}))):
        a, b = by_arm[arm_a][rep], by_arm[arm_b][rep]
        if a != 0 and b != 0:
            pairs.append((rep, a, b))
    return pairs


def median(xs):
    s = sorted(xs)
    n = len(s)
    if n == 0:
        return None
    if n % 2:
        return s[n // 2]
    return 0.5 * (s[n // 2 - 1] + s[n // 2])


def reduce_contrast(pairs, bootstrap=10000, seed=20261024):
    if not pairs:
        return {'n': 0}
    ratios = [b / a for _, a, b in pairs]
    diffs = [b - a for _, a, b in pairs]
    n = len(pairs)
    med_ratio = median(ratios)
    med_abs = median(diffs)
    up = sum(1 for r in ratios if r > 1.0)
    down = sum(1 for r in ratios if r < 1.0)
    # bootstrap over reps for the median ratio
    import random
    rng = random.Random(seed)
    samples = []
    if n >= 2:
        for _ in range(bootstrap):
            resample = [ratios[rng.randrange(n)] for _ in range(n)]
            samples.append(median(resample))
        samples.sort()
        lo = samples[int(0.025 * bootstrap)]
        hi = samples[int(0.975 * bootstrap) - 1]
    else:
        lo = hi = med_ratio
    return {
        'n': n, 'median_ratio': med_ratio,
        'ratio_ci95': [lo, hi], 'ratio_min': min(ratios), 'ratio_max': max(ratios),
        'median_abs_diff': med_abs,
        'pairs_b_greater': up, 'pairs_b_smaller': down, 'pairs_equal': n - up - down,
    }


def reduce_file(path, contrasts, metrics):
    rows = load_rows(path)
    groups = sorted({group_key(r) for r in rows})
    out = {}
    for case, stats in groups:
        for (arm_a, arm_b, label) in contrasts:
            for metric in metrics:
                pairs = collect_pairs(rows, case, stats, arm_a, arm_b, metric)
                res = reduce_contrast(pairs)
                if res.get('n'):
                    out[f'{case}|stats={stats}|{label}|{metric}'] = res
    return out


def selftest():
    # Fixture 1: exactly reproducible 10% B>A on wall_s, 3 reps
    rows = []
    for rep in range(1, 4):
        rows.append({'case': 'C1', 'stats': 0, 'arm': 'a', 'rep': rep, 'wall_s': 1.0})
        rows.append({'case': 'C1', 'stats': 0, 'arm': 'b', 'rep': rep, 'wall_s': 1.1})
    pairs = collect_pairs(rows, 'C1', 0, 'a', 'b', 'wall_s')
    assert len(pairs) == 3, pairs
    res = reduce_contrast(pairs, bootstrap=1000)
    assert abs(res['median_ratio'] - 1.1) < 1e-12, res
    assert res['pairs_b_greater'] == 3, res
    assert res['ratio_ci95'][0] == 1.1 and res['ratio_ci95'][1] == 1.1, res

    # Fixture 2: alternating direction — median ratio must be the middle value
    rows = []
    for rep, (a, b) in enumerate([(2.0, 1.0), (1.0, 2.0), (1.0, 1.0)], start=1):
        rows.append({'case': 'C2', 'stats': 0, 'arm': 'a', 'rep': rep, 'wall_s': a})
        rows.append({'case': 'C2', 'stats': 0, 'arm': 'b', 'rep': rep, 'wall_s': b})
    res = reduce_contrast(collect_pairs(rows, 'C2', 0, 'a', 'b', 'wall_s'))
    assert res['n'] == 3
    assert res['pairs_b_greater'] == 1 and res['pairs_b_smaller'] == 1
    assert res['pairs_equal'] == 1, res
    assert abs(res['median_ratio'] - 1.0) < 1e-12, res

    # Fixture 3: missing rep for one arm must drop the pair (never impute)
    rows = [
        {'case': 'C3', 'stats': 0, 'arm': 'a', 'rep': 1, 'wall_s': 1.0},
        {'case': 'C3', 'stats': 0, 'arm': 'b', 'rep': 1, 'wall_s': 1.0},
        {'case': 'C3', 'stats': 0, 'arm': 'a', 'rep': 2, 'wall_s': 1.0},
        # arm b rep 2 missing
        {'case': 'C3', 'stats': 0, 'arm': 'a', 'rep': 3, 'wall_s': 1.0},
        {'case': 'C3', 'stats': 0, 'arm': 'b', 'rep': 3, 'wall_s': 2.0},
    ]
    res = reduce_contrast(collect_pairs(rows, 'C3', 0, 'a', 'b', 'wall_s'))
    assert res['n'] == 2, res

    # Fixture 4: zero/negative metric values must be dropped, not divided
    rows = [
        {'case': 'C4', 'stats': 0, 'arm': 'a', 'rep': 1, 'wall_s': 0.0},
        {'case': 'C4', 'stats': 0, 'arm': 'b', 'rep': 1, 'wall_s': 1.0},
        {'case': 'C4', 'stats': 0, 'arm': 'a', 'rep': 2, 'wall_s': 1.0},
        {'case': 'C4', 'stats': 0, 'arm': 'b', 'rep': 2, 'wall_s': 1.5},
    ]
    res = reduce_contrast(collect_pairs(rows, 'C4', 0, 'a', 'b', 'wall_s'))
    assert res['n'] == 1 and abs(res['median_ratio'] - 1.5) < 1e-12, res

    # Fixture 5: groups must not mix stats modes or cases
    rows = [
        {'case': 'C5', 'stats': 0, 'arm': 'a', 'rep': 1, 'wall_s': 1.0},
        {'case': 'C5', 'stats': 1, 'arm': 'b', 'rep': 1, 'wall_s': 9.0},
        {'case': 'C6', 'stats': 0, 'arm': 'b', 'rep': 1, 'wall_s': 9.0},
    ]
    res = collect_pairs(rows, 'C5', 0, 'a', 'b', 'wall_s')
    assert res == [], res

    print('reduce_575 selftest: all 5 fixtures passed')
    return 0


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == '--selftest':
        return selftest()
    if len(sys.argv) < 3:
        print('usage: reduce_575.py <ab.jsonl> <armA,armB,label> [...] [--metric m ...]',
              file=sys.stderr)
        return 2
    path = sys.argv[1]
    contrasts = []
    metrics = []
    for arg in sys.argv[2:]:
        if arg.startswith('--metric='):
            metrics.append(arg.split('=', 1)[1])
        else:
            a, b, label = arg.split(',')
            contrasts.append((a, b, label))
    if not metrics:
        metrics = ['wall_s', 'ops_per_sec', 'instructions', 'cycles',
                   'time_user_s', 'time_sys_s']
    out = reduce_file(path, contrasts, metrics)
    for key in sorted(out):
        r = out[key]
        ci = r['ratio_ci95']
        print(f"{key}: n={r['n']} median_ratio={r['median_ratio']:.5f} "
              f"CI95=[{ci[0]:.5f},{ci[1]:.5f}] "
              f"min={r['ratio_min']:.5f} max={r['ratio_max']:.5f} "
              f"B>A:{r['pairs_b_greater']} B<A:{r['pairs_b_smaller']} "
              f"={r['pairs_equal']} med_abs_diff={r['median_abs_diff']:.6g}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
