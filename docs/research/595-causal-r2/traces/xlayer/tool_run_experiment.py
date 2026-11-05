#!/usr/bin/env python3 -I
"""Drive one cross-layer trace experiment: held bench + root bpftrace join.

Usage: s595_run.py <label> <arm> [extra env KEY=VAL ...]
Starts the bench pinned to cpu2 behind a stdin hold, attaches
/tmp/s595_kernel.bt filtered to the bench pid, releases the hold, waits for
completion, and stops bpftrace. Outputs land in evidence/traces/xlayer/<label>/.
"""
import os
import subprocess
import sys
import time

label = sys.argv[1]
arm = sys.argv[2]
extra_env = sys.argv[3:]

out = f"/home/jnhu/sluice-595-e5/causal-r2/evidence/traces/xlayer/{label}"
os.makedirs(out, exist_ok=True)
bin_path = "/home/jnhu/sluice-595-e5/causal-r2/worktree/build/linux/x86_64/release/bench_w02_pipeline"
fifo = f"/tmp/s595_hold_{label}"
if os.path.exists(fifo):
    os.unlink(fifo)
os.mkfifo(fifo)

env = dict(os.environ)
env.update({
    "SLUICE_595_TRACE": "1",
    "SLUICE_595_TRACE_HOLD": "1",
    "SLUICE_595_TRACE_OUT": f"{out}/ev.bin",
    "SLUICE_595_ARM": arm,
    "SLUICE_BENCH_WARMUP": "0",
    "SLUICE_BENCH_MEASURED": "1",
    "SLUICE_BENCH_REUSE_FIXTURE": "1",
    "SLUICE_BENCH_TMP_DIR": "/tmp/sluice-595-cost",
})
for kv in extra_env:
    k, _, v = kv.partition("=")
    env[k] = v

hold_rd = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)
hold_wr = os.open(fifo, os.O_WRONLY)
os.set_blocking(hold_rd, True)
bench = subprocess.Popen(
    ["taskset", "-c", "2", bin_path],
    stdin=hold_rd, stdout=open(f"{out}/bench_stdout.txt", "w"),
    stderr=open(f"{out}/bench_stderr.txt", "w"), env=env,
)
deadline = time.time() + 30
while time.time() < deadline:
    try:
        with open(f"{out}/bench_stderr.txt") as f:
            if "S595_TRACE_HOLD waiting" in f.read():
                break
    except FileNotFoundError:
        pass
    if bench.poll() is not None:
        print(f"BENCH_EXITED_EARLY rc={bench.returncode}")
        sys.exit(1)
    time.sleep(0.2)
else:
    print("HOLD_MARKER_TIMEOUT")
    sys.exit(1)
pid = bench.pid
print(f"bench pid={pid} held, attaching bpftrace")

kout = open(f"{out}/kernel.txt", "w")
kerr = open(f"{out}/kernel.err", "w")
script = f"/tmp/s595_kernel_{pid}.bt"
with open("/tmp/s595_kernel.bt") as tmpl, open(script, "w") as gen:
    gen.write(tmpl.read().replace("TARGET_PID", str(pid)))
bt = subprocess.Popen(
    ["sudo", "-S", "env", "BPFTRACE_PERF_RB_PAGES=2048", "bpftrace", script],
    stdin=subprocess.PIPE, stdout=kout, stderr=kerr,
)
bt.stdin.write(b"121212\n")
bt.stdin.flush()
deadline = time.time() + 60
while time.time() < deadline:
    with open(f"{out}/kernel.txt") as f:
        if "S595K BEGIN" in f.read():
        # attached
            break
    if bt.poll() is not None:
        print(f"BPFTRACE_EXITED_EARLY rc={bt.returncode}")
        sys.exit(1)
    time.sleep(0.2)
else:
    print("BPFTRACE_ATTACH_TIMEOUT")
    sys.exit(1)
print("bpftrace attached, releasing bench")
os.write(hold_wr, b"release\n")
rc = bench.wait(timeout=600)
print(f"bench rc={rc}")
time.sleep(1.0)
bt.send_signal(subprocess.signal.SIGINT) if bt.poll() is None else None
try:
    bt.wait(timeout=30)
except subprocess.TimeoutExpired:
    bt.kill()
print(f"bpftrace rc={bt.returncode}")
os.close(hold_rd)
os.close(hold_wr)
os.unlink(fifo)
with open(f"{out}/run_meta.txt", "w") as f:
    f.write(f"label={label}\narm={arm}\nbench_pid={pid}\nbench_rc={rc}\n"
            f"extra_env={' '.join(extra_env)}\n")
sys.exit(0 if rc == 0 else 1)
