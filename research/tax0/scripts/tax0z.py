#!/usr/bin/env python3
"""TAX-0B semantic-floor Z-ladder session runner (#250 / #259 / PR #260).

Drives research/tax0/bench/tax0_z_ladder_bench.cpp (preregistered
Z1/Z1b/Z1bw/Z2/Z3 arms, census `z_ladder_preregistration`) through a formal
measurement protocol and stores every session under an immutable
research/tax0/results/<session-id>/ directory:

    environment.json   host fingerprint + QUALIFIED_* classification
    manifest.json      exact commands, binary sha256, cell/rep protocol
    raw/               per-run perf-stat output + bench JSON (verbatim)
    summary.csv        per (cell, op, arm) normalized rows
    summary.json       same rows, machine-readable
    notes.md           protocol notes (normalization conventions)

Measurement conventions (recorded here because they are part of the
preregistered protocol, not implementation detail):

  * Windowing: perf stat can only see whole processes, so setup/teardown
    pollution is eliminated by DOUBLE DIFFERENCE: every combo runs twice
    (R=7 and R=14 measured reps, --warmup 0) with identical setup/teardown;
    per-rep workload work = (total(14) - total(7)) / 7, then /ops.
  * Write-arm final verification runs in the RUNNER (after the measured
    process exits, --runner-verify) so the verify pread+word-sum never lands
    in the :u instruction totals. Read-arm word_sum is computed inline in
    every arm (uniform workload component, shootout precedent).
  * Primary counters are USER-mode (:u): the campaign question is the
    userspace control plane; the kernel medium is profiled separately in
    TAX-0C with whole-process events.
  * A canonical perf-evidence artifact (kind "perf",
    scripts/bench/perf-attribution.py) is additionally produced for the P1
    representative cells so committed claims carry validator-checked
    provenance; its event set is process-wide (user+kernel).

Environment classification: this session host is WSL2 (virtualized PMU,
virtual NVMe ext4); per the frozen census `environment_note` every claim is
ENVIRONMENT-LIMITED and pilot-grade for topology, not a native throughput
conclusion.
"""

import argparse
import csv
import datetime
import hashlib
import json
import os
import pathlib
import platform
import re
import shutil
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[3]
BENCH = REPO / "build/linux/x86_64/release/tax0_z_ladder_bench"
ABLATION_BENCH = REPO / "build/linux/x86_64/release/tax0_ablation_bench"
RUNNER = REPO / "scripts/bench/perf-attribution.py"
RESULTS_ROOT = REPO / "research/tax0/results"
DATA_DIR = REPO / "build/tax0z-data"

P1_CELLS = [
    # (request_size, depth)
    (4096, 1),
    (4096, 32),
    (4096, 64),
    (65536, 8),
    (1048576, 8),
]
FULL_CELLS = P1_CELLS + [
    (4096, 8),
    (65536, 1),
    (65536, 32),
    (1048576, 1),
    (1048576, 32),
]
ARMS = ["z1", "z1b", "z1bw", "z2", ("z3", 1), ("z3", 4)]
OPS = ["read", "write"]

SESSION_PERF_EVENTS = ["instructions:u", "cycles:u", "branch-misses:u",
                       "cache-misses:u"]
FORMAL_REPS_A = 7
FORMAL_REPS_B = 14


def arm_label(arm):
    if isinstance(arm, tuple):
        return f"{arm[0]}w{arm[1]}"
    return arm


def arm_args(arm):
    if isinstance(arm, tuple):
        return ["--arm", arm[0], "--workers", str(arm[1])]
    return ["--arm", arm]


def total_bytes_for(request_size):
    return 256 * 1024 * 1024 if request_size >= 1048576 else 64 * 1024 * 1024


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def git_facts():
    def g(*args):
        r = subprocess.run(["git", "-C", str(REPO), *args],
                           capture_output=True, text=True)
        return r.stdout.strip() if r.returncode == 0 else None
    return {
        "head": g("rev-parse", "HEAD"),
        "branch": g("rev-parse", "--abbrev-ref", "HEAD"),
        "dirty": g("status", "--porcelain") != "",
    }


