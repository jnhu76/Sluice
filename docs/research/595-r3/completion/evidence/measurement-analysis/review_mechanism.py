#!/usr/bin/env python3
"""Independent review of the #595 R3 mechanism diagnostic window (raw/mechanism).

Recomputes, from raw bpftrace output / bench.jsonl / perf.csv / strace only:
  - pure-submit enter classification per run (d16/d2pos/d2serial x A0/A1/B)
  - submitter-vs-driver attribution (tid==pid), ret==to_submit (no partial submit)
  - bench r3_* adapter counters, accepted+untouched==r reconciliation
  - defer / eventfd / timer / clone / io_uring_setup counts
  - PMU event values and multiplexing check
  - malloc/free counts (alloc uprobe)
  - CPU bucket probe (main/driver/io-wq ticks, unattributed residual)
No benchmark is run.
"""
import glob
import json
import os
import re
import sys

ROOT = "/home/jnhu/sluice-595-e5/r3-completion-20261105T083818Z"
M = os.path.join(ROOT, "raw", "mechanism")
RUNS = [f"{c}-{a}" for c in ("d16", "d2pos", "d2serial") for a in ("A0", "A1", "B")]


def parse_bt(path):
    events = []
    maps = {}
    lost = None
    for line in open(path, errors="replace"):
        line = line.rstrip("\n")
        if line.startswith("E "):
            parts = line.split()
            # E pid tid fd to_submit min_complete flags
            events.append({"pid": int(parts[1]), "tid": int(parts[2]),
                           "fd": int(parts[3]), "ts": int(parts[4]),
                           "mc": int(parts[5]), "flags": int(parts[6])})
        m = re.match(r"^@([\w]+)(?:\[(\S+)\])?: (\d+)", line)
        if m:
            maps.setdefault(m.group(1), {})[m.group(2)] = int(m.group(3))
        if "lost events" in line.lower():
            lost = line.strip()
    return events, maps, lost


def summarize_enter(run):
    p = os.path.join(M, "enter", run, "bpftrace.out")
    ev, maps, lost = parse_bt(p)
    pure = [e for e in ev if e["ts"] > 0 and e["mc"] == 0 and e["flags"] == 0]
    ge = [e for e in ev if e["flags"] & 1 or e["mc"] > 0]
    buckets = {}
    for e in ev:
        k = (e["ts"], e["mc"], e["flags"])
        buckets[k] = buckets.get(k, 0) + 1
    pid = ev[0]["pid"] if ev else None
    submitter_pure = sum(1 for e in pure if e["tid"] == pid)
    driver_pure = len(pure) - submitter_pure
    ret = maps.get("ret_hist", {})
    ts = maps.get("ts_hist", {})
    ret_eq_ts = all(ret.get(str(k), 0) == v for k, v in ts.items())
    return {
        "run": run,
        "total_enter_lines": len(ev),
        "pure_submit": len(pure),
        "getevents_or_wait_lines": len(ge),
        "pure_wait_only": sum(1 for e in ev if e["ts"] == 0),
        "buckets": {str(k): v for k, v in sorted(buckets.items())},
        "submitter_pure": submitter_pure,
        "driver_pure": driver_pure,
        "ret_hist": ret, "ts_hist": ts,
        "ret_eq_to_submit": ret_eq_ts,
        "max_to_submit": max((e["ts"] for e in ev), default=0),
        "enter_gt1": sum(1 for e in pure if e["ts"] > 1),
        "sqe_opcode": maps.get("sqe_opcode", {}),
        "sqe_total": maps.get("sqe_total", {}).get(None),
        "defer_total": maps.get("defer_total", {}).get(None),
        "eventfd2": maps.get("eventfd2", {}).get(None),
        "eventfd_write": maps.get("eventfd_write", {}).get(None),
        "timer_create": maps.get("timer_create", {}).get(None),
        "iouring_setup": maps.get("iouring_setup", {}).get(None),
        "iouring_register": maps.get("iouring_register", {}).get(None),
        "futex_total": maps.get("futex_total", {}).get(None),
        "epoll_total": maps.get("epoll_total", {}).get(None),
        "lost": lost,
    }


def bench_stats(run):
    rows = []
    for l in open(os.path.join(M, "enter", run, "bench.jsonl")):
        x = json.loads(l)
        if x["case_id"].startswith("CAL"):
            continue
        rows.append({k: x.get(k) for k in (
            "case_id", "measured", "ops", "ok", "r3_arm", "r3_entry_calls",
            "r3_group_calls", "r3_single_calls", "r3_accepted", "r3_prefix_max",
            "r3_suffix_single_calls", "r3_untouched", "r3_flush_attempts",
            "r3_ineligible_fallbacks", "r3_gate_stops", "stats_submitted_ops",
            "stats_completed_ops", "notifications", "empty_wakes", "submits")})
    return rows


