#!/usr/bin/env python3
"""g1_control_c0.py — G1-CONTROL-C0 campaign driver (#279).

Runs the frozen preregistration (research/g1-control-c0/
G1-CONTROL-C0-PREREGISTRATION.md) on Host-0 via the research-only
direct-liburing mechanism bench (bench/g1_control_c0_bench.cpp; production
code untouched).

Subcommands:
  status                bench binary / perf / memlock quick check
  probe <session-id>    capability preflight (behavioral fixed-file
                        round-trip, feature flags, memlock, perf probe)
                        + FILE-ID-E0 deterministic identity witness +
                        replacement-window probe (AUDIT boundaries A/D)
  generate <session-id> create fixtures for BOTH filesystems (tmpfs +
                        btrfs) at both sizes (512 MiB for 4 KiB cells,
                        1 GiB for 64 KiB / 2 MiB cells), validate the C++
                        pattern generator against the Python generator,
                        freeze the expected dst sha256 constants
  q0 <session-id>       Phase Q0 stability qualification: 30 runs of F0 at
                        4 KiB x d8 on tmpfs READ, full same-work gates.
  formal <session-id>   frozen matrix: (op x 5 cells x 2 fs x 4 arms) x 7
                        seeded-interleaved rounds = 560 runs, perf-wrapped
  summarize <session-id> runs.jsonl -> summary + analysis (per-cell
                        wall/op medians, F0-vs-F1 materiality per frozen
                        rule, neighbor consistency, threaded arms,
                        verdict vocabulary per prereg)

Immutable session layout (mirrors research/rbuf-e0):
  results/<session-id>/{environment.json, manifest.json, gates.json,
                        notes.md, raw/runs.jsonl, raw/perf.csv,
                        raw/probe.json, raw/fileid.json,
                        raw/replacement-window.json, summary.csv,
                        summary.json, analysis.json}
"""

from __future__ import annotations

import hashlib
import json
import os
import random
import struct
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
DATA_ROOT = Path(os.environ["G1C0_DATA_DIR"]) if "G1C0_DATA_DIR" in os.environ \
    else REPO / "build/g1-control-c0-data"
BENCH = Path(os.environ["G1C0_BENCH"]) if "G1C0_BENCH" in os.environ else \
    REPO / "build/linux/x86_64/release/g1_control_c0_bench"
RESULTS = Path(os.environ["G1C0_RESULTS"]) if "G1C0_RESULTS" in os.environ \
    else REPO / "research/g1-control-c0/results"

SEED = 0xE1E1E1E121212121
ROUNDS = 7
THREADED_WORKERS = 4
Q0_RUNS = 30
Q0_CELL = (4096, 8)
MATERIAL_RATIO = 1.03
MATERIAL_MAD_K = 1.5

OPS = ["READ", "WRITE"]
SIZES = [4096, 65536, 2097152]
# size -> depth set (prereg §6 shrink)
DEPTHS_BY_SIZE = {4096: [1, 8, 32], 65536: [1], 2097152: [1]}
FS = ["tmpfs", "btrfs"]
ARMS = ["F0", "F1", "F0-T", "F1-T"]
FILE_BYTES = {4096: 512 * 1024 * 1024, 65536: 1 << 30, 2097152: 1 << 30}

KBLOCK = 4096


# ---- deterministic pattern (must match bench/g1_control_c0_bench.cpp) ----

