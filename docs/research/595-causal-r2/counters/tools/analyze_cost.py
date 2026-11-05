#!/usr/bin/env python3
"""Paired analysis for the #595 causal-r2 cost-accounting blocks."""

import json
import os
import random
import statistics
import sys

ROOT = "/home/jnhu/sluice-595-e5/causal-r2/evidence/counters/raw"
CASE_OF = {
    "pmu-d2-ab": "W02-uring-4k-d2",
    "pmu-d2-aa": "W02-uring-4k-d2",
    "pmu-d1-ab": "W02-uring-4k-d1",
    "cnt-on-full-d2": "W02-uring-4k-d2",
    "cnt-off-full-d2": "W02-uring-4k-d2",
    "cnt-on-pmu-d2": "W02-uring-4k-d2",
    "cnt-off-pmu-d2": "W02-uring-4k-d2",
    "frozen-full-d2": "W02-uring-4k-d2",
    "cnt-on-full-d1": "W02-uring-4k-d1",
    "cnt-off-full-d1": "W02-uring-4k-d1",
    "split-d2": "W02-uring-4k-d2",
    "split-d1": "W02-uring-4k-d1",
    "pmu-d1x-ab": "W02-uring-4k-d1",
    "cnt-on-full-d1x": "W02-uring-4k-d1",
    "cnt-off-full-d1x": "W02-uring-4k-d1",
    "split-d1x": "W02-uring-4k-d1",
}
PMU_EVENTS = ["cycles:u", "instructions:u", "cache-references:u", "cache-misses:u",
              "branch-misses:u", "task-clock:u", "page-faults:u", "minor-faults:u",
              "major-faults:u"]
COUNTERS = ["dispatch_passes", "poll_passes", "empty_polls", "sqe_preparations",
            "sqes_deferred", "flushes", "submit_attempts", "actual_enters",
            "wait_attempts", "cqe_reap_attempts", "cqes_reaped",
            "progress_handoffs", "wakeups", "wakeup_writes", "wakeup_coalesced"]


def load(path):
    out = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line:
                out.append(json.loads(line))
    return out


def boot_ci(values, seed=5950, boot=10000):
    n = len(values)
    rng = random.Random(seed)
    means = []
    for _ in range(boot):
        means.append(sum(values[rng.randrange(n)] for _ in range(n)) / n)
    means.sort()
    return {
        "n": n,
        "mean": sum(values) / n,
        "median": statistics.median(values),
        "ci_lo": means[int(0.025 * boot)],
        "ci_hi": means[min(int(0.975 * boot), boot - 1)],
        "sd": statistics.stdev(values) if n > 1 else 0.0,
    }


def diff_ci(a, b, seed=5950, boot=10000):
    rng = random.Random(seed)
    diffs = []
    for _ in range(boot):
        ma = sum(a[rng.randrange(len(a))] for _ in range(len(a))) / len(a)
        mb = sum(b[rng.randrange(len(b))] for _ in range(len(b))) / len(b)
        if ma:
            diffs.append((mb - ma) / ma)
    diffs.sort()
    if not diffs:
        return {"ci_lo": 0.0, "ci_hi": 0.0, "defined": False}
    return {"ci_lo": diffs[int(0.025 * boot)], "ci_hi": diffs[min(int(0.975 * boot), boot - 1)],
            "defined": True}


