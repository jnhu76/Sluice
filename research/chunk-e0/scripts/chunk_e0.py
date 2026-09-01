#!/usr/bin/env python3
"""chunk_e0.py — CHUNK-E0 Phase H0 chunk-size x depth sweep driver (#270).

Runs the frozen preregistration (research/chunk-e0/
CHUNK-E0-H0-PREREGISTRATION.md): the production buffered READ+WRITE copy
engine (run_pipelined_copy_with_backend + ThreadPoolBackend, workers=1)
over 15 chunks (16K..4M) x depth {1,2,4,8}, 1 GiB READ+WRITE copy,
R=7 seeded interleaved rounds. Host-0 local only.

Subcommands:
  generate <session-id>   create the 1 GiB src file (via the bench
                          --generate) and record its sha256.
  validate <session-id>   validation session: {16K,4M} x {d1,d4} x 2 reps +
                          the cycles:u stability probe (prereg §7).
  sweep <session-id>      full frozen sweep (prereg §6/§9): 7 rounds x 60
                          cells.
  summarize <session-id>  runs.jsonl -> summary.csv/json + analysis.json
                          (tested-range peak, 95% point, plateau entry,
                          knee, Pareto frontier, verdict per prereg
                          §10-12; plus the POST-HOC sustained-to-boundary
                          flatness robustness diagnostic, which does not
                          feed the frozen verdict).
  knee-diagnostics        synthetic analysis-unit diagnostics for
                          two_segment_knee (single line / injected
                          two-segment / flat); not a measurement.

Immutable session layout (prereg §14):
  results/<session-id>/{environment.json, manifest.json, gates.json,
                        notes.md, summary.csv, summary.json, analysis.json,
                        raw/runs.jsonl, raw/perf.csv}
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
# Host-scoped path overrides (run_host.py). These relocate fixture, binary
# and evidence directories only; frozen scientific constants below are
# unaffected by them.
DATA_DIR = Path(os.environ["CHUNK_E0_DATA_DIR"]) \
    if "CHUNK_E0_DATA_DIR" in os.environ else REPO / "build/chunk-e0-data"
BENCH = Path(os.environ["CHUNK_E0_BENCH"]) \
    if "CHUNK_E0_BENCH" in os.environ else \
    REPO / "build/linux/x86_64/release/chunk_e0_bench"
RESULTS = Path(os.environ["CHUNK_E0_RESULTS"]) \
    if "CHUNK_E0_RESULTS" in os.environ else REPO / "research/chunk-e0/results"

FILE_BYTES = 1_073_741_824  # 1 GiB (frozen by the smoke probe, prereg §5.1)
CHUNKS = [16384, 32768, 65536, 98304, 131072, 196608, 262144, 393216,
          524288, 786432, 1048576, 1572864, 2097152, 3145728, 4194304]
DEPTHS = [1, 2, 4, 8]
ROUNDS = 7
PREREG_SEED = 0xE1E1E1E121212121
P95_FRACTION = 0.95
FLAT_GAIN = 0.03       # plateau material threshold (prereg §10)
MIN_FLAT_PAIRS = 2     # consecutive flat pairs required for PLATEAU_ENTRY
KNEE_SSE_REDUCTION = 0.10


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
    env = {
        "uname": run("uname", "-a"),
        "kernel": run("uname", "-r"),
        "distribution": run("cat", "/etc/os-release") if
        Path("/etc/os-release").is_file() else "?",
        "cpu_model": [l for l in lscpu.splitlines()
                      if l.startswith("Model name")],
        "cpu_flags": [l for l in lscpu.splitlines()
                      if l.startswith("Flags")],
        "cpu_threads": [l for l in lscpu.splitlines()
                        if l.startswith("CPU(s)") or l.startswith("Thread")],
        "cpu_max_mhz": [l for l in lscpu.splitlines()
                        if l.startswith("CPU max MHz")],
        "cache_hierarchy": run("lscpu", "-C"),
        "meminfo": {k: v.strip() for k, v in (
            l.split(":", 1) for l in Path("/proc/meminfo").read_text()
            .splitlines() if l.startswith(("MemTotal", "MemAvailable")))},
        "page_size": run("getconf", "PAGESIZE"),
        "cache_line": run("getconf", "LEVEL1_DCACHE_LINESIZE"),
        "smbios": run("cat", "/sys/class/dmi/id/product_name"),
        "filesystem": run("findmnt", "-no", "FSTYPE,OPTIONS", "-T", DATA_DIR)
        if DATA_DIR.is_dir() else "?",
        "block_device": run("findmnt", "-no", "SOURCE", "-T", DATA_DIR)
        if DATA_DIR.is_dir() else "?",
        "governor": run("cat",
                        "/sys/devices/system/cpu/cpu0/cpufreq/"
                        "scaling_governor"),
        "scaling_driver": run("cat",
                              "/sys/devices/system/cpu/cpu0/cpufreq/"
                              "scaling_driver"),
        "no_turbo": run("cat", "/sys/devices/system/cpu/intel_pstate/"
                               "no_turbo"),
        "glibc": run("ldd", "--version").splitlines()[0] if
        len(run("ldd", "--version").splitlines()) > 0 else "?",
        "clang": run("clang", "--version").splitlines()[0] if
        len(run("clang", "--version").splitlines()) > 0 else "?",
        "xmake": run("xmake", "--version").splitlines()[0] if
        len(run("xmake", "--version").splitlines()) > 0 else "?",
        "perf": run("perf", "--version"),
        "perf_paranoid": run("cat", "/proc/sys/kernel/perf_event_paranoid"),
        "virtualization": run("systemd-detect-virt"),
        "git": git_state(),
        "bench_binary_sha256": sha256_file(BENCH) if BENCH.is_file() else "?",
        "bench_binary_size": BENCH.stat().st_size if BENCH.is_file() else 0,
    }
    return env


def median(vals):
    if not vals:
        return 0.0
    s = sorted(vals)
    return s[len(s) // 2]


def mad(vals, med):
    if not vals:
        return 0.0
    return median([abs(v - med) for v in vals])


def new_session(session_id: str, purpose: str, manifest: dict,
                prereg_ref: str = "research/chunk-e0/"
                "CHUNK-E0-H0-PREREGISTRATION.md (FROZEN)") -> Path:
    sd = RESULTS / session_id
    raw = sd / "raw"
    raw.mkdir(parents=True, exist_ok=False)
    env = environment_json()
    env["data_src_sha256"] = sha256_file(DATA_DIR / "src.bin") \
        if (DATA_DIR / "src.bin").is_file() else "?"
    (sd / "environment.json").write_text(json.dumps(env, indent=1) + "\n")
    manifest["purpose"] = purpose
    manifest["preregistration"] = prereg_ref
    manifest["data_dir"] = str(DATA_DIR)
    (sd / "manifest.json").write_text(json.dumps(manifest, indent=1) + "\n")
    (sd / "gates.json").write_text("{\n}\n")
    (sd / "notes.md").write_text(f"# {session_id} — notes\n\n"
                                 f"(authored after the session)\n")
    return sd


def parse_perf_stat(text: str) -> dict:
    """Parse `perf stat -x,` output. Layout is version-dependent:
    `<value>,<unit>,<event>,...` — the event name marks the column; the
    counter value is the closest numeric field before it (unit may be
    empty)."""
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
              chunk: int, depth: int) -> dict:
    """One measured run under perf; fail-closed in the driver."""
    raw_dir = session_dir / "raw"
    cmd = ["perf", "stat", "-x,", "-e", "instructions:u,cycles:u,task-clock",
           "--", str(BENCH), "--run", "--chunk", str(chunk),
           "--depth", str(depth), "--file-bytes", str(FILE_BYTES),
           "--src", str(DATA_DIR / "src.bin"), "--dst",
           str(DATA_DIR / "dst.bin"), "--label", run_id]
    t0 = time.monotonic()
    try:
        p = subprocess.run(cmd, capture_output=True, text=True)
    except FileNotFoundError:
        # perf binary absent: the run is recorded fail-closed as a gate
        # error; wall-clock benchmarking infrastructure stays usable.
        rec = {"run_id": run_id, "module": "engine", "chunk": chunk,
               "depth": depth, "in_flight_bytes": chunk * depth,
               "wall_driver_s": round(time.monotonic() - t0, 4),
               "perf": {}, "bench_exit": 127, "bench_line": "",
               "gate_fail": "perf_missing", "ok": False}
        gates.record(rec)
        return rec
    tout = p.stdout.strip()
    perf = parse_perf_stat(p.stderr)
    rec = {
        "run_id": run_id,
        "module": "engine",
        "chunk": chunk,
        "depth": depth,
        "in_flight_bytes": chunk * depth,
        "wall_driver_s": round(time.monotonic() - t0, 4),
        "perf": perf,
        "bench_exit": p.returncode,
        "bench_line": tout,
        "ok": False,
    }
    try:
        bench = json.loads(tout) if tout else {}
        rec["bench"] = bench
        rec["total_ns"] = bench.get("total_ns", 0)
    except json.JSONDecodeError:
        rec["bench"] = None
        rec["total_ns"] = 0
    # Driver-side gates (prereg §8): bench exit, perf exit, dst hash.
    if p.returncode != 0:
        rec["gate_fail"] = "bench_exit"
        gates.record(rec)
        return rec
    if not perf.get("instructions:u") or perf.get("instructions:u") <= 0:
        rec["gate_fail"] = "perf_instructions_missing"
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
    # Append-only raw evidence (values preserved; run identity kept).
    with (raw_dir / "runs.jsonl").open("a") as f:
        f.write(json.dumps(rec) + "\n")
    with (raw_dir / "perf.csv").open("a") as f:
        f.write(f"{run_id},{perf.get('instructions:u')},"
                f"{perf.get('cycles:u')},{perf.get('task-clock')}\n")
    return rec


def run_matrix(session_dir: Path, gates: Gates, manifest: dict,
               cells: list[tuple], label_prefix: str) -> None:
    for i, (chunk, depth) in enumerate(cells):
        run_id = f"{label_prefix}-{i:04d}"
        bench_run(session_dir, gates, manifest, run_id, chunk, depth)


def run_plan(cells: list[tuple], rounds: int = ROUNDS,
             seed: int = PREREG_SEED) -> list[tuple[str, int, int]]:
    """Frozen execution order (prereg §9): each round shuffles ALL cells
    with random.Random(seed + round); run ids are r<round>-NNNN in shuffle
    position order. Single ordering authority for both sweep execution and
    session validation (run_host.py)."""
    plan = []
    for rnd in range(1, rounds + 1):
        order = cells[:]
        random.Random(seed + rnd).shuffle(order)
        plan.extend((f"r{rnd}-{i:04d}", c, d)
                    for i, (c, d) in enumerate(order))
    return plan


def ordered_run_ids(cells: list[tuple], rounds: int = ROUNDS,
                    seed: int = PREREG_SEED) -> list[str]:
    return [rid for rid, _, _ in run_plan(cells, rounds, seed)]


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
    print("src sha256:", sha256_file(DATA_DIR / "src.bin"))


def cmd_validate(session_id: str) -> None:
    manifest = {"base": "master (post-#269)", "kind": "validate",
                "data_src_sha256": sha256_file(DATA_DIR / "src.bin"),
                "file_bytes": FILE_BYTES}
    sd = new_session(session_id, "harness validation (frozen subset)",
                     manifest)
    gates = Gates(sd)
    cells = [(c, d) for c in (16384, 4194304) for d in (1, 4) for _ in range(2)]
    run_matrix(sd, gates, manifest, cells, "val")
    # cycles:u stability probe (prereg §7): 3 consecutive identical runs;
    # cycles is upgraded only if the consecutive-run per-op DOUBLE-
    # DIFFERENCE is non-negative at both {16K,4M} x d1. Any negative
    # difference -> DEMOTED.
    probe = {}
    for c in (16384, 4194304):
        vals = []
        for i in range(3):
            rec = bench_run(sd, gates, manifest, f"probe-{c}-{i}", c, 1)
            cyc = rec.get("perf", {}).get("cycles:u")
            bench = rec.get("bench") or {}
            ops = bench.get("read_ops", 0) + bench.get("write_ops", 0)
            vals.append((cyc / ops) if cyc and ops else None)
        diffs = [vals[i + 1] - vals[i] for i in range(2)
                 if vals[i] is not None and vals[i + 1] is not None]
        probe[str(c)] = {"per_op": vals, "diffs": diffs}
    gates.persist(manifest | {"runs_total": len(cells) + 6},
                  runs_total=len(cells) + 6)
    (sd / "notes.md").write_text(
        f"# {session_id} — notes\n\nvalidation subset: {{16K,4M}} x {{d1,d4}}"
        f" x 2 reps = {len(cells)} runs + 6-run cycles stability probe.\n")
    cycles_negative = any(d < 0 for cell in probe.values()
                          for d in cell["diffs"])
    print(f"validate: {len(cells) + 6} runs, "
          f"{len(gates.errors)} gate errors")
    probe_label = ("cycles:u DEMOTED (negative consecutive double-difference)"
                   if cycles_negative else
                   "cycles:u no negative double-difference -> upgrade candidate")
    (sd / "cycles_probe.json").write_text(json.dumps(probe, indent=1) + "\n")
    print(f"cycles probe: {probe_label}")
    if len(gates.errors) > 0:
        sys.exit("validation FAILED (gate errors present)")


def cmd_sweep(session_id: str, resume: bool = False) -> None:
    current = {"base": "master (post-#269)", "kind": "sweep",
               "data_src_sha256": sha256_file(DATA_DIR / "src.bin"),
               "rounds": ROUNDS, "chunks": CHUNKS, "depths": DEPTHS,
               "file_bytes": FILE_BYTES}
    sd = RESULTS / session_id
    gates = Gates(sd)
    cells = [(c, d) for c in CHUNKS for d in DEPTHS]
    plan = run_plan(cells)
    done: set[str] = set()
    if resume:
        if not (sd / "manifest.json").is_file() or \
                not (sd / "raw" / "runs.jsonl").is_file():
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
        sd = new_session(session_id, "frozen chunk x depth sweep", manifest)
    last_of_round = {}
    for rid, _, _ in plan:
        last_of_round[rid.split("-")[0]] = rid
    todo = [(rid, c, d) for rid, c, d in plan if rid not in done]
    print(f"sweep: {len(todo)} runs to execute"
          + (f" (resuming; {len(done)} already recorded)" if resume else ""),
          flush=True)
    for rid, chunk, depth in todo:
        bench_run(sd, gates, manifest, rid, chunk, depth)
        if rid == last_of_round[rid.split("-")[0]]:
            print(f"round {rid.split('-')[0][1:]}/{ROUNDS} done "
                  f"({len(gates.errors)} gate errors so far)", flush=True)
    gates.persist(manifest | {"runs_total": len(plan)}, runs_total=len(plan))
    print(f"sweep: {len(plan)} runs, {len(gates.errors)} gate errors")
    if len(gates.errors) > 0:
        sys.exit("sweep FAILED (gate errors present)")


def load_runs(session_id: str) -> list[dict]:
    raw = RESULTS / session_id / "raw" / "runs.jsonl"
    runs = []
    for line in raw.read_text().splitlines():
        if line.strip():
            runs.append(json.loads(line))
    return runs


def cells_stats(runs: list[dict], file_bytes: int = FILE_BYTES) -> dict:
    """Per (chunk, depth): median/MAD over reps of key metrics."""
    by_cell: dict[tuple, list[dict]] = {}
    for r in runs:
        if not r.get("ok"):
            continue
        key = (r["chunk"], r["depth"])
        by_cell.setdefault(key, []).append(r)
    stats = {}
    for (chunk, depth), rs in by_cell.items():
        tot = [r["total_ns"] for r in rs]
        ins = [r.get("perf", {}).get("instructions:u", 0) for r in rs]
        cpu = [(r.get("bench", {}).get("utime_us", 0) +
                r.get("bench", {}).get("stime_us", 0)) for r in rs]
        rss = [r.get("bench", {}).get("maxrss_kb", 0) for r in rs]
        mf = [r.get("bench", {}).get("minflt", 0) for r in rs]
        chunks = rs[0].get("bench", {}).get("chunks", 1)
        med_t = median(tot)
        med_i = median(ins)
        stats[(chunk, depth)] = {
            "n": len(rs),
            "chunk": chunk,
            "chunk_kib": round(chunk / 1024, 1),
            "depth": depth,
            "in_flight_bytes": chunk * depth,
            "in_flight_mib": round(chunk * depth / (1 << 20), 3),
            "total_ns_median": med_t,
            "total_ns_mad": mad(tot, med_t),
            "mibps_median": (file_bytes / med_t * 1e9 / (1 << 20))
            if med_t else 0,
            "mibps_mad": (file_bytes / (med_t - mad(tot, med_t)) * 1e9 /
                          (1 << 20)) - (file_bytes / med_t * 1e9 / (1 << 20))
            if med_t and (med_t - mad(tot, med_t)) > 0 else 0,
            "wall_per_chunk_ns_median": med_t / chunks if chunks else 0,
            "instructions_median": med_i,
            "instructions_per_byte": med_i / file_bytes if med_i else 0,
            "instructions_per_chunk": med_i / chunks if chunks else 0,
            "cpu_us_median": median(cpu),
            "maxrss_kb_median": median(rss),
            "minflt_median": median(mf),
        }
    return stats


def _least_squares_sse(xs: list[float], ys: list[float]) -> float:
    """SSE of the ordinary least-squares line y = a + b*x over (xs, ys).
    mean(x) and mean(y) are SEPARATE statistics; conflating them puts the
    intercept at the wrong place and biases every segment fit (the H0
    remediation bug)."""
    n = len(xs)
    if n < 2:
        return 0.0  # any line through one point fits it exactly
    xm = sum(xs) / n
    ym = sum(ys) / n
    sxx = sum((x - xm) ** 2 for x in xs)
    sxy = sum((x - xm) * (y - ym) for x, y in zip(xs, ys))
    slope = sxy / sxx if sxx else 0
    intercept = ym - slope * xm
    return sum((y - (intercept + slope * x)) ** 2
               for x, y in zip(xs, ys))


def two_segment_knee(xs: list[float], ys: list[float]):
    """Deterministic two-segment LS fit on (x=log2 chunk, y=MiB/s median).
    Breakpoint = interior point with >=2 points per side minimizing SSE.
    Returns (breakpoint_index, sse_reduction_vs_single_line)."""
    n = len(xs)
    if n < 5:
        return None, 0.0
    sse_single = _least_squares_sse(xs, ys)
    best = None
    for k in range(2, n - 2):
        sse = _least_squares_sse(xs[:k], ys[:k]) + \
            _least_squares_sse(xs[k:], ys[k:])
        if best is None or sse < best[1]:
            best = (k, sse)
    if best is None:
        return None, 0.0
    k, sse = best
    reduction = (sse_single - sse) / sse_single if sse_single else 0
    return k, reduction


def knee_synthetic_diagnostics() -> list[dict]:
    """Analysis-unit diagnostics for two_segment_knee (deterministic
    synthetic curves; NOT scientific measurement of any host):
    A. perfect single line            -> KNEE NOT LOCATED
    B. obvious injected two-segment   -> knee near the known breakpoint
    C. flat (constant) curve          -> no forced knee

    Numerical note: on exactly-linear curves SSE_single degenerates to
    float epsilon and the RELATIVE reduction is only meaningful when the
    single-line residual is well above machine precision (true for every
    real H0 curve: MiB/s-scale residuals, reductions 0.37-0.72). The flat
    case is exact in float (residuals are exactly 0), so it is the
    well-conditioned no-knee construction; a near-flat NOISY curve cannot
    be asserted under a relative rule at n=15 (expected reduction from 2
    extra fit dof alone is ~2/15)."""
    xs = [math.log2(float(c)) for c in CHUNKS]
    diags = []

    ys = [120.0 + 8.0 * x for x in xs]
    k, r = two_segment_knee(xs, ys)
    diags.append({
        "name": "synthetic_single_line",
        "expect": "KNEE NOT LOCATED (reduction < 10%)",
        "breakpoint_index": k, "sse_reduction": round(r, 6),
        "pass": k is None or r < KNEE_SSE_REDUCTION,
    })

    bp = xs.index(math.log2(1048576.0))  # injected breakpoint at 1 MiB
    ys = [200.0 + 90.0 * x if x < xs[bp]
          else 200.0 + 90.0 * xs[bp] + 3.0 * (x - xs[bp]) for x in xs]
    k, r = two_segment_knee(xs, ys)
    diags.append({
        "name": "synthetic_two_segment",
        "expect": f"KNEE near injected breakpoint index {bp} (1 MiB), "
                  f"reduction >= 10%",
        "breakpoint_index": k, "sse_reduction": round(r, 6),
        "pass": k is not None and abs(k - bp) <= 1 and
        r >= KNEE_SSE_REDUCTION,
    })

    ys = [500.0] * len(xs)
    k, r = two_segment_knee(xs, ys)
    diags.append({
        "name": "synthetic_flat",
        "expect": "KNEE NOT LOCATED (no forced knee)",
        "breakpoint_index": k, "sse_reduction": round(r, 6),
        "pass": k is None or r < KNEE_SSE_REDUCTION,
    })
    return diags


def cmd_knee_diagnostics() -> None:
    diags = knee_synthetic_diagnostics()
    for d in diags:
        print(f"{d['name']}: {'PASS' if d['pass'] else 'FAIL'} "
              f"(breakpoint_index={d['breakpoint_index']}, "
              f"sse_reduction={d['sse_reduction']})")
    if not all(d["pass"] for d in diags):
        sys.exit("knee synthetic diagnostics FAILED")
    print("knee synthetic diagnostics: 3/3 PASS "
          "(analysis-unit only, not scientific measurement)")


def sustained_to_boundary(chunks_sorted: list[int], mibps: dict[int, float],
                          plateau_entry: int | None,
                          flat_gain: float = FLAT_GAIN) -> dict:
    """POST-HOC adversarial diagnostic (H0 remediation; NOT part of the
    frozen verdict and NOT a preregistration rewrite): starting at the
    frozen plateau-entry candidate and up to the sampled upper boundary,
    no further >= flat_gain material throughput rise may occur. A frozen
    plateau that is followed by a material rise is a LOCAL-FLATNESS
    CANDIDATE, never 'plateau from here on'."""
    if plateau_entry is None or plateau_entry not in chunks_sorted:
        return {
            "frozen_plateau_chunk": plateau_entry,
            "upper_boundary_chunk": chunks_sorted[-1] if chunks_sorted
            else None,
            "material_rises_after_entry": [],
            "sustained_to_boundary": None,
            "label": "N/A (frozen plateau not located)",
        }
    i0 = chunks_sorted.index(plateau_entry)
    rises = [{"from": a, "to": b, "gain": round(mibps[b] / mibps[a] - 1.0, 4)}
             for a, b in zip(chunks_sorted[i0:], chunks_sorted[i0 + 1:])
             if mibps[b] / mibps[a] - 1.0 >= flat_gain]
    sustained = not rises
    return {
        "frozen_plateau_chunk": plateau_entry,
        "upper_boundary_chunk": chunks_sorted[-1],
        "material_rises_after_entry": rises,
        "sustained_to_boundary": sustained,
        "label": ("SUSTAINED PLATEAU TO THE TESTED BOUNDARY" if sustained
                  else "LOCAL-FLATNESS CANDIDATE, NOT A SUSTAINED PLATEAU "
                       "TO THE TESTED BOUNDARY"),
    }


def plateau_entry(chunks: list[int], mibps: dict[int, float],
                  flat_gain: float = FLAT_GAIN,
                  min_flat_pairs: int = MIN_FLAT_PAIRS):
    """Deterministic plateau entry (prereg §10): smallest chunk c* such
    that at least min_flat_pairs CONSECUTIVE flat pairs exist at and
    after (c*, next), where a pair is flat iff
    mibps(next)/mibps(c) - 1 < flat_gain. Returns (entry_chunk, gains,
    flat_pair_flags) or (None, ...) if not located."""
    gains = {}
    flat = {}
    for a, b in zip(chunks, chunks[1:]):
        g = (mibps.get(b, 0) / mibps.get(a, 1.0)) - 1.0
        gains[a] = g
        flat[a] = g < flat_gain
    entry = None
    for i, c in enumerate(chunks):
        if i >= len(chunks) - 1:
            break  # last chunk has no outgoing pair
        window = chunks[i:len(chunks) - 1]  # pair-start chunks from c on
        if len(window) >= min_flat_pairs and \
                all(flat.get(p, False) for p in window[:min_flat_pairs]):
            entry = c
            break
    return entry, gains, flat


def analyze(session_id: str, file_bytes: int = FILE_BYTES) -> dict:
    runs = load_runs(session_id)
    stats = cells_stats(runs, file_bytes)
    by_depth: dict[int, dict[int, float]] = {}
    for (c, d), s in stats.items():
        by_depth.setdefault(d, {})[c] = s["mibps_median"]

    def sweet(depth: int) -> dict:
        cp = by_depth.get(depth, {})
        if not cp:
            return {"error": "no data"}
        chunks_sorted = sorted(cp)
        peak_v = max(cp.values())
        peak_c = min(c for c in chunks_sorted if cp[c] == peak_v)
        p95_v = P95_FRACTION * peak_v
        p95_c = next((c for c in chunks_sorted if cp[c] >= p95_v), None)
        xs = [math.log2(float(c)) for c in chunks_sorted]
        ys = [cp[c] for c in chunks_sorted]
        knee_i, reduction = two_segment_knee(xs, ys)
        entry, gains, flat = plateau_entry(chunks_sorted, cp)
        return {
            "tested_range_peak_chunk": peak_c,
            "tested_range_peak_mibps": peak_v,
            "peak_at_4m_boundary": peak_c == chunks_sorted[-1],
            "p95_point_chunk": p95_c,
            "p95_threshold_mibps": p95_v,
            "plateau_entry_chunk": entry,
            "pair_gains": {str(a): round(g, 4) for a, g in gains.items()},
            "pair_flat": {str(a): f for a, f in flat.items()},
            "plateau_rule": {
                "flat_gain": FLAT_GAIN, "min_flat_pairs": MIN_FLAT_PAIRS,
            },
            "knee_chunk": chunks_sorted[knee_i] if knee_i is not None else None,
            "knee_sse_reduction": round(reduction, 4),
            "knee_label": ("KNEE" if knee_i is not None and
                           reduction >= KNEE_SSE_REDUCTION
                           else ("NO KNEE (flat)"
                                 if knee_i is not None and reduction >= 0
                                 else "NO KNEE (fit not improved)")),
            # POST-HOC ROBUSTNESS DIAGNOSTIC — does not feed the frozen
            # verdict below (prereg §12 priority order is untouched).
            "sustained_flatness": sustained_to_boundary(chunks_sorted, cp,
                                                        entry),
        }

    sweet_spots = {str(d): sweet(d) for d in DEPTHS}

    # Pareto frontier over the full 60-cell grid (prereg §11):
    # maximize throughput, minimize instructions/byte, minimize
    # in_flight_bytes. Non-dominated set.
    cells = list(stats.values())
    frontier = []
    for a in cells:
        dominated = False
        for b in cells:
            if b is a:
                continue
            if (b["mibps_median"] >= a["mibps_median"] and
                b["instructions_per_byte"] <= a["instructions_per_byte"] and
                b["in_flight_bytes"] <= a["in_flight_bytes"] and
                (b["mibps_median"] > a["mibps_median"] or
                 b["instructions_per_byte"] < a["instructions_per_byte"] or
                 b["in_flight_bytes"] < a["in_flight_bytes"])):
                dominated = True
                break
        if not dominated:
            frontier.append(a)

    # Verdict (prereg §12, priority order).
    last_pair_gain = {}
    for d in DEPTHS:
        cp = by_depth.get(d, {})
        cs = sorted(cp)
        if len(cs) >= 2:
            last_pair_gain[str(d)] = cp[cs[-1]] / cp[cs[-2]] - 1.0
        else:
            last_pair_gain[str(d)] = None
    rising_at_boundary = sum(1 for d in DEPTHS
                             if last_pair_gain.get(str(d)) is not None and
                             last_pair_gain[str(d)] >= FLAT_GAIN)
    peak_at_boundary = sum(1 for d in DEPTHS
                           if sweet_spots[str(d)][
                               "tested_range_peak_chunk"] == CHUNKS[-1])
    entry_located = {str(d): sweet_spots[str(d)]["plateau_entry_chunk"]
                     for d in DEPTHS
                     if sweet_spots[str(d)]["plateau_entry_chunk"] is not None}
    if rising_at_boundary >= 2 and peak_at_boundary >= 1:
        verdict = ("PLATEAU NOT REACHED — EXTENSION REQUIRED "
                   f"({rising_at_boundary}/4 depths rising at 4M boundary)")
    elif len(entry_located) >= 2:
        verdict = ("HOST-LOCAL SWEET REGION LOCATED "
                   f"(plateau entry at {entry_located})")
    elif len(entry_located) == 1:
        verdict = ("DEPTH-SPECIFIC REGIMES "
                   f"(plateau at depth {list(entry_located)[0]} only)")
    else:
        verdict = "NO STABLE SWEET REGION"

    return {
        "sessions": [session_id],
        "rounds": ROUNDS,
        "file_bytes": file_bytes,
        "gates": {
            "gate_errors": json.loads((RESULTS / session_id /
                                       "gates.json").read_text())
            .get("gate_errors", -1),
        },
        "sweet_spots": sweet_spots,
        "pareto": {
            "rule": "non-dominated over (throughput max, "
                    "instructions/byte min, in_flight_bytes min)",
            "frontier": [
                {
                    "chunk": c["chunk"], "chunk_kib": c["chunk_kib"],
                    "depth": c["depth"], "in_flight_bytes": c[
                        "in_flight_bytes"],
                    "mibps_median": round(c["mibps_median"], 2),
                    "instructions_per_byte": round(
                        c["instructions_per_byte"], 4),
                }
                for c in sorted(frontier, key=lambda x: x["mibps_median"],
                                reverse=True)
            ],
            "frontier_size": len(frontier),
        },
        "last_pair_gain_3m_to_4m": last_pair_gain,
        "verdict": verdict,
    }


def cmd_summarize(session_id: str) -> None:
    sd = RESULTS / session_id
    runs = load_runs(session_id)
    gates = json.loads((sd / "gates.json").read_text())
    stats = cells_stats(runs)
    rows = [stats[k] for k in sorted(stats)]
    write_summary(sd, rows)
    analysis = analyze(session_id)
    analysis["gate_errors"] = gates["gate_errors"]
    (sd / "analysis.json").write_text(json.dumps(analysis, indent=1) + "\n")
    print(json.dumps({
        "runs": len(runs),
        "gate_errors": gates["gate_errors"],
        "verdict": analysis["verdict"],
        "last_pair_gain_3m_to_4m": analysis["last_pair_gain_3m_to_4m"],
        "pareto_frontier_size": analysis["pareto"]["frontier_size"],
    }, indent=1))


def write_summary(session_dir: Path, rows: list[dict]) -> None:
    cols = ["chunk", "chunk_kib", "depth", "in_flight_bytes", "in_flight_mib",
            "n", "total_ns_median", "total_ns_mad", "mibps_median",
            "mibps_mad", "wall_per_chunk_ns_median", "instructions_median",
            "instructions_per_byte", "instructions_per_chunk",
            "cpu_us_median", "maxrss_kb_median", "minflt_median"]
    with (session_dir / "summary.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols, lineterminator="\n")
        w.writeheader()
        for r in rows:
            w.writerow({k: (round(v, 6) if isinstance(v, float) else v)
                        for k, v in r.items() if k in cols})
    with (session_dir / "summary.json").open("w") as f:
        json.dump(rows, f, indent=1)


def cmd_status() -> None:
    ok = BENCH.is_file()
    print(f"bench binary: {BENCH} exists={ok}")
    if ok:
        print(f"bench sha256: {sha256_file(BENCH)}")
    p = subprocess.run(["perf", "stat", "-x,", "-e", "instructions:u",
                        "true"], capture_output=True, text=True)
    print(f"perf self-probe: {'OK' if p.returncode == 0 else 'FAIL'}")
    env = environment_json()
    print(f"host: {env['kernel']} / {env['cpu_model']} / page "
          f"{env['page_size']} / cache line {env['cache_line']} / "
          f"governor {env['governor']} / turbo(no_turbo={env['no_turbo']})")


def usage() -> None:
    print(__doc__)
    sys.exit(1)


def main() -> None:
    if len(sys.argv) < 2:
        usage()
    cmd = sys.argv[1]
    if cmd == "status":
        cmd_status()
    elif cmd == "knee-diagnostics" and len(sys.argv) == 2:
        cmd_knee_diagnostics()
    elif cmd == "generate" and len(sys.argv) == 3:
        cmd_generate(sys.argv[2])
    elif cmd in ("validate", "summarize") and len(sys.argv) == 3:
        {"validate": cmd_validate,
         "summarize": cmd_summarize}[cmd](sys.argv[2])
    elif cmd == "sweep" and len(sys.argv) in (3, 4) and \
            (len(sys.argv) == 3 or sys.argv[3] == "--resume"):
        cmd_sweep(sys.argv[2], resume=len(sys.argv) == 4)
    else:
        usage()


if __name__ == "__main__":
    main()
