#!/usr/bin/env python3
"""M2 A/B analysis: paired arm ratios against the baseline arm.

Reads the run_ab.sh JSONL, validates every repetition (oracle PASS, timed phase
PASS with no failure, zero timed allocations, one timed row per arm/config/rep)
and reports the paired ratio distribution with a bootstrap confidence interval.
CV thresholds are not used as an equivalence proof.
"""

import argparse
import collections
import json
import random
import statistics
import sys

ARMS = ["base", "h1", "h2a", "h2b", "h2ab"]


def load(path):
    rows = []
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    return rows


def median(values):
    return statistics.median(values)


def bootstrap_ci(values, iterations=10000, alpha=0.05, seed=20261021):
    if not values:
        return (float("nan"), float("nan"))
    rng = random.Random(seed)
    n = len(values)
    samples = []
    for _ in range(iterations):
        samples.append(median([values[rng.randrange(n)] for _ in range(n)]))
    samples.sort()
    lo = samples[max(0, int(alpha / 2 * iterations) - 1)]
    hi = samples[min(iterations - 1, int((1 - alpha / 2) * iterations) - 1)]
    return (lo, hi)


def config_key(row):
    return (row["block"], row["capacity"], row["depth"])


def label(key):
    block, capacity, depth = key
    return f"block={block} capacity={capacity} depth={depth}"


def validate(rows, reps):
    problems = []
    grouped = collections.defaultdict(list)
    for row in rows:
        grouped[(config_key(row), row["arm"], row["rep"], row["phase"])].append(row)
    configs = sorted({config_key(r) for r in rows})
    for key in configs:
        for arm in ARMS:
            for rep in range(1, reps + 1):
                timed = grouped.get((key, arm, rep, "timed"), [])
                oracle = grouped.get((key, arm, rep, "oracle"), [])
                latency = grouped.get((key, arm, rep, "latency"), [])
                if len(timed) != 1:
                    problems.append(f"{label(key)} arm={arm} rep={rep}: {len(timed)} timed rows")
                    continue
                if len(oracle) != 1 or not oracle[0]["ok"]:
                    problems.append(f"{label(key)} arm={arm} rep={rep}: oracle not PASS")
                if not timed[0]["ok"] or timed[0]["failure"]:
                    problems.append(
                        f"{label(key)} arm={arm} rep={rep}: timed phase failed: "
                        f"{timed[0]['failure'] or 'ok=false'}"
                    )
                if len(latency) != 1 or not latency[0]["ok"]:
                    problems.append(f"{label(key)} arm={arm} rep={rep}: latency phase failed")
                if timed[0]["allocations"] != 0:
                    problems.append(
                        f"{label(key)} arm={arm} rep={rep}: "
                        f"{timed[0]['allocations']} timed allocations"
                    )
                if timed[0]["ops"] != timed[0]["requests"]:
                    problems.append(f"{label(key)} arm={arm} rep={rep}: op count mismatch")
    return problems


def paired_values(rows, key, phase, field):
    by_rep = collections.defaultdict(dict)
    for row in rows:
        if config_key(row) == key and row["phase"] == phase:
            by_rep[row["rep"]][row["arm"]] = row[field]
    return by_rep


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("jsonl")
    parser.add_argument("--reps", type=int, default=15)
    parser.add_argument("--json-out")
    args = parser.parse_args()

    rows = load(args.jsonl)
    problems = validate(rows, args.reps)
    configs = sorted({config_key(r) for r in rows})
    result = {"configs": [], "validation": problems}

    for key in configs:
        wall = paired_values(rows, key, "timed", "wall_s")
        cpu = paired_values(rows, key, "timed", "cpu_ns_per_op")
        latency = paired_values(rows, key, "latency", "p99_ns")
        entry = {"config": {"block": key[0], "capacity": key[1], "depth": key[2]}, "arms": {}}
        base_wall = [wall[r]["base"] for r in sorted(wall) if "base" in wall[r]]
        entry["base_wall_median_s"] = median(base_wall)
        entry["base_cpu_ns_per_op_median"] = median(
            [cpu[r]["base"] for r in sorted(cpu) if "base" in cpu[r]]
        )
        entry["base_p99_ns_median"] = median(
            [latency[r]["base"] for r in sorted(latency) if "base" in latency[r]]
        )
        for arm in ARMS:
            ratios = []
            cpu_ratios = []
            p99_ratios = []
            for rep in sorted(wall):
                if arm in wall[rep] and "base" in wall[rep] and wall[rep]["base"] > 0:
                    ratios.append(wall[rep][arm] / wall[rep]["base"])
                if arm in cpu[rep] and "base" in cpu[rep] and cpu[rep]["base"] > 0:
                    cpu_ratios.append(cpu[rep][arm] / cpu[rep]["base"])
                if (
                    arm in latency[rep]
                    and "base" in latency[rep]
                    and latency[rep]["base"] > 0
                ):
                    p99_ratios.append(latency[rep][arm] / latency[rep]["base"])
            if arm == "base":
                entry["arms"][arm] = {"pairs": len(ratios)}
                continue
            low, high = bootstrap_ci(ratios)
            cpu_low, cpu_high = bootstrap_ci(cpu_ratios)
            p99_low, p99_high = bootstrap_ci(p99_ratios)
            entry["arms"][arm] = {
                "pairs": len(ratios),
                "wall_ratio_median": median(ratios) if ratios else None,
                "wall_ratio_ci95": [low, high],
                "cpu_ratio_median": median(cpu_ratios) if cpu_ratios else None,
                "cpu_ratio_ci95": [cpu_low, cpu_high],
                "p99_ratio_median": median(p99_ratios) if p99_ratios else None,
                "p99_ratio_ci95": [p99_low, p99_high],
            }
        result["configs"].append(entry)

    for entry in result["configs"]:
        key = entry["config"]
        print(
            f"\nblock={key['block']} capacity={key['capacity']} depth={key['depth']} "
            f"base_wall={entry['base_wall_median_s']*1e6:.1f}us "
            f"base_cpu={entry['base_cpu_ns_per_op_median']:.0f}ns/op"
        )
        for arm in ARMS[1:]:
            data = entry["arms"][arm]
            if not data["pairs"]:
                continue
            print(
                f"  {arm:5s} pairs={data['pairs']:2d} "
                f"wall x{data['wall_ratio_median']:.3f} "
                f"[{data['wall_ratio_ci95'][0]:.3f},{data['wall_ratio_ci95'][1]:.3f}] "
                f"cpu x{data['cpu_ratio_median']:.3f} "
                f"p99 x{data['p99_ratio_median']:.3f}"
            )

    if problems:
        print("\nVALIDATION PROBLEMS:")
        for problem in problems[:40]:
            print("  " + problem)
        print(f"  total={len(problems)}")
    else:
        print(
            "\nvalidation: all repetitions PASS "
            "(oracle, timed phase, op count, zero timed allocations)"
        )

    if args.json_out:
        with open(args.json_out, "w", encoding="utf-8") as handle:
            json.dump(result, handle, indent=2)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
