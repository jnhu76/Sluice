#!/usr/bin/env python3
"""E5 candidate #1 (M-28 syscall_count_ gating) timing harness.

Frozen protocol: docs/evidence/e5-fullpath-machine-work/experiment-protocol.md (r2).
- No CPU pinning (protocol §2 CPU placement row).
- warmup=2 measured=7, exact-match case filter, 540000/54000 ops denominators.
- P2 pass = sudo perf stat -e cycles,instructions,task-clock,context-switches -x,
- P1 pass = perf stat -e instructions:u,cycles:u -x, (no sudo)
- warm gate: /proc/diskstats sda dreads==0 and dsectors==0 across the window.
- driver only; lives outside the repository.
"""
import json
import os
import subprocess
import sys
import time

SUDO_PASS = "121212"
BIN_A = "/home/jnhu/sluice-e5-elim-base/build-bench-ring/linux/x86_64/release/bench_w02_pipeline"
BIN_B = "/home/jnhu/sluice-e5-elim/build-bench-ring/linux/x86_64/release/bench_w02_pipeline"
SESSION = "/home/jnhu/sluice-e5-elim/results/e5m28-s1-20261118"
SCRATCH = "/home/jnhu/e5-elim-scratch-e5m28-s1"

CELLS = {
    "main": dict(case="W02-threadpool-4k-d16-w8", ops=540000),
    "uring": dict(case="W02-uring-4k-d16", ops=540000),
    "64k": dict(case="W02-threadpool-64k-d16-w4", ops=54000),
}
PASSES = {
    "P2": dict(events="cycles,instructions,task-clock,context-switches", sudo=True),
    "P1": dict(events="instructions:u,cycles:u", sudo=False),
}


def diskstats_sda():
    for line in open("/proc/diskstats"):
        f = line.split()
        if f[2] == "sda":
            return int(f[3]), int(f[5])
    raise RuntimeError("sda missing in /proc/diskstats")


def placement():
    out = {}
    for line in open("/proc/self/status"):
        if line.startswith("Cpus_allowed_list"):
            out["cpus_allowed_list"] = line.split(":", 1)[1].strip()
    return out


def loadavg():
    return os.getloadavg()


def perf_stat(events, sudo, logfile, cmd, env):
    argv = ["perf", "stat", "-e", events, "-x,", "-o", logfile, "--"] + cmd
    if sudo:
        # sudo env_reset strips the environment; re-inject the bench env explicitly.
        envpfx = ["sudo", "-S", "-p", "", "env"]
        envpfx += [f"{k}={v}" for k, v in env.items()
                   if k.startswith("SLUICE_BENCH_")]
        argv = envpfx + argv
    p = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.PIPE, env=env)
    out, err = p.communicate(input=(SUDO_PASS + "\n").encode() if sudo else b"")
    return p.returncode, out.decode(errors="replace"), err.decode(errors="replace")


def parse_perf_csv(path):
    rows = {}
    with open(path) as f:
        for line in f:
            parts = line.rstrip("\n").split(",")
            if len(parts) < 3:
                continue
            try:
                val = float(parts[0])
            except ValueError:
                continue
            ev = parts[2]
            rows[ev] = val
    return rows


def parse_jsonl(path, case_id):
    measured = []
    for line in open(path):
        line = line.strip()
        if not line:
            continue
        r = json.loads(line)
        if r.get("case_id") == case_id and r.get("measured"):
            measured.append(r)
    return measured


