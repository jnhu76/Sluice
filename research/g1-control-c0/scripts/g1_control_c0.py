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
  generate <session-id> [--fs tmpfs,btrfs]
                        create fixtures for the requested filesystem labels,
                        validate the C++ pattern generator against the
                        Python generator, freeze the expected dst sha256
                        constants
  q0 <session-id>       Phase Q0 stability qualification: 30 runs of F0 at
                        4 KiB x d8 on tmpfs READ, full same-work gates.
  formal <session-id> [--arms A1,A2] [--fs L1,L2]
                        frozen matrix: (op x 5 cells x 2 fs x 4 arms) x 7
                        = 560 runs, perf-wrapped. --arms / --fs restrict
                        EXECUTION to a subset of the frozen combo list
                        (run ids keep their full-plan positions); the
                        session manifest records the scope. Combining an
                        --arms restriction with an --fs restriction is
                        refused (no corrective needs it; keeps the scope
                        vocabulary unambiguous). Corrective-1 used
                        --arms F0-T,F1-T; Corrective-2 uses
                        --fs tmpfs (native-3).
  summarize <session-id> runs.jsonl -> summary + analysis (per-cell
                        wall/op medians, F0-vs-F1 materiality per frozen
                        rule, neighbor consistency, threaded arms,
                        verdict vocabulary per prereg); scope-aware via the
                        session manifest ("full", "threaded-corrective",
                        "tmpfs-corrective")
  composite <sid-single> <sid-threaded> <sid-tmpfs>
                        Corrective-2 campaign evidence composition,
                        substrate-authoritative (a session contributes
                        only filesystem labels whose env-resolved fstype
                        equals the canonical fstype of the label):
                        F0/F1 btrfs cells from the single-thread session,
                        F0-T/F1-T btrfs cells from the threaded-corrective
                        session, all tmpfs cells from the tmpfs-corrective
                        session (real tmpfs). Mislabeled rows are
                        SUPERSEDED — WRONG SUBSTRATE and excluded from
                        every derived number; fail-closed,
                        provenance-carrying; writes composite-summary.json
                        into the tmpfs-corrective session

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

# Corrective-2 substrate gate (prereg Amendment-5): the directory label is
# NOT the substrate. A label is only usable when the filesystem it actually
# resolves to equals its canonical fstype; anything else is fail-closed.
# Per-label roots can be redirected (native-3: G1C0_FS_ROOT_TMPFS points at
# real tmpfs) so the frozen "tmpfs (primary, /tmp)" cells can finally be
# executed on the preregistered substrate.
CANONICAL_FS = {"tmpfs": "tmpfs", "btrfs": "btrfs"}


def fs_root(label: str) -> Path:
    override = os.environ.get(f"G1C0_FS_ROOT_{label.upper()}")
    return Path(override) if override else DATA_ROOT / label


def resolved_fstype(path: Path) -> str:
    r = subprocess.run(["findmnt", "-no", "FSTYPE", "-T", str(path)],
                       capture_output=True, text=True)
    return r.stdout.strip() or "unresolved"


def substrate_problems(labels) -> list:
    return [f"substrate gate: label '{label}' root {fs_root(label)} resolves "
            f"to fstype '{resolved_fstype(fs_root(label))}', expected "
            f"'{CANONICAL_FS[label]}'"
            for label in labels
            if resolved_fstype(fs_root(label)) != CANONICAL_FS[label]]


