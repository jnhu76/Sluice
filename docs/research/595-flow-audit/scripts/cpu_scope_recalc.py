#!/usr/bin/env python3
"""CPU accounting-scope recalculation from the r5-repair raw records.

Repairs review finding R4. The withdrawn v1 script (see CORRECTIONS.md R4)
divided whole-invocation wait4 rusage and whole-invocation /proc tick
buckets by the per-rep op count and emitted them as ns/op — numbers that
are neither measured-phase CPU nor comparable with the primary caliber.

This version keeps the three windows strictly separate and never converts a
whole-invocation quantity into a measured ns/op figure:

1. PRIMARY (measured phase, per frozen harness definition): per invocation
   cpu_ns_per_op = 1e9 * sum(cpu_s) / sum(ops) over the case-matched
   measured reps only (each rep's cpu_s is the bench's
   CLOCK_PROCESS_CPUTIME_ID delta around that measured phase), aggregated
   as the arm mean over invocations. Cross-checked against the
   harness-embedded metrics; scope = submitting + driver threads of the
   child process. NOT a whole-system number.
2. INVOCATION window (whole child, from the parent's wait4 rusage):
   ru_utime/ru_stime/sum in seconds. Covers warmup, oracle self-test,
   instrumented phase, setup/teardown — everything the child ever ran.
   Reported in seconds only.
3. TICKS window (invocation, 10 ms granularity, first-to-last observation
   per task): app (main thread), driver (other /proc/<pid>/task threads),
   io-wq (iou-wrk-<tgid> kernel workers found by the frozen sampler's
   top-level /proc scan every 0.5 s — a LOWER BOUND; workers that appear
   and retire between scans are missed entirely). Shares are reported only
   as "fraction of sampled-and-classified ticks". Whether io-wq kthread
   CPU is inside or outside the child's process rusage is NOT provable
   from these records (comm-based attribution follows the kernel's
   iou-wrk-<tgid> naming) and is reported as UNRESOLVED.

Required fields that are absent are reported as MISSING/UNAVAILABLE and
make the exit code nonzero; they are never defaulted to zero.
"""
import argparse
import hashlib
import json
import os
import sys
from collections import defaultdict

DEFAULT_ROOT = "/home/jnhu/sluice-595-e5/r5-repair-measure"
DEFAULT_CELLS = ("main-w03-d16", "prot-w03-d4", "prot-w03-d2serial",
                 "prot-w02-d1", "prot-w02-d2")
CASES = {"main-w03-d16": "W03-uring-4k-d16", "prot-w03-d4": "W03-uring-4k-d4",
         "prot-w03-d2serial": "W03-uring-4k-d2-r3d2serial",
         "prot-w02-d1": "W02-uring-4k-d1", "prot-w02-d2": "W02-uring-4k-d2"}

MEASURED_REPS = 5
WARMUP_REPS = 1
OPS_PER_REP = 20000
BLOCKS = 24
ARMS = ("A0", "R", "C")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def load_cell(root, cdir):
    path = os.path.join(root, "raw", cdir, "main", "results.jsonl")
    if not os.path.exists(path):
        return None, path, [f"missing {path}"]
    recs = [json.loads(l) for l in open(path) if l.strip()]
    return recs, path, None


def admit(recs, cdir):
    """Same admission as verify_inherited_evidence: unique valid slot."""
    slots = defaultdict(list)
    for r in recs:
        slots[(r.get("block"), r.get("position"), r.get("arm"))].append(r)
    out = {}
    problems = []
    for slot, group in slots.items():
        valid = [r for r in group if r.get("valid") is True and r.get("seg") == "main"]
        if len(valid) != 1:
            problems.append(f"{cdir} slot {slot}: {len(valid)} valid invocations")
            continue
        out[slot] = valid[0]
    for arm in ARMS:
        n = sum(1 for k in out if k[2] == arm)
        if n != BLOCKS:
            problems.append(f"{cdir} arm {arm}: {n}/{BLOCKS}")
    return out, problems