def splitmix64(x: int) -> int:
    x = (x + 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF
    x = ((x ^ (x >> 30)) * 0xBF58476D1CE4E5B9) & 0xFFFFFFFFFFFFFFFF
    x = ((x ^ (x >> 27)) * 0x94D049BB133111EB) & 0xFFFFFFFFFFFFFFFF
    return x ^ (x >> 31)


def master_tile() -> bytes:
    out = bytearray()
    for i in range(KBLOCK // 8):
        out += struct.pack("<Q", splitmix64(SEED + i) & 0xFFFFFFFFFFFFFFFF)
    return bytes(out)


def pattern_bytes(length: int, tile: bytes) -> bytes:
    """Deterministic per-offset pattern: file byte at offset o ==
    tile[o % 4096] (must match bench fill_pattern)."""
    reps, rem = divmod(length, KBLOCK)
    return tile * reps + tile[:rem]


def sha256_bytes(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


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
    return {"head": run("git", "rev-parse", "HEAD"),
            "branch": run("git", "branch", "--show-current"),
            "prereg_sha": run("git", "rev-parse", "HEAD:research/g1-control-c0/"
                             "G1-CONTROL-C0-PREREGISTRATION.md"),
            "dirty": bool(run("git", "status", "--porcelain"))}


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
        "distribution": run("cat", "/etc/os-release")
        if Path("/etc/os-release").is_file() else "?",
        "cpu_model": [l for l in lscpu.splitlines()
                      if l.startswith("Model name")],
        "cpu_topology": [l for l in lscpu.splitlines()
                         if l.startswith(("CPU(s)", "Thread", "Core",
                                          "Socket", "NUMA"))],
        "meminfo": {k: v.strip() for k, v in (
            l.split(":", 1) for l in Path("/proc/meminfo").read_text()
            .splitlines() if l.startswith(("MemTotal", "MemAvailable")))},
        "page_size": run("getconf", "PAGESIZE"),
        "filesystems": {fs: {"type": run("findmnt", "-no", "FSTYPE", "-T",
                                         str(DATA_ROOT / fs)),
                             "opts": run("findmnt", "-no", "OPTIONS", "-T",
                                         str(DATA_ROOT / fs)),
                             "source": run("findmnt", "-no", "SOURCE", "-T",
                                           str(DATA_ROOT / fs))}
                        for fs in FS},
        "governor": run("cat", "/sys/devices/system/cpu/cpu0/cpufreq/"
                        "scaling_governor"),
        "no_turbo": run("cat", "/sys/devices/system/cpu/intel_pstate/no_turbo"),
        "glibc": (run("ldd", "--version").splitlines() or ["?"])[0],
        "clang": (run("clang", "--version").splitlines() or ["?"])[0],
        "liburing_pkg": run("pkg-config", "--modversion", "liburing"),
        "perf": run("perf", "--version"),
        "perf_paranoid": run("cat", "/proc/sys/kernel/perf_event_paranoid"),
        "io_uring_disabled": run("cat", "/proc/sys/kernel/io_uring_disabled"),
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
    (sd / "environment.json").write_text(json.dumps(env, indent=1) + "\n")
    manifest["purpose"] = purpose
    manifest["preregistration"] = \
        "research/g1-control-c0/G1-CONTROL-C0-PREREGISTRATION.md (FROZEN)"
    manifest["prereg_sha"] = env["git"]["prereg_sha"]
    manifest["data_dir"] = str(DATA_ROOT)
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
        summary = {"runs_total": runs_total,
                   "runs_recorded": len(self.records),
                   "gate_errors": len(self.errors),
                   "errors": self.errors}
        (self.session_dir / "gates.json").write_text(
            json.dumps(summary, indent=1) + "\n")
        (self.session_dir / "manifest.json").write_text(
            json.dumps(manifest, indent=1) + "\n")


def data_paths(fs: str, size: int):
    d = DATA_ROOT / fs
    return d / f"src-{size}.bin", d / f"dst-{size}.bin"


def bench_run(session_dir: Path, gates: Gates, manifest: dict, run_id: str,
              op: str, arm: str, size: int, depth: int, fs: str) -> dict:
    """One measured run under perf; fail-closed. dst hashed post-exit for
    WRITE runs."""
    raw_dir = session_dir / "raw"
    src, dst = data_paths(fs, size)
    file_bytes = FILE_BYTES[size]
    cmd = ["perf", "stat", "-x,", "-e", "instructions:u,cycles:u,task-clock",
           "--", str(BENCH), "--mode", "run", "--op", op, "--arm", arm,
           "--size", str(size), "--depth", str(depth),
           "--file-bytes", str(file_bytes),
           "--src", str(src), "--dst", str(dst), "--label", run_id]
    t0 = time.monotonic()
    try:
        p = subprocess.run(cmd, capture_output=True, text=True)
    except FileNotFoundError:
        rec = {"run_id": run_id, "op": op, "arm": arm, "size": size,
               "depth": depth, "fs": fs,
               "wall_driver_s": round(time.monotonic() - t0, 4),
               "perf": {}, "bench_exit": 127, "bench_line": "",
               "gate_fail": "perf_missing", "ok": False}
        gates.record(rec)
        return rec
    tout = p.stdout.strip()
    perf = parse_perf_stat(p.stderr)
    rec = {"run_id": run_id, "op": op, "arm": arm, "size": size,
           "depth": depth, "fs": fs,
           "wall_driver_s": round(time.monotonic() - t0, 4),
           "perf": perf, "bench_exit": p.returncode, "bench_line": tout,
           "ok": False}
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
    b = rec["bench"]
    if b.get("canceled", 1) != 0 or b.get("errors", 1) != 0 or \
            b.get("short_reads", 1) != 0 or b.get("short_writes", 1) != 0:
        rec["gate_fail"] = "unexpected_terminal_or_short_io"
        gates.record(rec)
        return rec
    if b.get("cqe_count", 0) != b.get("chunks", 0):
        rec["gate_fail"] = "cqe_accounting"
        gates.record(rec)
        return rec
    if b.get("bytes_read") != (file_bytes if op == "READ" else 0) or \
            b.get("bytes_written") != (file_bytes if op == "WRITE" else 0):
        rec["gate_fail"] = "byte_accounting"
        gates.record(rec)
        return rec
    if b.get("align_remainder") != 0 or b.get("slot_stride") != size:
        rec["gate_fail"] = "causal_isolation_storage"
        gates.record(rec)
        return rec
    if arm in ("F1", "F1-T") and b.get("registered_files") != 1:
        rec["gate_fail"] = "registration_table"
        gates.record(rec)
        return rec
    if arm in ("F0-T", "F1-T") and (
            b.get("threads_spawned") != THREADED_WORKERS or
            b.get("threads_io_ok") != THREADED_WORKERS or
            b.get("threads_joined") != THREADED_WORKERS):
        rec["gate_fail"] = "threaded_condition"
        gates.record(rec)
        return rec
    # WRITE: dst must hash to the frozen per-cell constant (pattern is
    # per-offset, independent of depth/arm -> one constant per size).
    if op == "WRITE":
        key = f"expected_dst_sha256_{size}"
        expected = manifest.get(key)
        if not expected:
            rec["gate_fail"] = "expected_dst_hash_missing"
            gates.record(rec)
            return rec
        actual = sha256_file(dst)
        rec["dst_sha256"] = actual
        if actual != expected:
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


def run_plan(combos: list, rounds: int = ROUNDS, seed: int = SEED):
    """Seeded blocked-interleaved order (RBUF-E0 convention)."""
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


# ---- subcommands --------------------------------------------------------

def cmd_status() -> None:
    print(f"bench binary: {BENCH} exists={BENCH.is_file()}")
    if BENCH.is_file():
        print(f"bench sha256: {sha256_file(BENCH)}")
    p = subprocess.run(["perf", "stat", "-x,", "-e", "instructions:u", "true"],
                       capture_output=True, text=True)
    print(f"perf self-probe: {'OK' if p.returncode == 0 else 'FAIL'}")
    print(f"RLIMIT_MEMLOCK soft: {memlock_limit_bytes()} bytes")
    print(f"liburing: pkg-config={environment_json()['liburing_pkg']}")


def cmd_probe(session_id: str) -> None:
    sd = RESULTS / session_id
    sd.mkdir(parents=True, exist_ok=True)
    tmp = DATA_ROOT / "probe"
    tmp.mkdir(parents=True, exist_ok=True)
    p = subprocess.run([str(BENCH), "--mode", "probe",
                        "--src", str(tmp), "--dst", str(tmp)],
                       capture_output=True, text=True)
    print(p.stdout.strip())
    if p.stderr.strip():
        print(p.stderr.strip(), file=sys.stderr)
    try:
        cap = json.loads(p.stdout.strip().splitlines()[-1])
    except Exception:
        cap = {"capable": False, "parse": "FAILED"}
    fi = subprocess.run([str(BENCH), "--mode", "fileid",
                         "--src", str(tmp), "--dst", str(tmp)],
                        capture_output=True, text=True)
    print(fi.stdout.strip())
    if fi.stderr.strip():
        print(fi.stderr.strip(), file=sys.stderr)
    try:
        fileid = json.loads(fi.stdout.strip().splitlines()[-1])
    except Exception:
        fileid = {"parse": "FAILED"}
    rw = subprocess.run([str(BENCH), "--mode", "replacement-window",
                         "--src", str(tmp), "--dst", str(tmp)],
                        capture_output=True, text=True)
    print(rw.stdout.strip())
    if rw.stderr.strip():
        print(rw.stderr.strip(), file=sys.stderr)
    try:
        window = json.loads(rw.stdout.strip().splitlines()[-1])
    except Exception:
        window = {"parse": "FAILED"}
    memlock = memlock_limit_bytes()
    perf_ok = subprocess.run(
        ["perf", "stat", "-x,", "-e", "instructions:u", "true"],
        capture_output=True).returncode == 0
    eligible = cap.get("capable") and perf_ok and BENCH.is_file()
    (sd / "raw").mkdir(exist_ok=True)
    (sd / "raw" / "probe.json").write_text(json.dumps(cap, indent=1) + "\n")
    (sd / "raw" / "fileid.json").write_text(json.dumps(fileid, indent=1) + "\n")
    (sd / "raw" / "replacement-window.json").write_text(
        json.dumps(window, indent=1) + "\n")
    (sd / "probe.json").write_text(json.dumps({
        "capability": cap, "fileid": fileid, "replacement_window": window,
        "memlock_soft_bytes": memlock, "perf_instructions": perf_ok,
        "formal_eligible": bool(eligible),
    }, indent=1) + "\n")
    print(f"FILE-ID-E0: {fileid.get('ordinary_verdict', '?')} / "
          f"{fileid.get('fixed_verdict', '?')}")
    print(f"WINDOW: {window.get('boundary_a_verdict', '?')} / "
          f"{window.get('boundary_d_verdict', '?')}")
    print(f"FORMAL_ELIGIBLE: {'YES' if eligible else 'NO'}")


def cmd_generate(session_id: str) -> None:
    sd = RESULTS / session_id
    sd.mkdir(parents=True, exist_ok=True)
    tile = master_tile()
    expected = {}
    for fs in FS:
        d = DATA_ROOT / fs
        d.mkdir(parents=True, exist_ok=True)
    for size in SIZES:
        fb = FILE_BYTES[size]
        pat = pattern_bytes(fb, tile)
        pat_sha = sha256_bytes(pat)
        expected[f"expected_dst_sha256_{size}"] = pat_sha
        for fs in FS:
            src, dst = data_paths(fs, size)
            if not src.is_file() or src.stat().st_size != fb:
                subprocess.run([str(BENCH), "--mode", "generate",
                                "--src", str(src),
                                "--file-bytes", str(fb)], check=True)
            actual = sha256_file(src)
            if actual != pat_sha:
                print(f"GENERATOR MISMATCH {fs} src-{size}: C++ {actual} != "
                      f"Python {pat_sha}", file=sys.stderr)
                sys.exit(1)
            print(f"src {fs}/{size}: sha256 {actual} (matches Python "
                  f"generator)")
    (sd / "fixtures.json").write_text(json.dumps(expected, indent=1) + "\n")
    print(f"expected dst hashes frozen: {expected}")
    # persist (or merge into) the session manifest so q0/formal can gate on
    # the frozen constants without re-derivation.
    mf = RESULTS / session_id / "manifest.json"
    if mf.is_file():
        manifest = json.loads(mf.read_text())
        manifest.update(expected)
    else:
        manifest = {"purpose": "generate", **expected}
    (mf).write_text(json.dumps(manifest, indent=1) + "\n")
    env = environment_json()
    (RESULTS / session_id / "environment.json").write_text(
        json.dumps(env, indent=1) + "\n")


def cmd_q0(session_id: str, resume: bool = False) -> None:
    sd = RESULTS / session_id
    if not sd.is_dir():
        print(f"session {session_id} does not exist; run probe first",
              file=sys.stderr)
        sys.exit(1)
    manifest = json.loads((sd / "manifest.json").read_text())
    gates = Gates(sd)
    size, depth = Q0_CELL
    for i in range(1, Q0_RUNS + 1):
        bench_run(sd, gates, manifest, f"q0-{i:02d}", "READ", "F0", size,
                  depth, "tmpfs")
    gates.persist(manifest, Q0_RUNS)
    if gates.errors:
        print(f"Q0 FAIL: {len(gates.errors)} gate errors -> C0-PERF STOPPED; "
              f"#262 becomes blocking")
        for e in gates.errors[:5]:
            print(f"  {e.get('run_id')}: {e.get('gate_fail')}")
        sys.exit(2)
    print(f"Q0 PASS: {Q0_RUNS}/{Q0_RUNS} valid, 0 gate errors -> "
          f"single-worker uring path QUALIFIED")


def cmd_formal(session_id: str, resume: bool = False) -> None:
    sd = RESULTS / session_id
    if not sd.is_dir():
        print(f"session {session_id} does not exist; run probe first",
              file=sys.stderr)
        sys.exit(1)
    manifest = json.loads((sd / "manifest.json").read_text())
    # expected dst hashes must be frozen
    for size in SIZES:
        key = f"expected_dst_sha256_{size}"
        if not manifest.get(key):
            if (sd / "fixtures.json").is_file():
                manifest.update(json.loads((sd / "fixtures.json").read_text()))
            else:
                print(f"{key} not frozen; run generate first", file=sys.stderr)
                sys.exit(1)
    gates = Gates(sd)
    combos = [(op, size, depth, fs, arm)
              for op in OPS
              for size, depths in DEPTHS_BY_SIZE.items()
              for depth in depths
              for fs in FS
              for arm in ARMS]
    plan = run_plan(combos)
    done_ids = {r.get("run_id") for r in
                (load_runs(session_id) if (sd / "raw" / "runs.jsonl").is_file()
                 else [])}
    for run_id, (op, size, depth, fs, arm) in plan:
        if resume and run_id in done_ids:
            continue
        bench_run(sd, gates, manifest, run_id, op, arm, size, depth, fs)
    gates.persist(manifest, len(plan))
    if gates.errors:
        print(f"FORMAL session has {len(gates.errors)} gate errors")
        for e in gates.errors[:5]:
            print(f"  {e.get('run_id')}: {e.get('gate_fail')}")
        sys.exit(2)
    print(f"FORMAL complete: {len(plan)} runs, 0 gate errors")


# ---- analysis -----------------------------------------------------------

def material(f0_vals: list, f1_vals: list) -> dict:
    """Frozen materiality rule (prereg §13). F1 faster if ratio > 1."""
    if not f0_vals or not f1_vals:
        return {"ratio": None, "material": False, "regression": False,
                "direction": None}
    m0, m1 = median(f0_vals), median(f1_vals)
    mad0, mad1 = mad(f0_vals, m0), mad(f1_vals, m1)
    ratio = m0 / m1 if m1 else float("inf")
    benefit = ratio >= MATERIAL_RATIO and \
        m1 + MATERIAL_MAD_K * mad1 < m0 - MATERIAL_MAD_K * mad0
    regression = ratio <= 1.0 / MATERIAL_RATIO and \
        m0 + MATERIAL_MAD_K * mad0 < m1 - MATERIAL_MAD_K * mad1
    return {"f0_median_ns": m0, "f1_median_ns": m1,
            "f0_mad_ns": mad0, "f1_mad_ns": mad1,
            "ratio": round(ratio, 4),
            "benefit": bool(benefit), "regression": bool(regression),
            "direction": "F1_FASTER" if benefit else
                         ("F1_SLOWER" if regression else "NONE")}


def cmd_summarize(session_id: str) -> None:
    sd = RESULTS / session_id
    runs = load_runs(session_id)
    if not runs:
        print(f"no runs in {session_id}", file=sys.stderr)
        sys.exit(1)
    # Q0 qualification runs share the READ 4K d8 tmpfs F0 cell signature but
    # are NOT part of the formal matrix (prereg §12); exclude them.
    runs = [r for r in runs if not r["run_id"].startswith("q0-")]
    if not runs:
        print(f"no formal runs in {session_id}", file=sys.stderr)
        sys.exit(1)
    manifest = json.loads((sd / "manifest.json").read_text())
    gates = json.loads((sd / "gates.json").read_text())
    cells = {}
    for r in runs:
        if not r.get("ok"):
            continue
        key = (r["op"], r["size"], r["depth"], r["fs"], r["arm"])
        cells.setdefault(key, []).append(r["bench"]["wall_per_op_ns"])
    all_keys = {(op, size, depth, fs, arm)
                for op in OPS for size in SIZES
                for depth in DEPTHS_BY_SIZE[size] for fs in FS for arm in ARMS}
    missing = sorted(all_keys - set(cells))
    summary = {"session": session_id, "runs_total": len(runs),
               "runs_ok": len([r for r in runs if r.get("ok")]),
               "gate_errors": len(gates.get("errors", [])),
               "cells_missing": missing,
               "per_cell": {}}
    for op in OPS:
        for size in SIZES:
            for depth in DEPTHS_BY_SIZE[size]:
                for fs in FS:
                    f0 = cells.get((op, size, depth, fs, "F0"), [])
                    f1 = cells.get((op, size, depth, fs, "F1"), [])
                    f0t = cells.get((op, size, depth, fs, "F0-T"), [])
                    f1t = cells.get((op, size, depth, fs, "F1-T"), [])
                    m = material(f0, f1)
                    mt = material(f0t, f1t)
                    summary["per_cell"][f"{op}_{size}_{depth}_{fs}"] = {
                        "f0": m, "threaded": mt,
                        "n": len(f0) + len(f1) + len(f0t) + len(f1t),
                    }
    # primary cells: 4 KiB family on tmpfs
    prim = []
    for depth in DEPTHS_BY_SIZE[4096]:
        c = summary["per_cell"].get(f"READ_4096_{depth}_tmpfs", {})
        prim.append({"cell": f"READ 4K d{depth} tmpfs",
                     **{k: c.get("f0", {}).get(k) for k in
                        ("ratio", "benefit", "regression", "direction")}})
    neighbor_support = {}
    for op in OPS:
        for depth in DEPTHS_BY_SIZE[4096]:
            d = summary["per_cell"][f"{op}_4096_{depth}_tmpfs"]["f0"][
                "direction"]
            neighbors = [summary["per_cell"][f"{op}_4096_{d2}_tmpfs"]["f0"]
                         for d2 in DEPTHS_BY_SIZE[4096] if d2 != depth]
            neighbors += [summary["per_cell"][f"{op}_4096_{depth}_btrfs"]
                          ["f0"]]
            same = [n for n in neighbors if n["direction"] == d]
            neighbor_support[f"{op}_4096_{depth}_tmpfs"] = bool(same)
    # verdicts (prereg §13.1)
    verdicts = {}
    for op in OPS:
        dirs = [summary["per_cell"][f"{op}_4096_{d}_tmpfs"]["f0"]["direction"]
                for d in DEPTHS_BY_SIZE[4096]]
        any_benefit = any(x == "F1_FASTER" for x in dirs)
        any_regress = any(x == "F1_SLOWER" for x in dirs)
        if any_benefit and any(
                neighbor_support.get(f"{op}_4096_{d}_tmpfs")
                for d in DEPTHS_BY_SIZE[4096]):
            verdicts[op] = "FIXED-FILE PERFORMANCE BENEFIT ESTABLISHED"
        elif any_regress and any(
                neighbor_support.get(f"{op}_4096_{d}_tmpfs")
                for d in DEPTHS_BY_SIZE[4096]):
            verdicts[op] = "FIXED-FILE PERFORMANCE REGRESSION"
        elif any_benefit or any_regress:
            verdicts[op] = "REGIME-LOCAL BENEFIT ESTABLISHED" \
                if any_benefit else "REGIME-LOCAL EFFECT (ISOLATED CELL ONLY)"
        else:
            verdicts[op] = "FIXED-FILE PERFORMANCE BENEFIT NOT ESTABLISHED"
    summary["primary_cells"] = prim
    summary["neighbor_support"] = neighbor_support
    summary["verdicts"] = verdicts
    (sd / "summary.json").write_text(json.dumps(summary, indent=1) + "\n")
    with (sd / "summary.csv").open("w") as f:
        f.write("op,size,depth,fs,f0_median_ns,f1_median_ns,ratio,"
                "direction,threaded_ratio,threaded_direction\n")
        for k, v in summary["per_cell"].items():
            op, size, depth, fs = k.split("_")
            f0 = v["f0"]
            f.write(f"{op},{size},{depth},{fs},{f0.get('f0_median_ns')},"
                    f"{f0.get('f1_median_ns')},{f0.get('ratio')},"
                    f"{f0.get('direction')},{v['threaded'].get('ratio')},"
                    f"{v['threaded'].get('direction')}\n")
    print(f"summary written to {sd / 'summary.json'}")
    for op in OPS:
        print(f"{op}: {verdicts[op]}")
    if missing:
        print(f"MISSING CELLS: {missing}", file=sys.stderr)


def usage() -> None:
    print(__doc__)
    sys.exit(0)


def main() -> None:
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        usage()
    cmd = sys.argv[1]
    session = sys.argv[2] if len(sys.argv) > 2 else None
    resume = "--resume" in sys.argv
    if cmd == "status":
        cmd_status()
    elif cmd == "probe" and session:
        cmd_probe(session)
    elif cmd == "generate" and session:
        cmd_generate(session)
    elif cmd == "q0" and session:
        cmd_q0(session, resume)
    elif cmd == "formal" and session:
        cmd_formal(session, resume)
    elif cmd == "summarize" and session:
        cmd_summarize(session)
    else:
        usage()


if __name__ == "__main__":
    main()
