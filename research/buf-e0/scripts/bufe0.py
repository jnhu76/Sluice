#!/usr/bin/env python3
"""BUF-E0 (#263) session driver.

Runs the frozen BUF-E0 matrix (research/buf-e0/BUF-E0-PREREGISTRATION.md):
  phases A/B/C/D x arms b0/b1/b2/b3 x sizes {4K,64K,1M} x slots {1,8,32,128}
each cell-arm-phase as an R7/R14 process pair wrapped in
  perf stat -x, -e instructions:u,cycles:u
with per-op attribution by double-difference
  (total(R14) - total(R7)) / 7 / ops_per_rep
(the TAX-0-line normalization) plus in-process wall/fault statistics from the
bench JSON (median/MAD over reps, per-region fault attribution).

Fail-closed same-work gates per cell/phase: identical ops/bytes/kc across
arms and R-pairs; all_reps_ok everywhere; exit code 0. Exit 4 from the bench
records `SKIPPED - MEMORY BUDGET`.

Sessions are immutable: written once under
research/buf-e0/results/<session-id>/, never edited afterwards.

Usage:
  python3 research/buf-e0/scripts/bufe0.py run --session bufe0-micro-wsl2-1
  python3 research/buf-e0/scripts/bufe0.py report --session bufe0-micro-wsl2-1
  python3 research/buf-e0/scripts/bufe0.py arena --session bufe0-arena-wsl2-1
"""

from __future__ import annotations

import argparse
import csv
import datetime
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
RESULTS = REPO / "research/buf-e0/results"
DATA_DIR = REPO / "build/bufe0-data"
DATA_FILE = DATA_DIR / "src.bin"
DATA_BYTES = 256 * 1024 * 1024
BENCH = REPO / "build/linux/x86_64/release/buf_e0_bench"

PHASES = ["A", "B", "D", "C"]  # deterministic run order within a cell
ARMS = ["b0", "b1", "b2", "b3"]
SIZES = [4096, 65536, 1048576]
SLOTS = [1, 8, 32, 128]
REP_LO, REP_HI = 7, 14  # R7/R14 double-difference pair
PERF_EVENTS = ["instructions:u", "cycles:u"]
CELL_MEMORY_BUDGET = 512 * 1024 * 1024  # prereg §3


# ---------------------------------------------------------------------------
# Environment / session plumbing
# ---------------------------------------------------------------------------

def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def git_state() -> dict:
    def g(*args: str) -> str:
        return subprocess.run(["git", *args], cwd=REPO, capture_output=True,
                              text=True, check=True).stdout.strip()
    return {
        "head": g("rev-parse", "HEAD"),
        "branch": g("rev-parse", "--abbrev-ref", "HEAD"),
        "dirty": bool(g("status", "--porcelain")),
    }


def wsl2() -> bool:
    try:
        return "microsoft" in Path("/proc/version").read_text().lower()
    except OSError:
        return False


def cpu_model() -> str | None:
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return None