def read_first(path, default=None):
    try:
        return pathlib.Path(path).read_text().strip()
    except OSError:
        return default


def is_wsl():
    return "microsoft" in platform.release().lower() or \
        read_first("/proc/sys/kernel/osrelease", "").find("microsoft") >= 0


def capture_environment(session_dir):
    env = {}
    now = datetime.datetime.now(datetime.timezone.utc)
    env["timestamp_utc"] = now.isoformat()
    env["git"] = git_facts()
    env["system"] = {
        "kernel": platform.release(),
        "wsl": is_wsl(),
        "logical_cpus": os.cpu_count(),
        "cpu_model": next((l.split(":", 1)[1].strip()
                           for l in read_first("/proc/cpuinfo", "").splitlines()
                           if l.startswith("model name")), None),
        "affinity_cpus": sorted(os.sched_getaffinity(0)),
        "governor": read_first(
            "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor"),
        "mem_total_kb": next((int(l.split()[1])
                              for l in read_first("/proc/meminfo", "").splitlines()
                              if l.startswith("MemTotal")), None),
        "perf_event_paranoid": read_first("/proc/sys/kernel/perf_event_paranoid"),
    }
    env["filesystem"] = {
        "data_dir": str(DATA_DIR),
        "data_dir_fs": _fs_type(DATA_DIR),
        "notes": "WSL2 ext4 on virtual block device (no raw NVMe semantics)",
    }
    env["virtualization"] = {
        "wsl2": is_wsl(),
        "classification": "QUALIFIED_BUT_VIRTUALIZED",
        "claim_limit": "ENVIRONMENT-LIMITED: control-plane user-instruction "
                       "attribution pilot only; no native NVMe/throughput "
                       "conclusions (census z_ladder_preregistration."
                       "environment_note)",
    }
    env["tools"] = {
        "perf": _tool_version(["perf", "--version"]),
        "python": platform.python_version(),
    }
    env["liburing"] = _liburing_version()
    env["build"] = {
        "mode": "release",
        "bench_binary": str(BENCH),
        "bench_binary_sha256": sha256_file(BENCH) if BENCH.exists() else None,
        "compiler": _compiler_version(),
    }
    out = session_dir / "environment.json"
    out.write_text(json.dumps(env, indent=2) + "\n")
    return env


def _fs_type(path):
    try:
        out = subprocess.check_output(["df", "-T", str(path)],
                                      text=True).strip().splitlines()
        return out[-1].split()[1]
    except Exception:
        return None


def _tool_version(cmd):
    try:
        return subprocess.check_output(cmd, text=True,
                                       stderr=subprocess.STDOUT).strip()
    except Exception:
        return None


def _liburing_version():
    v = _tool_version(["pkg-config", "--modversion", "liburing"])
    if v:
        return v
    return "unknown"


def _compiler_version():
    for c in (["clang", "--version"], ["gcc", "--version"]):
        v = _tool_version(c)
        if v:
            return v.splitlines()[0]
    return None


# ---------------------------------------------------------------------------
# run protocol
# ---------------------------------------------------------------------------

def bench_cmd(arm, request_size, depth, op, reps, warmup, binary=None,
              extra=None):
    data = DATA_DIR / f"data-{request_size}.bin"
    cmd = [str(binary or BENCH), *arm_args(arm),
           "--op", op,
           "--file", str(data),
           "--request-size", str(request_size),
           "--total-bytes", str(total_bytes_for(request_size)),
           "--depth", str(depth),
           "--reps", str(reps),
           "--warmup", str(warmup),
           "--runner-verify"]
    if extra:
        cmd += extra
    return cmd


def parse_perf_stat(stderr_text):
    counters = {}
    for line in stderr_text.splitlines():
        parts = line.split(",")
        if len(parts) >= 3 and parts[0]:
            key = parts[2].split(":")[0]  # strip :u/:k modifiers
            try:
                counters[key] = float(parts[0])
            except ValueError:
                pass
    return counters


