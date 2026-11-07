#!/usr/bin/env python3
"""#595 R3 continuation mechanism diagnostic session (single lock window).

Short diagnostic lane only: no paired timing, no performance statistics.
Reads the sudo password from stdin (first line) and never writes it to any
file, log or result. All steps run while holding the exclusive flock on
/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock.
"""
import argparse
import fcntl
import hashlib
import json
import os
import re
import signal
import subprocess
import sys
import time

TASKSET = "/usr/sbin/taskset"
BPFTRACE = "/usr/sbin/bpftrace"
PERF = "/usr/sbin/perf"
STRACE = "/usr/sbin/strace"
BENCH = ("/home/jnhu/sluice-595-e5/r3/worktree/build/linux/x86_64/release/"
         "bench_w03_external_loop")
BENCH_SHA = "0ebfb7f0c204579fe2cc9e8d7e88cee2de9112e25c0cfb11e8502e59fc4fe2d1"
BUCKET_PROBE = ("/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z/"
                "harness/bucket_probe.py")

CASES = {
    "d16": {"case": "W03-uring-4k-d16", "research": False, "ops": 20000},
    "d2pos": {"case": "W03-uring-4k-d2-r3d2pos", "research": True, "ops": 2},
    "d2serial": {"case": "W03-uring-4k-d2-r3d2serial", "research": True, "ops": 20000},
}
ARMS = ["A0", "A1", "B"]
TMPDIR = "/tmp/sluice-595-r3c-mech"

_pw = None
_log = None


def iso():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def say(msg):
    line = f"[{iso()}] {msg}"
    print(line, flush=True)
    if _log:
        _log.write(line + "\n")
        _log.flush()


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def run(cmd, *, timeout=180, input_bytes=None, stdout=None, stderr=None, env=None,
        check=True):
    out_f = open(stdout, "wb") if isinstance(stdout, str) else subprocess.PIPE
    err_f = open(stderr, "wb") if isinstance(stderr, str) else (
        subprocess.STDOUT if not isinstance(stdout, str) and stdout is None
        else subprocess.PIPE)
    try:
        p = subprocess.run(cmd, input=input_bytes, timeout=timeout, env=env,
                           stdout=out_f if stdout else subprocess.PIPE,
                           stderr=err_f if stderr else subprocess.PIPE,
                           stdin=None if input_bytes is None else subprocess.PIPE)
        rc = p.returncode
        out = None if stdout else p.stdout
        err = None if (stderr or stdout) else p.stderr
    finally:
        if isinstance(stdout, str):
            out_f.close()
        if isinstance(stderr, str):
            err_f.close()
    if check and rc != 0:
        tail = b"" if out is None else out[-400:]
        say(f"COMMAND_FAILED rc={rc} cmd={' '.join(cmd[:6])}... tail={tail!r}")
    return rc, out, err


def sudo(args, *, timeout=180, stdout=None, stderr=None, check=False):
    """Run sudo non-interactively; on cache miss revalidate via stdin password."""
    global _pw
    cmd = ["sudo", "-n"] + args
    rc, out, err = run(cmd, timeout=timeout, stdout=stdout, stderr=stderr, check=False)
    if rc != 0 and _pw is not None:
        v = subprocess.run(["sudo", "-S", "-p", "", "-v"], input=(_pw + "\n").encode(),
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=30)
        if v.returncode == 0:
            rc, out, err = run(cmd, timeout=timeout, stdout=stdout, stderr=stderr, check=False)
    if check and rc != 0:
        say(f"SUDO_COMMAND_FAILED rc={rc} cmd={' '.join(args[:6])}")
    return rc, out, err


def bench_env(case_key, arm, jsonl):
    env = {k: v for k, v in os.environ.items() if not k.startswith("SLUICE_")}
    c = CASES[case_key]
    env.update({
        "SLUICE_BENCH_WARMUP": "0",
        "SLUICE_BENCH_MEASURED": "1",
        "SLUICE_BENCH_CASE_FILTER": "=" + c["case"],
        "SLUICE_BENCH_STATS": "1",
        "SLUICE_BENCH_JSONL": jsonl,
        "SLUICE_BENCH_TMP_DIR": TMPDIR,
        "SLUICE_595_R3_ARM": arm,
    })
    if c["research"]:
        env["SLUICE_595_R3_CASES"] = "1"
    return env


