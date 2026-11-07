#!/usr/bin/env python3
"""CF-2 deferoff sysio companion: per-thread /proc io+stat sampling, A/B pairs.

Non-timing run. Each invocation = fresh bench process pinned to cpu2,
WARMUP=1 MEASURED=5 x 20k op (matches the timing protocol shape so syscw/op
is comparable with CF-1 numbers). Buckets threads by comm prefix:
bench_w02_pipeline* -> bench, iou*/io_* -> iowq, rest -> other.
Emits one JSON record per invocation.
"""
import json
import os
import subprocess
import sys
import threading
import time

CASE = "W02-uring-4k-d2"
TASKSET = "/usr/bin/taskset"


class Sampler(threading.Thread):
    def __init__(self, pid, period_s=0.01):
        super().__init__(daemon=True)
        self.pid = pid
        self.period = period_s
        self.stop = threading.Event()
        self.samples = []
        self.errors = 0

    def sample_once(self):
        base = f"/proc/{self.pid}/task"
        snap = {}
        try:
            tids = os.listdir(base)
        except OSError:
            return
        for tid in tids:
            try:
                with open(f"{base}/{tid}/comm") as f:
                    comm = f.read().strip()
                with open(f"{base}/{tid}/stat") as f:
                    st = f.read()
                rp = st.rfind(")")
                fields = st[rp + 2:].split()
                utime, stime = int(fields[11]), int(fields[12])
                io = {}
                with open(f"{base}/{tid}/io") as f:
                    for line in f:
                        k, _, v = line.partition(":")
                        io[k.strip()] = int(v.strip())
                snap[tid] = {"comm": comm, "utime": utime, "stime": stime,
                             "rchar": io.get("rchar", 0),
                             "wchar": io.get("wchar", 0),
                             "syscr": io.get("syscr", 0),
                             "syscw": io.get("syscw", 0)}
            except (OSError, IndexError, ValueError):
                self.errors += 1
        if snap:
            self.samples.append({"t": time.monotonic(), "threads": snap})

    def run(self):
        while not self.stop.is_set():
            self.sample_once()
            self.stop.wait(self.period)

    def aggregate(self, hz=100.0):
        if not self.samples:
            return {}
        last = self.samples[-1]["threads"]
        buckets = {}
        for tid, th in last.items():
            comm = th["comm"]
            if comm.startswith("bench"):
                key = "bench"
            elif comm.startswith(("iou", "io_")):
                key = "iowq"
            else:
                key = "other"
            b = buckets.setdefault(key, {"user_s": 0.0, "kernel_s": 0.0,
                                         "threads": 0, "rchar": 0, "wchar": 0,
                                         "syscr": 0, "syscw": 0})
            b["user_s"] += th["utime"] / hz
            b["kernel_s"] += th["stime"] / hz
            b["threads"] += 1
            b["rchar"] += th["rchar"]
            b["wchar"] += th["wchar"]
            b["syscr"] += th["syscr"]
            b["syscw"] += th["syscw"]
        return buckets


def run_once(binary, arm, out_dir, label, pair, pos):
    tag = f"sysio-{label}_p{pair:02d}_pos{pos}_{arm}"
    jpath = os.path.join(out_dir, f"{tag}.jsonl")
    opath = os.path.join(out_dir, f"{tag}.out")
    epath = os.path.join(out_dir, f"{tag}.err")
    if os.path.exists(jpath):
        os.unlink(jpath)
    env = os.environ.copy()
    env.update({
        "SLUICE_BENCH_WARMUP": "1",
        "SLUICE_BENCH_MEASURED": "5",
        "SLUICE_BENCH_CASE_FILTER": "=" + CASE,
        "SLUICE_BENCH_595_DEPTHS": "1",
        "SLUICE_BENCH_STATS": "1",
        "SLUICE_BENCH_JSONL": jpath,
        "SLUICE_BENCH_TMP_DIR": "/tmp/sluice-595-bench",
        "SLUICE_595_ARM": arm,
    })
    with open(opath, "wb") as fo, open(epath, "wb") as fe:
        t0 = time.monotonic()
        p = subprocess.Popen([TASKSET, "-c", "2", binary], env=env,
                             stdout=fo, stderr=fe)
        sampler = Sampler(p.pid)
        sampler.start()
        rc = p.wait()
        sampler.stop.set()
        sampler.join(timeout=1.0)
        elapsed = time.monotonic() - t0
    records = []
    if os.path.exists(jpath):
        with open(jpath) as f:
            for line in f:
                line = line.strip()
                if line:
                    records.append(json.loads(line))
    rec = {"label": f"{label}", "pair": pair, "pos": pos, "arm": arm,
           "case": CASE, "rc": rc, "elapsed_s": elapsed,
           "threads": sampler.aggregate(),
           "comm_snapshot": sorted({th["comm"] for th in
                                    sampler.samples[-1]["threads"].values()})
           if sampler.samples else [],
           "samples": len(sampler.samples), "errors": sampler.errors,
           "measured_records": sum(1 for r in records if r.get("measured"))}
    return rec


def main():
    out_dir = sys.argv[1]
    binaries = [(sys.argv[2], sys.argv[3]), (sys.argv[4], sys.argv[5])]
    os.makedirs(out_dir, exist_ok=True)
    results = []
    for binary, label in binaries:
        for pair in range(6):
            order = ["A", "B"] if pair % 2 == 0 else ["B", "A"]
            for pos, arm in enumerate(order):
                rec = run_once(binary, arm, out_dir, label, pair, pos)
                results.append(rec)
                syscw = rec["threads"].get("bench", {}).get("syscw", -1)
                print(f"[{label}] pair={pair} pos={pos} arm={arm} rc={rec['rc']} "
                      f"bench_syscw={syscw} samples={rec['samples']}", flush=True)
    with open(os.path.join(out_dir, "results.json"), "w") as f:
        json.dump(results, f, indent=1)


if __name__ == "__main__":
    main()
