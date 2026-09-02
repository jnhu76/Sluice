#!/usr/bin/env python3
"""rbuf_e0.py — RBUF-E0 io_uring registered/fixed-buffer campaign driver (#272).

Runs the frozen preregistration (research/rbuf-e0/RBUF-E0-PREREGISTRATION.md)
on Host-0 via the research-only direct-liburing mechanism bench
(bench/rbuf_e0_bench.cpp; production code untouched). Single
submission/completion thread everywhere (workers=1 frozen; no multi-worker).

Subcommands:
  status                bench binary / perf / memlock quick check
  probe <session-id>    capability preflight (behavioral register + fixed
                        read/write round-trip, RLIMIT_MEMLOCK, perf probe);
                        prints URING/FIXED/MEMLOCK/PERF/FORMAL_ELIGIBLE lines
  generate <session-id> create the 1 GiB src fixture (bench --generate) and
                        record its sha256 (cross-checked against the CHUNK-E0
                        canonical fixture sha when known)
  q0 <session-id>       Phase Q0 io_uring stability qualification: 50 runs of
                        U1 at 2 MiB x d2, 1 GiB copy, full same-work gates.
                        50/50 valid -> QUALIFIED (does NOT close #262);
                        any failure -> RBUF-E0 STOPPED, #262 blocking.
  steady <session-id>   frozen steady-state matrix: cells x arms x R=7
                        seeded-interleaved rounds, perf-wrapped
  amort <session-id>    frozen reuse-horizon experiment at 2 MiB x d2:
                        horizons {1,4,16,64} x {U1,U2} x 7, seeded-interleaved
  summarize <session-id> runs.jsonl -> summary.csv/json + analysis.json
                        (per-cell U1/U2 materiality, neighbor consistency,
                        lifecycle costs, amortization crossover; verdict
                        vocabulary per prereg)

RLIMIT_MEMLOCK boundary: cells whose U2 registration would meet or exceed
the observed memlock limit are REGISTRATION-INFEASIBLE. The driver marks
them infeasible in the manifest BEFORE measurement (from the probe's observed
limit) and records them in analysis.json — they are a reportable resource
capability boundary, not gate errors and not anomalies. No ulimit/sysctl is
ever adjusted (prereg rule).

Immutable session layout (mirrors research/chunk-e0):
  results/<session-id>/{environment.json, manifest.json, gates.json,
                        notes.md, raw/runs.jsonl, raw/perf.csv,
                        summary.csv, summary.json, analysis.json}
"""

from __future__ import annotations

import csv
import hashlib
import json
import math
import os
import random
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
DATA_DIR = Path(os.environ["RBUF_E0_DATA_DIR"]) \
    if "RBUF_E0_DATA_DIR" in os.environ else REPO / "build/rbuf-e0-data"
BENCH = Path(os.environ["RBUF_E0_BENCH"]) \
    if "RBUF_E0_BENCH" in os.environ else \
    REPO / "build/linux/x86_64/release/rbuf_e0_bench"
RESULTS = Path(os.environ["RBUF_E0_RESULTS"]) \
    if "RBUF_E0_RESULTS" in os.environ else REPO / "research/rbuf-e0/results"

FILE_BYTES = 1_073_741_824          # 1 GiB useful bytes per transfer (frozen)
CELLS = [(524_288, 2), (1_048_576, 2), (2_097_152, 1), (2_097_152, 2),
         (2_097_152, 4), (4_194_304, 2)]
PRIMARY_CELL = (2_097_152, 2)
ARMS = ["U0", "U1", "U2"]
ROUNDS = 7
SEED = 0xE1E1E1E121212121
Q0_RUNS = 50
Q0_CELL = PRIMARY_CELL
HORIZONS = [1, 4, 16, 64]

# CHUNK-E0 H0 canonical fixture sha256 (same generator + seed -> identical
# bytes; reused instead of diverging, prereg workload rule).
CHUNK_E0_FIXTURE_SHA = \
    "8a3c4bf01ec3d32c0da34e9ed93a091bfaedfc48e39dad5bcd6c8b1bf548fd53"

# Frozen decision rules (prereg; DO NOT tune after formal measurement starts).
MATERIAL_RATIO = 1.03
MATERIAL_MAD_K = 1.5


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            h.update(blk)
    return h.hexdigest()


def git_state() -> dict:
    def run(*a):
        try:
            return subprocess.run(a, capture_output=True, text=True,
                                  cwd=REPO).stdout.strip()
        except Exception:
            return "?"
    return {
        "head": run("git", "rev-parse", "HEAD"),
        "branch": run("git", "branch", "--show-current"),
        "dirty": bool(run("git", "status", "--porcelain")),
    }


