#!/usr/bin/env python3
"""Aggregates the G0 (#403) raw JSONL evidence into report tables.

Reads every raw/w*.jsonl file in the evidence directory and prints:
  * an integrity check over the recorded repetitions,
  * one row per case with median throughput/latency/counters over the measured
    repetitions,
  * the pre-declared comparisons from the preregistration.

Only measured repetitions are aggregated; warmups are counted but excluded.
"""
from __future__ import annotations

import json
import statistics
import sys
from collections import defaultdict
from pathlib import Path


def load(raw_dir: Path):
    records = []
    for path in sorted(raw_dir.glob("w*.jsonl")):
        for line in path.read_text().splitlines():
            if line.strip():
                record = json.loads(line)
                record["_file"] = path.name
                records.append(record)
    return records


def median(values):
    return statistics.median(values) if values else 0.0


def spread(values):
    if len(values) < 2:
        return 0.0
    mean = statistics.fmean(values)
    return (max(values) - min(values)) / mean * 100.0 if mean else 0.0


def cv(values):
    if len(values) < 2:
        return 0.0
    mean = statistics.fmean(values)
    return statistics.stdev(values) / mean * 100.0 if mean else 0.0


REQUIRED_FIELDS = (
    "ops_per_sec", "lat_p50_ns", "lat_p95_ns", "lat_p99_ns", "lat_max_ns", "ops",
    "cpu_s", "wall_s", "stats_poll_calls", "stats_wait_calls", "notifications",
    "passes", "would_block", "stats_max_outstanding", "stats_completion_errors",
    "stats_invalid_state",
)


def check_fields(records):
    missing = set()
    for record in records:
        for field in REQUIRED_FIELDS:
            if field not in record:
                missing.add(field)
    return sorted(missing)