def invocation_metrics(record, case):
    recs = [r for r in record["records"] if r.get("case_id") == case]
    measured = [r for r in recs if r.get("measured")]
    if not recs or not measured:
        return None
    ops_measured = sum(r["ops"] for r in measured)
    cpu_s = sum(r["cpu_s"] for r in measured)
    wall_s = sum(r["wall_s"] for r in measured)
    p95 = sum(r["lat_p95_ns"] for r in measured) / len(measured)
    p99 = sum(r["lat_p99_ns"] for r in measured) / len(measured)
    passes = len(recs) * (1 if record["pmu_only"] else 3)
    ops_window = passes * 20000

    m = {
        "ops_measured": ops_measured,
        "ops_window": ops_window,
        "cpu_ns_per_op": 1e9 * cpu_s / ops_measured,
        "tput": ops_measured / wall_s if wall_s else None,
        "p95_ns": p95,
        "p99_ns": p99,
        "bench_user_s": record["threads"]["bench"]["user_s"],
        "bench_kernel_s": record["threads"]["bench"]["kernel_s"],
        "iowq_kernel_s": record["threads"]["iowq"]["kernel_s"],
        "bench_voluntary": record["threads"]["bench"]["voluntary"],
        "bench_nonvoluntary": record["threads"]["bench"]["nonvoluntary"],
        "iowq_voluntary": record["threads"]["iowq"]["voluntary"],
        "iowq_nonvoluntary": record["threads"]["iowq"]["nonvoluntary"],
        "bench_migrations": record["threads"]["bench"]["migrations"],
        "iowq_migrations": record["threads"]["iowq"]["migrations"],
        "sample_errors": record["threads"]["errors"],
    }
    if record["mode"] == "pmu":
        for event in PMU_EVENTS:
            entry = record["perf"].get(event)
            if entry and entry["value"] is not None:
                m[event + "/op"] = entry["value"] / ops_window
                m[event + "_pct"] = entry["pct"]
        cyc = record["perf"].get("cycles:u", {}).get("value")
        ins = record["perf"].get("instructions:u", {}).get("value")
        m["ipc"] = (ins / cyc) if cyc else None
    if record["counter_lines"]:
        counts = {}
        for line in record["counter_lines"]:
            for key in COUNTERS:
                counts[key] = counts.get(key, 0) + line.get(key, 0)
        for key in COUNTERS:
            m["cnt_" + key + "/op"] = counts[key] / ops_measured
        m["counter_lines"] = len(record["counter_lines"])
    return m


def block_metrics(label):
    case = CASE_OF[label]
    records = load(os.path.join(ROOT, label, "results.jsonl"))
    metrics = []
    for record in records:
        m = invocation_metrics(record, case)
        if m is None:
            print("WARN missing metrics %s pair=%s pos=%s" % (label, record["pair"], record["pos"]),
                  file=sys.stderr)
            continue
        m.update({"pair": record["pair"], "pos": record["pos"], "arm": record["arm"],
                  "rc": record["rc"], "elapsed_s": record["elapsed_s"]})
        metrics.append(m)
    return metrics


def pairs_of(metrics, arms):
    by_pair = {}
    for m in metrics:
        by_pair.setdefault(m["pair"], {})[m["pos"]] = m
    out = []
    for pair in sorted(by_pair):
        d = by_pair[pair]
        if 0 not in d or 1 not in d:
            continue
        if arms == "AB":
            a = next(m for m in (d[0], d[1]) if m["arm"] == "A")
            b = next(m for m in (d[0], d[1]) if m["arm"] == "B")
        else:
            a, b = d[0], d[1]
        out.append((a, b))
    return out


def summarize(label, arms):
    metrics = block_metrics(label)
    pairs = pairs_of(metrics, arms)
    excluded = {"pair", "pos", "arm", "rc", "elapsed_s", "ops_measured", "ops_window",
                "counter_lines"}
    keys = set()
    for m in metrics:
        keys.update(k for k in m if k not in excluded and not k.endswith("_pct"))
    out = {"label": label, "n_pairs": len(pairs), "metrics": {}}
    sampler_keys = {"bench_user_s", "bench_kernel_s", "iowq_kernel_s", "bench_voluntary",
                    "bench_nonvoluntary", "iowq_voluntary", "iowq_nonvoluntary",
                    "bench_migrations", "iowq_migrations"}
    for key in sorted(keys):
        vals = []
        abs_vals = []
        for a, b in pairs:
            if key == "tput":
                if a[key] and b[key]:
                    vals.append((a[key] - b[key]) / a[key])
            elif key in sampler_keys:
                denom = a["ops_window"]
                va = a[key] / denom
                vb = b[key] / denom
                if va:
                    vals.append((vb - va) / va)
                abs_vals.append(vb - va)
            else:
                if a[key]:
                    vals.append((b[key] - a[key]) / a[key])
                abs_vals.append(b[key] - a[key])
        vals = [v for v in vals if v is not None]
        entry = {"arm_a_mean": statistics.mean(a[key] for a, _ in pairs),
                 "arm_b_mean": statistics.mean(b[key] for _, b in pairs),
                 "n": len(pairs)}
        if vals:
            entry.update(boot_ci(vals))
            entry["rel"] = entry["mean"]
        if abs_vals:
            entry["abs_mean"] = statistics.mean(abs_vals)
            entry["abs_ci"] = [boot_ci(abs_vals)["ci_lo"], boot_ci(abs_vals)["ci_hi"]]
        out["metrics"][key] = entry
    return out


