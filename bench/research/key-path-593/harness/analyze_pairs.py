#!/usr/bin/env python3
"""#593 K2-a native paired experiment — interleaved AB/BA analysis.

Reads pairs-<tag>-<mode>/full-<i>.jsonl and {var,full2}-<i>.jsonl, computes
per-run medians over the 7 measured repetitions, then paired deltas
(variant - baseline) / baseline * 100.  A negative delta means the variant is
lower; for throughput a positive delta means the variant is higher.

Every input run was already validated by bench/check_run.py (which also
enforces the in-process CAL-oracle self-test and the per-repetition byte
verification phase); this script re-checks record counts and ok flags.
"""
from __future__ import annotations

import json
import math
import os
import random
import statistics
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PAIRS_EXPECTED = 15

METRICS = {
    "wall_us_per_op": lambda r: r["wall_s"] / r["ops"] * 1e6,
    "cpu_us_per_op": lambda r: r["cpu_s"] / r["ops"] * 1e6,
    "throughput_ops_s": lambda r: r["ops_per_sec"],
    "cpu_util": lambda r: r["cpu_util"],
    "p50_ns": lambda r: r["lat_p50_ns"],
    "p95_ns": lambda r: r["lat_p95_ns"],
    "p99_ns": lambda r: r["lat_p99_ns"],
    "max_ns": lambda r: r["lat_max_ns"],
    "stats_poll_calls_per_op": lambda r: r["stats_poll_calls"] / r["ops"],
    "stats_wait_calls_per_op": lambda r: r["stats_wait_calls"] / r["ops"],
    "stats_submit_calls_per_op": lambda r: r["stats_submit_calls"] / r["ops"],
    "stats_completed_ops_per_op": lambda r: r["stats_completed_ops"] / r["ops"],
    "waits_per_op": lambda r: r["waits"] / r["ops"],
    "submits_per_op": lambda r: r["submits"] / r["ops"],
    "notifications_per_op": lambda r: r["notifications"] / r["ops"],
    "passes_per_op": lambda r: r["passes"] / r["ops"],
    "empty_wakes_per_op": lambda r: r["empty_wakes"] / r["ops"],
    "queue_full_retries_per_op": lambda r: r["stats_queue_full_retries"] / r["ops"],
    "invalid_state_per_op": lambda r: r["stats_invalid_state"] / r["ops"],
}


def read_run(path: Path):
    rows = [json.loads(line) for line in path.read_text().splitlines() if line.strip()]
    work = [r for r in rows if not r["case_id"].startswith("CAL-")]
    measured = [r for r in work if r["measured"]]
    if len(measured) != 7 or any(not r["ok"] for r in measured):
        raise ValueError(f"{path}: {len(measured)} measured records or a failed record")
    return measured


def run_metrics(path: Path):
    measured = read_run(path)
    return {name: statistics.median(fn(r) for r in measured) for name, fn in METRICS.items()}


def pct_delta(base, var):
    if base == 0:
        return 0.0 if var == 0 else float("inf")
    return (var - base) / base * 100.0


def bootstrap_ci(values, iterations=10000, seed=593):
    rng = random.Random(seed)
    medians = []
    n = len(values)
    for _ in range(iterations):
        sample = [values[rng.randrange(n)] for _ in range(n)]
        medians.append(statistics.median(sample))
    medians.sort()
    return [medians[int(0.025 * iterations)], medians[int(0.975 * iterations) - 1]]


def analyze_dir(d: Path):
    mode = "aa-full" if d.name.endswith("-aa-full") else "elide-vs-full"
    second = "full2" if mode == "aa-full" else "var"
    pairs = []
    problems = []
    for i in range(PAIRS_EXPECTED):
        a = d / f"full-{i}.jsonl"
        b = d / f"{second}-{i}.jsonl"
        try:
            ma = run_metrics(a)
            mb = run_metrics(b)
        except FileNotFoundError:
            problems.append(f"pair {i}: missing {a.name} or {b.name}")
            continue
        except ValueError as exc:
            problems.append(f"pair {i}: {exc}")
            continue
        pairs.append({"pair": i, "base": ma, "var": mb})
    if problems:
        return {"dir": d.name, "mode": mode, "valid_pairs": len(pairs),
                "problems": problems}
    result = {"dir": d.name, "mode": mode, "valid_pairs": len(pairs),
              "problems": [], "metrics": {}, "per_pair": []}
    for name in METRICS:
        deltas = [pct_delta(p["base"][name], p["var"][name]) for p in pairs]
        finite = [x for x in deltas if math.isfinite(x)]
        base_values = [p["base"][name] for p in pairs]
        var_values = [p["var"][name] for p in pairs]
        result["metrics"][name] = {
            "base_median": statistics.median(base_values),
            "var_median": statistics.median(var_values),
            "delta_pct_median": statistics.median(finite) if finite else None,
            "delta_pct_min": min(finite) if finite else None,
            "delta_pct_max": max(finite) if finite else None,
            "delta_pct_ci95": bootstrap_ci(finite) if finite else None,
            "neg_pairs": sum(1 for x in finite if x < 0),
            "pos_pairs": sum(1 for x in finite if x > 0),
            "zero_base_pairs": len(deltas) - len(finite),
            "n": len(finite),
        }
    for p in pairs:
        result["per_pair"].append({
            "pair": p["pair"],
            "base": p["base"],
            "var": p["var"],
            "delta_pct": {
                name: pct_delta(p["base"][name], p["var"][name])
                for name in METRICS
            },
        })
    return result


def fmt_metric(m):
    if m["delta_pct_median"] is None:
        return "n/a (zero baseline in all pairs)"
    lo, hi = m["delta_pct_ci95"]
    return (f"median={m['delta_pct_median']:+.2f}% ci95=[{lo:+.2f},{hi:+.2f}] "
            f"range=[{m['delta_pct_min']:+.2f},{m['delta_pct_max']:+.2f}] "
            f"neg={m['neg_pairs']}/{m['n']} zero_base={m['zero_base_pairs']}")


def main() -> int:
    dirs = sorted(p for p in ROOT.iterdir() if p.is_dir() and p.name.startswith("pairs-"))
    if not dirs:
        print("no pairs-* directories", file=sys.stderr)
        return 1
    results = []
    for d in dirs:
        r = analyze_dir(d)
        results.append(r)
        print(f"== {r['dir']}  valid_pairs={r['valid_pairs']}")
        for problem in r["problems"]:
            print(f"   PROBLEM: {problem}")
        if "metrics" not in r:
            continue
        print(f"   base wall/cpu per op = {r['metrics']['wall_us_per_op']['base_median']:.2f} / "
              f"{r['metrics']['cpu_us_per_op']['base_median']:.2f} us; "
              f"var = {r['metrics']['wall_us_per_op']['var_median']:.2f} / "
              f"{r['metrics']['cpu_us_per_op']['var_median']:.2f} us")
        for name in ("wall_us_per_op", "cpu_us_per_op", "throughput_ops_s", "p50_ns",
                     "p99_ns", "stats_poll_calls_per_op", "stats_wait_calls_per_op",
                     "notifications_per_op", "passes_per_op", "waits_per_op"):
            if name in r["metrics"]:
                print(f"   {name:28s} {fmt_metric(r['metrics'][name])}")
    (ROOT / "pair-results.json").write_text(json.dumps(results, indent=2) + "\n")
    print(f"wrote {ROOT / 'pair-results.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
