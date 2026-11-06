#!/usr/bin/env python3
"""#595 R5-repair three-arm measurement machinery (A0 / R_original / R_cleanup).

Reuses the frozen R4 pair-harness components verbatim (snapshot, Sampler,
io-wq scan, jsonl parse, arm readback) and adds:

- one invocation runner whose per-invocation metrics are computed exactly as
  R4's (cpu_ns_per_op = 1e9*sum(cpu_s)/sum(ops); p50/p95/p99 = mean over the
  5 measured reps of each rep's percentile; run-level p99 is this definition,
  never pooled per-request p99);
- the frozen three-arm block schedule (6 permutations x 4, seeded shuffle);
- the A/A and A0original/A0cleanup calibration schedules;
- a dual-flock measurement session (E5 top-level MEASUREMENT.lock first, then
  the R4-era r3/MEASUREMENT.lock, fixed order) with LOCK_RECEIPT entries
  following the R5 convention (UTC, fuser -v, second-fd exclusivity probe,
  pre/post environment state).
"""
import hashlib
import itertools
import json
import os
import random
import subprocess
import sys
import time

sys.path.insert(0, "/home/jnhu/sluice-595-e5/r4/harness")
import measure_pairs as mp  # noqa: E402  (frozen R4 harness components)

ROOT = "/home/jnhu/sluice-595-e5/r5-repair-measure"
RAW = os.path.join(ROOT, "raw")
LOGS = os.path.join(ROOT, "logs")
LOCK_TOP = "/home/jnhu/sluice-595-e5/MEASUREMENT.lock"
LOCK_R3 = "/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock"
LOCK_RECEIPT = os.path.join(LOGS, "LOCK_RECEIPT.md")
TMPDIR_BENCH = "/tmp/sluice-595-r5rm"

WARMUP_REPS = 1
MEASURED_REPS = 5
BLOCKS = 24
MON_CPUS = ("2", "12")
BENCH_CPU = "2"
ARM_VAR = "SLUICE_595_R3_ARM"

BINS = {
    "w03_original": (os.path.join(ROOT, "worktree-original/build/linux/x86_64/release/bench_w03_external_loop"),
                     "7bf83eb93fa589628649931a1a5cdba6a9e9375083e8ced38e09a73c455b9da5"),
    "w02_original": (os.path.join(ROOT, "worktree-original/build/linux/x86_64/release/bench_w02_pipeline"),
                     "b07fd060629e5baa25e20aaab14ec6212ccf8e6e7160b41f9f06300d8534047c"),
    "w03_cleanup": (os.path.join(ROOT, "worktree-cleanup/build/linux/x86_64/release/bench_w03_external_loop"),
                    "9442f02b24cec81a7040eea9a6e79e221a5fc9a7ac264b6db9c1fdae28cb6c6b"),
    "w02_cleanup": (os.path.join(ROOT, "worktree-cleanup/build/linux/x86_64/release/bench_w02_pipeline"),
                    "971a28da2fcfb3ab6099453aabd56a5a6f3524fdc956ec967245f72753a27131"),
}

# arm label -> (binary key, SLUICE_595_R3_ARM env token)
ARMS = {
    "A0": ("w03_original", "A0"),   # w02 variant chosen per cell kind below
    "R": ("w03_original", "R"),
    "C": ("w03_cleanup", "R"),
}


def cells():
    return [
        {"name": "MAIN-W03-D16", "kind": "w03", "case": "W03-uring-4k-d16", "env": {},
         "ops": 20000, "aa_pairs": 24, "calib_pairs": 24},
        {"name": "PROT-W03-D4", "kind": "w03", "case": "W03-uring-4k-d4", "env": {},
         "ops": 20000, "aa_pairs": 24, "calib_pairs": 8},
        {"name": "PROT-W03-D2SERIAL", "kind": "w03", "case": "W03-uring-4k-d2-r3d2serial",
         "env": {"SLUICE_595_R3_CASES": "1"}, "ops": 20000, "aa_pairs": 24, "calib_pairs": 8},
        {"name": "PROT-W02-D1", "kind": "w02", "case": "W02-uring-4k-d1", "env": {},
         "ops": 20000, "aa_pairs": 24, "calib_pairs": 8},
        {"name": "PROT-W02-D2", "kind": "w02", "case": "W02-uring-4k-d2",
         "env": {"SLUICE_BENCH_595_DEPTHS": "1"}, "ops": 20000, "aa_pairs": 24, "calib_pairs": 8},
    ]