def main() -> int:
    evidence = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(
        "docs/evidence/g0-403-performance-baseline-20261015")
    records = load(evidence / "raw")
    # The no-ring control captures share case ids with the primary run; they are
    # NOT_RUN evidence plus a single-repetition threadpool control, so they are
    # reported separately and never merged into the primary aggregates.
    control = [r for r in records if "noring-notrun" in r["_file"]]
    records = [r for r in records if "noring-notrun" not in r["_file"]]

    absent = check_fields(records)
    if absent:
        print("ERROR: records are missing fields the analysis reads:", absent)
        return 2
    measured = [r for r in records if r["measured"]]
    warmups = [r for r in records if not r["measured"]]

    print("## integrity")
    print(f"records total={len(records)} measured={len(measured)} warmup={len(warmups)}")
    by_case = defaultdict(list)
    for record in measured:
        by_case[(record["_file"], record["case_id"])].append(record)

    problems = []
    for (source, case_id), group in sorted(by_case.items()):
        bad = [r for r in group if not r["ok"]]
        if bad and "NOT_RUN" not in bad[0]["failure"]:
            problems.append(f"{case_id}: {len(bad)}/{len(group)} failed: {bad[0]['failure'][:90]}")
        if "NOT_RUN" in group[0].get("failure", ""):
            continue
        if len(group) != 7:
            problems.append(f"{case_id}: expected 7 measured repetitions, found {len(group)}")
        for r in group:
            if r["queue_depth"] and r["stats_max_outstanding"] > r["queue_depth"]:
                problems.append(
                    f"{case_id}: max_outstanding {r['stats_max_outstanding']} exceeded bound "
                    f"{r['queue_depth']}")
    control_cases = defaultdict(list)
    for record in control:
        control_cases[record["case_id"]].append(record)
    print(f"no-ring control records={len(control)} cases={len(control_cases)}")
    print("problems:", "none" if not problems else "")
    for problem in problems:
        print("  -", problem)

    print()
    print("## per-case summary (median over 7 measured repetitions)")
    header = (
        f"| {'CASE_ID':44s} | {'ops/s (median)':>14s} | {'spread%':>7s} | "
        f"{'CV%':>5s} | {'p50 ns':>8s} | {'p95 ns':>9s} | {'p99 ns':>9s} | "
        f"{'max ns':>10s} | {'util%':>6s} | {'ops/s per CPU':>13s} |")
    print(header)
    print("|" + "-" * 46 + "|" + "-" * 16 + "|" + "-" * 9 + "|" + "-" * 7 + "|"
          + "-" * 10 + "|" + "-" * 11 + "|" + "-" * 11 + "|" + "-" * 12 + "|"
          + "-" * 8 + "|" + "-" * 15 + "|")
    for (source, case_id), group in sorted(by_case.items(), key=lambda kv: kv[0][1]):
        if "NOT_RUN" in group[0].get("failure", ""):
            continue
        if case_id.startswith("CAL-"):
            print(f"| {case_id:44s} | {group[0]['lat_p50_ns']:.0f} ns per steady_clock read "
                  f"(calibration; wall_s is the calibration value itself) |")
            continue
        ops = [r["ops_per_sec"] for r in group]
        util = [r["cpu_util"] for r in group]
        per_cpu = median([r["ops_per_sec"] / r["cpu_util"] for r in group if r["cpu_util"] > 0])
        print(f"| {case_id:44s} | {median(ops):14,.0f} | {spread(ops):7.1f} | "
              f"{cv(ops):5.1f} | {median([r['lat_p50_ns'] for r in group]):8.0f} | "
              f"{median([r['lat_p95_ns'] for r in group]):9.0f} | "
              f"{median([r['lat_p99_ns'] for r in group]):9.0f} | "
              f"{median([r['lat_max_ns'] for r in group]):10.0f} | "
              f"{median(util) * 100:6.1f} | {per_cpu:13,.0f} |")

    print()
    print("## counters (median per operation)")
    print(f"| {'CASE_ID':44s} | {'polls/op':>9s} | {'waits/op':>9s} | {'notif/op':>9s} | "
          f"{'passes/op':>10s} | {'would_block':>11s} | {'max_outst':>10s} | {'compl errs':>10s} | "
          f"{'invalid st':>10s} |")
    print("|" + "-" * 46 + "|" + "-" * 11 + "|" + "-" * 11 + "|" + "-" * 11 + "|"
          + "-" * 12 + "|" + "-" * 13 + "|" + "-" * 12 + "|" + "-" * 12 + "|"
          + "-" * 12 + "|")
    for (source, case_id), group in sorted(by_case.items(), key=lambda kv: kv[0][1]):
        if "NOT_RUN" in group[0].get("failure", ""):
            continue

        def per_op(field):
            values = [r[field] / r["ops"] for r in group if r["ops"]]
            return median(values)

        print(f"| {case_id:44s} | {per_op('stats_poll_calls'):9.3f} | "
              f"{per_op('stats_wait_calls'):9.3f} | {per_op('notifications'):9.3f} | "
              f"{per_op('passes'):10.3f} | {median([r['would_block'] for r in group]):11.0f} | "
              f"{median([r['stats_max_outstanding'] for r in group]):10.0f} | "
              f"{median([r['stats_completion_errors'] for r in group]):10.0f} | "
              f"{median([r['stats_invalid_state'] for r in group]):10.0f} |")

    print()
    print("## no-ring profile control (1 repetition, evidence that uring is absent)")
    for case_id, group in sorted(control_cases.items()):
        failure = group[0].get("failure", "")
        if "NOT_RUN" in failure:
            print(f"  {case_id}: {failure}")
    print("  (threadpool cases also ran in this profile: "
          f"{sum(1 for g in control_cases.values() if 'NOT_RUN' not in g[0].get('failure',''))}"
          " cases, reported separately and not merged above)")

    print()
    print("## pre-declared comparisons")

    def case_ops(case_id):
        for (source, cid), group in by_case.items():
            if cid == case_id and "NOT_RUN" not in group[0].get("failure", ""):
                return statistics.median([r["ops_per_sec"] for r in group])
        return None

    def case_lat(case_id, field="lat_p50_ns"):
        for (source, cid), group in by_case.items():
            if cid == case_id and "NOT_RUN" not in group[0].get("failure", ""):
                return statistics.median([r[field] for r in group])
        return None

    def case_field(case_id, field):
        for (source, cid), group in by_case.items():
            if cid == case_id and "NOT_RUN" not in group[0].get("failure", ""):
                return statistics.median([r[field] for r in group])
        return None

    def show(label, value, unit=""):
        print(f"  {label:66s} {value}")

    direct = case_ops("W01-read_at-4k-seq")
    tp1 = case_ops("W02-threadpool-4k-d1-w1")
    show("Q1 direct 4k read ops/s (W01-read_at-4k-seq)", f"{direct:,.0f}")
    show("Q1 threadpool 4k single-outstanding ops/s", f"{tp1:,.0f}")
    show("Q1 ratio direct / request-path single", f"{direct / tp1:.1f}x lower via request path")
    show("Q1 direct 4k p50 ns", f"{case_lat('W01-read_at-4k-seq'):.0f}")
    show("Q1 threadpool 4k d1 p50 ns", f"{case_lat('W02-threadpool-4k-d1-w1'):.0f}")
    for case_id in ["W02-threadpool-4k-d1-w1", "W02-threadpool-4k-d4-w4",
                    "W02-threadpool-4k-d16-w4", "W02-threadpool-4k-d64-w4"]:
        show(f"Q2 {case_id}", f"{case_ops(case_id):,.0f} ops/s, "
             f"p50 {case_lat(case_id):,.0f} ns")
    for case_id in ["W02-threadpool-4k-d16-w1", "W02-threadpool-4k-d16-w4",
                    "W02-threadpool-4k-d16-w8"]:
        show(f"Q2 {case_id}", f"{case_ops(case_id):,.0f} ops/s, "
             f"cpu_util {case_field(case_id, 'cpu_util') * 100:.0f}%, "
             f"waits/op {case_field(case_id, 'stats_wait_calls') / 20000:.3f}")
    show("Q3 d4-w4 scope4 vs scope64",
         f"{case_ops('W02-threadpool-4k-d4-w4'):,.0f} vs "
         f"{case_ops('W02-threadpool-4k-d4-w4-scope64'):,.0f} ops/s")
    show("Q3 d16-w4 scope16 vs scope64",
         f"{case_ops('W02-threadpool-4k-d16-w4'):,.0f} vs "
         f"{case_ops('W02-threadpool-4k-d16-w4-scope64'):,.0f} ops/s")
    for case_id in ["W02-uring-4k-d1", "W02-uring-4k-d4", "W02-uring-4k-d16",
                    "W02-uring-4k-d64"]:
        show(f"Q4 {case_id}", f"{case_ops(case_id):,.0f} ops/s, "
             f"p50 {case_lat(case_id):,.0f} ns")
    for depth in ["d4", "d16", "d64"]:
        tp = case_ops(f"W03-threadpool-4k-{depth}-w4")
        ur = case_ops(f"W03-uring-4k-{depth}")
        tp_owned = case_ops(f"W02-threadpool-4k-{depth}-w4")
        show(f"Q5 depth {depth}: W03 threadpool vs W02 threadpool", f"{tp:,.0f} vs {tp_owned:,.0f} ops/s")
        show(f"Q5 depth {depth}: W03 uring vs W02 uring", f"{ur:,.0f} vs "
             f"{case_ops(f'W02-uring-4k-{depth}'):,.0f} ops/s")
    for depth in ["d4", "d16", "d64"]:
        for backend in ["threadpool-4k-", "uring-4k-"]:
            case_id = f"W03-{backend}{depth}" + ("-w4" if "threadpool" in backend else "")
            if case_ops(case_id) is None:
                continue
            show(f"Q6 {case_id} notif/op, passes/op",
                 f"{case_field(case_id, 'notifications') / 20000:.4f}, "
                 f"{case_field(case_id, 'passes') / 20000:.4f}")
    for case_id in ["W01-read_at-64k-seq", "W01-read_at-64k-seq-cold",
                    "W01-read_at-1m-seq", "W01-read_at-1m-seq-cold"]:
        show(f"Q7 {case_id}", f"{case_ops(case_id):,.0f} ops/s, "
             f"p50 {case_lat(case_id):,.0f} ns, "
             f"spread {spread([r['ops_per_sec'] for g in by_case.values() for r in g if r['case_id'] == case_id]):.0f}%")
    show("Q7 sync_data tmpfs vs ext4",
         f"{case_ops('W01-sync_data-4k'):,.0f} vs {case_ops('W01-sync_data-4k-disk'):,.0f} ops/s")
    show("Q7 sync_all tmpfs vs ext4",
         f"{case_ops('W01-sync_all-4k'):,.0f} vs {case_ops('W01-sync_all-4k-disk'):,.0f} ops/s")

    print()
    print("## not-run records")
    for (source, case_id), group in sorted(by_case.items(), key=lambda kv: kv[0][1]):
        failure = group[0].get("failure", "")
        if "NOT_RUN" in failure:
            print(f"  {case_id}: {failure} [{source}]")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
