#!/usr/bin/env python3
"""Parse perf callgraph traces: header stats, self sample counts, first-level callers."""

import json
import os
import re
import subprocess
import sys

SYMBOL_RE = re.compile(r"^\s*([0-9.]+)%\s+(\d+)\s+(\[\S*\]\s+)?(.*?)\s*$")
CALLER_RE = re.compile(r"^ {12}\|--([0-9.]+)%--(.+?)\s*$")
SINGLE_CALLER_RE = re.compile(r"^ {12}---(.+?)\s*$")
HEADER_SAMPLES_RE = re.compile(r"^# Samples:\s+(\S+)")
HEADER_EVENT_RE = re.compile(r"^# Event count \(approx\.\):\s+(\d+)")
LOST_RE = re.compile(r"^# Total Lost Samples:\s+(\d+)")


def report(path, extra=None):
    cmd = ["perf", "report", "-i", path, "--stdio", "--no-children",
           "--sort", "symbol", "--show-nr-samples", "--percent-limit", "0"]
    if extra:
        cmd = extra
    return subprocess.run(cmd, capture_output=True, text=True).stdout


def parse_file(path):
    text = report(path)
    out = {"samples": None, "event_count": None, "lost": None,
           "symbols": {}, "callers": {}}
    current = None
    for line in text.splitlines():
        m = HEADER_SAMPLES_RE.match(line)
        if m:
            raw = m.group(1)
            out["samples"] = int(raw.replace("K", "000")) if raw.endswith("K") else int(raw)
            continue
        m = HEADER_EVENT_RE.match(line)
        if m:
            out["event_count"] = int(m.group(1))
            continue
        m = LOST_RE.match(line)
        if m:
            out["lost"] = int(m.group(1))
            continue
        if "|" in line and "%" in line:
            m = CALLER_RE.match(line)
            if m and current:
                pct = float(m.group(1))
                name = m.group(2)
                out["callers"].setdefault(current, []).append((pct, name))
            continue
        if "|" in line:
            continue
        m = SYMBOL_RE.match(line)
        if m:
            pct = float(m.group(1))
            count = int(m.group(2))
            sym = m.group(4).strip()
            if sym and not sym.startswith("Symbol"):
                out["symbols"][sym] = out["symbols"].get(sym, 0) + count
                current = sym
            continue
        m = SINGLE_CALLER_RE.match(line)
        if m and current:
            out["callers"].setdefault(current, []).append((None, m.group(1)))
    exact = sum(out["symbols"].values())
    if exact:
        out["samples"] = exact
    err_path = path.replace(".data", ".err")
    if os.path.exists(err_path):
        text = open(err_path).read()
        m = re.search(r"\((\d+) samples\)", text)
        if m:
            out["samples"] = int(m.group(1))
    return out


def load_runs(dirname):
    runs = []
    for name in sorted(os.listdir(dirname)):
        if not name.endswith(".data"):
            continue
        arm = name.split("-")[1]
        ops = 400000
        jl = os.path.join(dirname, name.replace("perf-", "").replace(".data", ".jsonl"))
        wall = None
        if os.path.exists(jl):
            measured = [json.loads(l) for l in open(jl) if l.strip()]
            measured = [r for r in measured if r.get("measured")]
            wall = sum(r["wall_s"] for r in measured) if measured else None
        parsed = parse_file(os.path.join(dirname, name))
        parsed["symbol_sum"] = sum(parsed["symbols"].values())
        parsed.update({"file": name, "arm": arm, "ops": ops, "wall_s": wall})
        runs.append(parsed)
    return runs


def main():
    dirname = sys.argv[1] if len(sys.argv) > 1 else "."
    runs = load_runs(dirname)
    for run in runs:
        rate = run["samples"] / run["wall_s"] if run["wall_s"] else None
        print("%s arm=%s samples=%d event=%d lost=%s wall=%.3fs rate=%.0f/s" % (
            run["file"], run["arm"], run["samples"], run["event_count"],
            run["lost"], run["wall_s"], rate))
    print()

    interest = [
        "sluice::async::UringAsyncBackend::dispatch_after_accept",
        "sluice::async::UringAsyncBackend::defer_transport_flush_locked_",
        "sluice::async::UringAsyncBackend::poll",
        "sluice::async::UringAsyncBackend::dispatch_one_locked",
        "sluice::async::UringAsyncBackend::submit_transport_locked",
        "sluice::async::UringAsyncBackend::account_transport_result_locked",
        "sluice::async::AsyncIoContext::wait_one",
        "sluice::async::AsyncIoContext::run_progress_pass_",
        "pthread_mutex_lock@@GLIBC_2.2.5",
        "__GI___pthread_mutex_unlock_usercnt",
        "__GI___libc_write",
        "io_uring_submit",
        "sluice::async::detail::RequestCore::outstanding_summary",
    ]

    def match(sym, needle):
        return sym.startswith(needle) or needle in sym

    print("%-62s %10s %10s %10s" % ("symbol (self, cycles/op)", "A mean", "B mean", "B-A"))
    for needle in interest:
        a_vals, b_vals = [], []
        for run in runs:
            cycles = run["event_count"] / run["ops"]
            count = 0
            for sym, n in run["symbols"].items():
                if match(sym, needle):
                    count += n
            val = count / run["samples"] * run["event_count"] / run["ops"]
            (a_vals if run["arm"] == "A" else b_vals).append(val)
        if not a_vals or not b_vals:
            continue
        am = sum(a_vals) / len(a_vals)
        bm = sum(b_vals) / len(b_vals)
        print("%-62s %10.1f %10.1f %+10.1f" % (
            needle[:62], am, bm, bm - am))
        print("    A runs: %s" % ", ".join("%.1f" % v for v in a_vals))
        print("    B runs: %s" % ", ".join("%.1f" % v for v in b_vals))

    print("\nfirst-level caller evidence (pct of all samples):")
    for run in runs:
        hits = []
        for sym, callers in run["callers"].items():
            if "pthread_mutex_lock" not in sym and "std::mutex::lock" not in sym:
                continue
            for pct, name in callers:
                if "dispatch_after_accept" in name or "std::mutex::lock" in name:
                    hits.append((sym.split("@@")[0][:30], name[:60], pct))
        if hits:
            print("  %s:" % run["file"])
            for h in hits:
                print("    %-30s <- %-60s %s" % h)


if __name__ == "__main__":
    main()