def arm_bin(arm, kind):
    if kind == "w02":
        return ("w02_original", "A0") if arm == "A0" else \
               (("w02_original", "R") if arm == "R" else ("w02_cleanup", "R"))
    return ARMS[arm]


def block_orders():
    """Frozen: 24 blocks, each of the 6 permutations of (A0,R,C) used 4 times.

    perms shuffled with seed 5954 (same generator as R4's quad rule), then the
    24-slot sequence = perms*4 shuffled with seed 5955.
    """
    perms = [list(p) for p in itertools.permutations(["A0", "R", "C"])]
    random.Random(5954).shuffle(perms)
    seq = perms * 4
    random.Random(5955).shuffle(seq)
    return seq


def schedule_sha():
    h = hashlib.sha256()
    h.update(json.dumps(cells(), sort_keys=True).encode())
    h.update(json.dumps(block_orders()).encode())
    h.update(json.dumps({k: v[1] for k, v in BINS.items()}, sort_keys=True).encode())
    h.update(json.dumps({k: v for k, v in ARMS.items()}, sort_keys=True).encode())
    h.update(f"aa={BLOCKS} calib w03d16=24 else=8 warmup={WARMUP_REPS} measured={MEASURED_REPS}".encode())
    return h.hexdigest()


def run_invocation(cell, arm, block, position, seg, attempt, binary_key, arm_env):
    """One bench process; returns the R4-shaped record dict."""
    out_dir = os.path.join(RAW, cell["name"].lower(), seg)
    os.makedirs(os.path.join(out_dir, "logs"), exist_ok=True)
    os.makedirs(os.path.join(out_dir, "jsonl"), exist_ok=True)
    tag = f"{seg}-b{block:02d}-p{position}-{arm}-a{attempt}"
    jpath = os.path.join(out_dir, "jsonl", f"{tag}.jsonl")
    opath = os.path.join(out_dir, "logs", f"{tag}.log")
    if os.path.exists(jpath):
        os.unlink(jpath)

    binary, sha = BINS[binary_key]
    env = {k: v for k, v in os.environ.items() if not k.startswith("SLUICE_")}
    env.update({
        "SLUICE_BENCH_WARMUP": str(WARMUP_REPS),
        "SLUICE_BENCH_MEASURED": str(MEASURED_REPS),
        "SLUICE_BENCH_CASE_FILTER": "=" + cell["case"],
        "SLUICE_BENCH_STATS": "0",
        "SLUICE_BENCH_JSONL": jpath,
        "SLUICE_BENCH_TMP_DIR": TMPDIR_BENCH,
    })
    env.update(cell["env"])
    env[ARM_VAR] = arm_env

    before = mp.snapshot(binary, sha, cell["case"], arm_env, BENCH_CPU, MON_CPUS)
    irq_before = mp.read_interrupts()
    cmd = [mp.TASKSET, "-c", BENCH_CPU, binary]
    fa = [
        (os.POSIX_SPAWN_OPEN, 1, opath, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644),
        (os.POSIX_SPAWN_DUP2, 1, 2),
    ]
    t0 = time.monotonic()
    pid = os.posix_spawn(mp.TASKSET, cmd, env, file_actions=fa)
    sampler = mp.Sampler(pid)
    sampler.start()
    _, status, ru = os.wait4(pid, 0)
    wall = time.monotonic() - t0
    sampler.stop_ev.set()
    sampler.join(timeout=2.0)
    irq_after = mp.read_interrupts()
    exit_code = os.waitstatus_to_exitcode(status)
    after = mp.snapshot(binary, sha, cell["case"], arm_env, BENCH_CPU, MON_CPUS)

    measured, warmup_n = mp.parse_jsonl(jpath, cell["case"])
    readback = mp.arm_readback(opath)

    rec = {
        "cell": cell["name"], "case": cell["case"], "seg": seg,
        "block": block, "position": position, "arm": arm,
        "binary": binary_key, "binary_sha256": sha,
        "attempt": attempt, "ts_utc": before["ts_utc"],
        "exit": exit_code, "wall_s": wall,
        "ru_utime": ru.ru_utime, "ru_stime": ru.ru_stime,
        "ru_maxrss_kb": ru.ru_maxrss,
        "env": {k: env[k] for k in sorted(env) if k.startswith("SLUICE_")},
        "jsonl": jpath, "log": opath,
        "warmup_records": warmup_n, "measured_records": len(measured),
        "readback": readback,
        "buckets": mp.summarize_sampler(sampler, ru.ru_utime + ru.ru_stime, pid),
        "irq_delta": mp.irq_delta(irq_before, irq_after),
        "snapshot_pre": before, "snapshot_post": after,
    }
    invalid = []
    if exit_code != 0:
        invalid.append(f"exit={exit_code}")
    if len(measured) != MEASURED_REPS:
        invalid.append(f"measured_records={len(measured)}")
    if warmup_n != WARMUP_REPS:
        invalid.append(f"warmup_records={warmup_n}")
    if not before["binary_sha_ok"] or not after["binary_sha_ok"]:
        invalid.append("binary_sha_mismatch")
    for r in measured:
        if not r.get("ok"):
            invalid.append("bench_failure:" + str(r.get("failure")))
        if int(r.get("ops", -1)) != cell["ops"]:
            invalid.append(f"ops={r.get('ops')}")
    if readback is not None and readback.get("arm") != arm_env:
        invalid.append(f"arm_readback={readback.get('arm')}")

    if measured:
        ops_tot = sum(r["ops"] for r in measured)
        cpu_tot = sum(r["cpu_s"] for r in measured)
        wall_tot = sum(r["wall_s"] for r in measured)
        n = len(measured)
        mean_of = lambda key: sum(r[key] for r in measured) / n
        rec["reps"] = measured
        rec["metrics"] = {
            "cpu_s": cpu_tot, "ops": ops_tot,
            "cpu_ns_per_op": 1e9 * cpu_tot / ops_tot if ops_tot else None,
            "ops_per_sec": ops_tot / wall_tot if wall_tot else None,
            "p50_ns": mean_of("lat_p50_ns"),
            "p95_ns": mean_of("lat_p95_ns"),
            "p99_ns": mean_of("lat_p99_ns"),
            "wall_s": wall_tot,
            "stats_max_outstanding": max(int(r.get("stats_max_outstanding", 0)) for r in measured),
        }
        rec["metrics"]["cpu_util"] = (cpu_tot / wall_tot) if wall_tot else None
    rec["valid"] = not invalid
    rec["invalid_reasons"] = invalid

    with open(os.path.join(out_dir, "results.jsonl"), "a") as f:
        f.write(json.dumps(rec) + "\n")
    if invalid:
        with open(os.path.join(out_dir, "attempts.jsonl"), "a") as f:
            f.write(json.dumps({"seg": seg, "block": block, "position": position,
                                "arm": arm, "attempt": attempt, "tag": tag,
                                "reasons": invalid, "exit": exit_code,
                                "jsonl": jpath, "log": opath}) + "\n")
    m = rec.get("metrics", {})
    print(f"[{cell['name']}/{seg}] b{block:02d} p{position} {arm}={arm_env}/{binary_key} "
          f"a{attempt} exit={exit_code} valid={rec['valid']} "
          f"cpu/op={m.get('cpu_ns_per_op')} p99={m.get('p99_ns')} "
          f"tput={m.get('ops_per_sec')}", flush=True)
    return rec