def spawn_bench(case_key, arm, jsonl, log_path):
    env = bench_env(case_key, arm, jsonl)
    lf = open(log_path, "wb")
    t0 = time.monotonic()
    pid = os.posix_spawn(
        TASKSET, [TASKSET, "-c", "2", BENCH], env,
        file_actions=[(os.POSIX_SPAWN_OPEN, 1, log_path,
                       os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644),
                      (os.POSIX_SPAWN_DUP2, 1, 2)])
    return pid, t0, lf


def wait_bench(pid, t0, lf, timeout=180):
    _, status, ru = os.wait4(pid, 0)
    lf.close()
    return {"exit": os.waitstatus_to_exitcode(status),
            "wall_s": time.monotonic() - t0,
            "ru_utime_s": ru.ru_utime, "ru_stime_s": ru.ru_stime,
            "ru_nvcsw": ru.ru_nvcsw, "ru_nivcsw": ru.ru_nivcsw}


def stop_bpftrace():
    rc, out, _ = sudo(["pgrep", "-f", "bpftrace.*_session_enter_trace.bt"], check=False)
    if rc != 0 or not out:
        rc2, out2, _ = sudo(["pgrep", "-f", "bpftrace.*_session_alloc_trace.bt"], check=False)
        out = out2 if (rc2 == 0 and out2) else out
    pids = [int(x) for x in (out or b"").split()] if out else []
    for pid in pids:
        sudo(["kill", "-INT", str(pid)], check=False)
    return pids


def wait_exit(proc, timeout=40):
    try:
        proc.wait(timeout=timeout)
        return proc.returncode
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait(timeout=10)
        return "KILLED_TIMEOUT"


def traced_enter_run(raw, case_key, arm, bt_script):
    d = os.path.join(raw, "enter", f"{case_key}-{arm}")
    os.makedirs(d, exist_ok=True)
    jpath = os.path.join(d, "bench.jsonl")
    if os.path.exists(jpath):
        os.unlink(jpath)
    bt_out = open(os.path.join(d, "bpftrace.out"), "wb")
    bt_err = open(os.path.join(d, "bpftrace.err"), "wb")
    proc = subprocess.Popen(["sudo", "-n", "env", "BPFTRACE_PERF_RB_PAGES=16384",
                             BPFTRACE, bt_script],
                            stdout=bt_out, stderr=bt_err, stdin=subprocess.DEVNULL)
    attached = False
    deadline = time.monotonic() + 30
    err_path = os.path.join(d, "bpftrace.err")
    while time.monotonic() < deadline:
        time.sleep(0.2)
        try:
            data = open(err_path, "rb").read()
            if b"Attaching" in data:
                attached = True
                break
        except OSError:
            pass
        if proc.poll() is not None:
            break
    if not attached:
        say(f"ENTER_TRACE_ATTACH_FAILED {case_key}-{arm} rc={proc.poll()}")
    time.sleep(0.4)
    pid, t0, lf = spawn_bench(case_key, arm, jpath, os.path.join(d, "bench.log"))
    res = wait_bench(pid, t0, lf)
    stop_bpftrace()
    bt_rc = wait_exit(proc)
    bt_out.close()
    bt_err.close()
    meta = {"kind": "enter-trace", "case": CASES[case_key]["case"], "arm": arm,
            "bpftrace_rc": bt_rc, "attached": attached, "bench": res,
            "ts_utc": iso(), "bt_script": bt_script}
    with open(os.path.join(d, "meta.json"), "w") as f:
        json.dump(meta, f, indent=1)
    say(f"ENTER_RUN {case_key}-{arm} bench_exit={res['exit']} wall={res['wall_s']:.2f}s bt_rc={bt_rc}")
    return meta


def baseline_run(raw, bt_script, seconds=8):
    d = os.path.join(raw, "baseline")
    os.makedirs(d, exist_ok=True)
    bt_out = open(os.path.join(d, "bpftrace.out"), "wb")
    bt_err = open(os.path.join(d, "bpftrace.err"), "wb")
    t0 = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    proc = subprocess.Popen(["sudo", "-n", "env", "BPFTRACE_PERF_RB_PAGES=16384",
                             BPFTRACE, bt_script],
                            stdout=bt_out, stderr=bt_err, stdin=subprocess.DEVNULL)
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        time.sleep(0.2)
        if b"Attaching" in open(os.path.join(d, "bpftrace.err"), "rb").read():
            break
    time.sleep(seconds)
    stop_bpftrace()
    bt_rc = wait_exit(proc)
    bt_out.close()
    bt_err.close()
    with open(os.path.join(d, "meta.json"), "w") as f:
        json.dump({"kind": "baseline-no-bench", "seconds": seconds,
                   "start_utc": t0, "end_utc": iso(), "bpftrace_rc": bt_rc}, f, indent=1)
    say(f"BASELINE_RUN rc={bt_rc}")


