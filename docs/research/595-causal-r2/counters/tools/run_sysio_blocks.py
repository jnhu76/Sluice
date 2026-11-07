#!/usr/bin/env python3
"""Uninstrumented A/B runner for #595 causal-r2 C3: per-thread /proc/<tid>/io
differencing (kernel-accounted syscall face: syscr/syscw/rchar/wchar) plus the
same /proc stat/status sampling as the split blocks. The bench binary carries
no research probes (frozen build). read/write syscalls on the bench thread are
the eventfd drain/signal face; io_uring's internal read work does not pass the
syscall accounting layer, so any io-wq activity stays invisible here by
construction (that invisibility is itself recorded).
"""

import argparse
import json
import os
import re
import subprocess
import sys
import threading
import time

TICK = os.sysconf("SC_CLK_TCK")
BENCH_PREFIX = "bench_w02"
IOWQ_PREFIX = "iou-wrk"


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


IO_KEYS = ("rchar", "wchar", "syscr", "syscw")


def parse_io(text):
    out = {}
    if text is None:
        return out
    for line in text.splitlines():
        m = re.match(r"^(rchar|wchar|syscr|syscw|read_bytes|write_bytes):\s+(\d+)$",
                     line.strip())
        if m:
            out[m.group(1)] = int(m.group(2))
    return out


class Sampler(threading.Thread):
    def __init__(self, root_pid, interval=0.001):
        super().__init__(daemon=True)
        self.root_pid = root_pid
        self.interval = interval
        self.stop_evt = threading.Event()
        self.threads = {}
        self.samples = 0
        self.errors = 0

    def _sample_once(self):
        pid = self.root_pid
        task_dir = "/proc/%d/task" % pid
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
            entry = self.threads.get(int(tid_s))
            if entry is None:
                entry = {"comm": parsed["comm"], "utime": 0, "stime": 0}
                for key in IO_KEYS:
                    entry[key] = 0
                self.threads[int(tid_s)] = entry
            entry["comm"] = parsed["comm"]
            entry["utime"] = max(entry["utime"], parsed["utime"])
            entry["stime"] = max(entry["stime"], parsed["stime"])
            io = parse_io(read_text("%s/%s/io" % (task_dir, tid_s)))
            if io:
                for key in IO_KEYS:
                    if key in io:
                        entry[key] = max(entry[key], io[key])
            else:
                self.errors += 1
        self.samples += 1

    def run(self):
        while not self.stop_evt.is_set():
            self._sample_once()
            time.sleep(self.interval)

    def aggregate(self):
        buckets = {
            "bench": {"user_s": 0.0, "kernel_s": 0.0, "threads": 0,
                      **{k: 0 for k in IO_KEYS}},
            "iowq": {"user_s": 0.0, "kernel_s": 0.0, "threads": 0,
                     **{k: 0 for k in IO_KEYS}},
            "other": {"user_s": 0.0, "kernel_s": 0.0, "threads": 0,
                      **{k: 0 for k in IO_KEYS}},
        }
        for entry in self.threads.values():
            comm = entry["comm"]
            if comm.startswith(BENCH_PREFIX):
                bucket = buckets["bench"]
            elif comm.startswith(IOWQ_PREFIX):
                bucket = buckets["iowq"]
            else:
                bucket = buckets["other"]
            bucket["user_s"] += entry["utime"] / TICK
            bucket["kernel_s"] += entry["stime"] / TICK
            bucket["threads"] += 1
            for key in IO_KEYS:
                bucket[key] += entry[key]
        return {**buckets, "samples": self.samples, "errors": self.errors}


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


def run_one(args, arm, pair, pos, outdir):
    tag = "%s_p%02d_pos%d_%s" % (args.label, pair, pos, arm)
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
        "SLUICE_BENCH_REUSE_FIXTURE": "1",
    })
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
        "case": args.case, "rc": rc, "elapsed_s": elapsed,
        "threads": sampler.aggregate(),
        "records": load_jsonl(jsonl),
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
        order = ["A", "B"] if pair % 2 == 0 else ["B", "A"]
        for pos, arm in enumerate(order):
            record = run_one(args, arm, pair, pos, args.outdir)
            io = record["threads"]
            print("%s pair=%d pos=%d %s rc=%d elapsed=%.2fs bench_syscr=%d "
                  "bench_syscw=%d bench_rchar=%d bench_wchar=%d iowq_syscr=%d "
                  "iowq_syscw=%d" %
                  (args.label, pair, pos, arm, record["rc"], record["elapsed_s"],
                   io["bench"]["syscr"], io["bench"]["syscw"],
                   io["bench"]["rchar"], io["bench"]["wchar"],
                   io["iowq"]["syscr"], io["iowq"]["syscw"]), flush=True)
            if record["rc"] != 0:
                print("NONZERO EXIT", file=sys.stderr)
    print("done %s -> %s" % (args.label, results_path))


if __name__ == "__main__":
    main()
