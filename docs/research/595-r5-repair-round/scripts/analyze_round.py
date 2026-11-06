#!/usr/bin/env python3
"""#595 R5-repair three-arm analysis.

Units and estimators (frozen before any formal timing; see PREREGISTERED.md):
- inference unit = the matched block; a block's arm value is its invocation's
  metric (cpu_ns_per_op, ops_per_sec, p50/p95/p99_ns, wall). Run-level p99 =
  mean over the 5 measured reps of each rep's p99 (R4 caliber; never pooled
  per-request percentiles).
- paired relative effect of arm X vs arm Y per block:
      cpu/latency cost: (X - Y) / Y      throughput gain: (X - Y) / Y
  For gates we report C/A0 - 1 for every metric (cost口径 for cpu and
  latencies, improvement口径 for throughput), matching the task's frozen
  wording.
- bootstrap: 10,000 resamples of the 24 blocks (seed 5950, per-cell), each
  resample keeps the (A0, R, C) triple together; point estimate = mean of
  block effects; percentile CI = 2.5/97.5; one-sided 95% bounds = 5th or
  95th percentile as the gate direction requires.
- A/A gate: half-width = (ci97.5 - ci2.5)/2 of the same paired effect;
  limits cpu 5pp, tput 3pp, p99 3pp.
- Holm family: the five gate hypotheses of the main gate + protection gates
  as listed in PREREGISTERED.md §7; p-values are computed by inverting the
  bootstrap one-sided bound (fraction of resamples at/beyond the gate
  threshold direction), min-pinned at 1/resamples.
"""
import argparse
import json
import math
import os
import random
import sys

BOOT = 10000
SEED = 5950
AA_LIMITS = {"cpu": 0.05, "tput": 0.03, "p99": 0.03}

# Gate hypotheses (direction = the direction in which the gate is satisfied).
MAIN_GATE = [
    ("MAIN_CpuReduction", "cpu", "le", -0.10, -0.05),   # point <= -10%, 1s95 <= -5%
    ("MAIN_Throughput", "tput", "ge", None, -0.03),      # 1s95 lower bound >= -3%
    ("MAIN_P95", "p95", "le", None, 0.05),
    ("MAIN_P99", "p99", "le", None, 0.05),
]
PROT_GATE = [
    ("ProtCpu", "cpu", "le", None, 0.05),
    ("ProtTput", "tput", "ge", None, -0.03),
    ("ProtP95", "p95", "le", None, 0.05),
    ("ProtP99", "p99", "le", None, 0.05),
]


def load_seg(cell_dir, seg, arms):
    """Return {arm: {metric: [block values]}} with block alignment."""
    path = os.path.join(cell_dir, seg, "results.jsonl")
    by_block = {}
    if not os.path.exists(path):
        return None
    with open(path) as f:
        for line in f:
            rec = json.loads(line)
            if not rec.get("valid", False) or rec.get("seg") != seg:
                continue
            by_block.setdefault(rec["block"], {})[rec["arm"]] = rec
    out = {a: {"cpu": [], "tput": [], "p50": [], "p95": [], "p99": [], "wall": []}
           for a in arms}
    blocks_used = 0
    for b in sorted(by_block):
        d = by_block[b]
        if not all(a in d for a in arms):
            continue
        if any(not d[a].get("metrics") for a in arms):
            continue
        blocks_used += 1
        for a in arms:
            m = d[a]["metrics"]
            out[a]["cpu"].append(m["cpu_ns_per_op"])
            out[a]["tput"].append(m["ops_per_sec"])
            out[a]["p50"].append(m["p50_ns"])
            out[a]["p95"].append(m["p95_ns"])
            out[a]["p99"].append(m["p99_ns"])
            out[a]["wall"].append(m["wall_s"])
    return out, blocks_used


def effect(x, y, metric):
    """(x - y)/y per aligned block."""
    return [(xi - yi) / yi for xi, yi in zip(x, y)]


def paired_bootstrap_effects(eff, seed=SEED, boots=BOOT):
    """Mean-of-resampled-effects distribution; caller passes one effect vector
    per arm contrast computed from the same block ordering."""
    rng = random.Random(seed)
    n = len(eff)
    means = []
    for _ in range(boots):
        s = 0.0
        for _ in range(n):
            s += eff[rng.randrange(n)]
        means.append(s / n)
    means.sort()
    return means


def ci_and_bounds(means):
    """Frozen quantile convention: sorted means, index int(p*n) clamped to
    n-1 (for values 1..100: ci95 = [3, 98], lower95 = 6, upper95 = 96)."""
    n = len(means)
    lo = means[min(n - 1, int(0.025 * n))]
    hi = means[min(n - 1, int(0.975 * n))]
    lower95 = means[min(n - 1, int(0.05 * n))]
    upper95 = means[min(n - 1, int(0.95 * n))]
    return {"ci95": [lo, hi], "lower95": lower95, "upper95": upper95}


def contrast(armx, army, data):
    out = {}
    for metric in ("cpu", "tput", "p50", "p95", "p99", "wall"):
        eff = effect(data[armx][metric], data[army][metric], metric)
        point = sum(eff) / len(eff)
        means = paired_bootstrap_effects(eff)
        b = ci_and_bounds(means)
        b["point"] = point
        b["n"] = len(eff)
        out[metric] = b
    return out