def measured_reps(rec, case):
    path = rec.get("jsonl")
    if not path or not os.path.exists(path):
        return None, [f"missing rep file {path}"]
    rows = []
    non_measured = 0
    with open(path) as f:
        for line in f:
            r = json.loads(line)
            if r.get("case_id") != case:
                continue
            if r.get("measured"):
                rows.append(r)
            else:
                non_measured += 1
    problems = []
    if len(rows) != MEASURED_REPS:
        problems.append(f"{os.path.basename(path)}: {len(rows)} measured reps")
    if non_measured != WARMUP_REPS:
        problems.append(f"{os.path.basename(path)}: {non_measured} warmup records")
    return rows, problems


def bucket_facts(rec):
    """Invocation-window tick facts from the frozen sampler summary."""
    b = rec.get("buckets") or {}
    facts = {
        "available": bool(b.get("available")),
        "samples": b.get("samples", "MISSING"),
        "app_ticks": b.get("app_ticks", "MISSING"),
        "driver_ticks": b.get("driver_ticks", "MISSING"),
        "in_thread_kernel_stime_ticks": b.get("in_thread_kernel_stime_ticks", "MISSING"),
        "io_wq_ticks_lower_bound": b.get("io_wq_ticks", "MISSING"),
        "io_wq_in_process_tasks": len(b.get("io_wq_in_process") or {}),
        "residual_ticks_est": b.get("residual_ticks_est", "MISSING"),
        "residual_bound_ticks": b.get("residual_bound_ticks", "MISSING"),
    }
    if facts["available"]:
        r, bound = facts["residual_ticks_est"], facts["residual_bound_ticks"]
        facts["residual_within_bound"] = (
            isinstance(r, (int, float)) and isinstance(bound, (int, float))
            and r <= bound)
        # threads observed exactly once contribute no first-to-last delta and
        # signal partial coverage, not zero work
        threads = b.get("threads") or {}
        facts["threads_seen_once"] = sum(1 for t in threads.values()
                                         if t.get("samples", 0) <= 1)
        facts["threads_total"] = len(threads)
    return facts


