#!/usr/bin/env python3
"""Short R2 enter/SQE/residue/eventfd diagnostic (separate from timed blocks).

Runs the R2 binary under strace (syscall counting only; no PMU, no ftrace) for
the W-02 D2 case, one measured rep per invocation, and records:
  pure submit enters (to_submit > 0), total io_uring_enter calls, sum to_submit,
  SQE per enter, max to_submit, to_submit histogram, per-tid enter counts,
  io_uring_register / eventfd2 / eventfd calls, and the bench JSONL metrics.
These numbers are mechanism evidence only; they are never mixed with the
timed-block results.
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import time

STRACE = "/usr/sbin/strace"
TASKSET = "/usr/sbin/taskset"
LINE = re.compile(
    r"^(?:\[pid\s+(\d+)\]\s+|(\d+)\s+)?(\d+(?::\d+){0,2}\.\d+)\s+(\w+)\((.*)\)\s+=\s+(-?\d+)")


def iso_now():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def parse_trace(path):
    out = {
        "pure_submit_enters": 0,
        "zero_submit_enters": 0,
        "total_enters": 0,
        "sqes": 0,
        "max_to_submit": 0,
        "to_submit_histogram": {},
        "enters_by_tid": {},
        "register_calls": 0,
        "eventfd_calls": 0,
        "enter_flags": {},
    }
    with open(path) as f:
        for line in f:
            m = LINE.match(line.strip())
            if not m:
                continue
            tid = int(m.group(1) or m.group(2) or 0)
            name = m.group(4)
            args = [a.strip() for a in m.group(5).split(",")]
            if name == "io_uring_enter":
                out["total_enters"] += 1
                out["enters_by_tid"][str(tid)] = out["enters_by_tid"].get(str(tid), 0) + 1
                try:
                    to_submit = int(args[1])
                    flags = args[3] if len(args) > 3 else ""
                except (IndexError, ValueError):
                    continue
                out["enter_flags"][flags] = out["enter_flags"].get(flags, 0) + 1
                if to_submit > 0:
                    out["pure_submit_enters"] += 1
                    out["sqes"] += to_submit
                    out["max_to_submit"] = max(out["max_to_submit"], to_submit)
                    key = str(to_submit)
                    out["to_submit_histogram"][key] = out["to_submit_histogram"].get(key, 0) + 1
                else:
                    out["zero_submit_enters"] += 1
            elif name in ("io_uring_register",):
                out["register_calls"] += 1
            elif name in ("eventfd", "eventfd2"):
                out["eventfd_calls"] += 1
    p = out["pure_submit_enters"]
    out["sqe_per_enter"] = (out["sqes"] / p) if p else None
    return out


def parse_jsonl(path, case):
    measured = []
    warmup = 0
    if not os.path.exists(path):
        return measured, warmup
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            r = json.loads(line)
            if r.get("case_id") != case:
                continue
            if r.get("measured"):
                measured.append(r)
            else:
                warmup += 1
    return measured, warmup


def run_one(args, arm, repeat, case):
    d = args.phase_dir
    os.makedirs(os.path.join(d, "logs"), exist_ok=True)
    os.makedirs(os.path.join(d, "jsonl"), exist_ok=True)
    tag = f"{arm}-r{repeat}"
    jpath = os.path.join(d, "jsonl", f"{tag}.jsonl")
    tpath = os.path.join(d, "logs", f"{tag}.strace")
    opath = os.path.join(d, "logs", f"{tag}.log")
    for p in (jpath, tpath, opath):
        if os.path.exists(p):
            os.unlink(p)
    env = {k: v for k, v in os.environ.items() if not k.startswith("SLUICE_")}
    env.update({
        "SLUICE_BENCH_WARMUP": "0",
        "SLUICE_BENCH_MEASURED": "1",
        "SLUICE_BENCH_CASE_FILTER": "=" + case,
        "SLUICE_BENCH_STATS": "1",
        "SLUICE_BENCH_JSONL": jpath,
        "SLUICE_BENCH_TMP_DIR": args.tmp_dir,
        "SLUICE_BENCH_595_DEPTHS": "1",
        "SLUICE_595_ARM": arm,
    })
    cmd = [STRACE, "-f", "-ttt", "-e",
           "trace=io_uring_enter,io_uring_register,eventfd2,eventfd",
           "-o", tpath, TASKSET, "-c", args.cpu, args.binary]
    with open(opath, "wb") as log:
        t0 = time.monotonic()
        proc = subprocess.run(cmd, env=env, stdout=log, stderr=subprocess.STDOUT)
        wall = time.monotonic() - t0
    trace = parse_trace(tpath)
    measured, warmup = parse_jsonl(jpath, case)
    metrics = {}
    if measured:
        r = measured[0]
        metrics = {
            "cpu_s": r["cpu_s"],
            "cpu_ns_per_op": 1e9 * r["cpu_s"] / r["ops"] if r.get("ops") else None,
            "ops_per_sec": r["ops_per_sec"],
            "p50_ns": r["lat_p50_ns"],
            "p95_ns": r["lat_p95_ns"],
            "p99_ns": r["lat_p99_ns"],
            "ops": r["ops"],
            "submits": r["submits"],
            "stats_submit_calls": r["stats_submit_calls"],
            "stats_submitted_ops": r["stats_submitted_ops"],
            "stats_poll_calls": r["stats_poll_calls"],
            "stats_wait_calls": r["stats_wait_calls"],
            "stats_completed_ops": r["stats_completed_ops"],
            "stats_queue_full_retries": r["stats_queue_full_retries"],
            "stats_invalid_state": r["stats_invalid_state"],
        }
    rec = {
        "phase": args.phase,
        "arm": arm,
        "repeat": repeat,
        "case": case,
        "ts_utc": iso_now(),
        "exit": proc.returncode,
        "wall_s": wall,
        "binary_sha256": sha256_file(args.binary),
        "warmup_records": warmup,
        "measured_records": len(measured),
        "bench": metrics,
        "trace": trace,
        "strace": tpath,
        "jsonl": jpath,
        "log": opath,
    }
    with open(os.path.join(d, "results.jsonl"), "a") as f:
        f.write(json.dumps(rec) + "\n")
    print(f"[diag-r2] {arm} r{repeat} exit={proc.returncode} "
          f"enters={trace['pure_submit_enters']}/{trace['total_enters']} "
          f"sqes={trace['sqes']} max={trace['max_to_submit']} "
          f"hist={trace['to_submit_histogram']}", flush=True)
    return rec


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--phase", default="diag-r2")
    ap.add_argument("--phase-dir", required=True)
    ap.add_argument("--case", default="W02-uring-4k-d2")
    ap.add_argument("--cpu", default="2")
    ap.add_argument("--repeats", type=int, default=3)
    ap.add_argument("--tmp-dir", default="/tmp/sluice-595-r3c")
    args = ap.parse_args()
    os.makedirs(args.phase_dir, exist_ok=True)
    results = os.path.join(args.phase_dir, "results.jsonl")
    if os.path.exists(results):
        os.unlink(results)
    header = {
        "phase": args.phase,
        "case": args.case,
        "binary": os.path.abspath(args.binary),
        "binary_sha256": sha256_file(args.binary),
        "repeats_per_arm": args.repeats,
        "arms": ["A", "B"],
        "cpu_set": args.cpu,
        "warmup": 0,
        "measured": 1,
        "stats": 1,
        "env": {"SLUICE_BENCH_595_DEPTHS": "1"},
        "started_utc": iso_now(),
        "note": "syscall counting only; no PMU/ftrace; numbers are mechanism evidence",
    }
    with open(os.path.join(args.phase_dir, "header.json"), "w") as f:
        json.dump(header, f, indent=1)
    for arm in ("A", "B"):
        for r in range(1, args.repeats + 1):
            run_one(args, arm, r, args.case)
    return 0


if __name__ == "__main__":
    sys.exit(main())