def p_value(means, threshold, direction):
    """One-sided bootstrap p: fraction of resampled means failing the gate."""
    if direction == "le":
        fail = sum(1 for m in means if m > threshold)
    else:
        fail = sum(1 for m in means if m < threshold)
    return max(fail / len(means), 1.0 / len(means))


def holm(pvals):
    order = sorted(range(len(pvals)), key=lambda i: pvals[i])
    m = len(pvals)
    adj = [0.0] * m
    prev = 0.0
    for rank, i in enumerate(order):
        a = min(1.0, (m - rank) * pvals[i])
        a = max(a, prev)
        adj[i] = a
        prev = a
    return adj


def analyze_cell(cell_name):
    cell_dir = os.path.join("/home/jnhu/sluice-595-e5/r5-repair-measure/raw",
                            cell_name.lower())
    result = {"cell": cell_name}
    main = load_seg(cell_dir, "main", ["A0", "R", "C"])
    if main:
        data, nblocks = main
        result["n_blocks"] = nblocks
        result["abs"] = {a: {k: (sum(v) / len(v), min(v), max(v)) for k, v in data[a].items()}
                         for a in data}
        result["C_vs_A0"] = contrast("C", "A0", data)
        result["C_vs_R"] = contrast("C", "R", data)
        result["R_vs_A0"] = contrast("R", "A0", data)
        # gate p-values (bootstrap under the null of the gate threshold)
        gates = {}
        for name, metric, direction, point_req, bound in MAIN_GATE:
            eff = effect(data["C"][metric], data["A0"][metric], metric)
            means = paired_bootstrap_effects(eff)
            gates[name] = {
                "metric": metric, "direction": direction,
                "point_req": point_req, "bound_req": bound,
                "point": sum(eff) / len(eff), "ci95": ci_and_bounds(means)["ci95"],
                "p": p_value(means, bound, direction),
                "bound": ci_and_bounds(means)["upper95"] if direction == "le"
                         else ci_and_bounds(means)["lower95"],
            }
        result["main_gate"] = gates
        pvals = [g["p"] for g in gates.values()]
        adj = holm(pvals)
        for (name, _), a in zip(gates.items(), adj):
            gates[name]["p_holm"] = a
    aa_path = os.path.join(cell_dir, "aa", "results.jsonl")
    if os.path.exists(aa_path):
        result["aa"] = aa_halfwidths(cell_dir)
    calib = calib_analysis(cell_dir)
    if calib:
        result["a0o_vs_a0c"] = calib
    return result


def aa_halfwidths(cell_dir):
    """A/A drift: pair effect pos1 vs pos0 of the SAME binary."""
    path = os.path.join(cell_dir, "aa", "results.jsonl")
    by_pair = {}
    with open(path) as f:
        for line in f:
            rec = json.loads(line)
            if not rec.get("valid", False) or rec.get("seg") != "aa":
                continue
            by_pair.setdefault(rec["block"], {})[rec["position"]] = rec
    out = {}
    for metric, key in (("cpu", "cpu_ns_per_op"), ("tput", "ops_per_sec"),
                        ("p95", "p95_ns"), ("p99", "p99_ns")):
        eff = []
        for p in sorted(by_pair):
            d = by_pair[p]
            if 0 not in d or 1 not in d:
                continue
            x, y = d[1]["metrics"][key], d[0]["metrics"][key]
            eff.append((x - y) / y)
        if not eff:
            continue
        means = paired_bootstrap_effects(eff)
        b = ci_and_bounds(means)
        b["point"] = sum(eff) / len(eff)
        b["half_width"] = (b["ci95"][1] - b["ci95"][0]) / 2
        b["limit"] = AA_LIMITS.get(metric)
        b["pass"] = b["limit"] is None or b["half_width"] <= b["limit"]
        out[metric] = b
    return out


def calib_analysis(cell_dir):
    path = os.path.join(cell_dir, "calib", "results.jsonl")
    if not os.path.exists(path):
        return None
    by_pair = {}
    with open(path) as f:
        for line in f:
            rec = json.loads(line)
            if not rec.get("valid", False) or rec.get("seg") != "calib":
                continue
            by_pair.setdefault(rec["block"], {})[rec["arm"]] = rec
    out = {}
    for metric, key in (("cpu", "cpu_ns_per_op"), ("tput", "ops_per_sec"),
                        ("p95", "p95_ns"), ("p99", "p99_ns")):
        eff = []
        for p in sorted(by_pair):
            d = by_pair[p]
            if "A0o" not in d or "A0c" not in d:
                continue
            x, y = d["A0c"]["metrics"][key], d["A0o"]["metrics"][key]
            eff.append((x - y) / y)
        if not eff:
            continue
        means = paired_bootstrap_effects(eff)
        b = ci_and_bounds(means)
        b["point"] = sum(eff) / len(eff)
        b["n"] = len(eff)
        out[metric] = b
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cells", default="all")
    ap.add_argument("--out", default=None)
    args = ap.parse_args()
    names = ["MAIN-W03-D16", "PROT-W03-D4", "PROT-W03-D2SERIAL", "PROT-W02-D1",
             "PROT-W02-D2"] if args.cells == "all" else args.cells.split(",")
    results = [analyze_cell(n) for n in names]
    text = json.dumps(results, indent=1)
    if args.out:
        with open(args.out, "w") as f:
            f.write(text)
    print(text)


if __name__ == "__main__":
    main()