def median(xs):
    xs = sorted(xs)
    n = len(xs)
    if n == 0:
        return None
    return xs[n // 2] if n % 2 else (xs[n // 2 - 1] + xs[n // 2]) / 2.0


def invocation(cell, pass_type, arm, tag):
    c = CELLS[cell]
    p = PASSES[pass_type]
    binary = BIN_A if arm == "A" else BIN_B
    logdir = os.path.join(SESSION, "logs")
    jsonldir = os.path.join(SESSION, "jsonl")
    perfd = os.path.join(SESSION, "perf")
    for d in (logdir, jsonldir, perfd):
        os.makedirs(d, exist_ok=True)
    jpath = os.path.join(jsonldir, f"{tag}.jsonl")
    ppath = os.path.join(perfd, f"{tag}.csv")
    benchlog = os.path.join(logdir, f"{tag}.bench.log")
    if os.path.exists(jpath):
        os.unlink(jpath)

    ds0 = diskstats_sda()
    la0 = loadavg()
    env = os.environ.copy()
    env.update({
        "SLUICE_BENCH_WARMUP": "2",
        "SLUICE_BENCH_MEASURED": "7",
        "SLUICE_BENCH_CASE_FILTER": "=" + c["case"],
        "SLUICE_BENCH_JSONL": jpath,
        "SLUICE_BENCH_TMP_DIR": SCRATCH,
    })
    t0 = time.monotonic()
    rc, out, err = perf_stat(p["events"], p["sudo"], ppath, [binary], env)
    wall = time.monotonic() - t0
    if p["sudo"]:
        # root-created fixture would block the next unprivileged P1 invocation.
        subprocess.run(["sudo", "-S", "-p", "", "chown", "-R", "jnhu:jnhu", SCRATCH],
                       input=(SUDO_PASS + "\n").encode(), check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ds1 = diskstats_sda()
    la1 = loadavg()

    with open(benchlog, "w") as f:
        f.write(out)
        f.write("\n--- stderr ---\n")
        f.write(err)

    perf = parse_perf_csv(ppath)
    rows = parse_jsonl(jpath, c["case"])
    ok_bench = rc == 0 and "failing repetitions=0" in out
    dreads = ds1[0] - ds0[0]
    dsect = ds1[1] - ds0[1]
    warm_ok = dreads == 0 and dsect == 0

    obs = {
        "tag": tag, "cell": cell, "pass": pass_type, "arm": arm,
        "bench_exit": rc, "bench_ok": ok_bench,
        "wall_s": round(wall, 4),
        "dreads": dreads, "dsectors": dsect, "warm_ok": warm_ok,
        "loadavg_before": [round(x, 3) for x in la0],
        "loadavg_after": [round(x, 3) for x in la1],
        "cpus_allowed_list": placement()["cpus_allowed_list"],
        "governor_cpu0": open("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor").read().strip(),
        "ops_denominator": c["ops"],
    }
    for ev, val in perf.items():
        obs["perf_" + ev] = val
    if rows:
        cyc = perf.get("cycles")
        if cyc is not None:
            obs["cyc_per_op"] = cyc / c["ops"]
        instr = perf.get("instructions") or perf.get("instructions:u")
        if instr is not None:
            obs["instr_per_op"] = instr / c["ops"]
        if perf.get("cycles:u") is not None:
            obs["cyc_u_per_op"] = perf["cycles:u"] / c["ops"]
        obs["ops_s_median"] = median([r["ops_per_sec"] for r in rows])
        obs["cpu_op_us_median"] = median([r["cpu_s"] / r["ops"] * 1e6 for r in rows])
        obs["p99_ns_median"] = median([r["lat_p99_ns"] for r in rows])
        obs["n_measured_reps"] = len(rows)
    with open(os.path.join(SESSION, "observations.jsonl"), "a") as f:
        f.write(json.dumps(obs) + "\n")
    return obs


def pair(cell, i, arms, passes_per_arm, prefix=None):
    """arms: 2-char string like 'AB' or 'BA' (order) or 'AA'. passes: list like ['P1','P2']."""
    tags = []
    prefix = prefix or cell
    for pos, arm in enumerate(arms, 1):
        for k, p in enumerate(passes_per_arm, 1):
            tag = f"{prefix}-p{i:03d}-{pos}{k}-{arm}"
            obs = invocation(cell, p, arm, tag)
            flags = []
            if not obs["bench_ok"]:
                flags.append("BENCH_NOT_OK")
            if not obs["warm_ok"]:
                flags.append("NOTWARM")
            if flags:
                print(f"  FLAG {tag}: {flags} obs={obs}", flush=True)
                sys.exit(3)
            print(f"  {tag} cyc/op={obs.get('cyc_per_op')} instr/op={obs.get('instr_per_op')} "
                  f"cpu/op_us={obs.get('cpu_op_us_median')} wall={obs['wall_s']}", flush=True)
            tags.append(tag)
    return tags


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "inv":
        cell, p, arm, tag = sys.argv[2:6]
        obs = invocation(cell, p, arm, tag)
        print(json.dumps(obs, indent=1))
    elif cmd == "pair":
        cell, i, arms, passes = sys.argv[2:6]
        prefix = sys.argv[6] if len(sys.argv) > 6 else None
        passes_per_arm = passes.split(",")
        pair(cell, int(i), arms, passes_per_arm, prefix)
    else:
        raise SystemExit("usage: e5harness.py inv|pair ...")
