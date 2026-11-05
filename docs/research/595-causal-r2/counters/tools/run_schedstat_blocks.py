#!/usr/bin/env python3
"""C4 scheduler-domain A/B runner for #595 causal-r2.

Per-thread /proc differencing at scheduler granularity, no bench-binary probes
(frozen build). For every bench task tid the sampler keeps the last monotone
value of:

  /proc/<tid>/schedstat  -> on_cpu_ns, runqueue_wait_ns, timeslices
                            (CONFIG_SCHEDSTATS cumulative counters, ns exact)
  /proc/<tid>/stat       -> utime, stime (10 ms ticks), processor (last CPU)
  /proc/<tid>/status     -> voluntary/nonvoluntary ctxt switches
  /proc/<tid>/sched      -> nr_switches, nr_voluntary_switches,
                            nr_involuntary_switches, se.nr_migrations

plus /proc/pressure/cpu (PSI cpu some-total, microseconds) read once before and
once after each invocation (system-wide, includes every task on the host).

The runner process pins itself to --sampler-cpu so the sampler thread cannot
land on the bench CPU or its SMT sibling. --no-sampler runs the identical
protocol with zero external reads (probe-overhead OFF arm).
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
    if len(fields) < 40:
        return None
    return {"comm": comm, "utime": int(fields[11]), "stime": int(fields[12]),
            "processor": int(fields[36])}


def parse_schedstat(text):
    if text is None:
        return None
    parts = text.split()
    if len(parts) != 3:
        return None
    try:
        return tuple(int(p) for p in parts)
    except ValueError:
        return None


def parse_status(text):
    out = {}
    if text is None:
        return out
    for line in text.splitlines():
        if line.startswith("voluntary_ctxt_switches:"):
            out["voluntary"] = int(line.split(":")[1])
        elif line.startswith("nonvoluntary_ctxt_switches:"):
            out["nonvoluntary"] = int(line.split(":")[1])
    return out


SW_RE = {
    "nr_switches": re.compile(r"^nr_switches\s*:\s*(\d+)", re.M),
    "nr_voluntary": re.compile(r"^nr_voluntary_switches\s*:\s*(\d+)", re.M),
    "nr_involuntary": re.compile(r"^nr_involuntary_switches\s*:\s*(\d+)", re.M),
    "nr_migrations": re.compile(r"^se\.nr_migrations\s*:\s*(\d+)", re.M),
}


def parse_sched(text):
    out = {}
    if text is None:
        return out
    for key, rx in SW_RE.items():
        m = rx.search(text)
        if m:
            out[key] = int(m.group(1))
    return out


def read_psi_cpu_total_us():
    text = read_text("/proc/pressure/cpu")
    if text is None:
        return None
    m = re.search(r"some avg10=\S+ avg60=\S+ avg300=\S+ total=(\d+)", text)
    return int(m.group(1)) if m else None


class Sampler(threading.Thread):
    def __init__(self, root_pid, interval=0.001):
        super().__init__(daemon=True)
        self.root_pid = root_pid
        self.interval = interval
        self.stop_evt = threading.Event()
        self.threads = {}
        self.samples = 0
        self.errors = 0
        self.t_first = None
        self.t_last = None

    def _sample_once(self):
        task_dir = "/proc/%d/task" % self.root_pid
        try:
            tids = os.listdir(task_dir)
        except (FileNotFoundError, ProcessLookupError):
            return
        now = time.time()
        for tid_s in tids:
            stat = read_text("%s/%s/stat" % (task_dir, tid_s))
            if stat is None:
                self.errors += 1
                continue
            parsed = parse_stat(stat)
            if parsed is None:
                self.errors += 1
                continue
            tid = int(tid_s)
            entry = self.threads.get(tid)
            if entry is None:
                entry = {"comm": parsed["comm"], "utime": 0, "stime": 0,
                         "on_cpu_ns": 0, "runwait_ns": 0, "timeslices": 0,
                         "voluntary": 0, "nonvoluntary": 0,
                         "nr_switches": 0, "nr_voluntary": 0,
                         "nr_involuntary": 0, "nr_migrations": 0,
                         "cpus_seen": set(), "first_seen": now,
                         "last_seen": now}
                self.threads[tid] = entry
            entry["comm"] = parsed["comm"]
            entry["utime"] = max(entry["utime"], parsed["utime"])
            entry["stime"] = max(entry["stime"], parsed["stime"])
            entry["cpus_seen"].add(parsed["processor"])
            entry["last_seen"] = now
            ss = parse_schedstat(read_text("%s/%s/schedstat" % (task_dir, tid_s)))
            if ss is not None:
                entry["on_cpu_ns"] = max(entry["on_cpu_ns"], ss[0])
                entry["runwait_ns"] = max(entry["runwait_ns"], ss[1])
                entry["timeslices"] = max(entry["timeslices"], ss[2])
            else:
                self.errors += 1
            st = parse_status(read_text("%s/%s/status" % (task_dir, tid_s)))
            if st:
                if "voluntary" in st:
                    entry["voluntary"] = max(entry["voluntary"], st["voluntary"])
                if "nonvoluntary" in st:
                    entry["nonvoluntary"] = max(entry["nonvoluntary"],
                                                st["nonvoluntary"])
            sc = parse_sched(read_text("%s/%s/sched" % (task_dir, tid_s)))
            if sc:
                for key in SW_RE:
                    if key in sc:
                        entry[key] = max(entry[key], sc[key])
        if self.t_first is None:
            self.t_first = now
        self.t_last = now
        self.samples += 1

    def run(self):
        while not self.stop_evt.is_set():
            self._sample_once()
            time.sleep(self.interval)

    def aggregate(self):
        buckets = {}
        for name in ("bench", "iowq", "other"):
            buckets[name] = {"on_cpu_ns": 0, "runwait_ns": 0, "timeslices": 0,
                             "user_s": 0.0, "kernel_s": 0.0,
                             "voluntary": 0, "nonvoluntary": 0,
                             "nr_switches": 0, "nr_voluntary": 0,
                             "nr_involuntary": 0, "nr_migrations": 0,
                             "threads": 0, "cpus_seen": [],
                             "thread_lifetimes_s": 0.0}
        for entry in self.threads.values():
            comm = entry["comm"]
            if comm.startswith(BENCH_PREFIX):
                bucket = buckets["bench"]
            elif comm.startswith(IOWQ_PREFIX):
                bucket = buckets["iowq"]
            else:
                bucket = buckets["other"]
            bucket["on_cpu_ns"] += entry["on_cpu_ns"]
            bucket["runwait_ns"] += entry["runwait_ns"]
            bucket["timeslices"] += entry["timeslices"]
            bucket["user_s"] += entry["utime"] / TICK
            bucket["kernel_s"] += entry["stime"] / TICK
            bucket["voluntary"] += entry["voluntary"]
            bucket["nonvoluntary"] += entry["nonvoluntary"]
            bucket["nr_switches"] += entry["nr_switches"]
            bucket["nr_voluntary"] += entry["nr_voluntary"]
            bucket["nr_involuntary"] += entry["nr_involuntary"]
            bucket["nr_migrations"] += entry["nr_migrations"]
            bucket["threads"] += 1
            bucket["cpus_seen"].append(sorted(entry["cpus_seen"]))
            bucket["thread_lifetimes_s"] += entry["last_seen"] - entry["first_seen"]
        return {"samples": self.samples, "errors": self.errors,
                "sample_span_s": (self.t_last - self.t_first)
                if self.t_first is not None else 0.0,
                **buckets}


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


def measured_view(records, passes):
    measured = [r for r in records if r.get("measured")]
    ops = sum(r["ops"] for r in measured)
    cpu_s = sum(r["cpu_s"] for r in measured)
    wall_s = sum(r["wall_s"] for r in measured)
    p95 = sum(r["lat_p95_ns"] for r in measured) / len(measured) if measured else 0
    p99 = sum(r["lat_p99_ns"] for r in measured) / len(measured) if measured else 0
    return {"ops_measured": ops, "cpu_s": cpu_s, "wall_s": wall_s,
            "cpu_ns_per_op": 1e9 * cpu_s / ops if ops else None,
            "tput": ops / wall_s if wall_s else None,
            "p95_ns": p95, "p99_ns": p99,
            "ops_window": passes * 20000}


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

    passes = args.warmup + args.measured
    psi_pre = read_psi_cpu_total_us()
    start = time.time()
    with open(stdout, "wb") as fout, open(stderr, "wb") as ferr:
        proc = subprocess.Popen(["taskset", "-c", args.cpu, args.bin],
                                env=env, stdout=fout, stderr=ferr)
        sampler = None
        if not args.no_sampler:
            sampler = Sampler(proc.pid, interval=args.sampler_interval)
            sampler.start()
        rc = proc.wait()
        if sampler is not None:
            sampler.stop_evt.set()
            sampler.join(timeout=1.0)
    elapsed = time.time() - start
    psi_post = read_psi_cpu_total_us()

    record = {
        "label": args.label, "pair": pair, "pos": pos, "arm": arm,
        "case": args.case, "rc": rc, "elapsed_s": elapsed,
        "psi_cpu_total_us": {"pre": psi_pre, "post": psi_post},
        "measured_view": measured_view(load_jsonl(jsonl), passes),
    }
    if sampler is not None:
        record["threads"] = sampler.aggregate()
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
    parser.add_argument("--measured", type=int, default=20)
    parser.add_argument("--cpu", default="2")
    parser.add_argument("--sampler-cpu", type=int, default=5)
    parser.add_argument("--sampler-interval", type=float, default=0.001)
    parser.add_argument("--no-sampler", action="store_true")
    parser.add_argument("--tmpdir", default="/tmp/sluice-595-cost")
    parser.add_argument("--outdir", required=True)
    args = parser.parse_args()

    if not args.no_sampler:
        os.sched_setaffinity(0, {args.sampler_cpu})

    os.makedirs(args.outdir, exist_ok=True)
    results_path = os.path.join(args.outdir, "results.jsonl")
    if os.path.exists(results_path):
        os.remove(results_path)

    for pair in range(args.pairs):
        order = ["A", "B"] if pair % 2 == 0 else ["B", "A"]
        for pos, arm in enumerate(order):
            record = run_one(args, arm, pair, pos, args.outdir)
            mv = record["measured_view"]
            print("%s pair=%d pos=%d %s rc=%d elapsed=%.2fs cpu_ns/op=%.1f "
                  "tput=%.0f" %
                  (args.label, pair, pos, arm, record["rc"], record["elapsed_s"],
                   mv["cpu_ns_per_op"] if mv["cpu_ns_per_op"] else -1,
                   mv["tput"] if mv["tput"] else -1), flush=True)
            if record["rc"] != 0:
                print("NONZERO EXIT", file=sys.stderr)
    print("done %s -> %s" % (args.label, results_path))


if __name__ == "__main__":
    main()
