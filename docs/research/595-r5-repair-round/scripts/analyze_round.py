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
- Holm family (PREREGISTERED.md §5): the MAIN cell adjusts across its 4
  main-gate hypotheses; each protection cell adjusts across its own 4
  protection hypotheses. Gate decisions use the ONE-SIDED 95% bounds
  (upper95 for cost/gate "le", lower95 for "ge"), never the two-sided
  ci95 endpoints. p-values are computed by inverting the bootstrap
  one-sided bound (fraction of resamples at/beyond the gate threshold
  direction), min-pinned at 1/resamples.
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


def gate_set(cell_name):
    """PREREGISTERED.md §6: the MAIN cell runs the main gate; every other
    cell runs the protection gate (cpu upper<=+5%, tput lower>=-3%,
    p95/p99 upper<=+5%). The first published analysis wrongly applied
    MAIN_GATE to all cells; corrected per review (CORRECTIONS.md §R2)."""
    return MAIN_GATE if cell_name == "MAIN-W03-D16" else PROT_GATE


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
        # gate evaluation (bootstrap under the null of the gate threshold)
        gates = {}
        for name, metric, direction, point_req, bound_req in gate_set(cell_name):
            eff = effect(data["C"][metric], data["A0"][metric], metric)
            means = paired_bootstrap_effects(eff)
            cb = ci_and_bounds(means)
            one_sided = cb["upper95"] if direction == "le" else cb["lower95"]
            point = sum(eff) / len(eff)
            ok_bound = one_sided <= bound_req if direction == "le" else one_sided >= bound_req
            ok_point = point_req is None or (
                point <= point_req if direction == "le" else point >= point_req)
            gates[name] = {
                "metric": metric, "direction": direction,
                "point_req": point_req, "bound_req": bound_req,
                "point": point, "ci95": cb["ci95"],
                "bound_used": "upper95" if direction == "le" else "lower95",
                "one_sided_bound": one_sided,
                "pass": bool(ok_bound and ok_point),
                "p": p_value(means, bound_req, direction),
            }
        pvals = [g["p"] for g in gates.values()]
        adj = holm(pvals)
        for (name, _), a in zip(gates.items(), adj):
            gates[name]["p_holm"] = a
        result["gate"] = gates
        result["gate_family"] = "main" if cell_name == "MAIN-W03-D16" else "protection"
        result["gate_pass"] = all(g["pass"] for g in gates.values())
        if cell_name == "MAIN-W03-D16":
            result["main_gate"] = gates
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


def _pct(x):
    return f"{x * 100:+.2f}%"


def _ic(b):
    return f"[{b['ci95'][0] * 100:+.2f},{b['ci95'][1] * 100:+.2f}]"


def _bound(b, direction):
    return b["upper95"] if direction == "le" else b["lower95"]


def write_tables(results, path):
    """Generate every numeric table used by RESULTS.md from the analysis JSON,
    so prose numbers cannot drift from the recomputed data (review R2)."""
    L = []
    main = next((r for r in results if r["cell"] == "MAIN-W03-D16"), None)
    if main:
        L.append("## MAIN-W03-D16 absolute means (per arm, mean over 24 blocks)\n")
        L.append("| arm | cpu ns/op | p50 µs | p95 µs | p99 µs | ops/s |")
        L.append("|---|---:|---:|---:|---:|---:|")
        for arm in ("A0", "R", "C"):
            a = main["abs"][arm]
            L.append(f"| {arm} | {a['cpu'][0]:.1f} | {a['p50'][0] / 1000:.2f} | "
                     f"{a['p95'][0] / 1000:.2f} | {a['p99'][0] / 1000:.2f} | "
                     f"{a['tput'][0]:.0f} |")
        L.append("\n## MAIN-W03-D16 contrasts (paired block effects, % of A0 or R)\n")
        L.append("| contrast | cpu | throughput | p95 | p99 |")
        L.append("|---|---|---|---|---|")
        for label, key in (("C−A0", "C_vs_A0"), ("R−A0", "R_vs_A0"), ("C−R", "C_vs_R")):
            c = main[key]
            L.append(f"| {label} | {_pct(c['cpu']['point'])} {_ic(c['cpu'])} | "
                     f"{_pct(c['tput']['point'])} {_ic(c['tput'])} | "
                     f"{_pct(c['p95']['point'])} {_ic(c['p95'])} | "
                     f"{_pct(c['p99']['point'])} {_ic(c['p99'])} |")
        L.append("\n## Gate evaluation (frozen thresholds, one-sided 95% bounds)\n")
        L.append("| cell | item | point | 1s-bound | threshold | pass | p | p_holm |")
        L.append("|---|---|---:|---:|---|---|---:|---:|")
        for r in results:
            for name, g in r.get("gate", {}).items():
                rel = "≤" if g["direction"] == "le" else "≥"
                thr = (f"point{rel}{g['point_req'] * 100:+.0f}% & " if g["point_req"] is not None
                       else "") + f"{g['bound_used']}{rel}{g['bound_req'] * 100:+.0f}%"
                L.append(f"| {r['cell']} | {name} | {_pct(g['point'])} | "
                         f"{_pct(g['one_sided_bound'])} ({g['bound_used']}) | {thr} | "
                         f"{'PASS' if g['pass'] else 'FAIL'} | {g['p']:.4f} | "
                         f"{g['p_holm']:.4f} |")
            L.append(f"| {r['cell']} | **cell verdict** | | | | "
                     f"{'**PASS**' if r.get('gate_pass') else '**FAIL**'} | | |")
        L.append("\n## Cross-binary calibration A0c−A0o (same product path, two binaries)\n")
        L.append("| cell | cpu | p99 |")
        L.append("|---|---|---|")
        for r in results:
            cal = r.get("a0o_vs_a0c")
            if not cal:
                continue
            L.append(f"| {r['cell']} | {_pct(cal['cpu']['point'])} {_ic(cal['cpu'])} | "
                     f"{_pct(cal['p99']['point'])} {_ic(cal['p99'])} |")
        L.append("\n## C−R raw effects vs calibration band (report only — the band is a")
        L.append("same-path binary effect and need not transfer to the R path; no joint")
        L.append("uncertainty model, so residuals are observations, not estimates)\n")
        L.append("| cell | C−R cpu | calib cpu | C−R p99 | calib p99 |")
        L.append("|---|---|---|---|---|")
        for r in results:
            cal = r.get("a0o_vs_a0c")
            cr = r.get("C_vs_R")
            if not cal or not cr:
                continue
            L.append(f"| {r['cell']} | {_pct(cr['cpu']['point'])} | "
                     f"{_pct(cal['cpu']['point'])} | {_pct(cr['p99']['point'])} | "
                     f"{_pct(cal['p99']['point'])} |")
        L.append("")
    with open(path, "w") as f:
        f.write("\n".join(L))
    print(f"tables written to {path}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cells", default="all")
    ap.add_argument("--out", default=None)
    ap.add_argument("--tables", default=None,
                    help="also write the RESULTS.md numeric tables to this path")
    args = ap.parse_args()
    names = ["MAIN-W03-D16", "PROT-W03-D4", "PROT-W03-D2SERIAL", "PROT-W02-D1",
             "PROT-W02-D2"] if args.cells == "all" else args.cells.split(",")
    results = [analyze_cell(n) for n in names]
    text = json.dumps(results, indent=1)
    if args.out:
        with open(args.out, "w") as f:
            f.write(text)
        print(f"analysis written to {args.out}")
    if args.tables:
        write_tables(results, args.tables)


if __name__ == "__main__":
    main()
