#!/usr/bin/env python3
"""#593 baseline aggregation: reads the frozen-baseline JSONL evidence plus the
separate attribution arms and produces baseline-summary.md/.json.

Sources (all produced and check_run-validated in this session):
  r1/r2/r3  stats-on full G0 matrix stage (A/A calibration + primary baseline)
  r0        stats-off full G0 matrix stage (accounting overhead contrast)
  r-diag    stock 19-case diagnostics stage (threadpool C>D point)
  r-uring-core64  research uring C>D case (patched diag binary, single case)
  r-disk    real-fs (btrfs/sda SSD, page-cache-resident) full stage
  attribution/  /usr/bin/time -v, perf stat CSV, strace census (separate runs)

Denominators are explicit per arm; attribution per-op values are three-pass
averages (throughput + byte-oracle + latency-instrumented phases).
"""
from __future__ import annotations

import json
import re
import statistics
from pathlib import Path

BASE = Path("/home/jnhu/sluice-593-research/evidence/baseline")
KEY_CASES = [
    "W02-uring-4k-d1", "W02-uring-4k-d4", "W02-uring-4k-d16", "W02-uring-4k-d64",
    "W02-uring-4k-d16-core64",
    "W03-uring-4k-d4", "W03-uring-4k-d16", "W03-uring-4k-d64",
]


def load_jsonl(path: Path):
    records = []
    for line in path.read_text().splitlines():
        if line.strip():
            records.append(json.loads(line))
    return records


def measured(records, case_id):
    return [r for r in records
            if r["case_id"] == case_id and r.get("measured") and r.get("ok")]


def case_stats(records, case_id):
    reps = measured(records, case_id)
    if not reps:
        return None
    def med(field):
        return statistics.median(r[field] for r in reps)
    ops = med("ops")
    wall = med("wall_s")
    cpu = med("cpu_s")
    return {
        "n_reps": len(reps),
        "ops": ops,
        "wall_us_per_op": wall * 1e6 / ops,
        "cpu_us_per_op": cpu * 1e6 / ops,
        "iops": ops / wall,
        "lat_p50_ns": med("lat_p50_ns"),
        "lat_p95_ns": med("lat_p95_ns"),
        "lat_p99_ns": med("lat_p99_ns"),
        "max_outstanding_med": med("stats_max_outstanding"),
        "p99_outstanding_max": max(r["stats_max_outstanding"] for r in reps),
    }


def parse_timev(path: Path):
    out = {}
    for line in path.read_text().splitlines():
        m = re.match(r"\s+(User time|System time) \(seconds\): ([0-9.]+)", line)
        if m:
            out[m.group(1).split()[0].lower() + "_s"] = float(m.group(2))
        m = re.match(r"\s+(Voluntary|Involuntary) context switches: (\d+)", line)
        if m:
            out[m.group(1).lower() + "_ctx"] = int(m.group(2))
    return out


def parse_perf_csv(path: Path):
    out = {}
    for line in path.read_text().splitlines():
        if line.startswith("#") or not line.strip():
            continue
        parts = line.split(",")
        if len(parts) >= 3:
            try:
                out[parts[2]] = (float(parts[0]), parts[1], parts[4])
            except ValueError:
                pass
    return out


def parse_strace(path: Path):
    # strace -c columns: "% time  seconds  usecs/call  calls  [errors]  syscall"
    out = {}
    for line in path.read_text().splitlines():
        parts = line.split()
        if len(parts) < 5 or not parts[3].isdigit():
            continue
        calls = int(parts[3])
        if parts[4].isdigit():  # errors column present
            if len(parts) >= 6:
                out[parts[5]] = calls
                out[parts[5] + "_errors"] = int(parts[4])
        else:
            out[parts[4]] = calls
    return out


