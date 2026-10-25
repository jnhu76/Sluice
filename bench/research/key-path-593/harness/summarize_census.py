#!/usr/bin/env python3
"""Summarize K2-a census runs (strace counts, /usr/bin/time -v, perf stat).

Each benchmark repetition runs three phases (throughput + oracle + latency),
but emits one record carrying the throughput phase's ops.  The logical op
denominator for a 0/1 census run is therefore 3 * sum(record ops).
"""
from __future__ import annotations

import json
import re
import statistics
from pathlib import Path

ROOT = Path(__file__).resolve().parent / "census"
TRACE_RE = re.compile(r"^\s*[\d.]+\s+[\d.]+\s+\d+\s+(\d+)\s+(?:(\d+)\s+)?(\S+)\s*$")


def read_strace(path: Path):
    counts = {}
    if not path.is_file():
        return counts
    for line in path.read_text().splitlines():
        if line.startswith("-") or line.startswith("%"):
            continue
        m = TRACE_RE.match(line)
        if m:
            name = m.group(3)
            if name != "total":
                counts[name] = counts.get(name, 0) + int(m.group(1))
    return counts


def read_time(path: Path):
    out = {}
    if not path.is_file():
        return out
    for line in path.read_text().splitlines():
        if ":" not in line:
            continue
        key, _, value = line.strip().partition(":")
        key, value = key.strip(), value.strip()
        if key == "Elapsed (wall clock) time (h:mm:ss or m:ss)":
            parts = value.split(":")
            try:
                if len(parts) == 3:
                    out["elapsed_s"] = int(parts[0]) * 3600 + int(parts[1]) * 60 + float(parts[2])
                elif len(parts) == 2:
                    out["elapsed_s"] = int(parts[0]) * 60 + float(parts[1])
            except ValueError:
                pass
        elif key in ("User time (seconds)", "System time (seconds)",
                     "Voluntary context switches", "Involuntary context switches",
                     "Maximum resident set size (kbytes)"):
            try:
                out[key] = float(value)
            except ValueError:
                pass
    return out


def read_perf(path: Path):
    out = {}
    if not path.is_file():
        return out
    for line in path.read_text().splitlines():
        parts = line.split(",")
        if len(parts) < 3:
            continue
        try:
            out[parts[2]] = float(parts[0])
        except ValueError:
            continue
    return out


def read_jsonl(path: Path):
    if not path.is_file():
        return None
    rows = [json.loads(line) for line in path.read_text().splitlines() if line.strip()]
    work = [r for r in rows if not r["case_id"].startswith("CAL-")]
    if not work:
        return None
    last = work[-1]
    return {
        "record_ops": sum(x["ops"] for x in work),
        "logical_ops": 3 * sum(x["ops"] for x in work),
        "stats_poll_calls": last["stats_poll_calls"],
        "stats_wait_calls": last["stats_wait_calls"],
        "stats_submit_calls": last["stats_submit_calls"],
        "waits": last["waits"],
        "submits": last["submits"],
        "notifications": last["notifications"],
        "passes": last["passes"],
        "empty_wakes": last["empty_wakes"],
        "queue_full_retries": last["stats_queue_full_retries"],
        "max_outstanding": last["stats_max_outstanding"],
        "ok": all(x["ok"] for x in work),
    }


def median(rows, key):
    vals = [r[key] for r in rows if r.get(key) is not None]
    return statistics.median(vals) if vals else None