def alloc_stats():
    out = {}
    for a in ("A0", "A1", "B"):
        p = os.path.join(M, "alloc", f"d16-{a}", "bpftrace.out")
        ev, maps, lost = parse_bt(p)
        flat = {k: (v.get(None) if isinstance(v, dict) else v) for k, v in maps.items()}
        out[a] = {"malloc_calls": flat.get("malloc_calls"),
                  "malloc_bytes": flat.get("malloc_bytes"),
                  "free_calls": flat.get("free_calls"),
                  "calloc_calls": flat.get("calloc_calls"),
                  "realloc_calls": flat.get("realloc_calls"),
                  "memalign_calls": flat.get("memalign_calls"),
                  "lost": lost}
    return out


def pmu_stats():
    out = {}
    for a in ("A0", "A1", "B"):
        csvp = os.path.join(M, "pmu", f"d16-{a}", "perf.csv")
        vals = {}
        pct = set()
        for line in open(csvp):
            parts = [p for p in line.strip().split(",")]
            if len(parts) >= 5 and parts[2]:
                try:
                    vals[parts[2]] = int(parts[0])
                    pct.add(parts[4])
                except ValueError:
                    pass
        out[a] = {"events": vals, "percent_fields": sorted(pct)}
    return out


def bucket_stats():
    recs = [json.loads(l) for l in open(os.path.join(M, "bucket", "results.jsonl")) if l.strip()]
    out = {}
    for r in recs:
        arm = str(r.get("arm"))
        b = r.get("buckets", {})
        main_t = b.get("main", {}).get("ticks")
        drv_t = b.get("driver", {}).get("ticks")
        iowq = b.get("io_wq", {})
        iowq_ticks = sum(t.get("ticks", 0) for t in iowq.get("threads", []))
        out.setdefault(arm, []).append({
            "main_ticks": main_t, "driver_ticks": drv_t,
            "io_wq_ticks": iowq_ticks,
            "ru_ticks_est": r.get("ru_ticks_est"),
            "unattributed_ticks_est": r.get("unattributed_ticks_est"),
            "max_user_threads": r.get("max_user_threads"),
            "bench_cpu_s": r.get("bench", {}).get("cpu_s"),
            "ru_total_s": r.get("ru_total"),
        })
    # summarize per arm
    summ = {}
    for arm, rows in out.items():
        n = len(rows)
        summ[arm] = {
            "n": n,
            "main_ticks_mean": sum(x["main_ticks"] for x in rows) / n,
            "driver_ticks_mean": sum(x["driver_ticks"] for x in rows) / n,
            "io_wq_ticks_mean": sum(x["io_wq_ticks"] for x in rows) / n,
            "unattributed_ticks_mean": sum(x["unattributed_ticks_est"] for x in rows) / n,
            "max_user_threads": max(x["max_user_threads"] for x in rows),
        }
    return {"per_run": out, "summary": summ}


def resource_stats():
    out = {}
    # time -v
    for a in ("A0", "A1", "B"):
        p = os.path.join(M, "resource", "time-v", f"{a}.txt")
        d = {}
        if os.path.exists(p):
            for line in open(p):
                if "Maximum resident set size" in line:
                    d["max_rss_kb"] = int(line.split(":")[1].strip())
        out[a] = d
    # strace -c summary: "% time ... calls ... name"
    for a in ("A0", "A1", "B"):
        p = os.path.join(M, "resource", "strace", f"{a}.txt")
        c = {}
        if os.path.exists(p):
            for line in open(p, errors="replace"):
                m = re.match(r"^\s*[\d.]+\s+[\d.]+\s+\d+\s+(\d+)\s+(?:(\d+)\s+)?(\S+)", line)
                if m:
                    c[m.group(3)] = int(m.group(1))
        out.setdefault(a, {}).update(c)
    return out


def main():
    report = {"enters": {}, "bench": {}, "alloc": alloc_stats(), "pmu": pmu_stats(),
              "bucket": bucket_stats(), "resource": resource_stats()}
    for run in RUNS:
        report["enters"][run] = summarize_enter(run)
        report["bench"][run] = bench_stats(run)
    # cross-check vs executor's analysis jsons
    exec_enters = {}
    ad = os.path.join(M, "analysis")
    for f in glob.glob(os.path.join(ad, "enter-*.json")):
        exec_enters[os.path.basename(f)[6:-5]] = json.load(open(f))
    report["executor_enter_json"] = {k: {kk: v.get(kk) for kk in (
        "pure_submit_enters", "total_enters", "to_submit_hist", "ret_hist",
        "max_to_submit", "driver_pure_submit")} for k, v in exec_enters.items()}
    print(json.dumps(report, indent=1, sort_keys=True))
    with open(os.path.join(ROOT, "analysis", "review_mechanism_out.json"), "w") as f:
        json.dump(report, f, indent=1, sort_keys=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