def pmu_run(raw, case_key, arm):
    d = os.path.join(raw, "pmu", f"{case_key}-{arm}")
    os.makedirs(d, exist_ok=True)
    env_pairs = [f"{k}={v}" for k, v in sorted(bench_env(case_key, arm, "/dev/null").items())
                 if k.startswith("SLUICE_")]
    events = "cycles,instructions,cache-references,cache-misses,branch-misses,context-switches"
    csv = os.path.join(d, "perf.csv")
    txt = os.path.join(d, "perf.txt")
    cmd = [PERF, "stat", "-x", ",", "-e", events, "-o", csv, "--",
           TASKSET, "-c", "2", "/usr/bin/env"] + env_pairs + [BENCH]
    rc, _, _ = sudo(cmd, timeout=300)
    cmd2 = [PERF, "stat", "-e", events, "-o", txt, "--",
            TASKSET, "-c", "2", "/usr/bin/env"] + env_pairs + [BENCH]
    rc2, _, _ = sudo(cmd2, timeout=300)
    with open(os.path.join(d, "meta.json"), "w") as f:
        json.dump({"kind": "pmu", "case": CASES[case_key]["case"], "arm": arm,
                   "events": events, "perf_rc_csv": rc, "perf_rc_txt": rc2,
                   "scope": "root; u+k; whole process lifetime; bench runs as root child",
                   "ts_utc": iso()}, f, indent=1)
    say(f"PMU_RUN {case_key}-{arm} csv_rc={rc} txt_rc={rc2}")


def alloc_run(raw, case_key, arm, bt_script):
    d = os.path.join(raw, "alloc", f"{case_key}-{arm}")
    os.makedirs(d, exist_ok=True)
    jpath = os.path.join(d, "bench.jsonl")
    if os.path.exists(jpath):
        os.unlink(jpath)
    bt_out = open(os.path.join(d, "bpftrace.out"), "wb")
    bt_err = open(os.path.join(d, "bpftrace.err"), "wb")
    proc = subprocess.Popen(["sudo", "-n", "env", "BPFTRACE_PERF_RB_PAGES=16384",
                             BPFTRACE, bt_script],
                            stdout=bt_out, stderr=bt_err, stdin=subprocess.DEVNULL)
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        time.sleep(0.2)
        if b"Attaching" in open(os.path.join(d, "bpftrace.err"), "rb").read():
            break
        if proc.poll() is not None:
            break
    time.sleep(0.4)
    pid, t0, lf = spawn_bench(case_key, arm, jpath, os.path.join(d, "bench.log"))
    res = wait_bench(pid, t0, lf, timeout=600)
    rc, out, _ = sudo(["pgrep", "-f", "bpftrace.*_session_alloc_trace.bt"], check=False)
    for bp in [int(x) for x in (out or b"").split()]:
        sudo(["kill", "-INT", str(bp)], check=False)
    bt_rc = wait_exit(proc)
    bt_out.close()
    bt_err.close()
    with open(os.path.join(d, "meta.json"), "w") as f:
        json.dump({"kind": "alloc-trace", "case": CASES[case_key]["case"], "arm": arm,
                   "bpftrace_rc": bt_rc, "bench": res, "ts_utc": iso()}, f, indent=1)
    say(f"ALLOC_RUN {case_key}-{arm} bench_exit={res['exit']} wall={res['wall_s']:.2f}s bt_rc={bt_rc}")


