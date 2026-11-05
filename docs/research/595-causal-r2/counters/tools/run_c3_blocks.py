#!/usr/bin/env python3
"""Interleaved C3 probe-overhead and per-call-cost runner (#595 causal-r2 C3).

Design: per pair, four invocations in one of two orders:
  even pair: off-A, off-B, on-B, on-A
  odd  pair: off-B, off-A, on-A, on-B
so the OFF and ON A/B contrasts each balance arm order, and the ON/OFF
contrast for each arm is temporally adjacent (same pair). No perf/perf stat is
used in these runs: C3 source probes are the only instrumentation, and the
bench's own CLOCK_PROCESS_CPUTIME_ID plus per-thread /proc sampling provide
CPU. pmu/user events are intentionally not stacked.
"""

import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import threading
import time

TICK = os.sysconf("SC_CLK_TCK")
BENCH_PREFIX = "bench_w02"
IOWQ_PREFIX = "iou-wrk"
C3_PREFIX = "SLUICE595_C3 "


def read_text(path):
    try:
        with open(path, "r") as f:
            return f.read()
    except (FileNotFoundError, ProcessLookupError, PermissionError):
        return None


def parse_stat(text):
    close = text.rfind(")")
    if close < 0:
        return None
    comm = text[text.find("(") + 1:close]
    fields = text[close + 2:].split()
    if len(fields) < 22:
        return None
    return {"comm": comm, "utime": int(fields[11]), "stime": int(fields[12])}


def parse_status(text):
    out = {}
    for line in text.splitlines():
        if line.startswith("voluntary_ctxt_switches:"):
            out["voluntary"] = int(line.split(":")[1])
        elif line.startswith("nonvoluntary_ctxt_switches:"):
            out["nonvoluntary"] = int(line.split(":")[1])
    return out


MIG_RE = re.compile(r"se\.nr_migrations\s*:\s*(\d+)")


class Sampler(threading.Thread):
    def __init__(self, pid, interval=0.001):
        super().__init__(daemon=True)
        self.pid = pid
        self.interval = interval
        self.stop_evt = threading.Event()
        self.threads = {}
        self.samples = 0
        self.errors = 0

    def _sample_once(self):
        task_dir = "/proc/%d/task" % self.pid
        try:
            tids = os.listdir(task_dir)
        except (FileNotFoundError, ProcessLookupError):
            return
        for tid_s in tids:
            stat = read_text("%s/%s/stat" % (task_dir, tid_s))
            if stat is None:
                self.errors += 1
                continue
            parsed = parse_stat(stat)
            if parsed is None:
                self.errors += 1
                continue
            entry = self.threads.setdefault(
                tid_s, {"comm": parsed["comm"], "utime": 0, "stime": 0,
                        "voluntary": 0, "nonvoluntary": 0, "migrations": 0})
            entry["comm"] = parsed["comm"]
            entry["utime"] = max(entry["utime"], parsed["utime"])
            entry["stime"] = max(entry["stime"], parsed["stime"])
            status = read_text("%s/%s/status" % (task_dir, tid_s))
            if status is not None:
                st = parse_status(status)
                if "voluntary" in st:
                    entry["voluntary"] = max(entry["voluntary"], st["voluntary"])
                if "nonvoluntary" in st:
                    entry["nonvoluntary"] = max(entry["nonvoluntary"], st["nonvoluntary"])
            sched = read_text("%s/%s/sched" % (task_dir, tid_s))
            if sched is not None:
                m = MIG_RE.search(sched)
                if m:
                    entry["migrations"] = max(entry["migrations"], int(m.group(1)))
        self.samples += 1

    def run(self):
        while not self.stop_evt.is_set():
            self._sample_once()
            time.sleep(self.interval)

    def aggregate(self):
        out = {"bench": {"user_s": 0.0, "kernel_s": 0.0, "voluntary": 0,
                         "nonvoluntary": 0, "migrations": 0},
               "iowq": {"user_s": 0.0, "kernel_s": 0.0, "voluntary": 0,
                        "nonvoluntary": 0, "migrations": 0},
               "samples": self.samples, "errors": self.errors}
        for entry in self.threads.values():
            comm = entry["comm"]
            key = "bench" if comm.startswith(BENCH_PREFIX) else (
                "iowq" if comm.startswith(IOWQ_PREFIX) else None)
            if key is None:
                continue
            out[key]["user_s"] += entry["utime"] / TICK
            out[key]["kernel_s"] += entry["stime"] / TICK
            out[key]["voluntary"] += entry["voluntary"]
            out[key]["nonvoluntary"] += entry["nonvoluntary"]
            out[key]["migrations"] += entry["migrations"]
        return out


