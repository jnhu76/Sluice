#!/usr/bin/env python3
"""#595 R3-completion calibration/legacy pair runner (two-arm blocks only).

Each invocation is a fresh bench process pinned with taskset.  Before and after
every invocation the runner records loadavg, cpu2/cpu12 busy fraction,
governor/scaling_cur_freq, /tmp free space, binary sha256, case id and the exit
code.  A sampler thread records per-thread CPU ticks, io-wq kernel-worker ticks
and the max user-thread count; /proc/interrupts is read before/after.

A/A order (prereg section 6.2a): pair i even -> member1 then member2;
odd -> member2 then member1.  A/B order: even -> A then B; odd -> B then A.
Invalid invocations (exit != 0, oracle/stats failure, wrong measured count,
wrong ops, SHA mismatch, arm readback mismatch) are retried at most twice;
every attempt stays in results.jsonl and attempts.jsonl.

The runner itself must be launched with the shared flock already held on fd 9
(the wrapper records acquisition); the lock inode/pid are recorded in the
block header from /proc/locks.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import sys
import threading
import time

TASKSET = "/usr/sbin/taskset"
TICK = os.sysconf("SC_CLK_TCK")


def iso_now():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def read_text(path):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return None


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_cpu_totals():
    out = {}
    try:
        with open("/proc/stat") as f:
            for line in f:
                if not line.startswith("cpu"):
                    continue
                parts = line.split()
                name = parts[0]
                if name == "cpu":
                    continue
                nums = [int(x) for x in parts[1:]]
                idle = nums[3] + (nums[4] if len(nums) > 4 else 0)
                total = sum(nums)
                out[name[3:]] = (total - idle, total)
    except OSError:
        pass
    return out


def cpu_busy_fraction(cpus, interval=0.1):
    a = read_cpu_totals()
    time.sleep(interval)
    b = read_cpu_totals()
    out = {}
    for cpu in cpus:
        if cpu in a and cpu in b:
            db = b[cpu][0] - a[cpu][0]
            dt = b[cpu][1] - a[cpu][1]
            out[cpu] = round(db / dt, 6) if dt > 0 else None
        else:
            out[cpu] = None
    return out


def cpufreq_state(cpus):
    out = {}
    for cpu in cpus:
        base = f"/sys/devices/system/cpu/cpu{cpu}/cpufreq"
        gov = read_text(base + "/scaling_governor")
        freq = read_text(base + "/scaling_cur_freq")
        out[cpu] = {
            "governor": gov,
            "scaling_cur_freq": int(freq) if freq and freq.isdigit() else None,
        }
    return out


def tmp_df():
    st = os.statvfs("/tmp")
    total = st.f_blocks * st.f_frsize
    avail = st.f_bavail * st.f_frsize
    return {
        "total_bytes": total,
        "avail_bytes": avail,
        "used_pct": round(100.0 * (total - avail) / total, 2) if total else None,
    }


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
        vals = parts[1:1 + len(cpus)]
        for cpu, v in zip(cpus, vals):
            if v.isdigit():
                per_cpu[cpu] += int(v)
    return per_cpu


def irq_delta(before, after):
    cpus = sorted(set(before) | set(after))
    out = {}
    for cpu in cpus:
        out[cpu] = after.get(cpu, 0) - before.get(cpu, 0)
    out["total"] = sum(after.values()) - sum(before.values())
    return out


def proc_task_stat(pid, tid):
    try:
        with open(f"/proc/{pid}/task/{tid}/stat") as f:
            st = f.read()
    except OSError:
        return None
    rp = st.rfind(")")
    if rp < 0:
        return None
    comm = st[st.find("(") + 1:rp]
    fields = st[rp + 2:].split()
    if len(fields) < 37:
        return None
    return {
        "comm": comm,
        "utime": int(fields[11]),
        "stime": int(fields[12]),
        "processor": int(fields[36]),
    }


def scan_io_wq(pid):
    """Kernel io-wq workers for this ring are named iou-*-<tgid>."""
    wanted = {f"iou-wrk-{pid}", f"iou-wq-{pid}", f"iou-sqp-{pid}"}
    out = {}
    try:
        entries = os.listdir("/proc")
    except OSError:
        return out
    for entry in entries:
        if not entry.isdigit():
            continue
        comm = read_text(f"/proc/{entry}/comm")
        if comm in wanted:
            st = proc_task_stat(int(entry), int(entry))
            if st is not None:
                out[int(entry)] = st
    return out


class Sampler(threading.Thread):
    def __init__(self, pid, period_s=0.05, iowq_every=10):
        super().__init__(daemon=True)
        self.pid = pid
        self.period = period_s
        self.iowq_every = iowq_every
        self.stop_ev = threading.Event()
        self.samples = []
        self.max_threads = 0
        self.threads_ok = 0

    def run(self):
        n = 0
        while not self.stop_ev.is_set():
            self.sample_once(n)
            n += 1
            self.stop_ev.wait(self.period)

    def sample_once(self, n):
        pid = self.pid
        try:
            tids = os.listdir(f"/proc/{pid}/task")
        except OSError:
            return
        threads = {}
        for tid in tids:
            st = proc_task_stat(pid, int(tid))
            if st is not None:
                threads[int(tid)] = st
        if not threads:
            return
        threads_ok = read_text(f"/proc/{pid}/status")
        nthreads = None
        if threads_ok:
            m = re.search(r"^Threads:\s*(\d+)", threads_ok, re.M)
            if m:
                nthreads = int(m.group(1))
                self.max_threads = max(self.max_threads, nthreads)
        iowq = scan_io_wq(pid) if n % self.iowq_every == 0 else None
        self.samples.append({
            "t": time.monotonic(),
            "threads": threads,
            "nthreads": nthreads,
            "iowq": iowq,
        })
        self.threads_ok += 1


def summarize_sampler(sampler, ru_total_s, pid=None):
    samples = sampler.samples
    if not samples:
        return {"available": False}
    seen = {}
    for s in samples:
        for tid, st in s["threads"].items():
            rec = seen.setdefault(tid, {"first": st, "last": st, "n": 0})
            rec["last"] = st
            rec["n"] += 1
    per_tid = {}
    for tid, rec in seen.items():
        a, b = rec["first"], rec["last"]
        per_tid[tid] = {
            "comm": b["comm"],
            "ticks": (b["utime"] + b["stime"]) - (a["utime"] + a["stime"]),
            "utime_ticks": b["utime"] - a["utime"],
            "stime_ticks": b["stime"] - a["stime"],
            "processor_last": b["processor"],
            "samples": rec["n"],
        }
    if pid is not None and pid in per_tid:
        main_tid = pid
    else:
        main_tid = min(per_tid) if per_tid else None
    app_ticks = per_tid.get(main_tid, {}).get("ticks", 0) if main_tid else 0
    driver_ticks = 0
    iowq_task_ticks = 0
    iowq_task = {}
    for t, v in per_tid.items():
        if t == main_tid:
            continue
        if v["comm"].startswith("iou-"):
            iowq_task_ticks += v["ticks"]
            iowq_task[t] = {"comm": v["comm"], "ticks": v["ticks"]}
        else:
            driver_ticks += v["ticks"]
    stime_ticks = sum(v["stime_ticks"] for v in per_tid.values())
    iowq = {}
    for s in samples:
        if s["iowq"] is None:
            continue
        for wpid, st in s["iowq"].items():
            rec = iowq.setdefault(wpid, {"comm": st["comm"], "first": st, "last": st})
            rec["last"] = st
    iowq_out = {}
    for wpid, rec in iowq.items():
        iowq_out[wpid] = {
            "comm": rec["comm"],
            "ticks_lower_bound": (rec["last"]["utime"] + rec["last"]["stime"])
            - (rec["first"]["utime"] + rec["first"]["stime"]),
        }
    iowq_ticks = iowq_task_ticks + sum(v["ticks_lower_bound"] for v in iowq_out.values())
    ru_ticks_est = ru_total_s * TICK
    residual = ru_ticks_est - (app_ticks + driver_ticks + iowq_ticks)
    return {
        "available": True,
        "samples": len(samples),
        "max_user_threads": sampler.max_threads,
        "app_ticks": app_ticks,
        "driver_ticks": driver_ticks,
        "in_thread_kernel_stime_ticks": stime_ticks,
        "threads": per_tid,
        "io_wq_in_process": iowq_task,
        "io_wq": iowq_out,
        "io_wq_ticks": iowq_ticks,
        "io_wq_ticks_lower_bound": iowq_ticks,
        "ru_total_ticks_est": round(ru_ticks_est, 3),
        "residual_ticks_est": round(residual, 3),
        "residual_bound_ticks": round(1.0 + len(per_tid) * 0.5, 3),
        "tick_hz": TICK,
    }


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


def arm_readback(log_path):
    text = read_text(log_path) or ""
    m = re.search(r"research arm=(\S+) \(SLUICE_595_R3_ARM\), research cases=(\S+)", text)
    if not m:
        return None
    return {"arm": m.group(1), "research_cases": m.group(2)}


def expected_readback(arm_env):
    return arm_env


def lock_holder(lock_path):
    try:
        st = os.stat(lock_path)
    except OSError:
        return None
    ino = st.st_ino
    want_major = os.major(st.st_dev)
    want_minor = os.minor(st.st_dev)
    out = []
    try:
        with open("/proc/locks") as f:
            for line in f:
                parts = line.split()
                if len(parts) < 8:
                    continue
                dev_ino = parts[5]
                if dev_ino.count(":") != 2:
                    continue
                major_s, minor_s, inode_s = dev_ino.split(":")
                try:
                    major, minor, inode = int(major_s, 16), int(minor_s, 16), int(inode_s, 16)
                except ValueError:
                    continue
                if inode != ino or major != want_major or minor != want_minor:
                    continue
                out.append({
                    "type": parts[1],
                    "mode": parts[2],
                    "pid": int(parts[4]),
                    "range": [parts[6], parts[7]],
                })
    except OSError:
        return None
    return {"lock_file": os.path.abspath(lock_path), "inode": ino,
            "dev_major": want_major, "dev_minor": want_minor, "holders": out}


def snapshot(binary, expected_sha, case, arm_env, cpu, cpus, with_busy=True):
    snap = {
        "ts_utc": iso_now(),
        "loadavg": list(os.getloadavg()),
        "cpufreq": cpufreq_state(cpus),
        "tmp": tmp_df(),
        "binary_sha256": sha256_file(binary),
        "binary_sha_ok": (expected_sha is None or sha256_file(binary) == expected_sha),
        "case": case,
        "arm_env": arm_env,
        "cpu_set": cpu,
    }
    if with_busy:
        snap["cpu_busy"] = cpu_busy_fraction(cpus)
    return snap


def run_attempt(args, case, member, arm_env, pair, pos, order_label, attempt, binary_sha):
    raw_dir = args.phase_dir
    os.makedirs(os.path.join(raw_dir, "logs"), exist_ok=True)
    os.makedirs(os.path.join(raw_dir, "jsonl"), exist_ok=True)
    tag = f"p{pair:03d}-{pos}-{member}-a{attempt}"
    jpath = os.path.join(raw_dir, "jsonl", f"{tag}.jsonl")
    opath = os.path.join(raw_dir, "logs", f"{tag}.log")
    if os.path.exists(jpath):
        os.unlink(jpath)

    env = {k: v for k, v in os.environ.items() if not k.startswith("SLUICE_")}
    env.update({
        "SLUICE_BENCH_WARMUP": str(args.warmup),
        "SLUICE_BENCH_MEASURED": str(args.measured),
        "SLUICE_BENCH_CASE_FILTER": "=" + case,
        "SLUICE_BENCH_STATS": "1" if args.stats else "0",
        "SLUICE_BENCH_JSONL": jpath,
        "SLUICE_BENCH_TMP_DIR": args.tmp_dir,
    })
    for kv in args.env:
        k, v = kv.split("=", 1)
        env[k] = v
    if args.arm_var:
        env[args.arm_var] = arm_env

    before = snapshot(args.binary, args.expect_sha, case, arm_env, args.cpu, args.cpus)
    irq_before = read_interrupts()
    cmd = [TASKSET, "-c", args.cpu, args.binary]
    fa = [
        (os.POSIX_SPAWN_OPEN, 1, opath, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644),
        (os.POSIX_SPAWN_DUP2, 1, 2),
    ]
    t0 = time.monotonic()
    pid = os.posix_spawn(TASKSET, cmd, env, file_actions=fa)
    sampler = Sampler(pid)
    sampler.start()
    _, status, ru = os.wait4(pid, 0)
    wall = time.monotonic() - t0
    sampler.stop_ev.set()
    sampler.join(timeout=2.0)
    irq_after = read_interrupts()
    exit_code = os.waitstatus_to_exitcode(status)
    after = snapshot(args.binary, args.expect_sha, case, arm_env, args.cpu, args.cpus)

    measured, warmup_n = parse_jsonl(jpath, case)
    readback = arm_readback(opath)
    rec = {
        "phase": args.phase,
        "case": case,
        "pair": pair,
        "pos": pos,
        "member": member,
        "order": order_label,
        "attempt": attempt,
        "ts_utc": before["ts_utc"],
        "exit": exit_code,
        "wall_s": wall,
        "ru_utime": ru.ru_utime,
        "ru_stime": ru.ru_stime,
        "ru_total": ru.ru_utime + ru.ru_stime,
        "ru_maxrss_kb": ru.ru_maxrss,
        "before": before,
        "after": after,
        "irq_delta": irq_delta(irq_before, irq_after),
        "binary": os.path.abspath(args.binary),
        "binary_sha256": binary_sha,
        "env": {k: env[k] for k in sorted(env) if k.startswith("SLUICE_")},
        "jsonl": jpath,
        "log": opath,
        "warmup_records": warmup_n,
        "measured_records": len(measured),
        "readback": readback,
        "buckets": summarize_sampler(sampler, ru.ru_utime + ru.ru_stime, pid),
    }
    invalid = []
    if exit_code != 0:
        invalid.append(f"exit={exit_code}")
    if len(measured) != args.measured:
        invalid.append(f"measured_records={len(measured)}")
    if warmup_n != args.warmup:
        invalid.append(f"warmup_records={warmup_n}")
    if not before["binary_sha_ok"]:
        invalid.append("binary_sha_mismatch")
    for r in measured:
        if not r.get("ok"):
            invalid.append("bench_failure:" + str(r.get("failure")))
        if int(r.get("ops", -1)) != args.expected_ops:
            invalid.append(f"ops={r.get('ops')}")
    if readback is not None and readback.get("arm") != expected_readback(arm_env):
        invalid.append(f"arm_readback={readback.get('arm')}")

    if measured:
        ops_tot = sum(r["ops"] for r in measured)
        cpu_tot = sum(r["cpu_s"] for r in measured)
        wall_tot = sum(r["wall_s"] for r in measured)
        n = len(measured)
        mean_of = lambda key: sum(r[key] for r in measured) / n
        rec["reps"] = measured
        rec["metrics"] = {
            "cpu_s": cpu_tot,
            "ops": ops_tot,
            "cpu_ns_per_op": 1e9 * cpu_tot / ops_tot if ops_tot else None,
            "ops_per_sec": ops_tot / wall_tot if wall_tot else None,
            "p50_ns": mean_of("lat_p50_ns"),
            "p95_ns": mean_of("lat_p95_ns"),
            "p99_ns": mean_of("lat_p99_ns"),
            "wall_s": wall_tot,
            "submits": sum(r["submits"] for r in measured),
            "stats_submit_calls": sum(r["stats_submit_calls"] for r in measured),
            "stats_poll_calls": sum(r["stats_poll_calls"] for r in measured),
            "stats_wait_calls": sum(r["stats_wait_calls"] for r in measured),
            "stats_completed_ops": sum(r["stats_completed_ops"] for r in measured),
        }
        rec["metrics"]["cpu_util"] = (cpu_tot / wall_tot) if wall_tot else None
    rec["valid"] = not invalid
    rec["invalid_reasons"] = invalid

    with open(os.path.join(raw_dir, "results.jsonl"), "a") as f:
        f.write(json.dumps(rec) + "\n")
    if invalid:
        with open(os.path.join(raw_dir, "attempts.jsonl"), "a") as f:
            f.write(json.dumps({
                "pair": pair, "pos": pos, "member": member, "attempt": attempt,
                "tag": tag, "reasons": invalid,
                "exit": exit_code, "jsonl": jpath, "log": opath,
            }) + "\n")
    m = rec.get("metrics", {})
    print(f"[{args.phase}] pair={pair} pos={pos} {member}={arm_env!r} "
          f"attempt={attempt} exit={exit_code} valid={rec['valid']} "
          f"cpu/op={m.get('cpu_ns_per_op')} p99={m.get('p99_ns')} "
          f"tput={m.get('ops_per_sec')}", flush=True)
    return rec


def run_slot(args, case, member, arm_env, pair, pos, order_label, binary_sha):
    for attempt in range(1, args.max_attempts + 1):
        rec = run_attempt(args, case, member, arm_env, pair, pos, order_label,
                          attempt, binary_sha)
        if rec["valid"]:
            return rec
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--phase", required=True)
    ap.add_argument("--case", required=True)
    ap.add_argument("--pairs", type=int, required=True)
    ap.add_argument("--binary", required=True)
    ap.add_argument("--expect-sha", default=None)
    ap.add_argument("--cpu", default="2")
    ap.add_argument("--cpus", default="2,12")
    ap.add_argument("--arms", default="AA", choices=["AA", "AB"])
    ap.add_argument("--arm-var", default="")
    ap.add_argument("--arm-a", default="")
    ap.add_argument("--arm-b", default="")
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--warmup", type=int, default=1)
    ap.add_argument("--measured", type=int, default=5)
    ap.add_argument("--stats", type=int, default=0)
    ap.add_argument("--expected-ops", type=int, default=20000)
    ap.add_argument("--max-attempts", type=int, default=3)
    ap.add_argument("--tmp-dir", default="/tmp/sluice-595-r3c")
    ap.add_argument("--phase-dir", required=True)
    ap.add_argument("--lock-file", default="/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock")
    args = ap.parse_args()

    if not os.access(TASKSET, os.X_OK):
        sys.exit(f"taskset not executable: {TASKSET}")
    if not os.access(args.binary, os.X_OK):
        sys.exit(f"bench binary not executable: {args.binary}")
    os.makedirs(args.phase_dir, exist_ok=True)

    binary_sha = sha256_file(args.binary)
    cpus = args.cpus.split(",")
    header = {
        "phase": args.phase,
        "case": args.case,
        "pairs": args.pairs,
        "arms": args.arms,
        "arm_var": args.arm_var,
        "arm_a": args.arm_a,
        "arm_b": args.arm_b,
        "cpu_set": args.cpu,
        "cpus_monitored": cpus,
        "binary": os.path.abspath(args.binary),
        "binary_sha256": binary_sha,
        "expect_sha": args.expect_sha,
        "env_overrides": args.env,
        "warmup": args.warmup,
        "measured": args.measured,
        "stats": args.stats,
        "expected_ops": args.expected_ops,
        "tmp_dir": args.tmp_dir,
        "order_rule": ("AA: pair even m1,m2 / odd m2,m1" if args.arms == "AA"
                       else "AB: pair even A,B / odd B,A"),
        "lock": lock_holder(args.lock_file),
        "started_utc": iso_now(),
        "loadavg_start": list(os.getloadavg()),
        "tmp_start": tmp_df(),
        "cpufreq_start": cpufreq_state(cpus),
        "binary_sha_ok_start": (args.expect_sha is None or binary_sha == args.expect_sha),
        "host_kernel": os.uname().release,
        "runner_argv": sys.argv,
        "runner_sha256": sha256_file(os.path.abspath(__file__)),
    }
    with open(os.path.join(args.phase_dir, "header.json"), "w") as f:
        json.dump(header, f, indent=1)

    results_path = os.path.join(args.phase_dir, "results.jsonl")
    if os.path.exists(results_path):
        os.unlink(results_path)
    attempts_path = os.path.join(args.phase_dir, "attempts.jsonl")
    if os.path.exists(attempts_path):
        os.unlink(attempts_path)

    fixture_path = None
    fixture_sha_start = None
    fixture_sha_end = None
    valid_pairs = 0
    for pair in range(args.pairs):
        if args.arms == "AA":
            order = [("m1", args.arm_a), ("m2", args.arm_a)] if pair % 2 == 0 \
                else [("m2", args.arm_a), ("m1", args.arm_a)]
        else:
            order = [("A", args.arm_a), ("B", args.arm_b)] if pair % 2 == 0 \
                else [("B", args.arm_b), ("A", args.arm_a)]
        label = "".join(m for m, _ in order)
        ok = True
        for pos, (member, arm_env) in enumerate(order):
            rec = run_slot(args, args.case, member, arm_env, pair, pos, label, binary_sha)
            if rec is None:
                ok = False
        if ok:
            valid_pairs += 1
        if fixture_path is None:
            candidate = os.path.join(args.tmp_dir,
                                     "external.bin" if "W03" in args.case else "pipeline.bin")
            if os.path.exists(candidate):
                fixture_path = candidate
                fixture_sha_start = sha256_file(candidate)

    if fixture_path is not None and os.path.exists(fixture_path):
        fixture_sha_end = sha256_file(fixture_path)
    end = {
        "phase": args.phase,
        "ended_utc": iso_now(),
        "valid_pairs": valid_pairs,
        "expected_pairs": args.pairs,
        "loadavg_end": list(os.getloadavg()),
        "tmp_end": tmp_df(),
        "cpufreq_end": cpufreq_state(cpus),
        "binary_sha256_end": sha256_file(args.binary),
        "fixture_path": fixture_path,
        "fixture_sha256_start": fixture_sha_start,
        "fixture_sha256_end": fixture_sha_end,
        "fixture_unchanged": (fixture_sha_start is not None
                              and fixture_sha_start == fixture_sha_end),
        "lock_end": lock_holder(args.lock_file),
    }
    with open(os.path.join(args.phase_dir, "block_end.json"), "w") as f:
        json.dump(end, f, indent=1)
    print(f"[{args.phase}] done: valid pairs {valid_pairs}/{args.pairs}", flush=True)
    return 0 if valid_pairs == args.pairs else 3


if __name__ == "__main__":
    sys.exit(main())
