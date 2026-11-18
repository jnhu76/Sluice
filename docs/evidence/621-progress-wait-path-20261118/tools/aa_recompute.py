#!/usr/bin/env python3
"""Independent recomputation of paired A/A (or A/B) differences for #621.

Reads the runner's raw ``arms.jsonl`` and/or ``pairs.tsv`` and prints a JSON
summary to stdout.  It deliberately recomputes everything from the per-arm
records: paired differences (B - A), mean/median/CV, a fixed-seed bootstrap
95% CI half-width, AB/BA groups and drift over the pair ordinal.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import random
import sys
from collections import defaultdict

TOOL_VERSION = "1.0"
METRICS = (
    "auto",
    "wall_s",
    "cpu_us",
    "case_wall_s",
    "case_ops_per_sec",
    "case_cpu_s",
)
TIME_METRICS = {"wall_s", "cpu_us", "case_wall_s", "case_cpu_s"}


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            block = f.read(1 << 20)
            if not block:
                break
            h.update(block)
    return h.hexdigest()


def mean(values):
    return sum(values) / len(values) if values else None


def median(values):
    if not values:
        return None
    ordered = sorted(values)
    n = len(ordered)
    mid = n // 2
    if n % 2:
        return ordered[mid]
    return (ordered[mid - 1] + ordered[mid]) / 2.0


def sample_std(values):
    if len(values) < 2:
        return None
    m = mean(values)
    return math.sqrt(sum((v - m) ** 2 for v in values) / (len(values) - 1))


def percentile(sorted_values, p):
    if not sorted_values:
        return None
    if len(sorted_values) == 1:
        return sorted_values[0]
    k = (len(sorted_values) - 1) * p / 100.0
    lo = math.floor(k)
    hi = math.ceil(k)
    if lo == hi:
        return sorted_values[int(k)]
    return sorted_values[lo] * (hi - k) + sorted_values[hi] * (k - lo)


def bootstrap_ci(values, iterations, seed):
    if len(values) < 2 or iterations < 1:
        return None
    rng = random.Random(seed)
    n = len(values)
    means = []
    for _ in range(iterations):
        total = 0.0
        for _ in range(n):
            total += values[rng.randrange(n)]
        means.append(total / n)
    means.sort()
    low = percentile(means, 2.5)
    high = percentile(means, 97.5)
    return {
        "low": low,
        "high": high,
        "half_width": (high - low) / 2.0,
        "iterations": iterations,
        "seed": seed,
    }


def describe(values, iterations, seed):
    if not values:
        return {
            "n": 0,
            "mean": None,
            "median": None,
            "stddev": None,
            "cv": None,
            "ci95": None,
        }
    m = mean(values)
    sd = sample_std(values)
    return {
        "n": len(values),
        "mean": m,
        "median": median(values),
        "stddev": sd,
        "cv": (sd / abs(m)) if (sd is not None and m not in (None, 0.0)) else None,
        "ci95": bootstrap_ci(values, iterations, seed),
    }


def ols(xs, ys):
    n = len(xs)
    if n < 2:
        return None
    mx = mean(xs)
    my = mean(ys)
    sxx = sum((x - mx) ** 2 for x in xs)
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    if sxx == 0:
        return None
    slope = sxy / sxx
    intercept = my - slope * mx
    syy = sum((y - my) ** 2 for y in ys)
    r = sxy / math.sqrt(sxx * syy) if syy > 0 else None
    return {"slope": slope, "intercept": intercept, "pearson_r": r}


def load_arms(path):
    records = []
    with open(path, "r", encoding="utf-8") as f:
        for lineno, line in enumerate(f, start=1):
            if not line.strip():
                continue
            try:
                records.append(json.loads(line))
            except json.JSONDecodeError as exc:
                raise ValueError(f"{path}:{lineno}: {exc}") from exc
    return records


def load_pairs(path):
    with open(path, "r", encoding="utf-8") as f:
        header = f.readline().rstrip("\n").split("\t")
        rows = []
        for lineno, line in enumerate(f, start=2):
            if not line.strip():
                continue
            values = line.rstrip("\n").split("\t")
            if len(values) != len(header):
                raise ValueError(
                    f"{path}:{lineno}: {len(values)} fields, expected {len(header)}"
                )
            rows.append(dict(zip(header, values)))
    return rows


def as_float(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def as_int(value):
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def truthy(value):
    return str(value).strip().lower() in ("1", "true", "yes")


def metric_from_arm(arm, metric):
    if metric == "wall_s":
        return as_float(arm.get("wall_s"))
    if metric == "cpu_us":
        return as_float(arm.get("cpu_us"))
    bench = (arm.get("oracle") or {}).get("bench") or {}
    summary = bench.get("summary") or {}
    if metric == "case_wall_s":
        return as_float(summary.get("measured_wall_s_mean"))
    if metric == "case_ops_per_sec":
        return as_float(summary.get("measured_ops_per_sec_mean"))
    if metric == "case_cpu_s":
        return as_float(summary.get("measured_cpu_s_mean"))
    raise ValueError(f"unknown metric {metric}")


def metric_from_row(row, metric):
    column = {
        "wall_s": "wall_s",
        "cpu_us": "cpu_us",
        "case_wall_s": "case_measured_wall_s_mean",
        "case_ops_per_sec": "case_measured_ops_per_sec_mean",
        "case_cpu_s": "case_measured_cpu_s_mean",
    }.get(metric)
    if column is None:
        raise ValueError(f"unknown metric {metric}")
    return as_float(row.get(column))


def resolve_metric(requested, kinds):
    if requested != "auto":
        return requested
    if not kinds:
        return "wall_s"
    if kinds == {"sluice_copy"}:
        return "wall_s"
    if kinds <= {"bench_w02", "bench_w03"}:
        return "case_wall_s"
    raise ValueError(f"cannot auto-select a metric for mixed kinds: {sorted(kinds)}")


def extract_from_arms(records, metric):
    by_pair = defaultdict(dict)
    for record in records:
        by_pair[record["pair"]][record["arm"]] = record
    diffs = []
    excluded = []
    for pair in sorted(by_pair):
        arms = by_pair[pair]
        reasons = []
        if "A" not in arms or "B" not in arms:
            reasons.append("missing_arm")
        for arm in ("A", "B"):
            record = arms.get(arm)
            if record is not None and not record.get("valid", False):
                reasons.append(f"arm_{arm}_invalid")
        if reasons:
            excluded.append({"pair": pair, "reasons": sorted(set(reasons))})
            continue
        a = metric_from_arm(arms["A"], metric)
        b = metric_from_arm(arms["B"], metric)
        if a is None or b is None:
            excluded.append({"pair": pair, "reasons": [f"metric_missing:{metric}"]})
            continue
        diffs.append(
            {
                "pair": pair,
                "order": arms["A"].get("order"),
                "a": a,
                "b": b,
                "diff": b - a,
            }
        )
    return diffs, excluded


def extract_from_rows(rows, metric):
    by_pair = defaultdict(dict)
    for row in rows:
        pair = as_int(row.get("pair"))
        if pair is None:
            continue
        by_pair[pair][row.get("arm")] = row
    diffs = []
    excluded = []
    for pair in sorted(by_pair):
        arms = by_pair[pair]
        reasons = []
        if "A" not in arms or "B" not in arms:
            reasons.append("missing_arm")
        for arm in ("A", "B"):
            row = arms.get(arm)
            if row is not None and not truthy(row.get("valid")):
                reasons.append(f"arm_{arm}_invalid")
        if reasons:
            excluded.append({"pair": pair, "reasons": sorted(set(reasons))})
            continue
        a = metric_from_row(arms["A"], metric)
        b = metric_from_row(arms["B"], metric)
        if a is None or b is None:
            excluded.append({"pair": pair, "reasons": [f"metric_missing:{metric}"]})
            continue
        diffs.append(
            {
                "pair": pair,
                "order": arms["A"].get("order"),
                "a": a,
                "b": b,
                "diff": b - a,
            }
        )
    return diffs, excluded


def cross_check(records, rows, metric):
    arm_map = {}
    for record in records:
        arm_map[(record["pair"], record["arm"])] = record
    row_map = {}
    for row in rows:
        pair = as_int(row.get("pair"))
        if pair is not None:
            row_map[(pair, row.get("arm"))] = row
    mismatches = []
    for key in sorted(set(arm_map) | set(row_map)):
        arm = arm_map.get(key)
        row = row_map.get(key)
        if arm is None or row is None:
            mismatches.append({"pair": key[0], "arm": key[1], "reason": "missing_side"})
            continue
        if bool(arm.get("valid")) != truthy(row.get("valid")):
            mismatches.append({"pair": key[0], "arm": key[1], "reason": "valid_differs"})
        arm_metric = metric_from_arm(arm, metric)
        row_metric = metric_from_row(row, metric)
        if (arm_metric is None) != (row_metric is None) or (
            arm_metric is not None
            and row_metric is not None
            and abs(arm_metric - row_metric) > 1e-6
        ):
            mismatches.append(
                {
                    "pair": key[0],
                    "arm": key[1],
                    "reason": "metric_differs",
                    "arms_value": arm_metric,
                    "pairs_value": row_metric,
                }
            )
    return {
        "arms_records": len(records),
        "pairs_rows": len(rows),
        "mismatches": mismatches,
    }


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Recompute paired differences from #621 session raw data",
        allow_abbrev=False,
    )
    parser.add_argument("--arms", help="path to arms.jsonl")
    parser.add_argument("--pairs", help="path to pairs.tsv")
    parser.add_argument("--session-dir", help="directory containing arms.jsonl and pairs.tsv")
    parser.add_argument("--metric", choices=METRICS, default="auto")
    parser.add_argument("--bootstrap", type=int, default=10000)
    parser.add_argument("--seed", type=int, default=621)
    parser.add_argument("--indent", type=int, default=2)
    args = parser.parse_args()

    arms_path = args.arms
    pairs_path = args.pairs
    if args.session_dir:
        arms_path = arms_path or os.path.join(args.session_dir, "arms.jsonl")
        pairs_path = pairs_path or os.path.join(args.session_dir, "pairs.tsv")
    if not arms_path and not pairs_path:
        parser.error("provide --arms, --pairs or --session-dir")
    if args.bootstrap < 0:
        parser.error("--bootstrap must be >= 0")

    records = None
    rows = None
    try:
        if arms_path:
            if not os.path.isfile(arms_path):
                parser.error(f"--arms: not a file: {arms_path}")
            records = load_arms(arms_path)
        if pairs_path:
            if not os.path.isfile(pairs_path):
                parser.error(f"--pairs: not a file: {pairs_path}")
            rows = load_pairs(pairs_path)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))

    kinds = set()
    for record in records or []:
        if record.get("kind"):
            kinds.add(record["kind"])
    for row in rows or []:
        if row.get("kind"):
            kinds.add(row["kind"])
    try:
        metric = resolve_metric(args.metric, kinds)
    except ValueError as exc:
        parser.error(str(exc))

    if records is not None:
        diffs, excluded = extract_from_arms(records, metric)
        source = "arms.jsonl"
    else:
        diffs, excluded = extract_from_rows(rows, metric)
        source = "pairs.tsv"

    diff_values = [d["diff"] for d in diffs]
    stats = describe(diff_values, args.bootstrap, args.seed)
    groups = {}
    for index, order in enumerate(("AB", "BA")):
        values = [d["diff"] for d in diffs if d["order"] == order]
        groups[order] = describe(values, args.bootstrap, args.seed + 1 + index)

    drift = None
    if len(diffs) >= 2:
        xs = [d["pair"] for d in diffs]
        ys = [d["diff"] for d in diffs]
        regression = ols(xs, ys)
        ordered = sorted(diffs, key=lambda d: d["pair"])
        half = len(ordered) // 2
        early = ordered[:half]
        late = ordered[-half:] if half else []
        early_values = [d["diff"] for d in early]
        late_values = [d["diff"] for d in late]
        drift = {
            "ols_slope_per_pair": regression["slope"] if regression else None,
            "ols_intercept": regression["intercept"] if regression else None,
            "pearson_r": regression["pearson_r"] if regression else None,
            "early": {
                "n": len(early_values),
                "pairs": [d["pair"] for d in early],
                "mean": mean(early_values),
            },
            "late": {
                "n": len(late_values),
                "pairs": [d["pair"] for d in late],
                "mean": mean(late_values),
            },
            "late_minus_early_mean": (
                (mean(late_values) - mean(early_values))
                if early_values and late_values
                else None
            ),
        }

    a_values = [d["a"] for d in diffs]
    b_values = [d["b"] for d in diffs]
    result = {
        "tool": "aa_recompute.py",
        "tool_version": TOOL_VERSION,
        "inputs": {
            "source": source,
            "arms": os.path.abspath(arms_path) if arms_path else None,
            "arms_sha256": sha256_file(arms_path) if arms_path else None,
            "pairs": os.path.abspath(pairs_path) if pairs_path else None,
            "pairs_sha256": sha256_file(pairs_path) if pairs_path else None,
        },
        "metric": {
            "requested": args.metric,
            "resolved": metric,
            "difference": "B - A",
            "direction_note": (
                "positive diff means B is larger; for time metrics B is slower, "
                "for ops_per_sec B is faster"
            ),
            "scope": (
                "outer process (perf/taskset child)"
                if metric in ("wall_s", "cpu_us")
                else "bench measured-case records"
            ),
        },
        "bootstrap": {"iterations": args.bootstrap, "seed": args.seed},
        "n_pairs": len(diffs),
        "n_excluded_pairs": len(excluded),
        "excluded": excluded,
        "diffs": diffs,
        "stats": stats,
        "groups": groups,
        "drift": drift,
        "arm_means": {
            "A": mean(a_values),
            "B": mean(b_values),
        },
        "cross_check": (
            cross_check(records, rows, metric)
            if records is not None and rows is not None
            else None
        ),
    }
    if not diffs:
        result["warning"] = "no valid pairs; statistics are null"
    json.dump(result, sys.stdout, indent=args.indent, ensure_ascii=False)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