def environment_json() -> dict:
    st = os.statvfs(DATA_DIR) if DATA_DIR.exists() else None
    return {
        "timestamp_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "git": git_state(),
        "system": {
            "kernel": platform.release(),
            "wsl": wsl2(),
            "logical_cpus": os.cpu_count(),
            "cpu_model": cpu_model(),
            "mem_total_kb": int(
                Path("/proc/meminfo").read_text().split("MemTotal:")[1]
                .split("kB")[0].strip()),
            "page_size": 4096,
            "thp_enabled": Path(
                "/sys/kernel/mm/transparent_hugepage/enabled").read_text()
            .strip() if Path(
                "/sys/kernel/mm/transparent_hugepage/enabled").exists()
            else None,
            "perf_event_paranoid": Path("/proc/sys/kernel/perf_event_paranoid")
            .read_text().strip(),
        },
        "filesystem": {
            "data_dir": str(DATA_DIR),
            "data_dir_fs": "ext4 (WSL2 virtual block device)" if wsl2()
            else "native",
            "data_bytes": DATA_BYTES,
            "cache_regime": "warm (untimed warm sweep before formal reps)",
        },
        "virtualization": {
            "wsl2": wsl2(),
            "classification": "QUALIFIED_BUT_VIRTUALIZED",
            "claim_limit": ("ENVIRONMENT-LIMITED: allocation/initialization/"
                            "fault-shifting attribution and same-host causal "
                            "comparison only; no native NVMe/NUMA/TLB claims"),
        },
        "tools": {
            "perf": subprocess.run(["perf", "--version"], capture_output=True,
                                   text=True).stdout.strip(),
            "python": platform.python_version(),
        },
        "build": {
            "mode": "release",
            "bench_binary": str(BENCH),
            "bench_binary_sha256": sha256_file(BENCH),
        },
        "protocol": {
            "perf_events": PERF_EVENTS,
            "rep_pair": [REP_LO, REP_HI],
            "normalization": ("double-difference (total(R14)-total(R7))/7"
                              "/ops_per_rep"),
            "fault_source": "in-process getrusage ru_minflt (per region)",
            "run_order": "cells in matrix order; phases A,B,D,C; arms "
                         "b0,b1,b2,b3; R7 then R14 per pair",
            "memory_budget_per_cell": CELL_MEMORY_BUDGET,
        },
    }


def manifest_json() -> dict:
    return {
        "purpose": "BUF-E0 microbench formal session (#263 Phase 2)",
        "preregistration": "research/buf-e0/BUF-E0-PREREGISTRATION.md",
        "matrix": {
            "phases": PHASES,
            "arms": ARMS,
            "sizes": SIZES,
            "slots": SLOTS,
            "rep_pair": [REP_LO, REP_HI],
            "kc_formula": "clamp(256MiB/(slots*size), 1, 16) (phase C)",
        },
        "cells": [[s, d] for s in SIZES for d in SLOTS],
    }


def parse_perf_stat(stderr_text: str) -> dict:
    counters = {}
    for line in stderr_text.splitlines():
        parts = line.split(",")
        if len(parts) >= 3 and parts[0]:
            key = parts[2].split(":")[0]
            try:
                counters[key] = float(parts[0])
            except ValueError:
                pass
    return counters


