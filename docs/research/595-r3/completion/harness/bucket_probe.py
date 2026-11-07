#!/usr/bin/env python3
"""Thread-bucket probe (descriptive only; never used as a timed block).

The timed-block sampler compared only the first and the last global samples,
so threads whose lifetime lies strictly inside the invocation (the W-03 driver
thread, per-pass) were recorded as 0 ticks.  This probe samples every 20 ms
and takes each thread's first-seen and last-seen counters, so the buckets are
resolved:

  app/submitter : tid == pid
  driver        : other user threads (comm not iou-*)
  io-wq         : threads whose comm starts with iou- (kernel workers)
  in-thread kernel time (stime) is reported per class as a subset, never added
  IRQ           : /proc/interrupts per-CPU delta (upper bound, not attributable)

Total executor CPU = bench process CPU (all threads; primary timed metric)
plus any io-wq CPU outside the thread group (reported as a bound).  Buckets are
shares of the same process CPU and are never summed into a second total.
"""
import argparse
import hashlib
import json
import os
import re
import sys
import threading
import time

TASKSET = "/usr/sbin/taskset"
TICK = os.sysconf("SC_CLK_TCK")


def iso_now():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_text(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return None


def task_stat(pid, tid):
    try:
        with open(f"/proc/{pid}/task/{tid}/stat") as f:
            st = f.read()
    except OSError:
        return None
    rp = st.rfind(")")
    comm = st[st.find("(") + 1:rp]
    fields = st[rp + 2:].split()
    if len(fields) < 37:
        return None
    return {"comm": comm, "utime": int(fields[11]), "stime": int(fields[12]),
            "processor": int(fields[36])}


def read_interrupts():
    per_cpu = {}
    try:
        with open("/proc/interrupts") as f:
            lines = f.read().splitlines()
    except OSError:
        return per_cpu
    if not lines:
        return per_cpu
    cpus = [h.replace("CPU", "") for h in lines[0].split()]
    for cpu in cpus:
        per_cpu[cpu] = 0
    for line in lines[1:]:
        parts = line.split()
        if len(parts) < 1 + len(cpus):
            continue
        for cpu, v in zip(cpus, parts[1:1 + len(cpus)]):
            if v.isdigit():
                per_cpu[cpu] += int(v)
    return per_cpu


def irq_delta(a, b):
    out = {}
    for cpu in sorted(set(a) | set(b)):
        out[cpu] = b.get(cpu, 0) - a.get(cpu, 0)
    out["total"] = sum(b.values()) - sum(a.values())
    return out


class Sampler(threading.Thread):
    def __init__(self, pid, period=0.02):
        super().__init__(daemon=True)
        self.pid = pid
        self.period = period
        self.stop_ev = threading.Event()
        self.seen = {}
        self.max_threads = 0

    def run(self):
        while not self.stop_ev.is_set():
            self.sample()
            self.stop_ev.wait(self.period)

    def sample(self):
        pid = self.pid
        try:
            tids = os.listdir(f"/proc/{pid}/task")
        except OSError:
            return
        for tid in tids:
            st = task_stat(pid, int(tid))
            if st is None:
                continue
            rec = self.seen.setdefault(int(tid), {"first": st, "last": st, "n": 0})
            rec["last"] = st
            rec["n"] += 1
        status = read_text(f"/proc/{pid}/status")
        if status:
            m = re.search(r"^Threads:\s*(\d+)", status, re.M)
            if m:
                self.max_threads = max(self.max_threads, int(m.group(1)))


def parse_jsonl(path, case):
    measured = []
    if not os.path.exists(path):
        return measured
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            r = json.loads(line)
            if r.get("case_id") == case and r.get("measured"):
                measured.append(r)
    return measured


def run_one(args, tag):
    d = args.out_dir
    os.makedirs(os.path.join(d, "logs"), exist_ok=True)
    os.makedirs(os.path.join(d, "jsonl"), exist_ok=True)
    jpath = os.path.join(d, "jsonl", f"{tag}.jsonl")
    opath = os.path.join(d, "logs", f"{tag}.log")
    for p in (jpath, opath):
        if os.path.exists(p):
            os.unlink(p)
    env = {k: v for k, v in os.environ.items() if not k.startswith("SLUICE_")}
    env.update({
        "SLUICE_BENCH_WARMUP": "0",
        "SLUICE_BENCH_MEASURED": "1",
        "SLUICE_BENCH_CASE_FILTER": "=" + args.case,
        "SLUICE_BENCH_STATS": "1",
        "SLUICE_BENCH_JSONL": jpath,
        "SLUICE_BENCH_TMP_DIR": args.tmp_dir,
    })
    for kv in args.env:
        k, v = kv.split("=", 1)
        env[k] = v
    if args.arm_var:
        env[args.arm_var] = args.arm
    irq_a = read_interrupts()
    cmd = [TASKSET, "-c", args.cpu, args.binary]
    fa = [(os.POSIX_SPAWN_OPEN, 1, opath, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644),
          (os.POSIX_SPAWN_DUP2, 1, 2)]
    t0 = time.monotonic()
    pid = os.posix_spawn(TASKSET, cmd, env, file_actions=fa)
    sampler = Sampler(pid)
    sampler.start()
    _, status, ru = os.wait4(pid, 0)
    wall = time.monotonic() - t0
    sampler.stop_ev.set()
    sampler.join(timeout=2.0)
    irq_b = read_interrupts()
    measured = parse_jsonl(jpath, args.case)
    buckets = {"main": {"ticks": 0, "stime": 0, "threads": []},
               "driver": {"ticks": 0, "stime": 0, "threads": []},
               "io_wq": {"ticks": 0, "stime": 0, "threads": []}}
    for tid, rec in sampler.seen.items():
        a, b = rec["first"], rec["last"]
        ticks = (b["utime"] + b["stime"]) - (a["utime"] + a["stime"])
        stime = b["stime"] - a["stime"]
        comm = b["comm"]
        if tid == pid:
            cls = "main"
        elif comm.startswith("iou-"):
            cls = "io_wq"
        else:
            cls = "driver"
        buckets[cls]["ticks"] += ticks
        buckets[cls]["stime"] += stime
        buckets[cls]["threads"].append({"tid": tid, "comm": comm, "samples": rec["n"],
                                        "ticks": ticks, "stime": stime})
    bench = {}
    if measured:
        m = measured[0]
        bench = {"cpu_s": m["cpu_s"], "ops": m["ops"],
                 "cpu_ns_per_op": 1e9 * m["cpu_s"] / m["ops"] if m.get("ops") else None,
                 "ops_per_sec": m["ops_per_sec"],
                 "p50_ns": m["lat_p50_ns"], "p95_ns": m["lat_p95_ns"], "p99_ns": m["lat_p99_ns"],
                 "stats_submitted_ops": m["stats_submitted_ops"],
                 "stats_completed_ops": m["stats_completed_ops"]}
    rec = {
        "tag": tag, "case": args.case, "arm": args.arm, "ts_utc": iso_now(),
        "exit": os.waitstatus_to_exitcode(status), "wall_s": wall,
        "ru_total": ru.ru_utime + ru.ru_stime, "ru_maxrss_kb": ru.ru_maxrss,
        "binary_sha256": sha256_file(args.binary),
        "bench": bench, "buckets": buckets,
        "max_user_threads": sampler.max_threads,
        "thread_ticks_total": sum(v["ticks"] for v in buckets.values()),
        "ru_ticks_est": round((ru.ru_utime + ru.ru_stime) * TICK, 3),
        "unattributed_ticks_est": round((ru.ru_utime + ru.ru_stime) * TICK
                                        - sum(v["ticks"] for v in buckets.values()), 3),
        "irq_delta": irq_delta(irq_a, irq_b),
        "jsonl": jpath, "log": opath,
    }
    with open(os.path.join(d, "results.jsonl"), "a") as f:
        f.write(json.dumps(rec) + "\n")
    print(f"[bucket-probe] {tag} exit={rec['exit']} cpu_s={bench.get('cpu_s')} "
          f"main={buckets['main']['ticks']} driver={buckets['driver']['ticks']} "
          f"iowq={buckets['io_wq']['ticks']} ru={rec['ru_total']:.3f}s", flush=True)
    return rec


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--case", required=True)
    ap.add_argument("--arm-var", default="")
    ap.add_argument("--arm", default="")
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--cpu", default="2")
    ap.add_argument("--repeats", type=int, default=3)
    ap.add_argument("--tmp-dir", default="/tmp/sluice-595-r3c")
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--label", required=True)
    args = ap.parse_args()
    os.makedirs(args.out_dir, exist_ok=True)
    for r in range(1, args.repeats + 1):
        run_one(args, f"{args.label}-r{r}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