def parse_c3_lines(path):
    lines = []
    text = read_text(path)
    if text is None:
        return lines
    for line in text.splitlines():
        if not line.startswith(C3_PREFIX):
            continue
        fields = line[len(C3_PREFIX):].split()
        entry = {}
        for field in fields:
            if "=" not in field:
                continue
            key, value = field.split("=", 1)
            try:
                entry[key] = int(value)
            except ValueError:
                entry[key] = value
        lines.append(entry)
    return lines


def load_jsonl(path):
    records = []
    text = read_text(path)
    if text is None:
        return records
    for line in text.splitlines():
        line = line.strip()
        if line:
            try:
                records.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    return records


def run_one(args, condition, arm, pair, pos, outdir):
    tag = "%s_p%02d_pos%d_%s_%s" % (args.label, pair, pos, arm, condition)
    jsonl = os.path.join(outdir, tag + ".jsonl")
    stdout = os.path.join(outdir, tag + ".out")
    stderr = os.path.join(outdir, tag + ".err")
    env = dict(os.environ)
    env.update({
        "SLUICE_BENCH_595_DEPTHS": "1",
        "SLUICE_BENCH_CASE_FILTER": args.case,
        "SLUICE_BENCH_WARMUP": str(args.warmup),
        "SLUICE_BENCH_MEASURED": str(args.measured),
        "SLUICE_BENCH_STATS": "1",
        "SLUICE_BENCH_TMP_DIR": args.tmpdir,
        "SLUICE_BENCH_JSONL": jsonl,
        "SLUICE_595_COUNTERS": "0",
        "SLUICE_595_C3": "1" if condition == "on" else "0",
    })
    env["SLUICE_BENCH_REUSE_FIXTURE"] = "1"
    if arm == "A":
        env.pop("SLUICE_595_ARM", None)
    else:
        env["SLUICE_595_ARM"] = "B"

    for stale in (jsonl, stdout, stderr):
        try:
            os.remove(stale)
        except FileNotFoundError:
            pass

    start = time.time()
    with open(stdout, "wb") as fout, open(stderr, "wb") as ferr:
        proc = subprocess.Popen(["taskset", "-c", args.cpu, args.bin],
                                env=env, stdout=fout, stderr=ferr)
        sampler = Sampler(proc.pid)
        sampler.start()
        rc = proc.wait()
        sampler.stop_evt.set()
        sampler.join(timeout=1.0)
    elapsed = time.time() - start

    record = {
        "label": args.label, "pair": pair, "pos": pos, "arm": arm,
        "condition": condition, "case": args.case, "rc": rc,
        "elapsed_s": elapsed,
        "threads": sampler.aggregate(),
        "records": load_jsonl(jsonl),
        "c3_lines": parse_c3_lines(stderr),
    }
    with open(os.path.join(outdir, "results.jsonl"), "a") as f:
        f.write(json.dumps(record) + "\n")
    return record


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--label", required=True)
    parser.add_argument("--bin", required=True)
    parser.add_argument("--pairs", type=int, required=True)
    parser.add_argument("--case", default="W02-uring-4k-d2")
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--measured", type=int, default=5)
    parser.add_argument("--cpu", default="2")
    parser.add_argument("--tmpdir", default="/tmp/sluice-595-cost")
    parser.add_argument("--outdir", required=True)
    args = parser.parse_args()

    os.makedirs(args.outdir, exist_ok=True)
    results_path = os.path.join(args.outdir, "results.jsonl")
    if os.path.exists(results_path):
        os.remove(results_path)

    for pair in range(args.pairs):
        if pair % 2 == 0:
            order = [("off", "A"), ("off", "B"), ("on", "B"), ("on", "A")]
        else:
            order = [("off", "B"), ("off", "A"), ("on", "A"), ("on", "B")]
        for pos, (condition, arm) in enumerate(order):
            record = run_one(args, condition, arm, pair, pos, args.outdir)
            print("%s pair=%d pos=%d %s %s rc=%d elapsed=%.2fs" %
                  (args.label, pair, pos, condition, arm, record["rc"],
                   record["elapsed_s"]), flush=True)
            if record["rc"] != 0:
                print("NONZERO EXIT: %s" % os.path.join(
                    args.outdir, "%s_p%02d_pos%d_%s_%s.jsonl" %
                    (args.label, pair, pos, arm, condition)), file=sys.stderr)
    print("done %s -> %s" % (args.label, results_path))


if __name__ == "__main__":
    main()