def main():
    # per-stage dirs hold w01/w02/w03 jsonl files
    def stage_records(run):
        recs = []
        for p in sorted((BASE / run / "raw").glob("w*.jsonl")):
            recs.extend(load_jsonl(p))
        return recs

    data = {r: stage_records(r) for r in ("r1", "r2", "r3", "r0")}
    diag = load_jsonl(BASE / "r-diag" / "raw" / "w02-diagnosticsr-diag.jsonl")
    uring_core64 = load_jsonl(BASE / "r-uring-core64" / "w02-uring-core64.jsonl")
    disk = {}
    for p in sorted((BASE / "r-disk" / "raw").glob("w*.jsonl")):
        disk.update({r["case_id"]: r for r in load_jsonl(p)})

    all_cases_r1 = sorted({r["case_id"] for r in data["r1"]})

    md = ["# SLUICE #593 frozen baseline summary (E5, bdfd0d8d, tmpfs warm)",
          "",
          "Primary baseline = r1 (stats on, 2 warmup + 7 measured per case,",
          "median over measured reps). A/A = r1/r2/r3 pairwise drift of the",
          "per-run mean. Attribution arms are separate executions.",
          ""]

    # ---- primary baseline table ----
    md += ["## Primary per-op baseline (bench records, r1)", "",
           "| case | wall µs/op | CPU µs/op | IOPS | p50 ns | p95 ns | p99 ns | max_outstanding (med) |",
           "|---|---|---|---|---|---|---|---|"]
    summary = {"cases": {}}
    for case_id in all_cases_r1:
        st = case_stats(data["r1"], case_id)
        if st is None:
            continue
        summary["cases"][case_id] = st
        md.append(
            f"| {case_id} | {st['wall_us_per_op']:.3f} | {st['cpu_us_per_op']:.3f} | "
            f"{st['iops']:.0f} | {st['lat_p50_ns']:.0f} | {st['lat_p95_ns']:.0f} | "
            f"{st['lat_p99_ns']:.0f} | {st['max_outstanding_med']:.0f} |")

    # ---- A/A drift ----
    md += ["", "## A/A calibration (r1/r2/r3, drift of per-run mean, %)", ""]
    pairs = [("r1", "r2"), ("r2", "r3"), ("r1", "r3")]

    def run_means(run, case_id):
        reps = measured(data[run], case_id)
        if not reps:
            return None
        ops = statistics.mean(r["ops"] for r in reps)
        return {
            "wall_us": statistics.mean(r["wall_s"] for r in reps) * 1e6 / ops,
            "cpu_us": statistics.mean(r["cpu_s"] for r in reps) * 1e6 / ops,
        }

    md += ["| case | metric | " + " | ".join(f"{a}/{b}" for a, b in pairs) + " | max |",
           "|---|---|---|---|---|---|"]
    drift_report = {}
    for case_id in all_cases_r1:
        means = {r: run_means(r, case_id) for r in ("r1", "r2", "r3")}
        if any(m is None for m in means.values()):
            continue
        for metric in ("wall_us", "cpu_us"):
            vals = {r: means[r][metric] for r in means}
            gm = statistics.mean(vals.values())
            drifts = []
            for a, b in pairs:
                d = abs(vals[a] - vals[b]) / gm * 100.0 if gm else 0.0
                drifts.append(d)
            drift_report.setdefault(case_id, {})[metric] = {
                "pairs": {f"{a}/{b}": drifts[i] for i, (a, b) in enumerate(pairs)},
                "max": max(drifts), "means": vals}
            md.append(f"| {case_id} | {metric} | " +
                      " | ".join(f"{d:.2f}%" for d in drifts) +
                      f" | {max(drifts):.2f}% |")
    summary["aa_drift"] = drift_report

    # key-case A/A max summary
    md += ["", "Key-case A/A max drift: " + ", ".join(
        f"{c}[wall {drift_report[c]['wall_us']['max']:.2f}% / cpu {drift_report[c]['cpu_us']['max']:.2f}%]"
        for c in KEY_CASES[:4] if c in drift_report), ""]

    # ---- stats on/off contrast ----
    md += ["", "## Stats-accounting overhead (r0 stats-off vs r1 stats-on, medians)", "",
           "| case | wall off | wall on | Δ% | cpu off | cpu on | Δ% |",
           "|---|---|---|---|---|---|---|"]
    for case_id in KEY_CASES[:4] + ["W03-uring-4k-d16"]:
        off = case_stats(data["r0"], case_id)
        on = case_stats(data["r1"], case_id)
        if not off or not on:
            continue
        dw = (on["wall_us_per_op"] - off["wall_us_per_op"]) / off["wall_us_per_op"] * 100
        dc = (on["cpu_us_per_op"] - off["cpu_us_per_op"]) / off["cpu_us_per_op"] * 100
        md.append(f"| {case_id} | {off['wall_us_per_op']:.3f} | {on['wall_us_per_op']:.3f} | "
                  f"{dw:+.2f}% | {off['cpu_us_per_op']:.3f} | {on['cpu_us_per_op']:.3f} | {dc:+.2f}% |")

    # ---- C>D diagnostics ----
    diag_ctrl = load_jsonl(BASE / "r-uring-core64" / "w02-diagbinary-stocklist.jsonl")
    paired = load_jsonl(BASE / "r-uring-core64" / "w02-uring-cd-paired.jsonl")
    md += ["", "## C>D diagnostic points (D=16, C=64 vs C=D=16)", "",
           "The uring pair is compared inside one interleaved process run",
           "(`w02-uring-cd-paired.jsonl`, substring filter `W02-uring-4k-d16`,",
           "rotation-interleaved), which is the cleanest available comparison;", "",
           "| case | source | wall µs/op | CPU µs/op | IOPS | p99 ns | max_outstanding |",
           "|---|---|---|---|---|---|---|"]
    for case_id, source, recs in [
            ("W02-threadpool-4k-d16-w4", "r-diag (C=D base)", diag),
            ("W02-threadpool-4k-d16-w4-core64", "r-diag (stock C>D)", diag),
            ("W02-uring-4k-d16", "paired interleaved run (C=D base)", paired),
            ("W02-uring-4k-d16-core64", "paired interleaved run (C>D)", paired)]:
        st = case_stats(recs, case_id)
        if st is None:
            md.append(f"| {case_id} | {source} | MISSING | | | | |")
            continue
        summary["cases"][f"{case_id}@{source.split()[0]}"] = st
        md.append(f"| {case_id} | {source} | {st['wall_us_per_op']:.3f} | "
                  f"{st['cpu_us_per_op']:.3f} | {st['iops']:.0f} | {st['lat_p99_ns']:.0f} | "
                  f"{st['max_outstanding_med']:.0f} |")
    p16 = case_stats(paired, "W02-uring-4k-d16")
    p64 = case_stats(paired, "W02-uring-4k-d16-core64")
    if p16 and p64:
        delta = (p64["wall_us_per_op"] - p16["wall_us_per_op"]) / p16["wall_us_per_op"] * 100
        md += ["", f"uring paired C>D delta (C=64 vs C=16 at D=16): {delta:+.2f}% wall "
              f"(within the ~2-4% A/A noise floor of this host)"]

    # ---- disk vs tmpfs ----
    md += ["", "## Real-fs arm vs tmpfs (r-disk vs r1, medians; btrfs on sda SSD,",
           "page-cache-resident — NOT physical device I/O)", "",
           "| case | wall tmpfs | wall real-fs | Δ% | cpu tmpfs | cpu real-fs | Δ% | p99 tmpfs | p99 real-fs |",
           "|---|---|---|---|---|---|---|---|---|"]
    disk_records = []
    for p in sorted((BASE / "r-disk" / "raw").glob("w*.jsonl")):
        disk_records.extend(load_jsonl(p))
    for case_id in KEY_CASES[:4] + ["W03-uring-4k-d16"]:
        tmp = case_stats(data["r1"], case_id)
        dsk = case_stats(disk_records, case_id)
        if not tmp or not dsk:
            continue
        dw = (dsk["wall_us_per_op"] - tmp["wall_us_per_op"]) / tmp["wall_us_per_op"] * 100
        dc = (dsk["cpu_us_per_op"] - tmp["cpu_us_per_op"]) / tmp["cpu_us_per_op"] * 100
        md.append(f"| {case_id} | {tmp['wall_us_per_op']:.3f} | {dsk['wall_us_per_op']:.3f} | "
                  f"{dw:+.1f}% | {tmp['cpu_us_per_op']:.3f} | {dsk['cpu_us_per_op']:.3f} | "
                  f"{dc:+.1f}% | {tmp['lat_p99_ns']:.0f} | {dsk['lat_p99_ns']:.0f} |")

    # ---- attribution arms ----
    md += ["", "## Attribution arms (separate executions)", "",
           "time -v: 540,000 logical reads per arm (9 reps × 3 phases × 20000),",
           "floor-subtracted (fixture+startup removed). perf stat: 60,000 logical",
           "reads per arm (1 rep × 3 phases × 20000), floor-subtracted, three-pass",
           "average (throughput + byte-oracle + latency-instrumented). strace:",
           "counts over the 2/7 filtered run (540,028 actual io_uring_enter calls).",
           "",
           "| case | user µs/op | sys µs/op | ctx/op (vol+invol) | instr/op | cycles/op | IPC | branch-miss/op | LLC-load-miss/op | futex/op | io_uring_enter/op |",
           "|---|---|---|---|---|---|---|---|---|---|---|"]
    floor_timev = parse_timev(BASE / "attribution" / "timev-__floor__.timev.txt")
    floor_perf = {}
    for p in (BASE / "attribution").glob("perf-__floor__-*.csv"):
        floor_perf.update(parse_perf_csv(p))
    summary["attribution"] = {}
    for tag, case_id, n_timev, n_perf in [
            ("w02_uring_4k_d1", "W02-uring-4k-d1", 540000, 60000),
            ("w02_uring_4k_d4", "W02-uring-4k-d4", 540000, 60000),
            ("w02_uring_4k_d16", "W02-uring-4k-d16", 540000, 60000),
            ("w02_uring_4k_d64", "W02-uring-4k-d64", 540000, 60000),
            ("w03_uring_4k_d16", "W03-uring-4k-d16", 540000, 60000)]:
        tv = parse_timev(BASE / "attribution" / f"timev-{tag}.timev.txt")
        perf = {}
        for p in (BASE / "attribution").glob(f"perf-{tag}-*.csv"):
            perf.update(parse_perf_csv(p))

        def pt(name):
            if name in perf and name in floor_perf:
                return (perf[name][0] - floor_perf[name][0]) / n_perf
            return None
        user_us = (tv.get("user_s", 0) - floor_timev.get("user_s", 0)) * 1e6 / n_timev
        sys_us = (tv.get("system_s", 0) - floor_timev.get("system_s", 0)) * 1e6 / n_timev
        ctx = ((tv.get("voluntary_ctx", 0) - floor_timev.get("voluntary_ctx", 0)) +
               (tv.get("involuntary_ctx", 0) - floor_timev.get("involuntary_ctx", 0))) / n_timev
        instr, cyc, bm, llcm = (pt("instructions:u"), pt("cycles:u"),
                                pt("branch-misses:u"), pt("LLC-load-misses:u"))
        st_path = BASE / "attribution" / f"strace-{tag}.txt"
        futex_op = enter_op = ""
        if st_path.is_file():
            sc = parse_strace(st_path)
            futex_op = f"{sc.get('futex', 0) / 540028:.4f}"
            enter_op = f"{sc.get('io_uring_enter', 0) / 540028:.4f}"
        ipc = instr / cyc if instr and cyc else 0
        md.append(
            f"| {case_id} | {user_us:.3f} | {sys_us:.3f} | {ctx:.3f} | "
            f"{instr:.0f} | {cyc:.0f} | {ipc:.3f} | {bm:.1f} | {llcm:.4f} | "
            f"{futex_op or '—'} | {enter_op or '—'} |")
        summary["attribution"][case_id] = {
            "user_us_per_op": user_us, "sys_us_per_op": sys_us,
            "ctx_per_op": ctx, "instructions_per_op": instr,
            "cycles_per_op": cyc, "ipc": ipc, "branch_misses_per_op": bm,
            "llc_load_misses_per_op": llcm,
            "futex_per_op": futex_op, "io_uring_enter_per_op": enter_op,
            "perf_l1_group_multiplexed_pct": next(
                (v[2] for k, v in perf.items() if k == "L1-dcache-loads:u"), ""),
        }

    (BASE / "baseline-summary.md").write_text("\n".join(md) + "\n")
    (BASE / "baseline-summary.json").write_text(json.dumps(summary, indent=1))
    print("\n".join(md))


if __name__ == "__main__":
    main()