def independent_delta(label_a, label_b, keys):
    ma = block_metrics(label_a)
    mb = block_metrics(label_b)
    out = {}
    for key in keys:
        va = [m[key] for m in ma if m.get(key) is not None]
        vb = [m[key] for m in mb if m.get(key) is not None]
        if not va or not vb:
            continue
        mean_a = statistics.mean(va)
        mean_b = statistics.mean(vb)
        boot = diff_ci(va, vb)
        out[key] = {"a_mean": mean_a, "b_mean": mean_b,
                    "rel": (mean_b - mean_a) / mean_a if mean_a else None,
                    "abs": mean_b - mean_a,
                    "ci_lo": boot["ci_lo"], "ci_hi": boot["ci_hi"],
                    "ci_defined": boot["defined"],
                    "n_a": len(va), "n_b": len(vb)}
    return out


def main():
    result = {}
    for label, arms in [("pmu-d2-ab", "AB"), ("pmu-d2-aa", "AA"), ("pmu-d1-ab", "AB"),
                        ("cnt-on-full-d2", "AB"), ("cnt-off-full-d2", "AB"),
                        ("cnt-on-pmu-d2", "AB"), ("cnt-off-pmu-d2", "AB"),
                        ("frozen-full-d2", "AB"),
                        ("cnt-on-full-d1", "AB"), ("cnt-off-full-d1", "AB"),
                        ("split-d2", "AB"), ("split-d1", "AB"),
                        ("pmu-d1x-ab", "AB"), ("cnt-on-full-d1x", "AB"),
                        ("cnt-off-full-d1x", "AB"), ("split-d1x", "AB")]:
        result[label] = summarize(label, arms)

    keys_full = ["cpu_ns_per_op", "tput", "p95_ns", "p99_ns", "bench_user_s",
                 "bench_kernel_s", "iowq_kernel_s", "bench_voluntary",
                 "bench_nonvoluntary", "iowq_voluntary", "iowq_nonvoluntary",
                 "bench_migrations", "iowq_migrations"]
    keys_counter = ["cpu_ns_per_op", "tput"] + ["cnt_" + c + "/op" for c in COUNTERS]
    keys_pmu = ["cpu_ns_per_op", "tput", "ipc"] + [e + "/op" for e in PMU_EVENTS]

    result["overhead_on_vs_off_full_d2"] = independent_delta(
        "cnt-off-full-d2", "cnt-on-full-d2", keys_full + keys_counter)
    result["overhead_on_vs_off_pmu_d2"] = independent_delta(
        "cnt-off-pmu-d2", "cnt-on-pmu-d2", keys_counter)
    result["off_vs_frozen_full_d2"] = independent_delta(
        "frozen-full-d2", "cnt-off-full-d2", keys_full)
    result["counter_on_vs_off_full_d1"] = independent_delta(
        "cnt-off-full-d1", "cnt-on-full-d1", keys_counter)
    result["counter_on_vs_off_full_d1x"] = independent_delta(
        "cnt-off-full-d1x", "cnt-on-full-d1x", keys_counter)
    result["pmu_vs_frozen_cpu"] = independent_delta(
        "frozen-full-d2", "pmu-d2-ab", ["cpu_ns_per_op"])

    print(json.dumps(result, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
