#!/usr/bin/env python3
"""#595 E5 pairing harness: one pair = A then B (even) or B then A (odd).

Each invocation is a fresh bench process pinned with taskset, warmup=1,
measured=1, with per-invocation JSONL. Records: wait4 rusage, wall, loadavg,
bench per-rep record (phase CPU is the primary CPU metric per PREREGISTERED.md),
optional /proc thread samples for the accounting consistency check.

Experiment driver only; lives outside the repository.
"""
import argparse
import hashlib
import json
import os
import sys
import threading
import time

TASKSET = "/usr/bin/taskset"


def iso_now():
    return time.strftime("%Y-%m-%dT%H:%M:%S%z")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


class ThreadSampler(threading.Thread):
    """Samples /proc/<pid>/task/* comm + stat while the child lives."""

    def __init__(self, pid, period_s):
        super().__init__(daemon=True)
        self.pid = pid
        self.period = period_s
        self.stop = threading.Event()
        self.samples = []

    def run(self):
        while not self.stop.is_set():
            self.sample_once()
            self.stop.wait(self.period)

    def sample_once(self):
        base = f"/proc/{self.pid}/task"
        try:
            tids = os.listdir(base)
        except OSError:
            return
        threads = []
        for tid in tids:
            try:
                with open(f"{base}/{tid}/comm") as f:
                    comm = f.read().strip()
                with open(f"{base}/{tid}/stat") as f:
                    st = f.read()
                rp = st.rfind(")")
                fields = st[rp + 2:].split()
                proc_cpu = int(fields[36]) if len(fields) > 36 else -1
                threads.append({
                    "tid": int(tid),
                    "comm": comm,
                    "cpu_ticks": int(fields[11]) + int(fields[12]),
                    "processor": proc_cpu,
                })
            except (OSError, IndexError, ValueError):
                continue
        if threads:
            self.samples.append({"t": time.monotonic(), "threads": threads})


def parse_jsonl(path, case_id):
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
            if r.get("case_id") != case_id:
                continue
            if r.get("measured"):
                measured.append(r)
            else:
                warmup += 1
    return measured, warmup