def median(vals):
    vs = sorted(vals)
    n = len(vs)
    if n == 0:
        return None
    return vs[n // 2] if n % 2 else (vs[n // 2 - 1] + vs[n // 2]) / 2


def mad(vals, med):
    if med is None:
        return None
    return median([abs(v - med) for v in vals])


# ---------------------------------------------------------------------------
# Matrix execution
# ---------------------------------------------------------------------------

def bench_cmd(phase, arm, size, slots, reps, regime="pinned"):
    return [str(BENCH), "--phase", phase, "--arm", arm,
            "--size", str(size), "--slots", str(slots), "--reps", str(reps),
            "--regime", regime, "--file", str(DATA_FILE)]


def run_one(phase, arm, size, slots, reps, regime, raw_dir: Path):
    tag = f"{phase}-{size//1024}k-s{slots}-{arm}-{regime[:3]}-R{reps}"
    cmd = bench_cmd(phase, arm, size, slots, reps, regime)
    full = ["perf", "stat", "-x,", "-e", ",".join(PERF_EVENTS), "--"] + cmd
    env = dict(os.environ, LC_ALL="C")
    proc = subprocess.run(full, capture_output=True, text=True, env=env,
                          cwd=REPO)
    (raw_dir / f"{tag}.perf.txt").write_text(proc.stderr)
    bench = None
    if proc.stdout.strip():
        try:
            # whole stdout is the JSON document (buf_e0_bench emits
            # multi-line; nothing else writes to stdout)
            bench = json.loads(proc.stdout)
        except ValueError:
            try:  # fall back to last-line JSON (tax0 single-line convention)
                bench = json.loads(proc.stdout.strip().splitlines()[-1])
            except (ValueError, IndexError):
                bench = None
    if bench is not None:
        (raw_dir / f"{tag}.bench.json").write_text(
            json.dumps(bench, indent=1) + "\n")
    return {
        "tag": tag,
        "phase": phase, "arm": arm, "size": size, "slots": slots,
        "reps": reps, "regime": regime,
        "cmd": cmd,
        "returncode": proc.returncode,
        "perf_counters": parse_perf_stat(proc.stderr),
        "bench": bench,
    }


def ops_per_rep(rec) -> int | None:
    """Denominator unit per rep (prereg §6): A/D/B = slots; C = slots*kc."""
    b = rec["bench"]
    if b is None:
        return None
    if rec["phase"] == "C":
        return rec["slots"] * b.get("kc", 0)
    return rec["slots"]


def run_matrix(session_dir: Path, phases, arms, sizes, slots_list, regime):
    raw_dir = session_dir / "raw"
    raw_dir.mkdir(parents=True, exist_ok=True)
    records = []
    skips = []
    for size in sizes:
        for slots in slots_list:
            if size * slots > CELL_MEMORY_BUDGET:
                skips.append({"size": size, "slots": slots,
                              "reason": "SKIPPED - MEMORY BUDGET"})
                continue
            for phase in phases:
                for arm in arms:
                    pair = {}
                    for reps in (REP_LO, REP_HI):
                        rec = run_one(phase, arm, size, slots, reps, regime,
                                      raw_dir)
                        if rec["returncode"] == 4:
                            skips.append({"size": size, "slots": slots,
                                          "phase": phase, "arm": arm,
                                          "reason": "SKIPPED - MEMORY BUDGET"})
                            pair = {}
                            break
                        pair[reps] = rec
                        records.append(rec)
                        if rec["returncode"] != 0 or rec["bench"] is None:
                            print(f"FATAL: {rec['tag']} rc={rec['returncode']}",
                                  file=sys.stderr)
                            sys.exit(3)
                    if not pair:
                        continue
    return records, skips


# ---------------------------------------------------------------------------
# Same-work gates + aggregation
# ---------------------------------------------------------------------------

def same_work_gate(records) -> list[str]:
    errors = []
    groups = {}
    for r in records:
        b = r["bench"]
        if b is None:
            errors.append(f"{r['tag']}: no bench JSON")
            continue
        if r["returncode"] != 0:
            errors.append(f"{r['tag']}: rc={r['returncode']}")
        if not b.get("all_reps_ok", False):
            errors.append(f"{r['tag']}: all_reps_ok false")
        key = (r["phase"], r["size"], r["slots"], r["regime"])
        groups.setdefault(key, []).append(r)
    for key, recs in sorted(groups.items()):
        phase = key[0]
        # ops normalize per rep: R7 and R14 processes differ in total ops by
        # design; the per-rep unit must be identical across arms and R-pairs
        per_rep = {x["bench"]["same_work"]["ops"] // x["reps"] for x in recs}
        byts = {x["bench"]["same_work"]["bytes"] // x["reps"] for x in recs}
        if len(per_rep) != 1:
            errors.append(f"{key}: ops/rep differs across runs: {per_rep}")
        if len(byts) != 1:
            errors.append(f"{key}: bytes/rep differs: {byts}")
        if phase == "C":
            kcs = {x["bench"].get("kc") for x in recs}
            wins = {x["bench"].get("window_bytes") for x in recs}
            if len(kcs) != 1 or len(wins) != 1:
                errors.append(f"{key}: kc/window differ: {kcs} {wins}")
            expected_ops = None
            for x in recs:
                kc = x["bench"].get("kc") or 0
                if expected_ops is None:
                    expected_ops = x["slots"] * kc
                elif x["bench"]["same_work"]["ops"] // x["reps"] != expected_ops:
                    errors.append(f"{key}: phase C ops/rep != slots*kc")
                break
    return errors


def per_rep_field(rec, field):
    return [r[field] for r in rec["bench"]["reps_detail"]]


def summarize(records):
    """Aggregate cell×phase×arm rows: in-process wall/fault stats (median/MAD
    over reps, from reps_detail) + perf double-difference per rep-unit."""
    groups = {}
    for r in records:
        groups.setdefault((r["phase"], r["arm"], r["size"], r["slots"],
                           r["regime"]), {})[r["reps"]] = r
    rows = []
    for key in sorted(groups):
        phase, arm, size, slots, regime = key
        pair = groups[key]
        if REP_LO not in pair or REP_HI not in pair:
            continue  # incomplete pair (skip broke it); skips are recorded
        lo, hi = pair[REP_LO], pair[REP_HI]
        opr = ops_per_rep(hi) or 0
        row = {
            "phase": phase, "arm": arm, "size": size, "slots": slots,
            "regime": regime, "ops_per_rep": opr,
        }
        # in-process stats from the R14 run's reps_detail (14 useful reps)
        for field in ("alloc_ns", "io_ns", "touch_ns", "destroy_ns",
                      "minflt_alloc", "minflt_io", "minflt_touch"):
            vals = per_rep_field(hi, field)
            if vals and any(v > 0 for v in vals):
                m = median(vals)
                row[f"{field}_median"] = m
                row[f"{field}_mad"] = mad(vals, m)
        # derived: phase B total-to-first-useful per rep (sum of spans per
        # rep, then median — NOT a sum of medians)
        if phase == "B":
            totals = [a + i for a, i in zip(per_rep_field(hi, "alloc_ns"),
                                            per_rep_field(hi, "io_ns"))]
            m = median(totals)
            row["total_to_first_io_ns_median"] = m
            row["total_to_first_io_ns_mad"] = mad(totals, m)
            faults = [a + i for a, i in
                      zip(per_rep_field(hi, "minflt_alloc"),
                          per_rep_field(hi, "minflt_io"))]
            row["minflt_total_median"] = median(faults)
        # perf double-difference (R14 vs R7)
        if opr:
            for ev in ("instructions", "cycles"):
                try:
                    hi_v = hi["perf_counters"][ev]
                    lo_v = lo["perf_counters"][ev]
                    row[f"{ev}_per_unit"] = (hi_v - lo_v) / (REP_HI - REP_LO) \
                        / opr
                except (KeyError, TypeError):
                    row[f"{ev}_per_unit"] = None
        if phase == "C":
            b = hi["bench"]
            row["kc"] = b.get("kc")
            row["window_bytes"] = b.get("window_bytes")
            row["construct_ns"] = b.get("construct_ns")
            row["construct_minflt"] = b.get("construct_minflt")
            row["prefault_ns"] = b.get("prefault_ns")
            row["prefault_minflt"] = b.get("prefault_minflt")
        # per-buffer / per-page denominators (prereg §6)
        if phase in ("A", "B", "D"):
            row["alloc_ns_per_buffer"] = (
                row.get("alloc_ns_median", 0) / slots) if slots else None
            if phase == "D":
                pages = slots * (size // 4096)
                row["touch_ns_per_page"] = (
                    row.get("touch_ns_median", 0) / pages) if pages else None
            if phase == "B":
                row["io_ns_per_first_read"] = (
                    row.get("io_ns_median", 0) / slots) if slots else None
                row["total_to_first_io_ns_per_buffer"] = (
                    row.get("total_to_first_io_ns_median", 0) / slots
                ) if slots else None
        if phase == "C" and opr:
            row["io_ns_per_op"] = row.get("io_ns_median", 0) / opr
        rows.append(row)
    return rows


def write_summary(session_dir: Path, rows, skips):
    with open(session_dir / "summary.json", "w") as f:
        json.dump({"rows": rows, "skipped": skips}, f, indent=1)
    cols = ["phase", "arm", "size", "slots", "regime", "ops_per_rep",
            "kc", "window_bytes",
            "alloc_ns_median", "alloc_ns_mad", "alloc_ns_per_buffer",
            "io_ns_median", "io_ns_mad", "io_ns_per_first_read",
            "io_ns_per_op",
            "total_to_first_io_ns_median", "total_to_first_io_ns_mad",
            "total_to_first_io_ns_per_buffer",
            "touch_ns_median", "touch_ns_mad", "touch_ns_per_page",
            "destroy_ns_median",
            "minflt_alloc_median", "minflt_io_median", "minflt_touch_median",
            "minflt_total_median",
            "construct_ns", "construct_minflt",
            "prefault_ns", "prefault_minflt",
            "instructions_per_unit", "cycles_per_unit"]
    with open(session_dir / "summary.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols, extrasaction="ignore")
        w.writeheader()
        for r in rows:
            w.writerow(r)


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

def cmd_run(args):
    session_dir = RESULTS / args.session
    if session_dir.exists():
        print(f"session {session_dir} already exists (immutable)", file=sys.stderr)
        sys.exit(2)
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    # data file: generate once if missing (content is session-independent:
    # deterministic splitmix64 master tiling)
    if not DATA_FILE.exists() or DATA_FILE.stat().st_size != DATA_BYTES:
        subprocess.run([str(BENCH), "--generate", "--file", str(DATA_FILE),
                        "--generate-bytes", str(DATA_BYTES)], check=True)
    env = environment_json()
    env["data_file_sha256"] = sha256_file(DATA_FILE)
    session_dir.mkdir(parents=True)
    (session_dir / "environment.json").write_text(json.dumps(env, indent=1) + "\n")
    (session_dir / "manifest.json").write_text(json.dumps(manifest_json(), indent=1) + "\n")
    records, skips = run_matrix(session_dir, PHASES, ARMS, SIZES, SLOTS,
                                "pinned")
    errors = same_work_gate(records)
    (session_dir / "gates.json").write_text(json.dumps(
        {"same_work_errors": errors, "skipped": skips}, indent=1) + "\n")
    rows = summarize(records)
    write_summary(session_dir, rows, skips)
    print(f"session {args.session}: {len(records)} runs, "
          f"{len(rows)} rows, {len(skips)} skips, "
          f"gate errors: {len(errors)}")
    if errors:
        for e in errors:
            print("GATE:", e, file=sys.stderr)
        sys.exit(3)


def cmd_arena(args):
    """Secondary exploratory arena-regime probe (prereg §5): default glibc
    allocator, Phase A only, {4K, 64K} x slots 8, all arms."""
    session_dir = RESULTS / args.session
    if session_dir.exists():
        print(f"session {session_dir} already exists (immutable)", file=sys.stderr)
        sys.exit(2)
    if not DATA_FILE.exists():
        print("run the pinned session first (data file missing)", file=sys.stderr)
        sys.exit(2)
    env = environment_json()
    env["data_file_sha256"] = sha256_file(DATA_FILE)
    session_dir.mkdir(parents=True)
    env["purpose"] = ("arena-regime secondary probe: default glibc allocator, "
                      "phase A, {4K,64K} x slots 8 (prereg §5; not primary)")
    (session_dir / "environment.json").write_text(json.dumps(env, indent=1) + "\n")
    (session_dir / "manifest.json").write_text(json.dumps({
        "purpose": env["purpose"],
        "matrix": {"phases": ["A"], "arms": ARMS,
                   "sizes": [4096, 65536], "slots": [8]},
    }, indent=1) + "\n")
    records, skips = run_matrix(session_dir, ["A"], ARMS, [4096, 65536], [8],
                                "arena")
    errors = same_work_gate(records)
    (session_dir / "gates.json").write_text(json.dumps(
        {"same_work_errors": errors, "skipped": skips}, indent=1) + "\n")
    rows = summarize(records)
    write_summary(session_dir, rows, skips)
    print(f"session {args.session}: {len(records)} runs, gate errors: "
          f"{len(errors)}")
    if errors:
        sys.exit(3)


def cmd_align(args):
    """AMENDMENT 1 diagnostic session: arm b1a (B1 + page-aligned pointer)
    vs b1/b2/b3, phases B and C, full matrix, identical protocol. Purpose:
    mechanism attribution for the formal session's Outcome-D pattern
    (page-aligned arms 2.5-5x faster in prefaulted steady state)."""
    session_dir = RESULTS / args.session
    if session_dir.exists():
        print(f"session {session_dir} already exists (immutable)", file=sys.stderr)
        sys.exit(2)
    if not DATA_FILE.exists():
        print("run the pinned session first (data file missing)", file=sys.stderr)
        sys.exit(2)
    env = environment_json()
    env["data_file_sha256"] = sha256_file(DATA_FILE)
    env["purpose"] = ("AMENDMENT 1 alignment diagnostic: b1a (B1 mechanism + "
                      "page-aligned pointer) vs b1/b2/b3, phases B/C")
    session_dir.mkdir(parents=True)
    (session_dir / "environment.json").write_text(json.dumps(env, indent=1) + "\n")
    (session_dir / "manifest.json").write_text(json.dumps({
        "purpose": env["purpose"],
        "amendment": "BUF-E0-PREREGISTRATION.md AMENDMENT 1",
        "matrix": {"phases": ["B", "C"], "arms": ["b1", "b1a", "b2", "b3"],
                   "sizes": SIZES, "slots": SLOTS, "rep_pair": [REP_LO, REP_HI]},
    }, indent=1) + "\n")
    records, skips = run_matrix(session_dir, ["B", "C"], ["b1", "b1a", "b2", "b3"],
                                SIZES, SLOTS, "pinned")
    errors = same_work_gate(records)
    (session_dir / "gates.json").write_text(json.dumps(
        {"same_work_errors": errors, "skipped": skips}, indent=1) + "\n")
    rows = summarize(records)
    write_summary(session_dir, rows, skips)
    print(f"session {args.session}: {len(records)} runs, gate errors: "
          f"{len(errors)}")
    if errors:
        for e in errors:
            print("GATE:", e, file=sys.stderr)
        sys.exit(3)


AMP_BENCH = REPO / "build/linux/x86_64/release/buf_e0_amp_bench"
AMP_SRC = DATA_DIR / "amp-src.bin"
AMP_DST = DATA_DIR / "amp-dst.bin"
AMP_BYTES = 512 * 1024 * 1024
AMP_ARMS = ["engine-b0", "replica-b0", "replica-b1", "replica-b3"]
AMP_CELLS = [(1 << 20, 1), (1 << 20, 8)]  # (buffer_size, depth)


def file_sha256(path: Path) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def cmd_amp(args):
    """Amplifier session (prereg §8 + AMENDMENT 1): realistic PipelineSlot
    lifecycle end-to-end — production engine vs storage-variant replicas.
    R7/R14 perf pairs per arm x cell; post-exit src/dst hash verification
    (runner-side, outside every measured window)."""
    session_dir = RESULTS / args.session
    if session_dir.exists():
        print(f"session {session_dir} already exists (immutable)", file=sys.stderr)
        sys.exit(2)
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    if not AMP_SRC.exists() or AMP_SRC.stat().st_size != AMP_BYTES:
        subprocess.run([str(AMP_BENCH), "--generate", "--src", str(AMP_SRC),
                        "--file-bytes", str(AMP_BYTES)], check=True)
    src_hash = file_sha256(AMP_SRC)

    env = environment_json()
    env["data_file_sha256"] = src_hash
    env["purpose"] = ("BUF-E0 application amplifier: engine-b0 (production) "
                      "vs replica-b0/b1/b3 (storage variants); 512 MiB copy, "
                      "full engine call measured, workers=1, sync=none")
    env["build"]["amp_binary"] = str(AMP_BENCH)
    env["build"]["amp_binary_sha256"] = sha256_file(AMP_BENCH)
    session_dir.mkdir(parents=True)
    (session_dir / "environment.json").write_text(json.dumps(env, indent=1) + "\n")
    (session_dir / "manifest.json").write_text(json.dumps({
        "purpose": env["purpose"],
        "matrix": {"arms": AMP_ARMS, "cells": AMP_CELLS,
                   "file_bytes": AMP_BYTES, "workers": 1, "sync": "none",
                   "rep_pair": [REP_LO, REP_HI]},
    }, indent=1) + "\n")

    raw_dir = session_dir / "raw"
    raw_dir.mkdir()
    records = []
    for size, depth in AMP_CELLS:
        for arm in AMP_ARMS:
            for reps in (REP_LO, REP_HI):
                cmd = [str(AMP_BENCH), "--arm", arm, "--src", str(AMP_SRC),
                       "--dst", str(AMP_DST), "--buffer-size", str(size),
                       "--depth", str(depth), "--workers", "1",
                       "--reps", str(reps)]
                full = ["perf", "stat", "-x,", "-e", ",".join(PERF_EVENTS),
                        "--"] + cmd
                proc = subprocess.run(full, capture_output=True, text=True,
                                      env=dict(os.environ, LC_ALL="C"), cwd=REPO)
                tag = f"amp-{size // 1048576}m-d{depth}-{arm}-R{reps}"
                (raw_dir / f"{tag}.perf.txt").write_text(proc.stderr)
                bench = None
                if proc.stdout.strip():
                    try:
                        bench = json.loads(proc.stdout)
                    except ValueError:
                        bench = None
                if bench is not None:
                    (raw_dir / f"{tag}.bench.json").write_text(
                        json.dumps(bench, indent=1) + "\n")
                # Post-exit verification (runner-side): dst must equal src
                dst_hash = file_sha256(AMP_DST)
                verified = dst_hash == src_hash
                records.append({
                    "tag": tag, "arm": arm, "size": size, "depth": depth,
                    "reps": reps, "cmd": cmd,
                    "returncode": proc.returncode, "bench": bench,
                    "dst_verified": verified,
                    "perf_counters": parse_perf_stat(proc.stderr),
                })
                if proc.returncode != 0 or bench is None or not verified:
                    print(f"FATAL: {tag} rc={proc.returncode} "
                          f"verified={verified}", file=sys.stderr)
                    sys.exit(3)

    # aggregate: per arm x cell, wall medians from R14 reps + perf
    # double-difference per rep (ops unit = one full copy)
    rows = []
    groups = {}
    for r in records:
        groups.setdefault((r["arm"], r["size"], r["depth"]), {})[r["reps"]] = r
    for key in sorted(groups):
        arm, size, depth = key
        pair = groups[key]
        lo, hi = pair[REP_LO], pair[REP_HI]
        b = hi["bench"]

        def med(field, b=b):
            vals = [x[field] for x in b["reps_detail"]]
            return median(vals)
        row = {"arm": arm, "size": size, "depth": depth,
               "construct_ns": med("construct_ns"),
               "engine_ns": med("engine_ns"),
               "total_ns": med("total_ns"),
               "dst_verified": all(x["dst_verified"] for x in pair.values())}
        for ev in ("instructions", "cycles"):
            try:
                row[f"{ev}_per_copy"] = (
                    (hi["perf_counters"][ev] - lo["perf_counters"][ev])
                    / (REP_HI - REP_LO))
            except (KeyError, TypeError):
                row[f"{ev}_per_copy"] = None
        rows.append(row)
    with open(session_dir / "summary.json", "w") as f:
        json.dump({"rows": rows, "skipped": []}, f, indent=1)
    cols = ["arm", "size", "depth", "construct_ns", "engine_ns", "total_ns",
            "instructions_per_copy", "cycles_per_copy", "dst_verified"]
    with open(session_dir / "summary.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols, extrasaction="ignore")
        w.writeheader()
        for r in rows:
            w.writerow(r)
    (session_dir / "gates.json").write_text(json.dumps({
        "same_work_errors": [],
        "skipped": [],
        "post_exit_hash_verification": "all runs dst_sha256 == src_sha256",
    }, indent=1) + "\n")
    print(f"session {args.session}: {len(records)} runs, all hash-verified")


def cmd_report(args):
    session_dir = RESULTS / args.session
    summary = json.loads((session_dir / "summary.json").read_text())
    rows = summary["rows"]
    arms_present = []
    for r in rows:
        if r["arm"] not in arms_present:
            arms_present.append(r["arm"])
    print(f"== {args.session}: {len(rows)} rows, {len(summary['skipped'])} skipped")
    for phase in ("A", "B", "D", "C"):
        pr = [r for r in rows if r["phase"] == phase]
        if not pr:
            continue
        print(f"\n-- phase {phase}")
        if phase == "A":
            print(f"{'size':>8} {'slots':>5} | per-buffer alloc ns (median±mad):")
            for size in SIZES:
                for slots in SLOTS:
                    cells = {r["arm"]: r for r in pr
                             if r["size"] == size and r["slots"] == slots}
                    if len(cells) < len(arms_present):
                        continue
                    line = f"{size//1024:>7}k {slots:>5} |"
                    for arm in arms_present:
                        r = cells[arm]
                        line += (f" {arm}={r.get('alloc_ns_per_buffer', 0):>10.0f}"
                                 f"±{r.get('alloc_ns_mad', 0)/max(r['slots'],1):>7.0f}")
                    print(line)
        elif phase == "B":
            print(f"{'size':>8} {'slots':>5} | total-to-first-I/O ns/buffer | "
                  "faults(alloc+io):")
            for size in SIZES:
                for slots in SLOTS:
                    cells = {r["arm"]: r for r in pr
                             if r["size"] == size and r["slots"] == slots}
                    if len(cells) < len(arms_present):
                        continue
                    line = f"{size//1024:>7}k {slots:>5} |"
                    for arm in arms_present:
                        r = cells[arm]
                        line += (f" {arm}="
                                 f"{r.get('total_to_first_io_ns_per_buffer', 0):>10.0f}")
                    line += " |"
                    for arm in arms_present:
                        r = cells[arm]
                        line += f" {arm}={r.get('minflt_total_median', 0):>5.0f}"
                    print(line)
        elif phase == "D":
            print(f"{'size':>8} {'slots':>5} | first-touch ns/page:")
            for size in SIZES:
                for slots in SLOTS:
                    cells = {r["arm"]: r for r in pr
                             if r["size"] == size and r["slots"] == slots}
                    if len(cells) < len(arms_present):
                        continue
                    line = f"{size//1024:>7}k {slots:>5} |"
                    for arm in arms_present:
                        r = cells[arm]
                        line += (f" {arm}="
                                 f"{r.get('touch_ns_per_page', 0):>9.0f}")
                    print(line)
        elif phase == "C":
            print(f"{'size':>8} {'slots':>5} {'kc':>3} | steady-state ns/op:")
            for size in SIZES:
                for slots in SLOTS:
                    cells = {r["arm"]: r for r in pr
                             if r["size"] == size and r["slots"] == slots}
                    if len(cells) < len(arms_present):
                        continue
                    line = (f"{size//1024:>7}k {slots:>5} "
                            f"{cells[arms_present[0]].get('kc', 0):>3} |")
                    for arm in arms_present:
                        r = cells[arm]
                        line += f" {arm}={r.get('io_ns_per_op', 0):>11.0f}"
                    print(line)


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("run")
    p.add_argument("--session", required=True)
    p.set_defaults(fn=cmd_run)
    p = sub.add_parser("arena")
    p.add_argument("--session", required=True)
    p.set_defaults(fn=cmd_arena)
    p = sub.add_parser("align")
    p.add_argument("--session", required=True)
    p.set_defaults(fn=cmd_align)
    p = sub.add_parser("amp")
    p.add_argument("--session", required=True)
    p.set_defaults(fn=cmd_amp)
    p = sub.add_parser("report")
    p.add_argument("--session", required=True)
    p.set_defaults(fn=cmd_report)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
