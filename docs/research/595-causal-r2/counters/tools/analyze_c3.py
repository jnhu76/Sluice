#!/usr/bin/env python3
"""Analysis for #595 causal-r2 C3 interleaved ON/OFF blocks."""

import argparse
import json
import os
import random
import statistics
import sys

SEED = 5950
BOOT = 10000


def load(path):
    return [json.loads(line) for line in open(path) if line.strip()]


def boot_ci(values, seed=SEED, boot=BOOT):
    n = len(values)
    rng = random.Random(seed)
    means = []
    for _ in range(boot):
        means.append(sum(values[rng.randrange(n)] for _ in range(n)) / n)
    means.sort()
    return {"n": n, "mean": sum(values) / n, "median": statistics.median(values),
            "ci_lo": means[int(0.025 * boot)],
            "ci_hi": means[min(int(0.975 * boot), boot - 1)],
            "sd": statistics.stdev(values) if n > 1 else 0.0}


def rel_ci(values):
    return boot_ci(values)


def invocation_metrics(rec, case):
    recs = [r for r in rec["records"] if r.get("case_id") == case]
    measured = [r for r in recs if r.get("measured")]
    if not recs or not measured:
        return None
    ops = sum(r["ops"] for r in measured)
    cpu_s = sum(r["cpu_s"] for r in measured)
    wall_s = sum(r["wall_s"] for r in measured)
    m = {
        "ops": ops,
        "cpu_ns_per_op": 1e9 * cpu_s / ops if ops else None,
        "tput": ops / wall_s if wall_s else None,
        "p95_ns": sum(r["lat_p95_ns"] for r in measured) / len(measured),
        "p99_ns": sum(r["lat_p99_ns"] for r in measured) / len(measured),
        "bench_user_s": rec["threads"]["bench"]["user_s"],
        "bench_kernel_s": rec["threads"]["bench"]["kernel_s"],
        "iowq_kernel_s": rec["threads"]["iowq"]["kernel_s"],
        "bench_utime_ns_per_op": 1e9 * rec["threads"]["bench"]["user_s"] / ops,
        "bench_stime_ns_per_op": 1e9 * rec["threads"]["bench"]["kernel_s"] / ops,
        "iowq_stime_ns_per_op": 1e9 * rec["threads"]["iowq"]["kernel_s"] / ops,
        "sample_errors": rec["threads"]["errors"],
    }
    lines = rec.get("c3_lines") or []
    if lines:
        sums = {}
        for line in lines:
            for key, value in line.items():
                if isinstance(value, int):
                    sums[key] = sums.get(key, 0) + value
        tsc_hz = lines[0].get("tsc_hz", 0)
        m["c3_lines"] = len(lines)
        m["tsc_hz"] = tsc_hz
        m["c1_ops"] = ops
        for key in ("read_ok_count", "read_ok_tsc_sum",
                    "read_eagain_count", "read_eagain_tsc_sum",
                    "park_poll_count", "park_poll_tsc_sum",
                    "write_fd_count", "write_fd_tsc_sum",
                    "drain_park", "drain_ack",
                    "poll_wake_pollin", "poll_timeout", "poll_eintr",
                    "poll_other", "read_value_sum",
                    "read_ok_clean_count", "read_ok_clean_tsc_sum",
                    "read_ok_huge_count",
                    "read_eagain_clean_count", "read_eagain_clean_tsc_sum",
                    "read_eagain_huge_count",
                    "park_poll_clean_count", "park_poll_clean_tsc_sum",
                    "park_poll_huge_count",
                    "write_fd_clean_count", "write_fd_clean_tsc_sum",
                    "write_fd_huge_count"):
            m[key] = sums.get(key, 0)
        if tsc_hz:
            for key in ("read_ok", "read_eagain", "park_poll", "write_fd"):
                cnt = m.get(key + "_count", 0)
                tsum = m.get(key + "_tsc_sum", 0)
                m[key + "_ns_per_op"] = 1e9 * tsum / tsc_hz / ops if ops else None
                m[key + "_ns_per_call"] = 1e9 * tsum / tsc_hz / cnt if cnt else None
            m["read_total_count"] = m.get("read_ok_count",0)+m.get("read_eagain_count",0)
            m["read_total_tsc_sum"] = m.get("read_ok_tsc_sum",0)+m.get("read_eagain_tsc_sum",0)
            m["read_total_ns_per_call"] = (
                1e9 * m["read_total_tsc_sum"] / tsc_hz / m["read_total_count"]
                if m["read_total_count"] else None)
            m["read_total_ns_per_op"] = 1e9*m["read_total_tsc_sum"]/tsc_hz/ops
            m["read_value_mean"] = (
                m.get("read_value_sum",0)/m.get("read_ok_count",1)
                if m.get("read_ok_count") else None)
            for key in ("read_ok", "read_eagain", "park_poll", "write_fd"):
                ccnt = m.get(key + "_clean_count", 0)
                csum = m.get(key + "_clean_tsc_sum", 0)
                m[key + "_clean_ns_per_call"] = (
                    1e9 * csum / tsc_hz / ccnt if ccnt else None)
                m[key + "_clean_ns_per_op"] = 1e9 * csum / tsc_hz / ops
                m[key + "_clean_fraction"] = (
                    ccnt / m.get(key + "_count", 1) if m.get(key + "_count") else None)
            m["write_fd_frac_of_drain"] = (
                m.get("write_fd_count",0)/m.get("drain_park",1)
                if m.get("drain_park") else None)
    return m