def environment_json() -> dict:
    def run(*a):
        try:
            return subprocess.run(a, capture_output=True, text=True,
                                  check=True).stdout.strip()
        except Exception:
            return "?"
    lscpu = run("lscpu")
    return {
        "uname": run("uname", "-a"),
        "kernel": run("uname", "-r"),
        "distribution": run("cat", "/etc/os-release") if
        Path("/etc/os-release").is_file() else "?",
        "cpu_model": [l for l in lscpu.splitlines()
                      if l.startswith("Model name")],
        "cpu_threads": [l for l in lscpu.splitlines()
                        if l.startswith("CPU(s)") or l.startswith("Thread")],
        "meminfo": {k: v.strip() for k, v in (
            l.split(":", 1) for l in Path("/proc/meminfo").read_text()
            .splitlines() if l.startswith(("MemTotal", "MemAvailable")))},
        "page_size": run("getconf", "PAGESIZE"),
        "smbios": run("cat", "/sys/class/dmi/id/product_name"),
        "filesystem": run("findmnt", "-no", "FSTYPE,OPTIONS", "-T", DATA_DIR)
        if DATA_DIR.is_dir() else "?",
        "block_device": run("findmnt", "-no", "SOURCE", "-T", DATA_DIR)
        if DATA_DIR.is_dir() else "?",
        "governor": run("cat",
                        "/sys/devices/system/cpu/cpu0/cpufreq/"
                        "scaling_governor"),
        "no_turbo": run("cat",
                        "/sys/devices/system/cpu/intel_pstate/no_turbo"),
        "glibc": (run("ldd", "--version").splitlines() or ["?"])[0],
        "clang": (run("clang", "--version").splitlines() or ["?"])[0],
        "liburing_pkg": run("pkg-config", "--modversion", "liburing"),
        "perf": run("perf", "--version"),
        "perf_paranoid": run("cat", "/proc/sys/kernel/perf_event_paranoid"),
        "virtualization": run("systemd-detect-virt"),
        "git": git_state(),
        "bench_binary_sha256": sha256_file(BENCH) if BENCH.is_file() else "?",
        "bench_binary_size": BENCH.stat().st_size if BENCH.is_file() else 0,
    }