def run_one(arm, request_size, depth, op, reps, session_raw, tag,
            with_perf=True, binary=None, extra=None):
    """One measured process launch; returns a run record."""
    cmd = bench_cmd(arm, request_size, depth, op, reps, warmup=0,
                    binary=binary, extra=extra)
    if with_perf:
        full = ["perf", "stat", "-x,", "-e",
                ",".join(SESSION_PERF_EVENTS), "--"] + cmd
    else:
        full = cmd
    env = dict(os.environ, LC_ALL="C")
    proc = subprocess.run(full, capture_output=True, text=True, env=env)
    raw_perf = session_raw / f"{tag}.perf.txt"
    raw_json = session_raw / f"{tag}.bench.json"
    raw_perf.write_text(proc.stderr)
    try:
        bench = json.loads(proc.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        bench = None
    if bench is not None:
        raw_json.write_text(json.dumps(bench, indent=1) + "\n")
    record = {
        "tag": tag,
        "arm": arm_label(arm),
        "request_size": request_size,
        "depth": depth,
        "op": op,
        "reps": reps,
        "cmd": cmd,
        "returncode": proc.returncode,
        "perf_counters": parse_perf_stat(proc.stderr) if with_perf else {},
        "bench": bench,
    }
    return record


def runner_verify_write(arm, request_size, op):
    """Post-exit write verification (runnerside, outside any perf window)."""
    if op != "write":
        return None
    data = DATA_DIR / f"data-{request_size}.bin"
    kblock = 4096
    master = bytearray(kblock)
    seed = 0xE1E1E1E121212121
    words = [seed + i for i in range(kblock // 8)]
    import struct
    for i, w in enumerate(words):
        x = (w + 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF
        x = ((x ^ (x >> 30)) * 0xBF58476D1CE4E5B9) & 0xFFFFFFFFFFFFFFFF
        x = ((x ^ (x >> 27)) * 0x94D049BB133111EB) & 0xFFFFFFFFFFFFFFFF
        x = x ^ (x >> 31)
        struct.pack_into("<Q", master, i * 8, x)
    block = bytes(master)
    total = total_bytes_for(request_size)
    ok = True
    with open(data, "rb") as f:
        off = 0
        while off < total:
            chunk = f.read(kblock)
            if chunk != block:
                ok = False
                break
            off += kblock
    return ok


def aggregate_combo(records):
    """Double-difference normalization across the R=7 / R=14 pair."""
    a = next((r for r in records if r["reps"] == FORMAL_REPS_A), None)
    b = next((r for r in records if r["reps"] == FORMAL_REPS_B), None)
    if a is None or b is None or a["bench"] is None or b["bench"] is None:
        return None
    ops = a["bench"]["ops"]
    if ops != b["bench"]["ops"]:
        return None
    extra_reps = FORMAL_REPS_B - FORMAL_REPS_A
    row = {
        "arm": a["arm"],
        "op": a["op"],
        "request_size": a["request_size"],
        "depth": a["depth"],
        "ops": ops,
        "ok": a["returncode"] == 0 and b["returncode"] == 0,
    }

    def wall_med(rec):
        ws = sorted(r["wall_ns"] for r in rec["bench"]["reps"])
        return ws[len(ws) // 2]

    row["wall_ns_per_op"] = wall_med(a) / ops
    row["user_ns_per_op"] = a["bench"]["reps"][0]["user_ns"] / ops
    row["sys_ns_per_op"] = a["bench"]["reps"][0]["sys_ns"] / ops
    for ev in ("instructions", "cycles", "branch-misses", "cache-misses"):
        ca, cb = a["perf_counters"].get(ev), b["perf_counters"].get(ev)
        if ca is None or cb is None:
            row[f"{ev}_u_per_op"] = None
            continue
        per_rep_work = (cb - ca) / extra_reps
        row[f"{ev}_u_per_op"] = per_rep_work / ops
    # same-work witness
    row["word_sum"] = a["bench"]["reps"][0]["word_sum"]
    return row


def prepare_data_files(cells):
    """Pre-create input files via one throwaway invocation per size."""
    sizes = sorted({rs for rs, _ in cells})
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    for rs in sizes:
        data = DATA_DIR / f"data-{rs}.bin"
        if data.exists() and data.stat().st_size == total_bytes_for(rs):
            continue
        tmp_arm = ["--arm", "z1"]
        cmd = [str(BENCH), *tmp_arm, "--op", "write",
               "--file", str(data), "--request-size", str(rs),
               "--total-bytes", str(total_bytes_for(rs)),
               "--depth", "1", "--reps", "1", "--warmup", "0"]
        subprocess.run(cmd, capture_output=True, text=True, check=True)


def cross_arm_same_work(rows):
    """Rows grouped by cell: all arms must report identical accounting."""
    problems = []
    groups = {}
    for r in rows:
        groups.setdefault((r["op"], r["request_size"], r["depth"]), []).append(r)
    for key, grp in groups.items():
        op = key[0]
        sums = {r["word_sum"] for r in grp if r["op"] == op}
        if op == "read" and len(sums) > 1:
            problems.append(f"{key}: read word_sum differs across arms {sums}")
        if not all(r["ok"] for r in grp):
            problems.append(f"{key}: some arm failed (ok=false)")
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("action", choices=["env", "pilot", "formal", "ablation",
                                       "canon", "report"],
                    default="formal", nargs="?")
    ap.add_argument("--session", default=None,
                    help="session id (default: tax0b-zladder-<ts>)")
    ap.add_argument("--cells", choices=["p1", "full"], default="p1")
    args = ap.parse_args()

    if not BENCH.exists():
        sys.exit(f"bench binary missing: {BENCH} (xmake build tax0_z_ladder_bench)")

    cells = P1_CELLS if args.cells == "p1" else FULL_CELLS

    if args.action == "env":
        session = args.session or f"tax0b-zladder-{datetime.datetime.now():%Y%m%d-%H%M%S}"
        sdir = RESULTS_ROOT / session
        sdir.mkdir(parents=True, exist_ok=False)
        env = capture_environment(sdir)
        print(json.dumps(env, indent=2))
        return

    if args.action == "pilot":
        session = args.session or f"tax0b-zladder-pilot-{datetime.datetime.now():%Y%m%d-%H%M%S}"
        sdir = RESULTS_ROOT / session
        if sdir.exists():
            sys.exit(f"refusing to overwrite session dir: {sdir}")
        (sdir / "raw").mkdir(parents=True)
        capture_environment(sdir)
        prepare_data_files(cells)
        failures = []
        for op in OPS:
            for rs, d in cells:
                for arm in ARMS:
                    tag = f"pilot-{op}-r{rs}-d{d}-{arm_label(arm)}"
                    rec = run_one(arm, rs, d, op, reps=1, session_raw=sdir / "raw",
                                  tag=tag, with_perf=False)
                    status = "OK" if rec["returncode"] == 0 else "FAIL"
                    if rec["returncode"] != 0:
                        failures.append(tag)
                    print(f"[{status}] {tag}")
        print(f"\nPILOT {'CLEAN' if not failures else 'FAILURES: ' + str(failures)}")
        return

    if args.action == "ablation":
        if not ABLATION_BENCH.exists():
            sys.exit(f"ablation binary missing: {ABLATION_BENCH}")
        session = args.session or f"tax0d-ablation-{datetime.datetime.now():%Y%m%d-%H%M%S}"
        sdir = RESULTS_ROOT / session
        if sdir.exists():
            sys.exit(f"refusing to overwrite session dir: {sdir}")
        (sdir / "raw").mkdir(parents=True)
        capture_environment(sdir)
        prepare_data_files(cells)
        # One mechanism, one A/B (preregistration): R0 (identical harness,
        # seams at default) vs exactly one R1 mode per run.
        variants = [("r0", []),
                    ("f01r1", ["--f01-r1"]),
                    ("f02r1", ["--f02-r1"])]
        ablation_arms = [("z2", None), (("z3", 1), None)]
        rows = []
        for op in OPS:
            for rs, d in cells:
                for arm, _ in ablation_arms:
                    for vname, vflags in variants:
                        label = arm_label(arm)
                        tag = f"abl-{op}-r{rs}-d{d}-{label}-{vname}"
                        recs = [
                            run_one(arm, rs, d, op, FORMAL_REPS_A,
                                    sdir / "raw", f"{tag}-R{FORMAL_REPS_A}",
                                    binary=ABLATION_BENCH, extra=vflags),
                            run_one(arm, rs, d, op, FORMAL_REPS_B,
                                    sdir / "raw", f"{tag}-R{FORMAL_REPS_B}",
                                    binary=ABLATION_BENCH, extra=vflags),
                        ]
                        row = aggregate_combo(recs)
                        if row is None:
                            print(f"[FAIL] {tag}: aggregation failed")
                            continue
                        row["variant"] = vname
                        if op == "write":
                            row["write_runner_verified"] = \
                                runner_verify_write(arm, rs, op)
                            row["ok"] = row["ok"] and row["write_runner_verified"]
                        rows.append(row)
                        ins = row.get("instructions_u_per_op")
                        print(f"[{'OK' if row['ok'] else 'FAIL'}] {tag}: "
                              f"instr/op={ins:.0f}" if ins else
                              f"[{'OK' if row['ok'] else 'FAIL'}] {tag}: n/a")
        fieldnames = sorted({k for r in rows for k in r})
        with open(sdir / "summary.csv", "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=fieldnames, extrasaction="ignore")
            w.writeheader()
            w.writerows(rows)
        (sdir / "summary.json").write_text(json.dumps(rows, indent=1) + "\n")
        print(f"\nsession: {sdir}")
        return

    if args.action == "canon":
        # Canonical validator-bound artifacts (kind "perf") for the P1
        # representative cells on the claim-bearing arms. Whole-process
        # counters (user+kernel); the per-op ladder numbers themselves live
        # in the formal session summary.
        canon_arms = ["z1b", "z2", ("z3", 1)]
        for op in OPS:
            for rs, d in cells:
                for arm in canon_arms:
                    label = f"r{rs}-d{d}"
                    out = (REPO / "docs/results/performance-attribution" /
                           f"tax0z-{label}-{op}-{arm_label(arm)}.json")
                    tb = total_bytes_for(rs)
                    ops = tb // rs
                    cmd = bench_cmd(arm, rs, d, op, FORMAL_REPS_B, 0)
                    r = subprocess.run(
                        ["python3", str(RUNNER), "perf", "--output", str(out),
                         "--requests", str(FORMAL_REPS_B * ops),
                         "--exit-semantics", "strict-zero",
                         "--note", "TAX-0B Z-ladder canonical artifact "
                                   "(formal-session protocol, whole-process "
                                   "counters; per-op ladder numbers live in "
                                   "research/tax0/results/)",
                         "--"] + cmd,
                        capture_output=True, text=True)
                    status = "OK" if r.returncode == 0 else f"FAIL({r.returncode})"
                    print(f"[CANON {status}] {label} {op} {arm_label(arm)}")
        return

    if args.action == "report":
        session = args.session
        sdir = RESULTS_ROOT / session
        rows = json.loads((sdir / "summary.json").read_text())
        print(f"session {session}: {len(rows)} rows\n")
        print(f"{'cell':>16} {'op':>5} {'arm':>6} {'instr/op':>10} "
              f"{'cyc/op':>10} {'wall/op ns':>11}")
        for r in rows:
            cell = f"{r['request_size']//1024}K d{r['depth']}"
            ins = r.get("instructions_u_per_op")
            cyc = r.get("cycles_u_per_op")
            print(f"{cell:>16} {r['op']:>5} {r['arm']:>6} "
                  f"{ins:10.0f} {cyc:10.0f} {r['wall_ns_per_op']:11.0f}")
        return

    # formal
    session = args.session or f"tax0b-zladder-{datetime.datetime.now():%Y%m%d-%H%M%S}"
    sdir = RESULTS_ROOT / session
    if sdir.exists():
        sys.exit(f"refusing to overwrite session dir: {sdir}")
    (sdir / "raw").mkdir(parents=True)
    env = capture_environment(sdir)
    prepare_data_files(cells)

    manifest = {
        "protocol": {
            "warmup_reps_formal": 0,
            "formal_rep_pairs": [FORMAL_REPS_A, FORMAL_REPS_B],
            "normalization": "double-difference (total(R14)-total(R7))/7/ops",
            "write_verification": "runner-side post-exit byte compare",
            "primary_counters": SESSION_PERF_EVENTS,
        },
        "cells": cells,
        "arms": [arm_label(a) for a in ARMS],
        "runs": [],
    }

    rows = []
    for op in OPS:
        for rs, d in cells:
            for arm in ARMS:
                label = arm_label(arm)
                tag_a = f"formal-{op}-r{rs}-d{d}-{label}-R{FORMAL_REPS_A}"
                tag_b = f"formal-{op}-r{rs}-d{d}-{label}-R{FORMAL_REPS_B}"
                # OBSERVATION (session notes): z3w4 write under the perf
                # stat wrapper aborts intermittently (teardown abort, and
                # spurious canceled(EAGAIN) terminals) — 0/10 session combos
                # survived all retries, while the same geometry standalone
                # passes (11/12 probes). Minimal recovery: measure these
                # cells without the wrapper (wall/user/sys + same-work from
                # the bench JSON; :u counters NOT RUN — environment
                # limitation).
                with_perf = not (arm_label(arm) == "z3w4" and op == "write")
                retries = 0
                while True:
                    rec_a = run_one(arm, rs, d, op, FORMAL_REPS_A, sdir / "raw", tag_a,
                                    with_perf=with_perf)
                    rec_b = run_one(arm, rs, d, op, FORMAL_REPS_B, sdir / "raw", tag_b,
                                    with_perf=with_perf)
                    manifest["runs"] += [tag_a, tag_b]
                    if rec_a["returncode"] == 0 and rec_b["returncode"] == 0:
                        break
                    retries += 1
                    if retries > 2:
                        break
                    # Documented retry policy: the z3w4 write cells hit an
                    # intermittent spurious `canceled` terminal (~1/6 launches,
                    # recorded as OBSERVATION in the session notes). Retries
                    # are counted here, never silently dropped.
                    manifest.setdefault("combo_retries", []).append(
                        {"op": op, "request_size": rs, "depth": d,
                         "arm": arm_label(arm), "retries": retries})
                row = aggregate_combo([rec_a, rec_b])
                if row is None:
                    print(f"[FAIL] {op} r{rs} d{d} {label}: aggregation failed")
                    continue
                if not with_perf:
                    row["instructions_u_per_op"] = None
                    row["cycles_u_per_op"] = None
                    row["perf_wrapped"] = False
                if op == "write":
                    row["write_runner_verified"] = runner_verify_write(arm, rs, op)
                    if not row["write_runner_verified"]:
                        print(f"[FAIL] {op} r{rs} d{d} {label}: "
                              f"runner write verification mismatch")
                        row["ok"] = False
                rows.append(row)
                subprocess.run(["sync"], check=False)  # writeback settle:
                # repeated 256MiB buffered-write reps accumulate dirty pages;
                # under pressure this host returns spurious EAGAIN terminals
                subprocess.run(["sleep", "0.3"], check=False)
                ins = row.get("instructions_u_per_op")
                cyc = row.get("cycles_u_per_op")
                ins_s = f"{ins:.0f}" if ins else "n/a"
                cyc_s = f"{cyc:.0f}" if cyc else "n/a"
                print(f"[{'OK' if row['ok'] else 'FAIL'}] {op} r{rs} d{d} "
                      f"{label}: instr/op={ins_s} cycles/op={cyc_s} "
                      f"wall/op={row['wall_ns_per_op']:.0f}ns")

    problems = cross_arm_same_work(rows)
    for p in problems:
        print(f"SAME-WORK PROBLEM: {p}")

    (sdir / "summary.json").write_text(json.dumps(rows, indent=1) + "\n")
    fieldnames = sorted({k for r in rows for k in r})
    with open(sdir / "summary.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames, extrasaction="ignore")
        w.writeheader()
        w.writerows(rows)
    (sdir / "manifest.json").write_text(json.dumps(manifest, indent=1) + "\n")
    print(f"\nsession: {sdir}")


if __name__ == "__main__":
    main()