def main() -> int:
    rows = []
    for d in sorted(p for p in ROOT.iterdir() if p.is_dir()):
        for kind in ("time", "strace", "perf"):
            for p in sorted(d.glob(f"*-{kind}-*.jsonl")):
                arm = p.name.split("-")[0]
                rep = p.stem.rsplit("-", 1)[-1]
                base = p.with_suffix("")
                bench = read_jsonl(p)
                if bench is None:
                    continue
                trace = read_strace(base.with_suffix(".strace.txt")) if kind == "strace" else {}
                t = read_time(base.with_suffix(".time.txt")) if kind == "time" else {}
                perf = read_perf(base.with_suffix(".perf.txt")) if kind == "perf" else {}
                rows.append({
                    "case": d.name, "kind": kind, "arm": arm, "rep": int(rep),
                    "ops": bench["logical_ops"], "bench": bench,
                    "enters": trace.get("io_uring_enter"),
                    "read": trace.get("read"),
                    "write": trace.get("write"),
                    "futex": trace.get("futex"),
                    "epoll_wait": (trace.get("epoll_wait", 0) + trace.get("epoll_pwait", 0)
                                   + trace.get("poll", 0) + trace.get("ppoll", 0))
                    if any(k in trace for k in ("epoll_wait", "epoll_pwait", "poll", "ppoll"))
                    else None,
                    "vctx": t.get("Voluntary context switches"),
                    "ictx": t.get("Involuntary context switches"),
                    "user_s": t.get("User time (seconds)"),
                    "sys_s": t.get("System time (seconds)"),
                    "elapsed_s": t.get("elapsed_s"),
                    "max_rss_kb": t.get("Maximum resident set size (kbytes)"),
                    "cycles_u": perf.get("cycles:u"),
                    "instructions_u": perf.get("instructions:u"),
                    "cache_refs_u": perf.get("cache-references:u"),
                    "cache_misses_u": perf.get("cache-misses:u"),
                    "l1_loads_u": perf.get("L1-dcache-loads:u"),
                    "l1_misses_u": perf.get("L1-dcache-load-misses:u"),
                    "llc_loads_u": perf.get("LLC-loads:u"),
                    "llc_misses_u": perf.get("LLC-load-misses:u"),
                })
    (ROOT / "census.json").write_text(json.dumps(rows, indent=2) + "\n")

    def val(v, digits=4):
        if v is None:
            return ""
        return f"{v:.{digits}f}" if isinstance(v, float) else str(v)

    strace_lines = ["case arm rep ops enters enters/op read write futex epoll_wait"]
    time_lines = ["case arm rep ops vctx ictx user_s sys_s elapsed_s max_rss_kb "
                  "bench_poll bench_wait bench_notif bench_passes"]
    perf_lines = ["case arm rep ops cycles_u instr_u cache_refs_u cache_miss_u "
                  "l1_load_u l1_miss_u llc_load_u llc_miss_u"]
    for r in rows:
        if r["kind"] == "strace":
            strace_lines.append(" ".join([
                r["case"], r["arm"], str(r["rep"]), str(r["ops"]),
                val(r["enters"]), val(r["enters"] / r["ops"], 5),
                val(r["read"]), val(r["write"]), val(r["futex"]), val(r["epoll_wait"]),
            ]))
        elif r["kind"] == "time":
            b = r["bench"]
            time_lines.append(" ".join([
                r["case"], r["arm"], str(r["rep"]), str(r["ops"]),
                val(r["vctx"], 0), val(r["ictx"], 0), val(r["user_s"], 3), val(r["sys_s"], 3),
                val(r["elapsed_s"], 3), val(r["max_rss_kb"], 0),
                str(b["stats_poll_calls"]), str(b["stats_wait_calls"]),
                str(b["notifications"]), str(b["passes"]),
            ]))
        elif r["kind"] == "perf":
            perf_lines.append(" ".join([
                r["case"], r["arm"], str(r["rep"]), str(r["ops"]),
                val(r["cycles_u"], 0), val(r["instructions_u"], 0),
                val(r["cache_refs_u"], 0), val(r["cache_misses_u"], 0),
                val(r["l1_loads_u"], 0), val(r["l1_misses_u"], 0),
                val(r["llc_loads_u"], 0), val(r["llc_misses_u"], 0),
            ]))
    (ROOT / "census-strace.tsv").write_text("\n".join(strace_lines) + "\n")
    (ROOT / "census-time.tsv").write_text("\n".join(time_lines) + "\n")
    (ROOT / "census-perf.tsv").write_text("\n".join(perf_lines) + "\n")

    summary = ["# K2-a census medians (0 warmup / 1 measured; logical ops = 3x record ops)",
               "case arm reps enters/op vctx ictx user_s sys_s cycles_u instr_u "
               "cache_refs_u cache_miss_u l1_miss_u llc_miss_u poll/op notif/op"]
    cases = sorted({r["case"] for r in rows})
    for case in cases:
        for arm in ("base", "var"):
            st = [r for r in rows if r["case"] == case and r["arm"] == arm and r["kind"] == "strace"]
            ti = [r for r in rows if r["case"] == case and r["arm"] == arm and r["kind"] == "time"]
            pf = [r for r in rows if r["case"] == case and r["arm"] == arm and r["kind"] == "perf"]
            if not st or not ti or not pf:
                continue
            ops = st[0]["ops"]
            poll = ti[0]["bench"]["stats_poll_calls"] / ops
            notif = ti[0]["bench"]["notifications"] / ops
            summary.append(" ".join([
                case, arm, str(len(st)),
                val(median(st, "enters") / ops, 5),
                val(median(ti, "vctx"), 0), val(median(ti, "ictx"), 0),
                val(median(ti, "user_s"), 3), val(median(ti, "sys_s"), 3),
                val(median(pf, "cycles_u"), 0), val(median(pf, "instructions_u"), 0),
                val(median(pf, "cache_refs_u"), 0), val(median(pf, "cache_misses_u"), 0),
                val(median(pf, "l1_misses_u"), 0), val(median(pf, "llc_misses_u"), 0),
                val(poll, 5), val(notif, 5),
            ]))
    (ROOT / "census-summary.txt").write_text("\n".join(summary) + "\n")
    print("wrote census.json, census-strace.tsv, census-time.tsv, census-perf.tsv, census-summary.txt")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
