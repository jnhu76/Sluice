#!/usr/bin/env python3
"""Analyzer for the #595 R3 mechanism diagnostic session (diagnostic lane).

Parses bpftrace raw output (per-event E lines + exit maps), bench JSONL,
perf stat CSV, allocation maps, bucket-probe results and resource runs into
per-run and per-block summaries. No timing conclusions are produced.
"""
import argparse
import collections
import glob
import json
import os
import re
import statistics

GETEVENTS = 1  # IORING_ENTER_GETEVENTS
SQ_WAKEUP = 2
SQ_WAIT = 4

MAP_RE = re.compile(r"^@([A-Za-z_0-9]+)(?:\[([^\]]*)\])?:\s+(\d+)$")
E_RE = re.compile(r"^E (\d+) (\d+) (-?\d+) (\d+) (\d+) (\d+)$")


def _attached_from_err(err_txt):
    m = re.search(r"Attached (\d+) probes", err_txt)
    return int(m.group(1)) if m else None


def parse_bpftrace(path):
    events = []
    maps = collections.defaultdict(lambda: collections.Counter())
    scalars = {}
    lost = []
    attached = None
    with open(path, "r", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            m = E_RE.match(line)
            if m:
                pid, tid, fd, ts, mc, fl = map(int, m.groups())
                events.append({"pid": pid, "tid": tid, "fd": fd, "to_submit": ts,
                               "min_complete": mc, "flags": fl})
                continue
            if "Attached" in line or "Attaching" in line:
                try:
                    attached = int(re.search(r"\d+", line).group())
                except (AttributeError, ValueError):
                    attached = line
                continue
            if "events lost" in line or "event lost" in line:
                lost.append(line)
                continue
            m = MAP_RE.match(line)
            if m:
                name, key, val = m.group(1), m.group(2), int(m.group(3))
                if key is None:
                    scalars[name] = val
                else:
                    maps[name][key.strip()] = val
    return {"events": events, "maps": dict(maps), "scalars": scalars,
            "lost": lost, "attached": attached}


def classify(e):
    ts, mc, fl = e["to_submit"], e["min_complete"], e["flags"]
    if ts > 0 and mc == 0 and fl == 0:
        return "pure_submit"
    if ts > 0:
        return "submit_with_flags"
    if mc > 0 or (fl & GETEVENTS):
        return "wait_getevents"
    return "other_zero_submit"


def analyze_enter_run(d, expected_ops_mult=3):
    bt = parse_bpftrace(os.path.join(d, "bpftrace.out"))
    err_txt = open(os.path.join(d, "bpftrace.err"), "r", errors="replace").read()
    ev = bt["events"]
    classes = collections.Counter(classify(e) for e in ev)
    ts_hist = collections.Counter(e["to_submit"] for e in ev)
    mc_hist = collections.Counter(e["min_complete"] for e in ev)
    fl_hist = collections.Counter(e["flags"] for e in ev)
    pure_submit_ts = collections.Counter(
        e["to_submit"] for e in ev if classify(e) == "pure_submit")
    pure_by_tid = collections.Counter(
        e["tid"] for e in ev if classify(e) == "pure_submit")
    main_tid = ev[0]["pid"] if ev else None
    submitter_pure = pure_by_tid.get(main_tid, 0)
    driver_pure = sum(c for t, c in pure_by_tid.items() if t != main_tid)
    sum_ts = sum(e["to_submit"] for e in ev)
    sum_ts_pure = sum(e["to_submit"] for e in ev if classify(e) in
                      ("pure_submit", "submit_with_flags"))

    rec = None
    jpath = os.path.join(d, "bench.jsonl")
    if os.path.exists(jpath):
        for line in open(jpath):
            r = json.loads(line)
            if r.get("measured"):
                rec = r
    meta = json.load(open(os.path.join(d, "meta.json")))

    request_count = rec["request_count"] if rec else None
    expected_data_sqe = request_count * expected_ops_mult if request_count else None
    coverage = None
    if expected_data_sqe is not None:
        coverage = {"sum_to_submit_all": sum_ts,
                    "expected_data_sqe": expected_data_sqe,
                    "delta_all": sum_ts - expected_data_sqe,
                    "sum_to_submit_submit_classes": sum_ts_pure}
    e_map_count = bt["scalars"].get("enter_bench")
    return {
        "meta": meta,
        "bench_record": {k: rec[k] for k in (
            "case_id", "ops", "submits", "notifications", "empty_wakes", "passes",
            "r3_entry_calls", "r3_group_calls", "r3_single_calls", "r3_accepted",
            "r3_prefix_max", "r3_suffix_single_calls", "r3_flush_attempts",
            "r3_ineligible_fallbacks", "r3_gate_stops", "r3_untouched",
            "stats_submit_calls", "stats_submitted_ops", "stats_completed_ops",
            "stats_poll_calls", "stats_wait_calls", "stats_queue_full_retries",
            "stats_invalid_state", "request_count", "ok", "failure",
            "lat_p50_ns", "lat_p95_ns", "lat_p99_ns", "cpu_s", "ops_per_sec",
            "peak_rss_kb")} if rec else None,
        "enter": {
            "total_events_parsed": len(ev),
            "map_enter_bench": e_map_count,
            "map_enter_global": bt["scalars"].get("enter_global"),
            "parsed_equals_map": (e_map_count == len(ev)) if e_map_count is not None else None,
            "classes": dict(classes),
            "to_submit_hist": dict(sorted(ts_hist.items())),
            "pure_submit_to_submit_hist": dict(sorted(pure_submit_ts.items())),
            "min_complete_hist": dict(sorted(mc_hist.items())),
            "flags_hist": dict(sorted(fl_hist.items())),
            "max_to_submit": max(ts_hist) if ts_hist else 0,
            "sum_to_submit": sum_ts,
            "pure_submit_by_tid": {str(k): v for k, v in sorted(pure_by_tid.items())},
            "main_tid": main_tid,
            "submitter_pure_submits": submitter_pure,
            "driver_pure_submits": driver_pure,
            "exit_total": bt["scalars"].get("exit_total"),
            "ret_hist": dict(bt["maps"].get("ret_hist", {})),
            "coverage": coverage,
        },
        "park_wake": {
            "futex_total": bt["scalars"].get("futex_total"),
            "futex_wait": bt["scalars"].get("futex_wait"),
            "futex_wake": bt["scalars"].get("futex_wake"),
            "epoll_wait": bt["scalars"].get("epoll_wait"),
        },
        "global_kernel": {
            "sqe_total": bt["scalars"].get("sqe_total"),
            "sqe_opcode": dict(bt["maps"].get("sqe_opcode", {})),
            "cqe_total": bt["scalars"].get("cqe_total"),
            "cqe_res": dict(bt["maps"].get("cqe_res", {})),
            "defer_total": bt["scalars"].get("defer_total"),
            "defer_opcode": dict(bt["maps"].get("defer_opcode", {})),
            "queue_async_work": bt["scalars"].get("queue_async_work"),
            "iouring_setup": bt["scalars"].get("iouring_setup"),
            "iouring_register": bt["scalars"].get("iouring_register"),
            "eventfd2": bt["scalars"].get("eventfd2"),
            "eventfd_write": bt["scalars"].get("eventfd_write"),
            "timer_create": bt["scalars"].get("timer_create"),
            "timerfd_create": bt["scalars"].get("timerfd_create"),
        },
        "trace_health": {
            "attached_probes": bt["attached"],
            "attached_from_stderr": _attached_from_err(err_txt),
            "lost_events": bt["lost"],
            "stderr_has_lost": "lost" in err_txt,
        },
        "bucket_map": dict(bt["maps"].get("bucket", {})),
    }


def parse_perf_csv(path):
    out = []
    for line in open(path):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split(",")
        if len(parts) >= 5 and parts[2]:
            try:
                out.append({"value": int(parts[0]), "event": parts[2],
                            "time_running_ns": int(parts[3]),
                            "time_enabled_pct": float(parts[4])})
            except ValueError:
                pass
    return out


def parse_perf_txt_multiplexing(path):
    hits = []
    for line in open(path, errors="replace"):
        if "%" in line and ("cycles" in line or "instructions" in line
                            or "cache" in line or "branch" in line
                            or "context-switches" in line):
            m = re.search(r"\[([\d.]+)%\]", line)
            if m:
                hits.append({"line": line.strip(), "scaled_pct": float(m.group(1))})
    return hits


def parse_time_v(path):
    res = {}
    for line in open(path, errors="replace"):
        for key, tag in (("Maximum resident set size (kbytes)", "max_rss_kb"),
                         ("Voluntary context switches", "voluntary_csw"),
                         ("Involuntary context switches", "involuntary_csw"),
                         ("Exit status", "exit_status")):
            if line.startswith("\t" + key) or line.startswith(key):
                try:
                    res[tag] = int(line.split(":")[-1].strip())
                except ValueError:
                    res[tag] = line.split(":")[-1].strip()
    return res


def parse_strace_c(path):
    counts = {}
    for line in open(path, errors="replace"):
        m = re.match(r"^\s*[\d.]+\s+[\d.]+\s+\d+\s+(\d+)\s+(?:\d+\s+)?(\S+)$", line)
        if m and m.group(2) != "total":
            counts[m.group(2)] = int(m.group(1))
    return counts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--raw-root", required=True)
    ap.add_argument("--out-dir", required=True)
    args = ap.parse_args()
    raw = os.path.abspath(args.raw_root)
    out = os.path.abspath(args.out_dir)
    os.makedirs(out, exist_ok=True)
    ana = os.path.join(raw, "analysis")
    os.makedirs(ana, exist_ok=True)

    result = {"runs": {}, "pmu": {}, "alloc": {}, "bucket": [], "resource": {},
              "baseline": None, "comm_probe": None}

    for d in sorted(glob.glob(os.path.join(raw, "enter", "*"))):
        name = os.path.basename(d)
        r = analyze_enter_run(d)
        result["runs"][name] = r
        with open(os.path.join(ana, f"enter-{name}.json"), "w") as f:
            json.dump(r, f, indent=1, sort_keys=True)

    bd = os.path.join(raw, "baseline")
    if os.path.exists(os.path.join(bd, "bpftrace.out")):
        bt = parse_bpftrace(os.path.join(bd, "bpftrace.out"))
        meta = json.load(open(os.path.join(bd, "meta.json")))
        import datetime
        t0 = datetime.datetime.strptime(meta["start_utc"], "%Y-%m-%dT%H:%M:%SZ")
        t1 = datetime.datetime.strptime(meta["end_utc"], "%Y-%m-%dT%H:%M:%SZ")
        dur = max((t1 - t0).total_seconds(), 1e-9)
        rates = {k: round(v / dur, 6) for k, v in bt["scalars"].items()}
        result["baseline"] = {"duration_s": dur, "counts": bt["scalars"],
                              "rates_per_s": rates, "lost": bt["lost"],
                              "attached": bt["attached"]}
        with open(os.path.join(ana, "baseline.json"), "w") as f:
            json.dump(result["baseline"], f, indent=1, sort_keys=True)

    for arm_dir in sorted(glob.glob(os.path.join(raw, "pmu", "d16-*"))):
        arm = os.path.basename(arm_dir)
        result["pmu"][arm] = {
            "csv": parse_perf_csv(os.path.join(arm_dir, "perf.csv")),
            "scaled_lines": parse_perf_txt_multiplexing(os.path.join(arm_dir, "perf.txt")),
        }
    for arm_dir in sorted(glob.glob(os.path.join(raw, "alloc", "d16-*"))):
        arm = os.path.basename(arm_dir)
        bt = parse_bpftrace(os.path.join(arm_dir, "bpftrace.out"))
        result["alloc"][arm] = {"scalars": bt["scalars"], "attached": bt["attached"],
                                "lost": bt["lost"]}

    bpath = os.path.join(raw, "bucket", "results.jsonl")
    if os.path.exists(bpath):
        for line in open(bpath):
            result["bucket"].append(json.loads(line))

    for arm in ("A0", "A1", "B"):
        tv = os.path.join(raw, "resource", "time-v", f"{arm}.txt")
        st = os.path.join(raw, "resource", "strace", f"{arm}.txt")
        result["resource"][arm] = {
            "time_v": parse_time_v(tv) if os.path.exists(tv) else None,
            "strace_calls": parse_strace_c(st) if os.path.exists(st) else None,
        }

    cp = os.path.join(raw, "logs", "comm-probe.json")
    if os.path.exists(cp):
        result["comm_probe"] = json.load(open(cp))

    with open(os.path.join(ana, "mechanism-all.json"), "w") as f:
        json.dump(result, f, indent=1, sort_keys=True)
    print(f"analysis written to {ana}/mechanism-all.json")


if __name__ == "__main__":
    main()