def bucket_runs(raw):
    for arm in ARMS:
        rc, out, err = run(["/usr/sbin/taskset", "-c", "0", sys.executable, "-I",
                            BUCKET_PROBE,
                            "--binary", BENCH, "--case", CASES["d16"]["case"],
                            "--arm-var", "SLUICE_595_R3_ARM", "--arm", arm,
                            "--repeats", "3", "--tmp-dir", TMPDIR,
                            "--out-dir", os.path.join(raw, "bucket"),
                            "--label", f"R3-W03-D16-{arm}"],
                           timeout=600, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        with open(os.path.join(raw, "bucket", f"{arm}-runner.log"), "wb") as f:
            f.write(out or b"")
        say(f"BUCKET_RUN {arm} rc={rc}")


def resource_runs(raw):
    tdir = os.path.join(raw, "resource", "time-v")
    sdir = os.path.join(raw, "resource", "strace")
    for arm in ARMS:
        env_pairs = [f"{k}={v}" for k, v in sorted(bench_env("d16", arm, "/dev/null").items())
                     if k.startswith("SLUICE_")]
        rc, _, _ = run(["/usr/bin/time", "-v", "-o", os.path.join(tdir, f"{arm}.txt"), "--",
                        TASKSET, "-c", "2", "/usr/bin/env"] + env_pairs + [BENCH],
                       timeout=300, check=False)
        rc2, _, _ = run([STRACE, "-f", "-c",
                         "-e", "trace=timer_create,timerfd_create,setitimer,eventfd2,eventfd,io_uring_setup,io_uring_register,clone3,clone,mmap,munmap,brk,epoll_create1",
                         "-o", os.path.join(sdir, f"{arm}.txt"), "--",
                         TASKSET, "-c", "2", "/usr/bin/env"] + env_pairs + [BENCH],
                        timeout=300, check=False)
        say(f"RESOURCE_RUN {arm} time_rc={rc} strace_rc={rc2}")


def comm_probe(raw):
    d = os.path.join(raw, "logs")
    jpath = os.path.join(d, "comm-probe.jsonl")
    if os.path.exists(jpath):
        os.unlink(jpath)
    pid, t0, lf = spawn_bench("d2pos", "A0", jpath, os.path.join(d, "comm-probe.log"))
    comms = {}
    deadline = time.monotonic() + 20
    while time.monotonic() < deadline:
        try:
            for tid in os.listdir(f"/proc/{pid}/task"):
                try:
                    with open(f"/proc/{pid}/task/{tid}/comm") as f:
                        comms[int(tid)] = f.read().strip()
                except OSError:
                    pass
        except OSError:
            pass
        time.sleep(0.01)
        if os.wait4(pid, os.WNOHANG)[0] != 0:
            break
    try:
        _, status, _ = os.wait4(pid, 0)
        exit_code = os.waitstatus_to_exitcode(status)
    except ChildProcessError:
        exit_code = None
    lf.close()
    uniq = sorted(set(comms.values()))
    with open(os.path.join(d, "comm-probe.json"), "w") as f:
        json.dump({"comms_by_tid": {str(k): v for k, v in comms.items()},
                   "unique_comms": uniq, "bench_exit": exit_code, "ts_utc": iso()}, f, indent=1)
    say(f"COMM_PROBE unique={uniq} exit={exit_code}")
    return uniq


def main():
    global _pw, _log
    ap = argparse.ArgumentParser()
    ap.add_argument("--raw-root", required=True)
    ap.add_argument("--lock-file", default="/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock")
    args = ap.parse_args()

    raw = os.path.abspath(args.raw_root)
    os.makedirs(raw, exist_ok=True)
    os.makedirs(os.path.join(raw, "logs"), exist_ok=True)
    _log = open(os.path.join(raw, "logs", "session.log"), "a", buffering=1)

    pw_line = sys.stdin.readline()
    _pw = pw_line.rstrip("\n") or None

    say("MECH_SESSION_BEGIN")
    bench_sha = sha256_file(BENCH)
    if bench_sha != BENCH_SHA:
        say(f"BENCH_SHA_MISMATCH {bench_sha}")
        return 3

    lock = open(args.lock_file, "rb+")
    t0 = time.monotonic()
    acquired = None
    waited = None
    while time.monotonic() - t0 < 1800:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            acquired = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
            waited = time.monotonic() - t0
            break
        except BlockingIOError:
            time.sleep(1)
    if acquired is None:
        say("LOCK_TIMEOUT_1800s")
        return 4
    st = os.fstat(lock.fileno())
    rc, out, _ = run(["/usr/sbin/fuser", "-v", args.lock_file], check=False)
    say(f"LOCK_ACQUIRED {acquired} waited={waited:.1f}s inode={st.st_ino} "
        f"fuser_rc={rc} fuser={(out or b'').decode(errors='replace').strip()!r}")

    pre = {
        "ts_utc": iso(),
        "binary_sha256": bench_sha,
        "loadavg": open("/proc/loadavg").read().strip(),
        "cpuinfo_cpu2": open("/sys/devices/system/cpu/cpu2/cpufreq/scaling_cur_freq").read().strip(),
        "governor_cpu2": open("/sys/devices/system/cpu/cpu2/cpufreq/scaling_governor").read().strip(),
        "perf_event_paranoid": open("/proc/sys/kernel/perf_event_paranoid").read().strip(),
        "unprivileged_bpf_disabled": open("/proc/sys/kernel/unprivileged_bpf_disabled").read().strip(),
        "kernel": os.uname().release,
    }
    with open(os.path.join(raw, "pre-state.json"), "w") as f:
        json.dump(pre, f, indent=1)
    say(f"PRE_STATE {json.dumps(pre)}")

    try:
        # sudo cache warm-up (password used only here, from stdin)
        v = subprocess.run(["sudo", "-S", "-p", "", "-v"], input=(_pw + "\n").encode(),
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=30)
        say(f"SUDO_VALIDATE rc={v.returncode}")

        cap = os.path.join(raw, "pmu-capability")
        run([PERF, "stat", "-x", ",", "-e",
             "cycles,instructions,cache-references,cache-misses,branch-misses,context-switches",
             "-o", os.path.join(cap, "user-true.csv"), "--", "/bin/true"], check=False)
        sudo([PERF, "stat", "-e", "cycles:u,cycles:k,instructions:u,instructions:k",
              "-o", os.path.join(cap, "root-uk-true.txt"), "--", "/bin/true"], check=False)
        sudo([PERF, "stat", "-x", ",", "-e",
              "cycles:u,cycles:k,instructions:u,instructions:k,context-switches:u,context-switches:k",
              "-o", os.path.join(cap, "root-uk-true.csv"), "--", "/bin/true"], check=False)
        say("PMU_CAPABILITY_RECORDED")

        uniq = comm_probe(raw)
        comm = None
        for c in uniq:
            if c.startswith("bench_w03"):
                comm = c
                break
        if comm is None:
            say("COMM_PROBE_FAILED_NO_BENCH_COMM")
            return 5
        tmpl = open(os.path.join(raw, "scripts", "enter_trace.tmpl.bt")).read()
        enter_bt = os.path.join(raw, "scripts", "_session_enter_trace.bt")
        open(enter_bt, "w").write(tmpl.replace("__COMM__", comm))
        tmpl = open(os.path.join(raw, "scripts", "alloc_trace.tmpl.bt")).read()
        alloc_bt = os.path.join(raw, "scripts", "_session_alloc_trace.bt")
        open(alloc_bt, "w").write(tmpl.replace("__COMM__", comm))
        say(f"BPFTRACE_SCRIPTS_WRITTEN comm={comm}")

        # warm fixture (untraced)
        jpath = os.path.join(raw, "logs", "warmup.jsonl")
        if os.path.exists(jpath):
            os.unlink(jpath)
        pid, tw, lf = spawn_bench("d16", "A0", jpath, os.path.join(raw, "logs", "warmup.log"))
        res = wait_bench(pid, tw, lf)
        say(f"WARMUP_RUN exit={res['exit']} wall={res['wall_s']:.2f}s")

        for case_key in ("d16", "d2pos", "d2serial"):
            for arm in ARMS:
                traced_enter_run(raw, case_key, arm, enter_bt)
        baseline_run(raw, enter_bt)

        for arm in ARMS:
            pmu_run(raw, "d16", arm)
        for arm in ARMS:
            alloc_run(raw, "d16", arm, alloc_bt)

        bucket_runs(raw)
        resource_runs(raw)
    finally:
        post = {"ts_utc": iso(),
                "loadavg": open("/proc/loadavg").read().strip(),
                "cpuinfo_cpu2": open("/sys/devices/system/cpu/cpu2/cpufreq/scaling_cur_freq").read().strip()}
        with open(os.path.join(raw, "post-state.json"), "w") as f:
            json.dump(post, f, indent=1)
        fcntl.flock(lock, fcntl.LOCK_UN)
        say(f"LOCK_RELEASED {iso()}")
        with open(os.path.join(raw, "lock-log.jsonl"), "a") as f:
            f.write(json.dumps({
                "event": "mechanism-diagnostic-session",
                "acquired_utc": acquired, "released_utc": iso(),
                "wait_s": round(waited, 3),
                "lock_file": os.path.abspath(args.lock_file), "inode": st.st_ino,
            }) + "\n")
        _log.close()
    say("MECH_SESSION_END")
    return 0


if __name__ == "__main__":
    sys.exit(main())