def substrate_record(labels) -> dict:
    return {label: {"root": str(fs_root(label)),
                    "fstype": resolved_fstype(fs_root(label))}
            for label in labels}


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
            "dirty": bool(run("git", "status", "--porcelain")),
            # the execution-pin gate: tracked files identical to HEAD.
            # Untracked session output does not unpin the tooling.
            "dirty_tracked": bool(run("git", "status", "--porcelain",
                                      "--untracked-files=no"))}


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
        "filesystems": {fs: {"root": str(fs_root(fs)),
                             "type": run("findmnt", "-no", "FSTYPE", "-T",
                                         str(fs_root(fs))),
                             "opts": run("findmnt", "-no", "OPTIONS", "-T",
                                         str(fs_root(fs))),
                             "source": run("findmnt", "-no", "SOURCE", "-T",
                                           str(fs_root(fs)))}
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
        "driver_path": str(Path(__file__).relative_to(REPO)),
        "driver_sha256": sha256_file(Path(__file__)),
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
    d = fs_root(fs)
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
    if arm in ("F0-T", "F1-T"):
        # Corrective-1: the frozen prereg §5 threaded condition requires the
        # workers ALIVE AND PARKED across the measured span. The gate fields
        # are the machine-readable causality proof; all must hold fail-closed.
        if (b.get("threads_spawned") != THREADED_WORKERS or
                b.get("threads_io_ok") != THREADED_WORKERS or
                b.get("threads_joined") != THREADED_WORKERS or
                b.get("threads_ready") != THREADED_WORKERS or
                b.get("threads_released") != THREADED_WORKERS or
                b.get("thread_gate_ready") is not True or
                b.get("thread_gate_release_after_transfer") is not True):
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


def cmd_generate(session_id: str, fs_labels=None) -> None:
    fs_labels = fs_labels or FS
    unknown = [f for f in fs_labels if f not in FS]
    if unknown:
        print(f"unknown filesystem labels in --fs {fs_labels}",
              file=sys.stderr)
        sys.exit(1)
    sd = RESULTS / session_id
    sd.mkdir(parents=True, exist_ok=True)
    problems = substrate_problems(fs_labels)
    if problems:
        print("GENERATE FAIL (substrate gate):", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        sys.exit(1)
    tile = master_tile()
    expected = {}
    for label in fs_labels:
        (fs_root(label)).mkdir(parents=True, exist_ok=True)
    for size in SIZES:
        fb = FILE_BYTES[size]
        pat = pattern_bytes(fb, tile)
        pat_sha = sha256_bytes(pat)
        expected[f"expected_dst_sha256_{size}"] = pat_sha
        for label in fs_labels:
            src, dst = data_paths(label, size)
            if not src.is_file() or src.stat().st_size != fb:
                subprocess.run([str(BENCH), "--mode", "generate",
                                "--src", str(src),
                                "--file-bytes", str(fb)], check=True)
            actual = sha256_file(src)
            if actual != pat_sha:
                print(f"GENERATOR MISMATCH {label} src-{size}: C++ {actual}"
                      f" != Python {pat_sha}", file=sys.stderr)
                sys.exit(1)
            print(f"src {label}/{size}: sha256 {actual} (matches Python "
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
    manifest["fs_scope"] = fs_labels
    manifest["substrate"] = substrate_record(fs_labels)
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
    problems = substrate_problems(["tmpfs"])
    if problems:
        print("Q0 FAIL (substrate gate):", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        sys.exit(1)
    manifest = json.loads((sd / "manifest.json").read_text())
    manifest["substrate"] = substrate_record(["tmpfs"])
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


def cmd_formal(session_id: str, resume: bool = False, arms=None,
               fs_labels=None) -> None:
    sd = RESULTS / session_id
    if not sd.is_dir():
        print(f"session {session_id} does not exist; run probe first",
              file=sys.stderr)
        sys.exit(1)
    arms = arms or ARMS
    fs_labels = fs_labels or FS
    if any(a not in ARMS for a in arms):
        print(f"unknown arms in --arms {arms}", file=sys.stderr)
        sys.exit(1)
    if any(f not in FS for f in fs_labels):
        print(f"unknown filesystem labels in --fs {fs_labels}",
              file=sys.stderr)
        sys.exit(1)
    arms_restricted = sorted(arms) != sorted(ARMS)
    fs_restricted = sorted(fs_labels) != sorted(FS)
    if arms_restricted and fs_restricted:
        print("refusing to combine --arms and --fs restrictions (keeps the "
              "scope vocabulary unambiguous)", file=sys.stderr)
        sys.exit(1)
    problems = substrate_problems(fs_labels)
    if problems:
        print("FORMAL FAIL (substrate gate):", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
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
    if arms_restricted:
        # scoped execution (Corrective-1): the session manifest records the
        # scope so summarize/validators interpret the evidence fail-closed.
        manifest["scope"] = "threaded-corrective"
        manifest["corrective"] = 1
        manifest["supersedes"] = (
            "g1-control-c0-native-1 threaded arms (F0-T/F1-T): workers "
            "exited before the measured span, violating the frozen prereg "
            "§5 threaded condition")
    elif fs_restricted:
        # scoped execution (Corrective-2, prereg Amendment-5): native-1/
        # native-2 "tmpfs"-label rows resolved to btrfs (wrong substrate),
        # so the frozen tmpfs primary cells were never executed until this
        # session; the mislabeled rows are SUPERSEDED — WRONG SUBSTRATE.
        if fs_labels != ["tmpfs"]:
            print("fs-restricted scope is defined for tmpfs only",
                  file=sys.stderr)
            sys.exit(1)
        manifest["scope"] = "tmpfs-corrective"
        manifest["corrective"] = 2
        manifest["supersedes"] = (
            "g1-control-c0-native-1 and g1-control-c0-native-2-"
            "threaded-corrective tmpfs-label rows: both labels resolved to "
            "btrfs (/home), so the frozen tmpfs (primary, /tmp) cells were "
            "never executed on the preregistered substrate")
    else:
        manifest["scope"] = "full"
    manifest["arms_scope"] = arms
    manifest["fs_scope"] = fs_labels
    # record the RESOLVED substrate per label (Corrective-1 disclosure,
    # Corrective-2 gate): the label is the data-directory name, the true
    # substrate lives here and in environment.json
    manifest["substrate_fstypes"] = {label: resolved_fstype(fs_root(label))
                                     for label in fs_labels}
    manifest["substrate"] = substrate_record(fs_labels)
    gates = Gates(sd)
    # the plan is a property of the FULL frozen combo list (same seed, same
    # shuffle); scoped execution filters it, keeping full-plan run ids.
    combos = [(op, size, depth, fs, arm)
              for op in OPS
              for size, depths in DEPTHS_BY_SIZE.items()
              for depth in depths
              for fs in FS
              for arm in ARMS]
    plan = [(rid, c) for rid, c in run_plan(combos)
            if c[4] in arms and c[3] in fs_labels]
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


PRIMARY_DEPTHS = DEPTHS_BY_SIZE[4096]


def cell_directions(direction_of) -> dict:
    """All verdict-eligible cell directions for one arm pair (prereg §13):
    the 4 KiB tmpfs primary family, the 64 KiB tmpfs cell, and the 4 KiB
    btrfs cells. derive_verdicts selects the tmpfs primary family for the
    campaign verdict; neighbor_share consumes the FULL set — feeding it a
    primary-only dict silently drops the 64 KiB and btrfs neighbors
    (Corrective-2 P1-2 defect, now structurally impossible to reintroduce
    at this entry point)."""
    d = {}
    for op in OPS:
        for depth in PRIMARY_DEPTHS:
            d[f"{op}_4096_{depth}_tmpfs"] = direction_of(op, 4096, depth,
                                                         "tmpfs")
            d[f"{op}_4096_{depth}_btrfs"] = direction_of(op, 4096, depth,
                                                         "btrfs")
        d[f"{op}_65536_1_tmpfs"] = direction_of(op, 65536, 1, "tmpfs")
    return d


def neighbor_share(directions: dict) -> dict:
    """prereg §13 neighbor consistency: neighbors of a primary 4 KiB tmpfs
    cell are the other 4 KiB depths (tmpfs), the 64 KiB cell (tmpfs), and
    the same cell on btrfs. True when >= 1 neighbor shares the cell's
    direction (only meaningful for non-NONE directions)."""
    share = {}
    for op in OPS:
        for d in PRIMARY_DEPTHS:
            cell = f"{op}_4096_{d}_tmpfs"
            own = directions[cell]
            if own is None:
                share[cell] = False
                continue
            neighbor_dirs = [directions[f"{op}_4096_{d2}_tmpfs"]
                             for d2 in PRIMARY_DEPTHS if d2 != d]
            neighbor_dirs.append(directions.get(f"{op}_65536_1_tmpfs"))
            neighbor_dirs.append(directions.get(f"{op}_4096_{d}_btrfs"))
            share[cell] = any(x == own for x in neighbor_dirs if x)
    return share


def derive_verdicts(directions: dict, share: dict) -> dict:
    """Frozen prereg §13/§13.1 campaign verdict derivation from per-cell
    directions. Pure function: the validator re-implements this rule
    independently and compares."""
    verdicts = {}
    for op in OPS:
        cells = [f"{op}_4096_{d}_tmpfs" for d in PRIMARY_DEPTHS]
        dirs = [directions[c] for c in cells]
        benefit_supported = any(d == "F1_FASTER" and share[c]
                                for c, d in zip(cells, dirs))
        regress_supported = any(d == "F1_SLOWER" and share[c]
                                for c, d in zip(cells, dirs))
        if benefit_supported:
            verdicts[op] = "FIXED-FILE PERFORMANCE BENEFIT ESTABLISHED"
        elif regress_supported:
            verdicts[op] = "FIXED-FILE PERFORMANCE REGRESSION"
        elif any(d == "F1_FASTER" for d in dirs):
            verdicts[op] = "REGIME-LOCAL BENEFIT ESTABLISHED"
        else:
            # an ISOLATED regression is a per-cell observation only; the
            # frozen vocabulary has no isolated-regression campaign verdict
            verdicts[op] = "FIXED-FILE PERFORMANCE BENEFIT NOT ESTABLISHED"
    return verdicts


def scope_arms(scope: str) -> list:
    return ARMS if scope in ("full", "tmpfs-corrective") else \
        ["F0-T", "F1-T"]


def scope_fs(scope: str) -> list:
    return ["tmpfs"] if scope == "tmpfs-corrective" else FS


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
    scope = manifest.get("scope", "full")
    gates = json.loads((sd / "gates.json").read_text())
    cells = {}
    for r in runs:
        if not r.get("ok"):
            continue
        key = (r["op"], r["size"], r["depth"], r["fs"], r["arm"])
        cells.setdefault(key, []).append(r["bench"]["wall_per_op_ns"])
    expected_arms = scope_arms(scope)
    expected_fs = scope_fs(scope)
    all_keys = {(op, size, depth, fs, arm)
                for op in OPS for size in SIZES
                for depth in DEPTHS_BY_SIZE[size]
                for fs in expected_fs
                for arm in expected_arms}
    # scope discipline: a scoped session must not contain formal runs of
    # arms or filesystem labels outside its scope
    outside = sorted({(r["op"], r["size"], r["depth"], r["fs"], r["arm"])
                      for r in runs if r["arm"] not in expected_arms
                      or r["fs"] not in expected_fs})
    missing = sorted(all_keys - set(cells))
    summary = {"session": session_id, "scope": scope,
               "runs_total": len(runs),
               "runs_ok": len([r for r in runs if r.get("ok")]),
               "gate_errors": len(gates.get("errors", [])),
               "cells_missing": missing,
               "cells_outside_scope": outside,
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

    def direction_of(op, size, depth, fs, kind):
        entry = "f0" if kind == "single" else "threaded"
        return summary["per_cell"][f"{op}_{size}_{depth}_{fs}"][entry][
            "direction"]

    # primary cells: 4 KiB family on tmpfs
    prim = []
    for depth in PRIMARY_DEPTHS:
        c = summary["per_cell"].get(f"READ_4096_{depth}_tmpfs", {})
        prim.append({"cell": f"READ 4K d{depth} tmpfs",
                     **{k: c.get("f0", {}).get(k) for k in
                        ("ratio", "benefit", "regression", "direction")}})
    # single-thread directions: the FULL eligible set; the frozen rule
    # (neighbor_share + derive_verdicts) consumes it directly
    directions = cell_directions(
        lambda op, s, d, fs: direction_of(op, s, d, fs, "single"))
    tdirs = cell_directions(
        lambda op, s, d, fs: direction_of(op, s, d, fs, "threaded"))
    # threaded arms are exploratory in every scope: same frozen rule applied
    # to F0-T vs F1-T, reported separately, cannot carry a verdict
    summary["threaded_directions"] = tdirs
    summary["threaded_verdicts"] = derive_verdicts(tdirs,
                                                   neighbor_share(tdirs))
    if scope == "full":
        summary["primary_cells"] = prim
        summary["neighbor_support"] = neighbor_share(directions)
        # campaign verdicts (prereg §13.1, derived by the frozen rule)
        summary["verdicts"] = derive_verdicts(directions,
                                              summary["neighbor_support"])
        summary["verdict_basis"] = "campaign"
    elif scope == "tmpfs-corrective":
        # session-local derivation: btrfs neighbors are absent in-session
        # (scope-restricted), so neighbor support can only come from tmpfs
        # cells here; campaign verdicts derive from the composite
        summary["primary_cells"] = prim
        summary["neighbor_support"] = neighbor_share(directions)
        summary["verdicts"] = derive_verdicts(directions,
                                              summary["neighbor_support"])
        summary["verdict_basis"] = (
            "session-local (tmpfs-corrective scope; btrfs neighbors absent "
            "in-session; campaign verdicts derive from the composite)")
    else:
        summary["verdict_basis"] = (
            "threaded-only scope; campaign verdicts derive from the "
            "composite")
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
    print(f"summary written to {sd / 'summary.json'} (scope: {scope})")
    if scope in ("full", "tmpfs-corrective"):
        basis = "" if scope == "full" else " (session-local, see basis)"
        for op in OPS:
            print(f"{op}: {summary['verdicts'][op]}{basis}")
    for op in OPS:
        print(f"{op} (threaded, exploratory): "
              f"{summary['threaded_verdicts'][op]}")
    if scope != "full":
        print(f"verdict basis: {summary['verdict_basis']}")
    if missing:
        print(f"MISSING CELLS: {missing}", file=sys.stderr)
    if outside:
        print(f"CELLS OUTSIDE SCOPE: {outside}", file=sys.stderr)
        sys.exit(1)


def cmd_composite(sid_single: str, sid_threaded: str, sid_tmpfs: str) -> None:
    """Corrective-2 campaign evidence composition (fail-closed).

    Substrate-authoritative sources (a session contributes ONLY the
    filesystem labels whose env-resolved fstype equals the canonical
    fstype of the label — prereg Amendment-5):
        F0/F1 btrfs cells     <- sid_single   (native-1)
        F0-T/F1-T btrfs cells <- sid_threaded (native-2, Corrective-1)
        tmpfs cells, all arms <- sid_tmpfs   (native-3, real tmpfs)
    Superseded (retained byte-identical, excluded from every derived
    number):
        sid_single threaded runs       (frozen prereg §5 violation)
        sid_single tmpfs-label runs    (WRONG SUBSTRATE — resolved btrfs)
        sid_threaded tmpfs-label runs  (WRONG SUBSTRATE — resolved btrfs)
    The tmpfs-corrective session must additionally be a clean commit-pinned
    execution (tracked files identical to a recorded 40-hex HEAD at
    generate time — Corrective-2 P2). Any coverage/gate/scope/substrate
    problem aborts WITHOUT writing composite-summary.json. Verdicts are
    derived from the FULL eligible direction set through the frozen rule
    (Corrective-2 P1-2: the 64 KiB tmpfs and same-depth btrfs neighbors
    genuinely enter the verdict path).
    """
    s1, s2, s3 = (RESULTS / sid for sid in
                  (sid_single, sid_threaded, sid_tmpfs))
    problems: list = []

    def runs_of(sid: str) -> list:
        raw = RESULTS / sid / "raw" / "runs.jsonl"
        return [json.loads(l) for l in raw.read_text().splitlines()
                if l.strip()]

    runs1 = [r for r in runs_of(sid_single)
             if not r["run_id"].startswith("q0-")]
    runs2 = [r for r in runs_of(sid_threaded)
             if not r["run_id"].startswith("q0-")]
    runs3 = [r for r in runs_of(sid_tmpfs)
             if not r["run_id"].startswith("q0-")]
    for sid, runs in ((sid_single, runs1), (sid_threaded, runs2),
                      (sid_tmpfs, runs3)):
        g = json.loads((RESULTS / sid / "gates.json").read_text())
        if g.get("errors"):
            problems.append(f"{sid}: {len(g['errors'])} gate errors")
        ids = [r["run_id"] for r in runs]
        if len(ids) != len(set(ids)):
            problems.append(f"{sid}: duplicate run ids")

    def authoritative(env: dict, label: str) -> bool:
        actual = (env.get("filesystems", {}).get(label, {}) or {}).get("type")
        return actual == CANONICAL_FS[label]

    env1 = json.loads((s1 / "environment.json").read_text())
    env2 = json.loads((s2 / "environment.json").read_text())
    env3 = json.loads((s3 / "environment.json").read_text())
    man2 = json.loads((s2 / "manifest.json").read_text())
    man3 = json.loads((s3 / "manifest.json").read_text())
    if man2.get("scope") != "threaded-corrective":
        problems.append(f"{sid_threaded}: manifest scope is not "
                        f"threaded-corrective")
    if man3.get("scope") != "tmpfs-corrective":
        problems.append(f"{sid_tmpfs}: manifest scope is not "
                        f"tmpfs-corrective")
    if not authoritative(env1, "btrfs"):
        problems.append(f"{sid_single}: not substrate-authoritative for "
                        f"btrfs (env: {env1.get('filesystems', {})
                          .get('btrfs', {})})")
    if not authoritative(env2, "btrfs"):
        problems.append(f"{sid_threaded}: not substrate-authoritative for "
                        f"btrfs (env: {env2.get('filesystems', {})
                          .get('btrfs', {})})")
    if not authoritative(env3, "tmpfs"):
        problems.append(f"{sid_tmpfs}: not substrate-authoritative for "
                        f"tmpfs (env: {env3.get('filesystems', {})
                          .get('tmpfs', {})})")
    # Corrective-2 P2: the tmpfs-corrective session must be a clean
    # commit-pinned execution (recorded at generate time)
    g3 = env3.get("git", {})
    head3 = g3.get("head") or ""
    if g3.get("dirty_tracked") is not False:
        problems.append(f"{sid_tmpfs}: tracked worktree was dirty at "
                        f"generate time (dirty_tracked="
                        f"{g3.get('dirty_tracked')}) — not commit-pinned")
    if len(head3) != 40 or any(c not in "0123456789abcdef" for c in head3):
        problems.append(f"{sid_tmpfs}: recorded head is not a 40-hex sha: "
                        f"{head3!r}")

    def ok_vals(runs, labels, arms):
        """{(op,size,depth,fs,arm): [wall_per_op_ns]} for retained rows
        only: fs label must be substrate-authoritative in this session."""
        out: dict = {}
        for r in runs:
            if not (r.get("ok") and r.get("bench")):
                continue
            if r["arm"] not in arms or r["fs"] not in labels:
                continue
            out.setdefault((r["op"], r["size"], r["depth"], r["fs"],
                            r["arm"]), []).append(
                r["bench"]["wall_per_op_ns"])
        return out

    # retained sources
    vals_single = ok_vals(runs1, ["btrfs"], ("F0", "F1"))
    vals_threaded = ok_vals(runs2, ["btrfs"], ("F0-T", "F1-T"))
    vals_tmpfs_single = ok_vals(runs3, ["tmpfs"], ("F0", "F1"))
    vals_tmpfs_threaded = ok_vals(runs3, ["tmpfs"], ("F0-T", "F1-T"))

    # coverage: every frozen cell of the composite matrix exactly ROUNDS
    def coverage(vals, sid, expected):
        for op in OPS:
            for size in SIZES:
                for depth in DEPTHS_BY_SIZE[size]:
                    for fs in FS:
                        for arm in expected:
                            n = len(vals.get((op, size, depth, fs, arm), []))
                            if n != ROUNDS:
                                problems.append(
                                    f"{sid} {op}/{size}/{depth}/{fs}/{arm}: "
                                    f"{n} valid runs (expected {ROUNDS})")

    coverage(vals_single, sid_single, ("F0", "F1"))
    coverage(vals_threaded, sid_threaded, ("F0-T", "F1-T"))
    coverage(vals_tmpfs_single, sid_tmpfs, ("F0", "F1"))
    coverage(vals_tmpfs_threaded, sid_tmpfs, ("F0-T", "F1-T"))

    # superseded-shape discipline
    superseded_threaded_1 = [r for r in runs1 if r["arm"] in ("F0-T", "F1-T")]
    post_corrective_shape = [
        r["run_id"] for r in superseded_threaded_1
        if "threads_ready" in (r.get("bench") or {})]
    if post_corrective_shape:
        problems.append(
            f"{sid_single}: threaded runs already carry corrective gate "
            f"fields ({post_corrective_shape[:3]}...) — inconsistent with "
            f"the superseded-shape disposition")
    if len(superseded_threaded_1) != 280:
        problems.append(f"{sid_single}: expected 280 superseded threaded "
                        f"runs, found {len(superseded_threaded_1)}")
    superseded_fs_1 = [r for r in runs1 if r["fs"] == "tmpfs"
                       and r["arm"] in ("F0", "F1")]
    if len(superseded_fs_1) != 140:
        problems.append(f"{sid_single}: expected 140 superseded tmpfs-label "
                        f"single runs (WRONG SUBSTRATE), found "
                        f"{len(superseded_fs_1)}")
    superseded_fs_2 = [r for r in runs2 if r["fs"] == "tmpfs"]
    if len(superseded_fs_2) != 140:
        problems.append(f"{sid_threaded}: expected 140 superseded tmpfs-label "
                        f"runs (WRONG SUBSTRATE), found {len(superseded_fs_2)}")
    outside_scope_2 = [r["run_id"] for r in runs2
                       if r["arm"] not in ("F0-T", "F1-T")]
    if outside_scope_2:
        problems.append(f"{sid_threaded}: single-thread formal runs "
                        f"outside scope: {outside_scope_2[:3]}...")
    # retained threaded runs must carry the corrective gate fields
    for r in [x for x in runs2 + runs3
              if x.get("ok") and x["arm"] in ("F0-T", "F1-T")]:
        b = r["bench"]
        if (b.get("threads_spawned") != THREADED_WORKERS or
                b.get("threads_io_ok") != THREADED_WORKERS or
                b.get("threads_joined") != THREADED_WORKERS or
                b.get("threads_ready") != THREADED_WORKERS or
                b.get("threads_released") != THREADED_WORKERS or
                b.get("thread_gate_ready") is not True or
                b.get("thread_gate_release_after_transfer") is not True):
            problems.append(f"{r['run_id']}: corrective threaded gate "
                            f"fields missing/unsatisfied")

    if problems:
        print(f"COMPOSITE FAIL ({len(problems)}):")
        for p in problems:
            print(f"  - {p}")
        sys.exit(1)

    def material_of(vals_f0, vals_f1) -> dict:
        return material(vals_f0, vals_f1)

    per_cell = {}
    for op in OPS:
        for size in SIZES:
            for depth in DEPTHS_BY_SIZE[size]:
                for fs in FS:
                    f0 = vals_single.get((op, size, depth, fs, "F0"), []) \
                        if fs == "btrfs" else \
                        vals_tmpfs_single.get((op, size, depth, fs, "F0"), [])
                    f1 = vals_single.get((op, size, depth, fs, "F1"), []) \
                        if fs == "btrfs" else \
                        vals_tmpfs_single.get((op, size, depth, fs, "F1"), [])
                    f0t = vals_threaded.get((op, size, depth, fs, "F0-T"), []) \
                        if fs == "btrfs" else \
                        vals_tmpfs_threaded.get(
                            (op, size, depth, fs, "F0-T"), [])
                    f1t = vals_threaded.get((op, size, depth, fs, "F1-T"), []) \
                        if fs == "btrfs" else \
                        vals_tmpfs_threaded.get(
                            (op, size, depth, fs, "F1-T"), [])
                    per_cell[f"{op}_{size}_{depth}_{fs}"] = {
                        "f0": material_of(f0, f1),
                        "threaded": material_of(f0t, f1t),
                        "f0_source": sid_single if fs == "btrfs"
                        else sid_tmpfs,
                        "threaded_source": sid_threaded if fs == "btrfs"
                        else sid_tmpfs,
                    }

    def cell_dirs(kind: str) -> dict:
        entry = "f0" if kind == "single" else "threaded"
        return cell_directions(
            lambda op, s, d, fs: per_cell[f"{op}_{s}_{d}_{fs}"][entry][
                "direction"])

    dirs_single = cell_dirs("single")
    dirs_threaded = cell_dirs("threaded")
    verdicts = derive_verdicts(dirs_single, neighbor_share(dirs_single))
    threaded_verdicts = derive_verdicts(dirs_threaded,
                                        neighbor_share(dirs_threaded))

    def provenance(env, man, sid) -> dict:
        git = env.get("git", {})
        return {"session": sid,
                "git_head": git.get("head"),
                "git_dirty": git.get("dirty"),
                "git_dirty_tracked": git.get("dirty_tracked"),
                "driver_sha256": env.get("driver_sha256"),
                "bench_binary_sha256": env.get("bench_binary_sha256"),
                "scope": man.get("scope", "full")}

    composite = {
        "composite": True,
        "corrective": "Corrective-2",
        "substrate_rule": "a session contributes only filesystem labels "
                          "whose env-resolved fstype equals the canonical "
                          "fstype of the label (prereg Amendment-5)",
        "single_thread_source": {
            **provenance(env1, json.loads((s1 / "manifest.json").read_text()),
                         sid_single),
            "disposition": "authoritative for btrfs F0/F1 only",
        },
        "threaded_source": {
            **provenance(env2, man2, sid_threaded),
            "disposition": "authoritative for btrfs F0-T/F1-T (corrected "
                           "prereg §5 threaded condition: workers parked "
                           "across the measured span)",
        },
        "tmpfs_source": {
            **provenance(env3, man3, sid_tmpfs),
            "disposition": "authoritative for all tmpfs cells (frozen "
                           "tmpfs primary finally executed on the "
                           "preregistered substrate)",
        },
        "superseded": {
            sid_single: {
                "threaded_runs": {
                    "runs": len(superseded_threaded_1),
                    "reason": "frozen prereg §5 threaded condition "
                              "violated: workers were joined BEFORE the "
                              "measured span (Amendment-1)"},
                "tmpfs_label_single_runs": {
                    "runs": len(superseded_fs_1),
                    "reason": "WRONG SUBSTRATE: tmpfs label resolved to "
                              "btrfs (Amendment-5)"},
            },
            sid_threaded: {
                "tmpfs_label_threaded_runs": {
                    "runs": len(superseded_fs_2),
                    "reason": "WRONG SUBSTRATE: tmpfs label resolved to "
                              "btrfs (Amendment-5)"},
            },
        },
        "per_cell": per_cell,
        "verdicts": verdicts,
        "threaded_verdicts": threaded_verdicts,
        "threaded_interpretation": "EXPLORATORY ONLY (prereg §5/§13): "
                                   "threaded arms cannot carry a campaign "
                                   "verdict",
    }
    out = s3 / "composite-summary.json"
    out.write_text(json.dumps(composite, indent=1) + "\n")
    print(f"composite summary written to {out}")
    for op in OPS:
        print(f"{op}: {verdicts[op]}")
    for op in OPS:
        print(f"{op} (threaded, exploratory): {threaded_verdicts[op]}")


def usage() -> None:
    print(__doc__)
    sys.exit(0)


def main() -> None:
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        usage()
    cmd = sys.argv[1]
    session = sys.argv[2] if len(sys.argv) > 2 else None
    resume = "--resume" in sys.argv
    arms = None
    if "--arms" in sys.argv:
        i = sys.argv.index("--arms")
        if i + 1 >= len(sys.argv):
            usage()
        arms = [a.strip() for a in sys.argv[i + 1].split(",") if a.strip()]
    fs_labels = None
    if "--fs" in sys.argv:
        i = sys.argv.index("--fs")
        if i + 1 >= len(sys.argv):
            usage()
        fs_labels = [a.strip() for a in sys.argv[i + 1].split(",")
                     if a.strip()]
    if cmd == "status":
        cmd_status()
    elif cmd == "probe" and session:
        cmd_probe(session)
    elif cmd == "generate" and session:
        cmd_generate(session, fs_labels)
    elif cmd == "q0" and session:
        cmd_q0(session, resume)
    elif cmd == "formal" and session:
        cmd_formal(session, resume, arms, fs_labels)
    elif cmd == "summarize" and session:
        cmd_summarize(session)
    elif cmd == "composite" and session and len(sys.argv) > 4:
        cmd_composite(session, sys.argv[3], sys.argv[4])
    else:
        usage()


if __name__ == "__main__":
    main()