def agg(rows, key):
    vals = [key(r) if callable(key) else r[key] for r in rows]
    if not vals or any(isinstance(v, str) for v in vals):
        return "UNAVAILABLE"
    return sum(vals) / len(vals)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", default=DEFAULT_ROOT)
    ap.add_argument("--cells", default=",".join(DEFAULT_CELLS))
    ap.add_argument("--json", help="optional machine-readable output path")
    args = ap.parse_args(argv)

    out = {"root": args.root, "cells": {}, "notes": {
        "primary_scope": "CLOCK_PROCESS_CPUTIME_ID, measured phase only, "
                         "bench child threads (submitter+driver); whether "
                         "io-wq kthreads are included in this metric is "
                         "UNRESOLVED on this kernel (see "
                         "io_wq_rusage_inclusion note)",
        "invocation_window": "parent wait4 rusage of the whole child: warmup + "
                             "oracle self-test + measured + instrumented phases "
                             "+ setup/teardown; seconds only, no ns/op derived",
        "ticks_window": "invocation-scoped /proc sampling, 10 ms ticks, "
                        "first-to-last per task; io-wq scanned every 0.5 s => "
                        "lower bound; shares are sampled-classified only",
        "io_wq_rusage_inclusion": "UNRESOLVED (cannot be proven from these "
                                  "records; kernel names workers iou-wrk-<tgid>)",
    }}
    problems_all = []
    for cdir in args.cells.split(","):
        case = CASES.get(cdir)
        if case is None:
            problems_all.append(f"{cdir}: unknown cell")
            continue
        recs, path, err = load_cell(args.root, cdir)
        if err:
            problems_all += err
            continue
        admitted, probs = admit(recs, cdir)
        problems_all += probs
        per_arm = defaultdict(list)
        for slot, rec in sorted(admitted.items()):
            arm = slot[2]
            rows, rprobs = measured_reps(rec, case)
            problems_all += rprobs
            if rows is None or len(rows) != MEASURED_REPS:
                continue
            ops = sum(r["ops"] for r in rows)
            cpu = sum(r["cpu_s"] for r in rows)
            m = rec.get("metrics") or {}
            hv = m.get("cpu_ns_per_op")
            entry = {
                "primary_cpu_ns_per_op": 1e9 * cpu / ops,
                "harness_metrics_cpu_ns_per_op": hv,
            }
            if hv is None:
                problems_all.append(f"{cdir} {slot}: harness metrics missing")
            elif abs(entry["primary_cpu_ns_per_op"] - hv) > 1e-9 * max(hv, 1.0):
                problems_all.append(f"{cdir} {slot}: primary recompute {entry['primary_cpu_ns_per_op']}"
                                    f" vs harness {hv}")
            for k in ("ru_utime", "ru_stime", "wall_s"):
                if rec.get(k) is None:
                    entry[k] = "MISSING"
                    problems_all.append(f"{cdir} {slot}: {k} missing")
                else:
                    entry[k] = rec[k]
            entry["buckets"] = bucket_facts(rec)
            if not entry["buckets"]["available"]:
                problems_all.append(f"{cdir} {slot}: buckets missing")
            per_arm[arm].append(entry)
        cell_out = {"results_sha256": sha256_file(path),
                    "n_invocations": sum(len(v) for v in per_arm.values())}
        for arm, rows in sorted(per_arm.items()):
            sampled = [r for r in rows if r["buckets"]["available"]]
            share_denom = []
            for r in sampled:
                b = r["buckets"]
                d = b["app_ticks"] + b["driver_ticks"] + b["io_wq_ticks_lower_bound"]
                if d > 0:
                    share_denom.append(d)
            cell_out[arm] = {
                "n": len(rows),
                "primary_measured_cpu_ns_per_op_mean": agg(rows, "primary_cpu_ns_per_op"),
                "invocation_ru_utime_s_mean": agg(rows, "ru_utime"),
                "invocation_ru_stime_s_mean": agg(rows, "ru_stime"),
                "invocation_wall_s_mean": agg(rows, "wall_s"),
                "ticks_app_mean": agg(sampled, lambda r: r["buckets"]["app_ticks"])
                if sampled else "UNAVAILABLE",
                "ticks_driver_mean": agg(sampled, lambda r: r["buckets"]["driver_ticks"])
                if sampled else "UNAVAILABLE",
                "ticks_iowq_lower_bound_mean":
                    agg(sampled, lambda r: r["buckets"]["io_wq_ticks_lower_bound"])
                    if sampled else "UNAVAILABLE",
                "iowq_share_of_sampled_classified_pct":
                    (100.0 * sum(r["buckets"]["io_wq_ticks_lower_bound"]
                                 for r in sampled)
                     / sum(r["buckets"]["app_ticks"] + r["buckets"]["driver_ticks"]
                           + r["buckets"]["io_wq_ticks_lower_bound"]
                           for r in sampled)) if share_denom else "UNAVAILABLE",
                "coverage": {
                    "records_with_buckets": len(sampled),
                    "samples_mean": agg(sampled, lambda r: r["buckets"]["samples"]),
                    "all_residual_within_bound": all(
                        r["buckets"].get("residual_within_bound") for r in sampled),
                    "threads_seen_once_mean":
                        agg(sampled, lambda r: r["buckets"]["threads_seen_once"]),
                    "io_wq_in_process_tasks_total":
                        sum(r["buckets"]["io_wq_in_process_tasks"] for r in sampled),
                },
            }
            cell_out[arm]["_agg_note"] = (
                "tick/app shares use ONLY sampled-and-classified ticks; "
                "whole-invocation seconds are never divided by ops")
        out["cells"][cdir] = cell_out

    for p in problems_all:
        print(f"PROBLEM {p}")
    print(json.dumps(out, indent=1, sort_keys=True))
    if args.json:
        with open(args.json, "w") as f:
            json.dump(out, f, indent=1, sort_keys=True)
        print(f"wrote {args.json}")
    return 0 if (not problems_all
                 and all(v.get("n") == BLOCKS for c in out["cells"].values()
                         for v in c.values() if isinstance(v, dict) and "n" in v)) else 2


if __name__ == "__main__":
    sys.exit(main())