def run_invocation(binary_sha, args, case, arm, pair_index, pos, order_label):
    raw_dir = os.path.join(args.out_dir, args.phase)
    os.makedirs(os.path.join(raw_dir, "logs"), exist_ok=True)
    os.makedirs(os.path.join(raw_dir, "jsonl"), exist_ok=True)
    tag = f"p{pair_index:03d}-{pos}-{arm}"
    jpath = os.path.join(raw_dir, "jsonl", f"{tag}.jsonl")
    opath = os.path.join(raw_dir, "logs", f"{tag}.log")
    if os.path.exists(jpath):
        os.unlink(jpath)

    env = os.environ.copy()
    env.update({
        "SLUICE_BENCH_WARMUP": "1",
        "SLUICE_BENCH_MEASURED": str(args.measured),
        "SLUICE_BENCH_CASE_FILTER": "=" + case,
        "SLUICE_BENCH_STATS": "1",
        "SLUICE_BENCH_JSONL": jpath,
        "SLUICE_595_ARM": arm,
    })
    env.setdefault("SLUICE_BENCH_TMP_DIR", "/tmp/sluice-595-bench")
    for kv in args.env:
        k, v = kv.split("=", 1)
        env[k] = v

    cmd = [TASKSET, "-c", args.cpu, args.binary]
    fa = [(os.POSIX_SPAWN_OPEN, 1, opath,
           os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)]
    load_before = list(os.getloadavg())
    t0 = time.monotonic()
    pid = os.posix_spawn(cmd[0], cmd, env, file_actions=fa)
    sampler = None
    if args.sample_threads:
        sampler = ThreadSampler(pid, args.sample_period)
        sampler.start()
    _, status, ru = os.wait4(pid, 0)
    wall = time.monotonic() - t0
    if sampler is not None:
        sampler.stop.set()
        sampler.join(timeout=1.0)
    load_after = list(os.getloadavg())
    exit_code = os.waitstatus_to_exitcode(status)

    measured, warmup_n = parse_jsonl(jpath, case)
    rec = {
        "phase": args.phase,
        "case": case,
        "arm": arm,
        "pair": pair_index,
        "pos": pos,
        "order": order_label,
        "ts": iso_now(),
        "exit": exit_code,
        "wall_s": wall,
        "ru_utime": ru.ru_utime,
        "ru_stime": ru.ru_stime,
        "ru_total": ru.ru_utime + ru.ru_stime,
        "ru_maxrss_kb": ru.ru_maxrss,
        "loadavg_before": load_before,
        "loadavg_after": load_after,
        "cpu_set": args.cpu,
        "binary": os.path.abspath(args.binary),
        "binary_sha256": binary_sha,
        "env_overrides": args.env,
        "jsonl": jpath,
        "log": opath,
        "warmup_records": warmup_n,
        "measured_records": len(measured),
    }
    if len(measured) >= 1:
        ms = measured
        ops_tot = sum(r["ops"] for r in ms)
        cpu_tot = sum(r["cpu_s"] for r in ms)
        wall_tot = sum(r["wall_s"] for r in ms)
        nlat = len(ms)
        mean_of = lambda key: sum(r[key] for r in ms) / nlat
        rec["reps"] = ms
        rec["metrics"] = {
            "cpu_s": cpu_tot,
            "ops": ops_tot,
            "cpu_ns_per_op": 1e9 * cpu_tot / ops_tot if ops_tot else None,
            "ops_per_sec": ops_tot / wall_tot if wall_tot else None,
            "p50_ns": mean_of("lat_p50_ns"),
            "p95_ns": mean_of("lat_p95_ns"),
            "p99_ns": mean_of("lat_p99_ns"),
            "wall_s": wall_tot,
            "stats_submit_calls": sum(r["stats_submit_calls"] for r in ms),
            "stats_poll_calls": sum(r["stats_poll_calls"] for r in ms),
            "stats_wait_calls": sum(r["stats_wait_calls"] for r in ms),
            "submits": sum(r["submits"] for r in ms),
        }
    if sampler is not None:
        rec["thread_samples"] = sampler.samples

    results_path = os.path.join(raw_dir, "results.jsonl")
    with open(results_path, "a") as f:
        f.write(json.dumps(rec) + "\n")

    m = rec.get("metrics", {})
    print(f"[{args.phase}] pair={pair_index} pos={pos} arm={arm} exit={exit_code} "
          f"wall={wall:.3f}s cpu/op={m.get('cpu_ns_per_op')} p99={m.get('p99_ns')} "
          f"tput={m.get('ops_per_sec')}", flush=True)
    return rec


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--phase", required=True)
    ap.add_argument("--case", required=True)
    ap.add_argument("--pairs", type=int, required=True)
    ap.add_argument("--measured", type=int, default=1,
                    help="measured reps per invocation (pair metric aggregates them)")
    ap.add_argument("--binary", required=True)
    ap.add_argument("--cpu", default="2")
    ap.add_argument("--arms", default="AB", choices=["AB", "AA"])
    ap.add_argument("--out-dir", default="/home/jnhu/sluice-595-e5/evidence/measurement/raw")
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--sample-threads", action="store_true")
    ap.add_argument("--sample-period", type=float, default=0.02)
    ap.add_argument("--max-loadavg", type=float, default=1.5)
    args = ap.parse_args()

    if not os.access(TASKSET, os.X_OK):
        sys.exit(f"taskset not executable: {TASKSET}")
    if not os.access(args.binary, os.X_OK):
        sys.exit(f"bench binary not executable: {args.binary}")

    la = os.getloadavg()[0]
    if la > args.max_loadavg:
        sys.exit(f"loadavg {la:.2f} exceeds --max-loadavg {args.max_loadavg}: "
                 "foreign load would contaminate the round")

    binary_sha = sha256_file(args.binary)
    header = {
        "phase": args.phase, "case": args.case, "pairs": args.pairs,
        "arms": args.arms, "cpu": args.cpu, "binary": os.path.abspath(args.binary),
        "binary_sha256": binary_sha, "env_overrides": args.env,
        "measured_per_invocation": args.measured,
        "started": iso_now(), "loadavg_start": list(os.getloadavg()),
        "host_kernel": os.uname().release,
    }
    raw_dir = os.path.join(args.out_dir, args.phase)
    os.makedirs(raw_dir, exist_ok=True)
    with open(os.path.join(raw_dir, "header.json"), "w") as f:
        json.dump(header, f, indent=1)

    for pair in range(args.pairs):
        if args.arms == "AA":
            order = ["A", "A"]
        else:
            order = ["A", "B"] if pair % 2 == 0 else ["B", "A"]
        label = "".join(order)
        for pos, arm in enumerate(order):
            run_invocation(binary_sha, args, args.case, arm, pair, pos, label)

    print(f"[{args.phase}] done: {args.pairs} pairs -> {raw_dir}", flush=True)


if __name__ == "__main__":
    main()