def median(vals):
    if not vals:
        return 0.0
    s = sorted(vals)
    return s[len(s) // 2]


def mad(vals, med):
    if not vals:
        return 0.0
    return median([abs(v - med) for v in vals])


def new_session(session_id: str, purpose: str, manifest: dict) -> Path:
    sd = RESULTS / session_id
    raw = sd / "raw"
    raw.mkdir(parents=True, exist_ok=False)
    env = environment_json()
    env["data_src_sha256"] = sha256_file(DATA_DIR / "src.bin") \
        if (DATA_DIR / "src.bin").is_file() else "?"
    (sd / "environment.json").write_text(json.dumps(env, indent=1) + "\n")
    manifest["purpose"] = purpose
    manifest["preregistration"] = \
        "research/rbuf-e0/RBUF-E0-PREREGISTRATION.md (FROZEN)"
    manifest["data_dir"] = str(DATA_DIR)
    (sd / "manifest.json").write_text(json.dumps(manifest, indent=1) + "\n")
    (sd / "gates.json").write_text("{\n}\n")
    (sd / "notes.md").write_text(f"# {session_id} — notes\n\n"
                                 f"(authored after the session)\n")
    return sd


def parse_perf_stat(text: str) -> dict:
    out = {}
    for line in text.splitlines():
        parts = [p.strip() for p in line.split(",")]
        for i, p in enumerate(parts):
            if p.endswith((":u", "task-clock")):
                for j in range(i - 1, -1, -1):
                    if not parts[j] or parts[j] in ("msec", "sec"):
                        continue
                    try:
                        out[p] = float(parts[j])
                    except ValueError:
                        out[p] = None
                    break
                else:
                    out[p] = None
                break
    if "task-clock:u" in out:
        out["task-clock"] = out.pop("task-clock:u")
    return out


class Gates:
    def __init__(self, session_dir: Path):
        self.session_dir = session_dir
        self.errors: list[dict] = []
        self.records: list[dict] = []

    def record(self, rec: dict):
        self.records.append(rec)
        if not rec.get("ok", False):
            self.errors.append(rec)

    def persist(self, manifest: dict, runs_total: int):
        summary = {
            "runs_total": runs_total,
            "runs_recorded": len(self.records),
            "gate_errors": len(self.errors),
            "errors": self.errors,
        }
        (self.session_dir / "gates.json").write_text(
            json.dumps(summary, indent=1) + "\n")
        self.session_dir.joinpath("manifest.json").write_text(
            json.dumps(manifest, indent=1) + "\n")


def bench_run(session_dir: Path, gates: Gates, manifest: dict, run_id: str,
              arm: str, chunk: int, depth: int, transfers: int) -> dict:
    """One measured run under perf; fail-closed."""
    raw_dir = session_dir / "raw"
    cmd = ["perf", "stat", "-x,", "-e", "instructions:u,cycles:u,task-clock",
           "--", str(BENCH), "--run", "--arm", arm, "--chunk", str(chunk),
           "--depth", str(depth), "--transfers", str(transfers),
           "--file-bytes", str(FILE_BYTES),
           "--src", str(DATA_DIR / "src.bin"), "--dst",
           str(DATA_DIR / "dst.bin"), "--label", run_id]
    t0 = time.monotonic()
    try:
        p = subprocess.run(cmd, capture_output=True, text=True)
    except FileNotFoundError:
        rec = {"run_id": run_id, "arm": arm, "chunk": chunk, "depth": depth,
               "transfers": transfers, "wall_driver_s":
                   round(time.monotonic() - t0, 4),
               "perf": {}, "bench_exit": 127, "bench_line": "",
               "gate_fail": "perf_missing", "ok": False}
        gates.record(rec)
        return rec
    tout = p.stdout.strip()
    perf = parse_perf_stat(p.stderr)
    rec = {
        "run_id": run_id, "arm": arm, "chunk": chunk, "depth": depth,
        "transfers": transfers,
        "in_flight_bytes": chunk * depth,
        "registered_bytes": chunk * depth if arm == "U2" else 0,
        "wall_driver_s": round(time.monotonic() - t0, 4),
        "perf": perf, "bench_exit": p.returncode, "bench_line": tout,
        "ok": False,
    }
    try:
        bench = json.loads(tout) if tout else {}
        rec["bench"] = bench
    except json.JSONDecodeError:
        rec["bench"] = None
    if p.returncode != 0:
        rec["gate_fail"] = {4: "registration_lifecycle",
                            5: "probe_failed"}.get(p.returncode, "bench_exit")
        gates.record(rec)
        return rec
    if not rec.get("bench"):
        rec["gate_fail"] = "bench_json_missing"
        gates.record(rec)
        return rec
    if not perf.get("instructions:u") or perf.get("instructions:u") <= 0:
        rec["gate_fail"] = "perf_instructions_missing"
        gates.record(rec)
        return rec
    # Driver-side same-work re-checks (the bench already fail-closed its own;
    # these catch driver/bench contract divergence).
    b = rec["bench"]
    ops = b.get("read_ops", 0) + b.get("write_ops", 0)
    if b.get("canceled", 1) != 0 or b.get("errors", 1) != 0 or \
            b.get("short_reads", 1) != 0 or b.get("short_writes", 1) != 0:
        rec["gate_fail"] = "unexpected_terminal_or_short_io"
        gates.record(rec)
        return rec
    if b.get("cqe_count", 0) != ops:
        rec["gate_fail"] = "cqe_accounting"
        gates.record(rec)
        return rec
    if b.get("bytes_read") != FILE_BYTES * transfers or \
            b.get("bytes_written") != FILE_BYTES * transfers:
        rec["gate_fail"] = "byte_accounting"
        gates.record(rec)
        return rec
    # Causal-isolation fields (U1/U2 must expose identical aligned storage).
    if arm in ("U1", "U2"):
        if b.get("align_remainder") != 0 or \
                b.get("slot_stride") != chunk:
            rec["gate_fail"] = "causal_isolation_storage"
            gates.record(rec)
            return rec
    if arm == "U2" and (b.get("registered_buffers") != depth or
                        b.get("registered_bytes") != chunk * depth):
        rec["gate_fail"] = "registration_table"
        gates.record(rec)
        return rec
    src_hash = manifest["data_src_sha256"]
    dst_hash = sha256_file(DATA_DIR / "dst.bin")
    rec["src_sha256"] = src_hash
    rec["dst_sha256"] = dst_hash
    if dst_hash != src_hash:
        rec["gate_fail"] = "dst_hash_mismatch"
        gates.record(rec)
        return rec
    rec["ok"] = True
    gates.record(rec)
    with (raw_dir / "runs.jsonl").open("a") as f:
        f.write(json.dumps(rec) + "\n")
    with (raw_dir / "perf.csv").open("a") as f:
        f.write(f"{run_id},{perf.get('instructions:u')},"
                f"{perf.get('cycles:u')},{perf.get('task-clock')}\n")
    return rec


def run_plan(combos: list[tuple], rounds: int = ROUNDS,
             seed: int = SEED) -> list[tuple[str, tuple]]:
    """Seeded blocked-interleaved order: each round shuffles ALL combos with
    random.Random(seed + round); run ids r<round>-NNNN in shuffle position."""
    plan = []
    for rnd in range(1, rounds + 1):
        order = combos[:]
        random.Random(seed + rnd).shuffle(order)
        plan.extend((f"r{rnd}-{i:04d}", c) for i, c in enumerate(order))
    return plan


def load_runs(session_id: str) -> list[dict]:
    raw = RESULTS / session_id / "raw" / "runs.jsonl"
    runs = []
    for line in raw.read_text().splitlines():
        if line.strip():
            runs.append(json.loads(line))
    return runs


def memlock_limit_bytes() -> int:
    try:
        import resource
        soft, _ = resource.getrlimit(resource.RLIMIT_MEMLOCK)
        return soft
    except Exception:
        return -1


def u2_feasible(chunk: int, depth: int, memlock: int) -> bool:
    """REGISTRATION-INFEASIBLE iff the U2 registration for the cell meets or
    exceeds the OBSERVED memlock soft limit (probe-verified on Host-0: an
    exactly-at-limit 8 MiB registered-iovec request failed with ENOMEM; the
    kernel's exact extra accounting was not observed)."""
    if memlock < 0:
        return True  # unknown limit -> attempt, fail-closed at runtime
    return chunk * depth < memlock


# --------------------------------------------------------------------------
# subcommands
# --------------------------------------------------------------------------

def cmd_status() -> None:
    print(f"bench binary: {BENCH} exists={BENCH.is_file()}")
    if BENCH.is_file():
        print(f"bench sha256: {sha256_file(BENCH)}")
    p = subprocess.run(["perf", "stat", "-x,", "-e", "instructions:u",
                        "true"], capture_output=True, text=True)
    print(f"perf self-probe: {'OK' if p.returncode == 0 else 'FAIL'}")
    print(f"RLIMIT_MEMLOCK soft: {memlock_limit_bytes()} bytes")
    print(f"liburing: pkg-config={environment_json()['liburing_pkg']}")


def cmd_probe(session_id: str) -> None:
    sd = RESULTS / session_id
    sd.mkdir(parents=True, exist_ok=True)
    tmp = REPO / "build/rbuf-e0-probe"
    tmp.mkdir(parents=True, exist_ok=True)
    p = subprocess.run(
        [str(BENCH), "--probe", "--src", str(tmp / "probe-src"),
         "--dst", str(tmp / "probe-dst")], capture_output=True, text=True)
    print(p.stdout.strip())
    if p.stderr.strip():
        print(p.stderr.strip(), file=sys.stderr)
    try:
        cap = json.loads(p.stdout.strip().splitlines()[-1])
    except Exception:
        cap = {"capable": False, "parse": "FAILED"}
    memlock = memlock_limit_bytes()
    perf_ok = subprocess.run(
        ["perf", "stat", "-x,", "-e", "instructions:u", "true"],
        capture_output=True).returncode == 0
    feasible = [f"{c}x{d}" for c, d in CELLS if u2_feasible(c, d, memlock)]
    infeasible = [f"{c}x{d}" for c, d in CELLS
                  if not u2_feasible(c, d, memlock)]
    print(f"URING_AVAILABLE: "
          f"{'YES' if cap.get('uring_queue_init_errno') == 0 else 'NO'}")
    print(f"REGISTER_BUFFERS_AVAILABLE: "
          f"{'YES' if cap.get('register_errno') == 0 else 'NO'}")
    print(f"FIXED_READ_WRITE_AVAILABLE: "
          f"{'YES' if cap.get('capable') else 'NO'}")
    print(f"MEMLOCK_SUFFICIENT: soft={memlock} "
          f"U2-feasible-cells={feasible} U2-INFEASIBLE-cells={infeasible}")
    print(f"PERF_INSTRUCTIONS: {'YES' if perf_ok else 'NO'}")
    eligible = cap.get("capable") and perf_ok and BENCH.is_file()
    print(f"FORMAL_ELIGIBLE: {'YES' if eligible else 'NO'}")
    (sd / "probe.json").write_text(json.dumps({
        "capability": cap, "memlock_soft_bytes": memlock,
        "perf_instructions": perf_ok,
        "u2_feasible_cells": feasible, "u2_infeasible_cells": infeasible,
        "formal_eligible": bool(eligible),
    }, indent=1) + "\n")
    if not eligible:
        sys.exit("probe: host NOT formally eligible")


def cmd_generate(session_id: str) -> None:
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    if (DATA_DIR / "src.bin").exists():
        sys.exit("src already exists")
    p = subprocess.run([str(BENCH), "--generate", "--src",
                        str(DATA_DIR / "src.bin"),
                        "--file-bytes", str(FILE_BYTES)],
                       capture_output=True, text=True)
    print(p.stdout, p.stderr)
    if p.returncode != 0:
        sys.exit("generate failed")
    sha = sha256_file(DATA_DIR / "src.bin")
    print("src sha256:", sha)
    print("matches CHUNK-E0 canonical fixture:",
          sha == CHUNK_E0_FIXTURE_SHA)


def feasible_cell_plan(memlock: int) -> list[tuple[str, int, int]]:
    """Frozen steady-state combos: all cells x arms, minus U2 on
    REGISTRATION-INFEASIBLE cells (marked before measurement)."""
    combos = []
    for c, d in CELLS:
        for a in ARMS:
            if a == "U2" and not u2_feasible(c, d, memlock):
                continue
            combos.append((a, c, d))
    return combos


def cmd_q0(session_id: str) -> None:
    manifest = {"base": git_state()["head"], "kind": "q0",
                "data_src_sha256": sha256_file(DATA_DIR / "src.bin"),
                "file_bytes": FILE_BYTES, "cell": list(Q0_CELL),
                "arm": "U1", "runs": Q0_RUNS}
    sd = new_session(session_id, "Phase Q0 io_uring stability qualification",
                     manifest)
    gates = Gates(sd)
    chunk, depth = Q0_CELL
    for i in range(Q0_RUNS):
        rec = bench_run(sd, gates, manifest, f"q0-{i:03d}", "U1", chunk,
                        depth, 1)
        bad = rec.get("bench", {}) or {}
        if rec.get("ok") and (bad.get("canceled") or bad.get("errors")):
            rec["ok"] = False  # defensive; bench already fail-closed
        if (i + 1) % 10 == 0:
            print(f"q0 {i + 1}/{Q0_RUNS} "
                  f"({len(gates.errors)} failures so far)", flush=True)
    gates.persist(manifest, runs_total=Q0_RUNS)
    n_ok = sum(1 for r in gates.records if r.get("ok"))
    canceled = sum((r.get("bench") or {}).get("canceled", 0)
                   for r in gates.records)
    errors = sum((r.get("bench") or {}).get("errors", 0)
                 for r in gates.records)
    hash_fail = sum(1 for r in gates.records
                    if r.get("gate_fail") == "dst_hash_mismatch")
    print(f"Q0: runs={Q0_RUNS} valid={n_ok} unexpected_canceled={canceled} "
          f"error_terminals={errors} hash_failures={hash_fail} "
          f"gate_errors={len(gates.errors)}")
    verdict = ("Q0 PASS — single-worker uring path QUALIFIED for RBUF-E0 "
               "(does NOT close #262)" if n_ok == Q0_RUNS
               else "Q0 FAIL — RBUF-E0 STOPPED; #262 becomes blocking")
    print(verdict)
    (sd / "q0-verdict.json").write_text(json.dumps({
        "runs": Q0_RUNS, "valid": n_ok, "unexpected_canceled": canceled,
        "error_terminals": errors, "hash_failures": hash_fail,
        "gate_errors": len(gates.errors),
        "verdict": "PASS" if n_ok == Q0_RUNS else "FAIL",
    }, indent=1) + "\n")
    if n_ok != Q0_RUNS:
        sys.exit("Q0 FAILED")


def cmd_steady(session_id: str, resume: bool = False) -> None:
    memlock = memlock_limit_bytes()
    combos = feasible_cell_plan(memlock)
    current = {"base": git_state()["head"], "kind": "steady",
               "data_src_sha256": sha256_file(DATA_DIR / "src.bin"),
               "rounds": ROUNDS, "cells": [list(c) for c in CELLS],
               "arms": ARMS, "file_bytes": FILE_BYTES,
               "memlock_soft_bytes": memlock,
               "u2_infeasible_cells": [list((x, y)) for (x, y) in CELLS
                                       if not u2_feasible(x, y, memlock)]}
    sd = RESULTS / session_id
    gates = Gates(sd)
    plan = run_plan(combos)
    done: set[str] = set()
    if resume:
        if not (sd / "raw" / "runs.jsonl").is_file():
            sys.exit(f"resume REFUSED: {session_id} has no recorded runs")
        manifest = json.loads((sd / "manifest.json").read_text())
        for k, v in current.items():
            if manifest.get(k) != v:
                sys.exit(f"resume REFUSED: manifest {k}: stored "
                         f"{manifest.get(k)} != current {v}")
        done = {r["run_id"] for r in load_runs(session_id)}
        gates.errors = list(json.loads((sd / "gates.json").read_text())
                            .get("errors", []))
    else:
        manifest = current
        sd = new_session(session_id, "frozen steady-state matrix", manifest)
    todo = [(rid, c) for rid, c in plan if rid not in done]
    print(f"steady: {len(todo)} runs "
          f"(cells x arms = {len(combos)} combos x {ROUNDS} rounds)"
          + (f" (resuming; {len(done)} done)" if resume else ""), flush=True)
    last_of_round = {}
    for rid, _ in plan:
        last_of_round[rid.split("-")[0]] = rid
    for rid, (arm, chunk, depth) in todo:
        bench_run(sd, gates, manifest, rid, arm, chunk, depth, 1)
        if rid == last_of_round[rid.split("-")[0]]:
            print(f"round {rid.split('-')[0][1:]}/{ROUNDS} done "
                  f"({len(gates.errors)} gate errors so far)", flush=True)
    gates.persist(manifest | {"runs_total": len(plan)}, runs_total=len(plan))
    print(f"steady: {len(plan)} runs, {len(gates.errors)} gate errors")
    if len(gates.errors) > 0:
        sys.exit("steady FAILED (gate errors present)")


def cmd_amort(session_id: str, resume: bool = False) -> None:
    chunk, depth = PRIMARY_CELL
    memlock = memlock_limit_bytes()
    if not u2_feasible(chunk, depth, memlock):
        sys.exit("amort: primary cell U2-infeasible on this host")
    combos = [(h, a) for h in HORIZONS for a in ("U1", "U2")]
    current = {"base": git_state()["head"], "kind": "amortization",
               "data_src_sha256": sha256_file(DATA_DIR / "src.bin"),
               "rounds": ROUNDS, "horizons": HORIZONS,
               "cell": [chunk, depth], "file_bytes": FILE_BYTES,
               "memlock_soft_bytes": memlock}
    sd = RESULTS / session_id
    gates = Gates(sd)
    plan = run_plan(combos)
    done: set[str] = set()
    if resume:
        if not (sd / "raw" / "runs.jsonl").is_file():
            sys.exit(f"resume REFUSED: {session_id} has no recorded runs")
        manifest = json.loads((sd / "manifest.json").read_text())
        for k, v in current.items():
            if manifest.get(k) != v:
                sys.exit(f"resume REFUSED: manifest {k}: stored "
                         f"{manifest.get(k)} != current {v}")
        done = {r["run_id"] for r in load_runs(session_id)}
        gates.errors = list(json.loads((sd / "gates.json").read_text())
                            .get("errors", []))
    else:
        manifest = current
        sd = new_session(session_id,
                         "frozen reuse-horizon amortization experiment",
                         manifest)
    todo = [(rid, c) for rid, c in plan if rid not in done]
    print(f"amort: {len(todo)} runs (horizons x arms = {len(combos)} combos "
          f"x {ROUNDS} rounds)"
          + (f" (resuming; {len(done)} done)" if resume else ""), flush=True)
    last_of_round = {}
    for rid, _ in plan:
        last_of_round[rid.split("-")[0]] = rid
    for rid, (h, arm) in todo:
        bench_run(sd, gates, manifest, rid, arm, chunk, depth, h)
        if rid == last_of_round[rid.split("-")[0]]:
            print(f"round {rid.split('-')[0][1:]}/{ROUNDS} done "
                  f"({len(gates.errors)} gate errors so far)", flush=True)
    gates.persist(manifest | {"runs_total": len(plan)}, runs_total=len(plan))
    print(f"amort: {len(plan)} runs, {len(gates.errors)} gate errors")
    if len(gates.errors) > 0:
        sys.exit("amort FAILED (gate errors present)")


def material(u1_walls: list, u2_walls: list) -> dict:
    """Frozen materiality rule: ratio >= 1.03 AND robust 1.5*MAD separation
    (U2 median + 1.5*MAD < U1 median - 1.5*MAD)."""
    m1, m2 = median(u1_walls), median(u2_walls)
    d1, d2 = mad(u1_walls, m1), mad(u2_walls, m2)
    ratio = m1 / m2 if m2 else float("inf")
    separated = (m2 + MATERIAL_MAD_K * d2) < (m1 - MATERIAL_MAD_K * d1)
    return {
        "u1_median_ns": m1, "u1_mad_ns": d1,
        "u2_median_ns": m2, "u2_mad_ns": d2,
        "ratio_u1_over_u2": round(ratio, 4),
        "mad_separated": bool(separated),
        "material": bool(ratio >= MATERIAL_RATIO and separated),
    }


def cmd_summarize(session_id: str) -> None:
    sd = RESULTS / session_id
    runs = [r for r in load_runs(session_id) if r.get("ok")]
    gates = json.loads((sd / "gates.json").read_text())

    # ---- steady-state per cell x arm ----
    sd_manifest = json.loads((sd / "manifest.json").read_text())
    session_kind = sd_manifest.get("kind")
    steady: dict[tuple, list[dict]] = {}
    amort: dict[tuple, list[dict]] = {}
    for r in runs:
        # Route by the session's frozen kind, not by transfer count: an
        # H=1 run inside the amortization session is amortization data.
        key = (r["arm"], r["chunk"], r["depth"])
        if session_kind == "amortization":
            amort.setdefault((r["arm"], r["transfers"]), []).append(r)
        elif r["transfers"] == 1:
            steady.setdefault(key, []).append(r)
        else:
            amort.setdefault((r["arm"], r["transfers"]), []).append(r)

    def wall(r: dict) -> int:
        return r["bench"]["transfer_ns"][0]

    def mibps(ns: float) -> float:
        return FILE_BYTES / ns * 1e9 / (1 << 20)

    def ins_per_byte(r: dict) -> float:
        ins = (r.get("perf") or {}).get("instructions:u") or 0
        return ins / (FILE_BYTES * r["transfers"]) if ins else 0

    cell_rows = []
    cell_stats = {}
    for (chunk, depth) in CELLS:
        entry = {"chunk": chunk, "depth": depth,
                 "in_flight_mib": round(chunk * depth / (1 << 20), 1)}
        cstat = {}
        for arm in ARMS:
            rs = steady.get((arm, chunk, depth), [])
            if not rs:
                entry[arm.lower()] = None
                continue
            walls = [wall(r) for r in rs]
            ib = [ins_per_byte(r) for r in rs]
            stats = {
                "n": len(rs),
                "wall_ns_median": median(walls),
                "wall_ns_mad": mad(walls, median(walls)),
                "mibps_median": round(mibps(median(walls)), 1),
                "instructions_per_byte_median": round(median(ib), 4),
                "cpu_us_median": median(
                    [(r["bench"]["utime_us"] + r["bench"]["stime_us"])
                     for r in rs]),
                "maxrss_kb_median": median(
                    [r["bench"]["maxrss_kb"] for r in rs]),
                "minflt_median": median([r["bench"]["minflt"] for r in rs]),
            }
            cstat[arm] = stats
            entry[arm.lower()] = stats
        if "U1" in cstat and "U2" in cstat:
            m = material([wall(r) for r in steady[("U1", chunk, depth)]],
                         [wall(r) for r in steady[("U2", chunk, depth)]])
            entry["u1_vs_u2"] = m
            cstat["materiality"] = m
        cell_rows.append(entry)
        cell_stats[f"{chunk}x{depth}"] = cstat

    primary = cell_stats.get(f"{PRIMARY_CELL[0]}x{PRIMARY_CELL[1]}", {})
    pmat = primary.get("materiality")
    neighbors = [(1_048_576, 2), (2_097_152, 1), (2_097_152, 4),
                 (4_194_304, 2)]
    neighbor_material = []
    for c, d in neighbors:
        m = cell_stats.get(f"{c}x{d}", {}).get("materiality")
        if m:
            neighbor_material.append({"cell": f"{c}x{d}", **m})
    consistent = bool(pmat and pmat["material"] and any(
        n["material"] for n in neighbor_material))

    if pmat is None:
        steady_verdict = "REGISTERED BUFFER MIXED / UNSTABLE (no U2 data)"
    elif pmat["material"] and consistent:
        steady_verdict = "REGISTERED BUFFER STEADY-STATE MATERIAL"
    elif pmat["material"]:
        steady_verdict = "REGISTERED BUFFER REGIME-SPECIFIC"
    elif any(n["material"] for n in neighbor_material):
        steady_verdict = "REGISTERED BUFFER MIXED / UNSTABLE"
    else:
        steady_verdict = "REGISTERED BUFFER STEADY-STATE NOT MATERIAL"

    # ---- lifecycle ----
    u2_steady = steady.get(("U2",) + PRIMARY_CELL, [])
    u1_steady = steady.get(("U1",) + PRIMARY_CELL, [])
    lifecycle = {}
    for arm, rs in (("U1", u1_steady), ("U2", u2_steady)):
        if rs:
            lifecycle[arm] = {
                "alloc_ns_median":
                    median([r["bench"]["alloc_ns"] for r in rs]),
                "register_ns_median":
                    median([r["bench"]["register_ns"] for r in rs]) if
                    arm == "U2" else None,
                "unregister_ns_median":
                    median([r["bench"]["unregister_ns"] for r in rs]) if
                    arm == "U2" else None,
                "setup_ns_median":
                    median([r["bench"]["setup_ns"] for r in rs]),
                "teardown_ns_median":
                    median([r["bench"]["teardown_ns"] for r in rs]),
                "registered_bytes":
                    PRIMARY_CELL[0] * PRIMARY_CELL[1] if arm == "U2" else 0,
            }
    manifest = json.loads((sd / "manifest.json").read_text())

    # ---- amortization ----
    horizon_rows = []
    e2e = {}
    for h in HORIZONS:
        row = {"horizon_transfers": h}
        for arm in ("U1", "U2"):
            rs = amort.get((arm, h), [])
            if not rs:
                row[arm.lower()] = None
                continue
            costs = [r["bench"]["setup_ns"] + r["bench"]["teardown_ns"] +
                     sum(r["bench"]["transfer_ns"]) for r in rs]
            am = [c / h for c in costs]
            row[arm.lower()] = {
                "n": len(rs),
                "end_to_end_ns_median": median(costs),
                "amortized_per_transfer_ns_median": median(am),
                "steady_per_transfer_ns_median":
                    median([median(r["bench"]["transfer_ns"]) for r in rs]),
            # Fraction of the measured end-to-end span occupied by the
            # setup+teardown REGION. On this host that region is dominated
            # by the filesystem/dirty-page/close teardown stall, NOT by the
            # registration lifecycle (reported as absolute register_ns /
            # unregister_ns in `lifecycle`). The name states the formula.
            "setup_plus_teardown_fraction":
                round(median([r["bench"]["setup_ns"] +
                              r["bench"]["teardown_ns"] for r in rs]) /
                      median(costs), 5) if median(costs) else 0,
            }
            e2e[(arm, h)] = costs
        if ("U1", h) in e2e and ("U2", h) in e2e:
            m = material(e2e[("U1", h)], e2e[("U2", h)])
            row["u1_vs_u2_end_to_end"] = m
        horizon_rows.append(row)

    crossover_h = None
    amort_verdict = None
    if not amort:
        # Claim hygiene: a steady-state session has no reuse-horizon runs;
        # the amortization verdict belongs to the amortization session.
        amort_verdict = "N/A — no amortization runs in this session"
    else:
        for row in sorted(horizon_rows, key=lambda x: x["horizon_transfers"]):
            m = row.get("u1_vs_u2_end_to_end")
            if m and m["material"]:
                crossover_h = row["horizon_transfers"]
                break
        if crossover_h is not None:
            amort_verdict = f"AMORTIZATION CROSSOVER LOCATED @ H={crossover_h}"
        else:
            any_benefit = any(
                (row.get("u1_vs_u2_end_to_end") or {}).get(
                    "ratio_u1_over_u2", 0) > 1.0 for row in horizon_rows)
            amort_verdict = ("REGISTRATION NEVER RECOVERS SETUP COST "
                             "IN TESTED RANGE" if not any_benefit else
                             "AMORTIZATION CROSSOVER NOT LOCATED "
                             "IN TESTED RANGE")

    analysis = {
        "sessions": [session_id],
        "file_bytes": FILE_BYTES,
        "gates": {"gate_errors": gates.get("gate_errors", -1)},
        "rule": {"material_ratio": MATERIAL_RATIO,
                 "material_mad_k": MATERIAL_MAD_K},
        "primary_cell": list(PRIMARY_CELL),
        "cells": cell_rows,
        "neighbor_consistency": {
            "neighbors": neighbor_material,
            "consistent": consistent,
        },
        "steady_state_verdict": steady_verdict,
        "lifecycle": lifecycle,
        "u2_infeasible_cells": manifest.get("u2_infeasible_cells", []),
        "amortization": {"horizons": horizon_rows,
                         "crossover_horizon": crossover_h,
                         "verdict": amort_verdict},
    }
    # summary.csv (per cell x arm medians)
    cols = ["chunk", "depth", "arm", "n", "wall_ns_median", "wall_ns_mad",
            "mibps_median", "instructions_per_byte_median", "cpu_us_median",
            "maxrss_kb_median", "minflt_median"]
    with (sd / "summary.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols, lineterminator="\n")
        w.writeheader()
        for row in cell_rows:
            for arm in ARMS:
                s = row.get(arm.lower())
                if s:
                    w.writerow({"chunk": row["chunk"], "depth": row["depth"],
                                "arm": arm, **{k: s[k] for k in cols[3:]}})
    with (sd / "summary.json").open("w") as f:
        json.dump(cell_rows, f, indent=1)
    (sd / "analysis.json").write_text(json.dumps(analysis, indent=1) + "\n")
    print(json.dumps({
        "runs": len(runs),
        "gate_errors": gates.get("gate_errors", -1),
        "primary_2Mx2_u1_vs_u2": pmat,
        "steady_state_verdict": steady_verdict,
        "neighbor_consistency": consistent,
        "amortization_verdict": amort_verdict,
        "crossover_horizon": crossover_h,
    }, indent=1))


def usage() -> None:
    print(__doc__)
    sys.exit(1)


def main() -> None:
    if len(sys.argv) < 2:
        usage()
    cmd = sys.argv[1]
    if cmd == "status":
        cmd_status()
    elif cmd == "probe" and len(sys.argv) == 3:
        cmd_probe(sys.argv[2])
    elif cmd == "generate" and len(sys.argv) == 3:
        cmd_generate(sys.argv[2])
    elif cmd == "q0" and len(sys.argv) == 3:
        cmd_q0(sys.argv[2])
    elif cmd == "steady" and len(sys.argv) in (3, 4) and \
            (len(sys.argv) == 3 or sys.argv[3] == "--resume"):
        cmd_steady(sys.argv[2], resume=len(sys.argv) == 4)
    elif cmd == "amort" and len(sys.argv) in (3, 4) and \
            (len(sys.argv) == 3 or sys.argv[3] == "--resume"):
        cmd_amort(sys.argv[2], resume=len(sys.argv) == 4)
    elif cmd == "summarize" and len(sys.argv) == 3:
        cmd_summarize(sys.argv[2])
    else:
        usage()


if __name__ == "__main__":
    main()
