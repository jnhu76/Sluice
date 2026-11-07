#!/usr/bin/env python3
"""Synthetic self-test for analyze_pairs.py.

Checks (prereg section 11.4):
  1. direction: a +10% second-member CPU shift is reported as +10%;
  2. zero effect with small noise -> A/A gate PASS and CI covers 0;
  3. noise wider than the gate -> A/A gate FAIL;
  4. repeated analysis of fixed input is byte-identical;
  5. bootstrap mean/CI/half-width equal harness/analyze.py's implementation;
  6. A/B negative-control gate fails on +6% CPU and passes on a -10% shift.
"""
import importlib.util
import json
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REF = "/home/jnhu/sluice-595-e5/harness/analyze.py"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


ap = load("analyze_pairs", os.path.join(HERE, "analyze_pairs.py"))
ref = load("ref_analyze", REF)

failures = []


def check(ok, label):
    print(("PASS " if ok else "FAIL ") + label)
    if not ok:
        failures.append(label)


def make_records(effect_cpu, effect_p95, effect_p99, effect_tput, noise, seed=11):
    rng = random.Random(seed)
    recs = []
    for pair in range(24):
        base_cpu = 100.0
        base_tput = 200000.0
        base_p95 = 5000.0
        base_p99 = 9000.0
        m1 = {
            "pair": pair, "pos": 0, "member": "m1", "valid": True,
            "metrics": {
                "cpu_ns_per_op": base_cpu * (1 + rng.uniform(-noise, noise)),
                "ops_per_sec": base_tput * (1 + rng.uniform(-noise, noise)),
                "p50_ns": 1000.0, "p95_ns": base_p95, "p99_ns": base_p99,
            },
            "ru_total": 1.0, "ru_maxrss_kb": 20000,
        }
        m2 = {
            "pair": pair, "pos": 1, "member": "m2", "valid": True,
            "metrics": {
                "cpu_ns_per_op": base_cpu * (1 + effect_cpu) * (1 + rng.uniform(-noise, noise)),
                "ops_per_sec": base_tput / (1 + effect_tput) * (1 + rng.uniform(-noise, noise)),
                "p50_ns": 1000.0,
                "p95_ns": base_p95 * (1 + effect_p95),
                "p99_ns": base_p99 * (1 + effect_p99),
            },
            "ru_total": 1.0, "ru_maxrss_kb": 20000,
        }
        recs.extend([m1, m2])
    return recs


# 1. direction
r = ap.evaluate(make_records(0.10, 0.0, 0.0, 0.0, 0.0001), "AA")
cpu_mean = r["metrics"]["cpu"]["mean"]
check(abs(cpu_mean - 0.10) < 0.002, f"direction cpu mean={cpu_mean:+.4f} ~= +0.10")

# 2. zero effect, small noise -> AA PASS, CI covers 0
r0 = ap.evaluate(make_records(0.0, 0.0, 0.0, 0.0, 0.002), "AA")
check(r0["aa_pass"], "AA zero-effect small-noise PASS")
check(r0["metrics"]["cpu"]["ci_lo"] <= 0 <= r0["metrics"]["cpu"]["ci_hi"],
      "AA zero-effect CPU CI covers 0")

# 3. noise wider than the gate -> AA FAIL
r_noise = ap.evaluate(make_records(0.0, 0.0, 0.0, 0.0, 0.20), "AA")
check(not r_noise["aa_pass"], "AA wide-noise FAIL")

# 4. determinism
a = json.dumps(ap.evaluate(make_records(0.02, 0.01, 0.02, 0.01, 0.004), "AA"),
               sort_keys=True)
b = json.dumps(ap.evaluate(make_records(0.02, 0.01, 0.02, 0.01, 0.004), "AA"),
               sort_keys=True)
check(a == b, "fixed input -> byte-identical summary")

# 5. bootstrap cross-check against the frozen R1/R2 implementation
vals = [0.001, -0.004, 0.010, 0.002, -0.001, 0.006, -0.003, 0.000,
        0.004, -0.002, 0.007, -0.005, 0.001, 0.003, -0.001, 0.002,
        0.005, -0.003, 0.000, 0.004, -0.002, 0.001, 0.006, -0.004]
mine = ap.bootstrap_ci(vals)
theirs = ref.bootstrap_ci(vals)
same = (abs(mine["mean"] - theirs["mean"]) < 1e-15
        and abs(mine["ci_lo"] - theirs["ci_lo"]) < 1e-15
        and abs(mine["ci_hi"] - theirs["ci_hi"]) < 1e-15
        and abs(mine["half_width"] - theirs["half_width"]) < 1e-15)
check(same, "bootstrap equals harness/analyze.py (mean/ci_lo/ci_hi/half_width)")

# 6. A/B negative control gate
def ab_records(cpu_shift):
    recs = []
    rng = random.Random(21)
    for pair in range(24):
        a_rec = {"pair": pair, "pos": 0, "member": "A", "valid": True,
                 "metrics": {"cpu_ns_per_op": 100.0 * (1 + rng.uniform(-0.002, 0.002)),
                             "ops_per_sec": 200000.0 * (1 + rng.uniform(-0.002, 0.002)),
                             "p50_ns": 1000.0, "p95_ns": 5000.0, "p99_ns": 9000.0},
                 "ru_total": 1.0, "ru_maxrss_kb": 20000}
        b_rec = {"pair": pair, "pos": 1, "member": "B", "valid": True,
                 "metrics": {"cpu_ns_per_op": 100.0 * (1 + cpu_shift) * (1 + rng.uniform(-0.002, 0.002)),
                             "ops_per_sec": 200000.0 * (1 + rng.uniform(-0.002, 0.002)),
                             "p50_ns": 1000.0, "p95_ns": 5000.0, "p99_ns": 9000.0},
                 "ru_total": 1.0, "ru_maxrss_kb": 20000}
        recs.extend([a_rec, b_rec])
    return recs


check(not ap.evaluate(ab_records(0.06), "AB")["nc_pass"], "negative control +6% CPU FAIL")
check(ap.evaluate(ab_records(-0.10), "AB")["nc_pass"], "negative control -10% CPU PASS")

print(f"\nself-test failures: {len(failures)}")
sys.exit(1 if failures else 0)