def run_slot(cell, arm, binary_key, arm_env, block, position, seg, max_attempts=3):
    for attempt in range(1, max_attempts + 1):
        rec = run_invocation(cell, arm, block, position, seg, attempt, binary_key, arm_env)
        if rec["valid"]:
            return rec
    return rec


class DualLockSession:
    """flock(LOCK_TOP) then flock(LOCK_R3), fixed order; receipt on entry/exit."""

    def __init__(self, label, cmds):
        self.label = label
        self.cmds = cmds
        self.fd_top = None
        self.fd_r3 = None

    def _receipt(self, text):
        with open(LOCK_RECEIPT, "a") as f:
            f.write(text)

    def __enter__(self):
        os.makedirs(TMPDIR_BENCH, exist_ok=True)
        self.fd_top = os.open(LOCK_TOP, os.O_RDWR | os.O_CREAT, 0o644)
        self.fd_r3 = os.open(LOCK_R3, os.O_RDWR | os.O_CREAT, 0o644)
        t_wait = time.monotonic()
        import fcntl
        fcntl.flock(self.fd_top, fcntl.LOCK_EX)
        fcntl.flock(self.fd_r3, fcntl.LOCK_EX)
        t_wait = time.monotonic() - t_wait
        # second-fd exclusivity probe on both locks (must block => rc!=0)
        probe_top = subprocess.run(["flock", "-n", LOCK_TOP, "-c", "true"], capture_output=True)
        probe_r3 = subprocess.run(["flock", "-n", LOCK_R3, "-c", "true"], capture_output=True)
        fuser_top = subprocess.run(["fuser", "-v", LOCK_TOP], capture_output=True, text=True)
        fuser_r3 = subprocess.run(["fuser", "-v", LOCK_R3], capture_output=True, text=True)
        pre = {
            "utc": mp.iso_now(), "loadavg": mp.read_text("/proc/loadavg"),
            "cpu_busy": mp.cpu_busy_fraction(list(MON_CPUS)),
            "cpufreq": mp.cpufreq_state(list(MON_CPUS)),
            "tmp": mp.tmp_df(),
        }
        self._receipt(
            f"\n## {self.label} ACQUIRE {pre['utc']} (wait {t_wait:.1f}s)\n"
            f"- pid={os.getpid()} cmds: {'; '.join(self.cmds)}\n"
            f"- locks: {LOCK_TOP} fd={self.fd_top}, {LOCK_R3} fd={self.fd_r3} (top acquired first)\n"
            f"- probe rc: top={probe_top.returncode} r3={probe_r3.returncode} (nonzero=blocked OK)\n"
            f"- fuser top: {fuser_top.stdout.strip() or '(none)'}; r3: {fuser_r3.stdout.strip() or '(none)'}\n"
            f"- pre-state: load={pre['loadavg']} busy={pre['cpu_busy']} freq={pre['cpufreq']} tmp={pre['tmp']}\n")
        self.pre = pre
        return self

    def __exit__(self, exc_type, exc, tb):
        import fcntl
        post = {
            "utc": mp.iso_now(), "loadavg": mp.read_text("/proc/loadavg"),
            "cpu_busy": mp.cpu_busy_fraction(list(MON_CPUS)),
            "tmp": mp.tmp_df(),
        }
        fcntl.flock(self.fd_r3, fcntl.LOCK_UN)
        fcntl.flock(self.fd_top, fcntl.LOCK_UN)
        os.close(self.fd_r3)
        os.close(self.fd_top)
        self._receipt(
            f"## {self.label} RELEASE {post['utc']}\n"
            f"- post-state: load={post['loadavg']} busy={post['cpu_busy']} tmp={post['tmp']}\n"
            f"- exception={exc_type.__name__ if exc_type else 'none'}\n")
        return False


def env_ok():
    load = float(mp.read_text("/proc/loadavg").split()[0])
    if load >= 4.0:
        return False, f"load={load}"
    df = mp.tmp_df()
    if df["avail_bytes"] < 1024 ** 3:
        return False, f"tmp_avail={df['avail_bytes']}"
    for cpu, gov in mp.cpufreq_state(list(MON_CPUS)).items():
        if gov["governor"] != "schedutil":
            return False, f"governor={cpu}:{gov['governor']}"
    busy = mp.cpu_busy_fraction(list(MON_CPUS))
    for cpu, b in busy.items():
        if b is not None and b > 0.30:
            return False, f"busy={cpu}:{b}"
    ps = subprocess.run(["ps", "-eo", "pcpu,comm"], capture_output=True, text=True)
    for line in ps.stdout.splitlines()[1:]:
        parts = line.split()
        if len(parts) >= 2 and parts[1] in ("bench_w03_external_loop", "bench_w02_pipeline",
                                            "xmake", "bzip2", "xz", "tar"):
            try:
                if float(parts[0]) > 25.0:
                    return False, f"competing={parts[1]}:{parts[0]}"
            except ValueError:
                pass
    return True, "ok"
