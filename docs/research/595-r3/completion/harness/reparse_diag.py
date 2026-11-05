#!/usr/bin/env python3
"""Re-parse the already-captured R2 strace diagnostic (no new bench run).

The first diag_r2.py pass used a regex that did not accept strace's bare-pid
prefix, so its results.jsonl has zero trace counts.  The .strace files are
complete; this script parses them with the corrected parser and writes
results_reparsed.jsonl + summary.json next to them.
"""
import importlib.util
import json
import os
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("diag_r2", os.path.join(HERE, "diag_r2.py"))
diag = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diag)


def mean(vals):
    return sum(vals) / len(vals) if vals else None


def main():
    phase_dir = sys.argv[1]
    repeats = int(sys.argv[2]) if len(sys.argv) > 2 else 3
    case = "W02-uring-4k-d2"
    records = []
    for arm in ("A", "B"):
        for r in range(1, repeats + 1):
            tag = f"{arm}-r{r}"
            tpath = os.path.join(phase_dir, "logs", f"{tag}.strace")
            jpath = os.path.join(phase_dir, "jsonl", f"{tag}.jsonl")
            trace = diag.parse_trace(tpath)
            measured, warmup = diag.parse_jsonl(jpath, case)
            bench = {}
            if measured:
                m = measured[0]
                bench = {
                    "cpu_s": m["cpu_s"],
                    "cpu_ns_per_op": 1e9 * m["cpu_s"] / m["ops"] if m.get("ops") else None,
                    "ops_per_sec": m["ops_per_sec"],
                    "p50_ns": m["lat_p50_ns"],
                    "p95_ns": m["lat_p95_ns"],
                    "p99_ns": m["lat_p99_ns"],
                    "ops": m["ops"],
                    "submits": m["submits"],
                    "stats_submit_calls": m["stats_submit_calls"],
                    "stats_submitted_ops": m["stats_submitted_ops"],
                    "stats_poll_calls": m["stats_poll_calls"],
                    "stats_wait_calls": m["stats_wait_calls"],
                    "stats_completed_ops": m["stats_completed_ops"],
                    "stats_queue_full_retries": m["stats_queue_full_retries"],
                    "stats_invalid_state": m["stats_invalid_state"],
                }
            records.append({"arm": arm, "repeat": r, "trace": trace, "bench": bench,
                            "warmup_records": warmup, "measured_records": len(measured),
                            "strace": tpath, "jsonl": jpath})
    out = os.path.join(phase_dir, "results_reparsed.jsonl")
    with open(out, "w") as f:
        for rec in records:
            f.write(json.dumps(rec) + "\n")
    summary = {}
    for arm in ("A", "B"):
        rs = [r for r in records if r["arm"] == arm]
        traces = [r["trace"] for r in rs]
        benches = [r["bench"] for r in rs if r["bench"]]
        hist = {}
        for t in traces:
            for k, v in t["to_submit_histogram"].items():
                hist[k] = hist.get(k, 0) + v
        tids = {}
        for t in traces:
            for k, v in t["enters_by_tid"].items():
                tids[k] = tids.get(k, 0) + v
        summary[arm] = {
            "repeats": len(rs),
            "pure_submit_enters_mean": mean([t["pure_submit_enters"] for t in traces]),
            "total_enters_mean": mean([t["total_enters"] for t in traces]),
            "zero_submit_enters_mean": mean([t["zero_submit_enters"] for t in traces]),
            "sqes_mean": mean([t["sqes"] for t in traces]),
            "sqe_per_enter_mean": mean([t["sqe_per_enter"] for t in traces
                                        if t["sqe_per_enter"] is not None]),
            "max_to_submit": max(t["max_to_submit"] for t in traces),
            "to_submit_histogram_total": dict(sorted(hist.items(), key=lambda kv: int(kv[0]))),
            "enters_by_tid_total": tids,
            "register_calls_mean": mean([t["register_calls"] for t in traces]),
            "eventfd_calls_mean": mean([t["eventfd_calls"] for t in traces]),
            "enter_flags_total": {},
            "bench": {
                "cpu_ns_per_op_mean": mean([b["cpu_ns_per_op"] for b in benches]),
                "ops_per_sec_mean": mean([b["ops_per_sec"] for b in benches]),
                "p50_ns_mean": mean([b["p50_ns"] for b in benches]),
                "p95_ns_mean": mean([b["p95_ns"] for b in benches]),
                "p99_ns_mean": mean([b["p99_ns"] for b in benches]),
                "ops": [b["ops"] for b in benches],
                "stats_submitted_ops": [b["stats_submitted_ops"] for b in benches],
                "stats_completed_ops": [b["stats_completed_ops"] for b in benches],
                "stats_queue_full_retries": [b["stats_queue_full_retries"] for b in benches],
                "stats_invalid_state": [b["stats_invalid_state"] for b in benches],
            },
        }
        flags = {}
        for t in traces:
            for k, v in t["enter_flags"].items():
                flags[k] = flags.get(k, 0) + v
        summary[arm]["enter_flags_total"] = flags
    with open(os.path.join(phase_dir, "summary.json"), "w") as f:
        json.dump(summary, f, indent=1)
    print(json.dumps(summary, indent=1))


if __name__ == "__main__":
    main()
