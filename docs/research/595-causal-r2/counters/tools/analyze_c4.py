#!/usr/bin/env python3
"""Analysis for #595 causal-r2 C4 schedstat blocks.

Pair-relative A/B statistics (bootstrap 95% CI, 10,000 resamples, seed 5950,
percentile — same convention as analyze_cost.py) over per-thread scheduler
accounting normalized by ops_window = (warmup+measured)*20000, matching the
split-d2 window convention (the /proc counters cover the whole invocation
including warmup, so the denominator is the full window).
"""

import json
import random
import sys

BLOCKS = {
    "schedstat-d2": ("AB", "primary ON block, 12 pairs"),
    "schedstat-off-d2": ("AB", "OFF (no sampler), 6 pairs"),
    "schedstat-d2-br": ("AB", "ON bracket tail, 6 pairs"),
}

METRICS_BENCH = [
    "bench_on_cpu_ns", "bench_runwait_ns", "bench_timeslices",
    "bench_user_tick_ns", "bench_kernel_tick_ns",
    "bench_voluntary_status", "bench_nonvoluntary_status",
    "bench_voluntary_sched", "bench_nonvoluntary_sched",
    "bench_nr_migrations", "bench_sleep_derived_ns",
    "bench_timeslice_mean_ns",
]
METRICS_IOWQ = [
    "iowq_on_cpu_ns", "iowq_runwait_ns", "iowq_timeslices",
    "iowq_kernel_tick_ns", "iowq_voluntary_status",
    "iowq_nonvoluntary_status", "iowq_nr_migrations",
    "iowq_sleep_derived_ns", "iowq_timeslice_mean_ns",
]
METRICS_BENCHVIEW = ["cpu_ns_per_op", "tput", "p95_ns", "p99_ns"]
METRIC_ALL = METRICS_BENCH + METRICS_IOWQ + ["psi_cpu_total_us"]
LIFT = {"tput": False}  # tput: positive delta = B worse for all here (B tput lower)


def invocation_view(rec):
    mv = rec["measured_view"]
    out = {k: mv[k] for k in METRICS_BENCHVIEW}
    out["ops_measured"] = mv["ops_measured"]
    out["ops_window"] = mv["ops_window"]
    out["elapsed_s"] = rec["elapsed_s"]
    th = rec.get("threads")
    if th is None:
        return out, False
    bench, iowq = th["bench"], th["iowq"]
    win = mv["ops_window"]
    out["bench_on_cpu_ns"] = bench["on_cpu_ns"] / win
    out["bench_runwait_ns"] = bench["runwait_ns"] / win
    out["bench_timeslices"] = bench["timeslices"] / win
    out["bench_user_tick_ns"] = bench["user_s"] * 1e9 / win
    out["bench_kernel_tick_ns"] = bench["kernel_s"] * 1e9 / win
    out["bench_voluntary_status"] = bench["voluntary"] / win
    out["bench_nonvoluntary_status"] = bench["nonvoluntary"] / win
    out["bench_voluntary_sched"] = bench["nr_voluntary"] / win
    out["bench_nonvoluntary_sched"] = bench["nr_involuntary"] / win
    out["bench_nr_migrations"] = bench["nr_migrations"]
    out["bench_sleep_derived_ns"] = (
        bench["thread_lifetimes_s"] * 1e9 - bench["on_cpu_ns"] - bench["runwait_ns"]
    ) / win
    out["bench_timeslice_mean_ns"] = (
        bench["on_cpu_ns"] / bench["timeslices"] if bench["timeslices"] else None
    )
    out["iowq_on_cpu_ns"] = iowq["on_cpu_ns"] / win
    out["iowq_runwait_ns"] = iowq["runwait_ns"] / win
    out["iowq_timeslices"] = iowq["timeslices"] / win
    out["iowq_kernel_tick_ns"] = iowq["kernel_s"] * 1e9 / win
    out["iowq_user_tick_ns"] = iowq["user_s"] * 1e9 / win
    out["iowq_voluntary_status"] = iowq["voluntary"] / win
    out["iowq_nonvoluntary_status"] = iowq["nonvoluntary"] / win
    out["iowq_nr_migrations"] = iowq["nr_migrations"]
    out["iowq_sleep_derived_ns"] = (
        iowq["thread_lifetimes_s"] * 1e9 - iowq["on_cpu_ns"] - iowq["runwait_ns"]
    ) / win
    out["iowq_timeslice_mean_ns"] = (
        iowq["on_cpu_ns"] / iowq["timeslices"] if iowq["timeslices"] else None
    )
    psi = rec.get("psi_cpu_total_us") or {}
    if psi.get("pre") is not None and psi.get("post") is not None:
        out["psi_cpu_total_us"] = (psi["post"] - psi["pre"]) / win
    out["samples"] = th["samples"]
    out["errors"] = th["errors"]
    out["sampler_span_s"] = th["sample_span_s"]
    return out, True


def bootstrap_ci(diffs, resamples=10000, seed=5950):
    if not diffs:
        return None
    rng = random.Random(seed)
    n = len(diffs)
    means = []
    for _ in range(resamples):
        means.append(sum(rng.choice(diffs) for _ in range(n)) / n)
    means.sort()
    return means[int(0.025 * resamples)], means[int(0.975 * resamples)]


def analyze_block(records):
    pairs = {}
    for rec in records:
        if rec["rc"] != 0:
            continue
        view, sampled = invocation_view(rec)
        pairs.setdefault(rec["pair"], {})[rec["arm"]] = (view, sampled)
    results = {"n_pairs": 0, "metrics": {}}
    complete = {p: v for p, v in pairs.items() if "A" in v and "B" in v}
    results["n_pairs"] = len(complete)
    metric_names = METRIC_ALL + METRICS_BENCHVIEW
    for name in metric_names:
        abs_a, abs_b, rel, rel_lift = [], [], [], []
        skip = False
        for p in sorted(complete):
            va, _ = complete[p]["A"]
            vb, _ = complete[p]["B"]
            x, y = va.get(name), vb.get(name)
            if x is None or y is None:
                skip = True
                break
            abs_a.append(x)
            abs_b.append(y)
            if x != 0:
                rel.append((y - x) / x * 100.0)
                if LIFT.get(name, True):
                    rel_lift.append((y - x) / x * 100.0)
                else:
                    rel_lift.append((x - y) / x * 100.0)
        if skip or not abs_a:
            continue
        mean_a = sum(abs_a) / len(abs_a)
        mean_b = sum(abs_b) / len(abs_b)
        entry = {
            "mean_A": mean_a, "mean_B": mean_b,
            "delta_abs": mean_b - mean_a,
            "rel_pct": sum(rel) / len(rel) if rel else None,
        }
        ci = bootstrap_ci(rel)
        if ci:
            entry["rel_ci95"] = list(ci)
        ci_abs = bootstrap_ci([b - a for a, b in zip(abs_a, abs_b)])
        if ci_abs:
            entry["delta_abs_ci95"] = list(ci_abs)
        results["metrics"][name] = entry
    return results


def main():
    out = {}
    for label, (arms, desc) in BLOCKS.items():
        path = sys.argv[1].replace("LABEL", label) if len(sys.argv) > 1 else None
        records = []
        try:
            with open(path) as f:
                for line in f:
                    line = line.strip()
                    if line:
                        records.append(json.loads(line))
        except (FileNotFoundError, TypeError):
            continue
        out[label] = {"desc": desc, **analyze_block(records)}
    print(json.dumps(out, indent=1))


if __name__ == "__main__":
    main()
