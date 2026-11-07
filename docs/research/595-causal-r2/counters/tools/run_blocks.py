#!/usr/bin/env python3
"""Paired A/B runner for the #595 causal-r2 cost-accounting round.

Each invocation is one bench process, pinned to cpu2 with taskset. An external
sampler reads /proc/<bench>/task/*/{stat,status,sched} every 1 ms and keeps the
last monotone value per tid, so bench-thread and iou-wrk CPU are separated
without double counting. PMU mode wraps the invocation in perf stat with
user-mode events only (kernel-mode PMU is not permitted: perf_event_paranoid=2
and no sudo/CAP_PERFMON in this session).
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

PERF_EVENTS = (
    "cycles:u,instructions:u,cache-references:u,cache-misses:u,"
    "branch-misses:u,task-clock:u,page-faults:u,minor-faults:u,major-faults:u"
)


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
    return {
        "comm": comm,
        "minflt": int(fields[7]),
        "majflt": int(fields[9]),
        "utime": int(fields[11]),
        "stime": int(fields[12]),
    }


def parse_status(text):
    out = {}
    for line in text.splitlines():
        if line.startswith("voluntary_ctxt_switches:"):
            out["voluntary"] = int(line.split(":")[1])
        elif line.startswith("nonvoluntary_ctxt_switches:"):
            out["nonvoluntary"] = int(line.split(":")[1])
    return out


MIG_RE = re.compile(r"se\.nr_migrations\s*:\s*(\d+)")


def parse_migrations(text):
    match = MIG_RE.search(text)
    return int(match.group(1)) if match else None


class Sampler(threading.Thread):
    def __init__(self, root_pid, perf_child=False, interval=0.001):
        super().__init__(daemon=True)
        self.root_pid = root_pid
        self.perf_child = perf_child
        self.interval = interval
        self.stop_evt = threading.Event()
        self.threads = {}
        self.bench_pid = None
        self.samples = 0
        self.errors = 0

    def _find_bench_pid(self):
        if not self.perf_child:
            return self.root_pid
        if self.bench_pid is not None:
            return self.bench_pid
        children = read_text("/proc/%d/task/%d/children" % (self.root_pid, self.root_pid))
        if children:
            parts = children.split()
            if parts:
                self.bench_pid = int(parts[0])
                return self.bench_pid
        return None

    def _sample_once(self):
        pid = self._find_bench_pid()
        if pid is None:
            return
        task_dir = "/proc/%d/task" % pid
        try:
            tids = os.listdir(task_dir)
        except (FileNotFoundError, ProcessLookupError):
            return
        for tid_s in tids:
            tid = int(tid_s)
            stat = read_text("%s/%s/stat" % (task_dir, tid_s))
            if stat is None:
                self.errors += 1
                continue
            parsed = parse_stat(stat)
            if parsed is None:
                self.errors += 1
                continue
            entry = self.threads.get(tid)
            if entry is None:
                entry = {"comm": parsed["comm"], "utime": 0, "stime": 0,
                         "voluntary": 0, "nonvoluntary": 0, "migrations": 0,
                         "minflt": 0, "majflt": 0}
                self.threads[tid] = entry
            entry["comm"] = parsed["comm"]
            entry["utime"] = max(entry["utime"], parsed["utime"])
            entry["stime"] = max(entry["stime"], parsed["stime"])
            entry["minflt"] = max(entry["minflt"], parsed["minflt"])
            entry["majflt"] = max(entry["majflt"], parsed["majflt"])
            status = read_text("%s/%s/status" % (task_dir, tid_s))
            if status is not None:
                st = parse_status(status)
                if "voluntary" in st:
                    entry["voluntary"] = max(entry["voluntary"], st["voluntary"])
                if "nonvoluntary" in st:
                    entry["nonvoluntary"] = max(entry["nonvoluntary"], st["nonvoluntary"])
            sched = read_text("%s/%s/sched" % (task_dir, tid_s))
            if sched is not None:
                mig = parse_migrations(sched)
                if mig is not None:
                    entry["migrations"] = max(entry["migrations"], mig)
        self.samples += 1

    def run(self):
        while not self.stop_evt.is_set():
            self._sample_once()
            time.sleep(self.interval)

    def aggregate(self):
        bench = {"user_s": 0.0, "kernel_s": 0.0, "voluntary": 0, "nonvoluntary": 0,
                 "migrations": 0, "minflt": 0, "majflt": 0, "threads": 0}
        iowq = {"user_s": 0.0, "kernel_s": 0.0, "voluntary": 0, "nonvoluntary": 0,
                "migrations": 0, "minflt": 0, "majflt": 0, "threads": 0}
        other = {"user_s": 0.0, "kernel_s": 0.0, "voluntary": 0, "nonvoluntary": 0,
                 "migrations": 0, "minflt": 0, "majflt": 0, "threads": 0}
        for entry in self.threads.values():
            comm = entry["comm"]
            if comm.startswith(BENCH_PREFIX):
                bucket = bench
            elif comm.startswith(IOWQ_PREFIX):
                bucket = iowq
            else:
                bucket = other
            bucket["user_s"] += entry["utime"] / TICK
            bucket["kernel_s"] += entry["stime"] / TICK
            bucket["voluntary"] += entry["voluntary"]
            bucket["nonvoluntary"] += entry["nonvoluntary"]
            bucket["migrations"] += entry["migrations"]
            bucket["minflt"] += entry["minflt"]
            bucket["majflt"] += entry["majflt"]
            bucket["threads"] += 1
        return {"bench": bench, "iowq": iowq, "other": other,
                "samples": self.samples, "errors": self.errors}


def parse_perf_csv(path):
    events = {}
    text = read_text(path)
    if text is None:
        return events
    for line in text.splitlines():
        if not line or line.startswith("#"):
            continue
        fields = line.split(",")
        if len(fields) < 5:
            continue
        event = fields[2]
        raw = fields[0].strip()
        try:
            value = float(raw)
        except ValueError:
            value = None
        try:
            pct = float(fields[4])
        except ValueError:
            pct = None
        events[event] = {"value": value, "pct": pct,
                         "run_time_ns": int(fields[3]) if fields[3].isdigit() else None}
    return events


def parse_counter_lines(path):
    lines = []
    text = read_text(path)
    if text is None:
        return lines
    prefix = "SLUICE595_COUNTERS "
    for line in text.splitlines():
        if not line.startswith(prefix):
            continue
        fields = line[len(prefix):].split()
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
        if not line:
            continue
        try:
            records.append(json.loads(line))
        except json.JSONDecodeError:
            pass
    return records


def run_one(args, arm, pair, pos, outdir):
    tag = "%s_p%02d_pos%d_%s" % (args.label, pair, pos, arm)
    jsonl = os.path.join(outdir, tag + ".jsonl")
    stdout = os.path.join(outdir, tag + ".out")
    stderr = os.path.join(outdir, tag + ".err")
    perf_csv = os.path.join(outdir, tag + ".perf.csv")
    env = dict(os.environ)
    env.update({
        "SLUICE_BENCH_595_DEPTHS": "1",
        "SLUICE_BENCH_CASE_FILTER": args.case,
        "SLUICE_BENCH_WARMUP": str(args.warmup),
        "SLUICE_BENCH_MEASURED": str(args.measured),
        "SLUICE_BENCH_STATS": "1",
        "SLUICE_BENCH_TMP_DIR": args.tmpdir,
        "SLUICE_BENCH_JSONL": jsonl,
    })
    if args.pmu_only:
        env["SLUICE_BENCH_PMU_ONLY"] = "1"
        env["SLUICE_BENCH_REUSE_FIXTURE"] = "1"
    if args.counters is not None:
        env["SLUICE_595_COUNTERS"] = "1" if args.counters else "0"
    if arm == "A":
        env.pop("SLUICE_595_ARM", None)
    else:
        env["SLUICE_595_ARM"] = "B"

    if args.mode == "pmu":
        cmd = ["taskset", "-c", args.harness_cpu, "perf", "stat", "-x,", "-e",
               PERF_EVENTS, "-o", perf_csv, "--", "taskset", "-c", args.cpu, args.bin]
    else:
        cmd = ["taskset", "-c", args.cpu, args.bin]

    for stale in (jsonl, stdout, stderr, perf_csv):
        try:
            os.remove(stale)
        except FileNotFoundError:
            pass

    start = time.time()
    with open(stdout, "wb") as fout, open(stderr, "wb") as ferr:
        proc = subprocess.Popen(cmd, env=env, stdout=fout, stderr=ferr)
        sampler = Sampler(proc.pid, perf_child=(args.mode == "pmu"))
        sampler.start()
        rc = proc.wait()
        sampler.stop_evt.set()
        sampler.join(timeout=1.0)
    elapsed = time.time() - start
    aggregate = sampler.aggregate()

    record = {
        "label": args.label, "mode": args.mode, "counters": args.counters,
        "pmu_only": args.pmu_only, "pair": pair, "pos": pos, "arm": arm,
        "case": args.case, "rc": rc, "elapsed_s": elapsed,
        "perf": parse_perf_csv(perf_csv) if args.mode == "pmu" else {},
        "threads": aggregate,
        "jsonl_path": jsonl,
        "records": load_jsonl(jsonl),
        "counter_lines": parse_counter_lines(stderr),
    }
    with open(os.path.join(outdir, "results.jsonl"), "a") as f:
        f.write(json.dumps(record) + "\n")
    return record


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--label", required=True)
    parser.add_argument("--mode", choices=["pmu", "direct"], default="pmu")
    parser.add_argument("--bin", required=True)
    parser.add_argument("--pairs", type=int, required=True)
    parser.add_argument("--arms", choices=["AB", "AA"], default="AB")
    parser.add_argument("--case", default="W02-uring-4k-d2")
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--measured", type=int, default=1)
    parser.add_argument("--pmu-only", action="store_true")
    parser.add_argument("--counters", type=int, choices=[0, 1], default=None)
    parser.add_argument("--cpu", default="2")
    parser.add_argument("--harness-cpu", default="3")
    parser.add_argument("--tmpdir", default="/tmp/sluice-595-cost")
    parser.add_argument("--outdir", required=True)
    args = parser.parse_args()

    os.makedirs(args.outdir, exist_ok=True)
    results_path = os.path.join(args.outdir, "results.jsonl")
    if os.path.exists(results_path):
        os.remove(results_path)

    for pair in range(args.pairs):
        order = ["A", "B"] if (args.arms == "AA" or pair % 2 == 0) else ["B", "A"]
        if args.arms == "AA":
            order = ["A", "A"]
        for pos, arm in enumerate(order):
            record = run_one(args, arm, pair, pos, args.outdir)
            print("%s pair=%d pos=%d arm=%s rc=%d elapsed=%.3fs" %
                  (args.label, pair, pos, arm, record["rc"], record["elapsed_s"]),
                  flush=True)
            if record["rc"] != 0:
                print("NONZERO EXIT: %s" % record["jsonl_path"], file=sys.stderr)
    print("done %s -> %s" % (args.label, results_path))


if __name__ == "__main__":
    main()