def collect(metrics, condition, arm):
    return [m for m in metrics if m["condition"] == condition and m["arm"] == arm]


def pair_rel(metrics, cond_a, arm_a, cond_b, arm_b, key):
    by_pair = {}
    for m in metrics:
        by_pair.setdefault(m["pair"], {})[m["pos"]] = m
    vals = []
    for pair, d in by_pair.items():
        ma = next((m for m in d.values()
                   if m["condition"] == cond_a and m["arm"] == arm_a), None)
        mb = next((m for m in d.values()
                   if m["condition"] == cond_b and m["arm"] == arm_b), None)
        if ma is None or mb is None:
            continue
        va, vb = ma.get(key), mb.get(key)
        if va in (None, 0) or vb is None:
            continue
        vals.append((vb - va) / va)
    if not vals:
        return None
    return boot_ci(vals)


def mean_of(metrics, condition, arm, key):
    vals = [m[key] for m in metrics
            if m["condition"] == condition and m["arm"] == arm and m.get(key) is not None]
    return statistics.mean(vals) if vals else None


def fmt(stat, pct=True):
    if stat is None:
        return "n/a"
    scale = 100.0 if pct else 1.0
    return "%+.3f%% [%+.3f,%+.3f] n=%d" % (
        scale * stat["mean"], scale * stat["ci_lo"], scale * stat["ci_hi"], stat["n"])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dir", required=True)
    parser.add_argument("--case", default="W02-uring-4k-d2")
    parser.add_argument("--json-out", default="")
    args = parser.parse_args()

    records = load(os.path.join(args.dir, "results.jsonl"))
    metrics = []
    for rec in records:
        m = invocation_metrics(rec, args.case)
        if m is None:
            continue
        m.update({"pair": rec["pair"], "pos": rec["pos"], "arm": rec["arm"],
                  "condition": rec["condition"], "rc": rec["rc"]})
        metrics.append(m)

    print("invocations=%d with_metrics=%d rc_nonzero=%d" %
          (len(records), len(metrics), sum(1 for r in records if r["rc"] != 0)))

    out = {"dir": args.dir, "invocations": len(metrics),
           "cells": {}, "contrasts": {}}

    for condition in ("off", "on"):
        for arm in ("A", "B"):
            cell = {"cpu_ns_per_op": mean_of(metrics, condition, arm, "cpu_ns_per_op"),
                    "tput": mean_of(metrics, condition, arm, "tput"),
                    "p95_ns": mean_of(metrics, condition, arm, "p95_ns"),
                    "p99_ns": mean_of(metrics, condition, arm, "p99_ns"),
                    "bench_utime_ns_per_op": mean_of(metrics, condition, arm, "bench_utime_ns_per_op"),
                    "bench_stime_ns_per_op": mean_of(metrics, condition, arm, "bench_stime_ns_per_op"),
                    "iowq_stime_ns_per_op": mean_of(metrics, condition, arm, "iowq_stime_ns_per_op")}
            for key in ("read_ok_count", "read_eagain_count",
                        "park_poll_count", "write_fd_count",
                        "drain_park", "drain_ack",
                        "poll_wake_pollin", "poll_timeout",
                        "read_value_sum"):
                vals = [m[key] / m["ops"] for m in metrics
                        if m["condition"] == condition and m["arm"] == arm
                        and m.get(key) is not None and m.get("ops")]
                cell[key + "/op"] = statistics.mean(vals) if vals else None
            for key in ("read_ok_ns_per_call", "read_eagain_ns_per_call",
                        "park_poll_ns_per_call", "write_fd_ns_per_call",
                        "read_ok_clean_ns_per_call", "read_eagain_clean_ns_per_call",
                        "park_poll_clean_ns_per_call", "write_fd_clean_ns_per_call",
                        "read_ok_clean_fraction", "read_eagain_clean_fraction",
                        "park_poll_clean_fraction", "write_fd_clean_fraction",
                        "read_ok_ns_per_op", "read_eagain_ns_per_op",
                        "park_poll_ns_per_op", "write_fd_ns_per_op",
                        "read_ok_clean_ns_per_op", "read_eagain_clean_ns_per_op",
                        "write_fd_clean_ns_per_op",
                        "read_total_ns_per_call", "read_total_ns_per_op",
                        "read_value_mean", "write_fd_frac_of_drain"):
                cell[key] = mean_of(metrics, condition, arm, key)
            out["cells"]["%s_%s" % (condition, arm)] = cell
            print("\n[cell %s %s]" % (condition, arm))
            for key, value in cell.items():
                if value is None:
                    print("  %-28s n/a" % key)
                elif key.endswith("/op") and isinstance(value, float) and "count" in key:
                    print("  %-28s %.5f" % (key, value))
                else:
                    print("  %-28s %.3f" % (key, value))

    contrasts = [
        ("off", "A", "off", "B", "cpu_ns_per_op", "CPU/op OFF B vs A"),
        ("off", "A", "off", "B", "tput", "throughput OFF B vs A"),
        ("off", "A", "off", "B", "p95_ns", "p95 OFF B vs A"),
        ("off", "A", "off", "B", "p99_ns", "p99 OFF B vs A"),
        ("off", "A", "off", "B", "bench_utime_ns_per_op", "bench user OFF B vs A"),
        ("off", "A", "off", "B", "bench_stime_ns_per_op", "bench kernel OFF B vs A"),
        ("off", "A", "off", "B", "iowq_stime_ns_per_op", "iowq kernel OFF B vs A"),
        ("on", "A", "on", "B", "cpu_ns_per_op", "CPU/op ON B vs A"),
        ("on", "A", "on", "B", "tput", "throughput ON B vs A"),
        ("on", "A", "on", "B", "p95_ns", "p95 ON B vs A"),
        ("on", "A", "on", "B", "p99_ns", "p99 ON B vs A"),
        ("on", "A", "on", "B", "read_ok_ns_per_call", "read_ok ns/call ON B vs A"),
        ("on", "A", "on", "B", "read_eagain_ns_per_call", "read_eagain ns/call ON B vs A"),
        ("on", "A", "on", "B", "write_fd_ns_per_call", "write_fd ns/call ON B vs A"),
        ("on", "A", "on", "B", "park_poll_ns_per_call", "park poll ns/call ON B vs A"),
        ("off", "A", "on", "A", "cpu_ns_per_op", "CPU/op probe A: ON vs OFF"),
        ("off", "B", "on", "B", "cpu_ns_per_op", "CPU/op probe B: ON vs OFF"),
        ("off", "A", "on", "A", "tput", "throughput probe A: ON vs OFF"),
        ("off", "B", "on", "B", "tput", "throughput probe B: ON vs OFF"),
        ("off", "A", "on", "A", "p95_ns", "p95 probe A: ON vs OFF"),
        ("off", "A", "on", "A", "p99_ns", "p99 probe A: ON vs OFF"),
        ("off", "B", "on", "B", "p95_ns", "p95 probe B: ON vs OFF"),
        ("off", "B", "on", "B", "p99_ns", "p99 probe B: ON vs OFF"),
        ("off", "A", "on", "A", "bench_utime_ns_per_op", "user probe A: ON vs OFF"),
        ("off", "A", "on", "A", "bench_stime_ns_per_op", "kernel probe A: ON vs OFF"),
        ("off", "B", "on", "B", "bench_utime_ns_per_op", "user probe B: ON vs OFF"),
        ("off", "B", "on", "B", "bench_stime_ns_per_op", "kernel probe B: ON vs OFF"),
    ]
    print("\n[contrasts] (relative, positive = second cell larger)")
    for ca, aa, cb, ab, key, label in contrasts:
        stat = pair_rel(metrics, ca, aa, cb, ab, key)
        out["contrasts"][label] = stat
        print("  %-38s %s" % (label, fmt(stat)))


    # Probe-corrected ON/OFF A/B effect: ratio_true = ratio_on * (1+pA)/(1+pB)
    def cell_mean(condition, arm, key):
        vals = [m[key] for m in metrics
                if m["condition"] == condition and m["arm"] == arm and m.get(key) is not None]
        return statistics.mean(vals) if vals else None

    for key in ("cpu_ns_per_op", "tput", "p95_ns"):
        on = pair_rel(metrics, "on", "A", "on", "B", key)
        pa = pair_rel(metrics, "off", "A", "on", "A", key)
        pb = pair_rel(metrics, "off", "B", "on", "B", key)
        if on is None or pa is None or pb is None:
            continue
        ratio_true = (1 + on["mean"]) * (1 + pa["mean"]) / (1 + pb["mean"]) - 1
        print("  %-38s raw %s; corrected %+.3f%%" %
              ("corrected effect " + key, fmt(on), 100 * ratio_true))
        out["contrasts"]["corrected_" + key] = {
            "raw_mean": on["mean"], "pA": pa["mean"], "pB": pb["mean"],
            "corrected_mean": ratio_true}

    if args.json_out:
        with open(args.json_out, "w") as f:
            json.dump(out, f, indent=1)
        print("\nwrote %s" % args.json_out)


if __name__ == "__main__":
    main()
