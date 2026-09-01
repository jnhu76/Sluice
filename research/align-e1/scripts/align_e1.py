#!/usr/bin/env python3
"""align_e1.py — ALIGN-E1 application-materiality sweep driver (#268).

Runs the frozen preregistration (research/align-e1/ALIGN-E1-PREREGISTRATION.md):
  engine / replica-natural / replica-aligned  x  4K..64K(+1MiB ref) chunks  x
  depth {1,2,4,8}, 512 MiB READ+WRITE copy, R=7 seeded interleaved rounds.

Subcommands:
  generate <session-id>   create the 512 MiB src file (via the bench --generate)
                          and record its sha256.
  validate <session-id>   validation session: 3 modules x {4K,64K,1M} x {d1,d4}
                          x 2 reps + the cycles:u stability probe (prereg §6).
  sweep <session-id>      full frozen sweep (prereg §4/§5): 7 rounds x 120 cells.
  summarize <session-id>  runs.jsonl -> summary.csv/json + analysis.json
                          (sweet spots, materiality, regime, fidelity, verdict
                          per prereg §8-12).

Immutable session layout (B16.5):
  results/<session-id>/{environment.json, manifest.json, gates.json, notes.md,
                        summary.csv, summary.json, raw/runs.jsonl, raw/perf.csv}

Evidence packaging: ONE append-only runs.jsonl (one JSON object per run,
values preserved) + ONE perf.csv (parsed perf -x, values keyed by run_id).
No per-run tiny file trees.
"""

from __future__ import annotations

import csv
import hashlib
import io
import json
import math
import random
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
DATA_DIR = REPO / "build/aligne1-data"
BENCH = REPO / "build/linux/x86_64/release/align_e1_bench"
RESULTS = REPO / "research/align-e1/results"

FILE_BYTES = 536_870_912  # 512 MiB
CHUNKS = [4096, 6144, 8192, 12288, 16384, 24576, 32768, 49152, 65536, 1048576]
PRIMARY_CHUNKS = CHUNKS[:-1]  # 4K..64K (1 MiB is the historical reference)
DEPTHS = [1, 2, 4, 8]
MODULES = ["engine", "replica-natural", "replica-aligned"]
ROUNDS = 7
PREREG_SEED = 0xE1E1E1E121212121
MATERIAL_RATIO = 1.05
MATERIAL_MAD_SCALE = 1.5
P95_FRACTION = 0.95


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
        "meminfo": {k: v.strip() for k, v in (
            l.split(":", 1) for l in Path("/proc/meminfo").read_text()
            .splitlines() if l.startswith(("MemTotal", "MemAvailable")))},
        "page_size": run("getconf", "PAGESIZE"),
        "cache_line": run("getconf", "LEVEL1_DCACHE_LINESIZE"),
        "smbios": run("cat", "/sys/class/dmi/id/product_name"),
        "filesystem": run("findmnt", "-no", "FSTYPE,OPTIONS", "-T", DATA_DIR)
        if DATA_DIR.is_dir() else "?",
        "glibc": run("ldd", "--version").splitlines()[0] if
        len(run("ldd", "--version").splitlines()) > 0 else "?",
        "clang": run("clang", "--version").splitlines()[0] if
        len(run("clang", "--version").splitlines()) > 0 else "?",
        "xmake": run("xmake", "--version").splitlines()[0] if
        len(run("xmake", "--version").splitlines()) > 0 else "?",
        "perf": run("perf", "--version"),
        "perf_paranoid": run("cat", "/proc/sys/kernel/perf_event_paranoid"),
        "git": git_state(),
        "bench_binary_sha256": sha256_file(BENCH) if BENCH.is_file() else "?",
        "bench_binary_size": BENCH.stat().st_size if BENCH.is_file() else 0,
        "vm": not Path("/proc/sys/kernel/hypervisor").exists() and
        "hypervisor" not in lscpu,
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
        "research/align-e1/ALIGN-E1-PREREGISTRATION.md (FROZEN)"
    manifest["data_dir"] = str(DATA_DIR)
    (sd / "manifest.json").write_text(json.dumps(manifest, indent=1) + "\n")
    (sd / "gates.json").write_text("{\n}\n")
    (sd / "notes.md").write_text(f"# {session_id} — notes\n\n"
                                 f"(authored after the session)\n")
    return sd


def perf_cmd() -> list[str]:
    return ["perf", "stat", "-x,", "-e", "instructions:u,cycles:u,task-clock",
            "-o", "/dev/null"]


def parse_perf_stat(text: str) -> dict:
    out = {}
    for line in text.splitlines():
        parts = line.split(",")
        if len(parts) >= 2 and parts[0].endswith((":u", "task-clock")):
            try:
                out[parts[0]] = float(parts[1])
            except ValueError:
                out[parts[0]] = None
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
              module: str, chunk: int, depth: int) -> dict:
    """One measured run under perf; fail-closed in the driver."""
    raw_dir = session_dir / "raw"
    cmd = perf_cmd()[0:3] + ["-o", "/dev/null"] + \
        ["--", str(BENCH), "--run", "--module", module, "--chunk",
         str(chunk), "--depth", str(depth), "--src",
         str(DATA_DIR / "src.bin"), "--dst", str(DATA_DIR / "dst.bin"),
         "--label", run_id]
    t0 = time.monotonic()
    p = subprocess.run(cmd, capture_output=True, text=True)
    tout = p.stdout.strip()
    perf = parse_perf_stat(p.stderr)
    rec = {
        "run_id": run_id,
        "module": module,
        "chunk": chunk,
        "depth": depth,
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
    # Driver-side gates (prereg §7): bench exit, perf exit, dst hash.
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
    for i, (module, chunk, depth) in enumerate(cells):
        run_id = f"{label_prefix}-{i:04d}"
        bench_run(session_dir, gates, manifest, run_id, module, chunk, depth)


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
    manifest = {"base": "master (post-#266)", "kind": "validate",
                "data_src_sha256": sha256_file(DATA_DIR / "src.bin")}
    sd = new_session(session_id, "harness validation (frozen subset)",
                     manifest)
    gates = Gates(sd)
    cells = [(m, c, d)
             for c in (4096, 65536, 1048576)
             for d in (1, 4)
             for m in MODULES
             for _ in range(2)]
    run_matrix(sd, gates, manifest, cells, "val")
    # cycles:u stability probe (prereg §6): 3 consecutive identical runs.
    probe = []
    for c in (4096, 65536):
        for m in ("replica-natural", "replica-aligned"):
            for i in range(3):
                rec = bench_run(sd, gates, manifest,
                                f"probe-{c}-{m}-{i}", m, c, 1)
                cyc = rec.get("perf", {}).get("cycles:u")
                bench = rec.get("bench") or {}
                ops = bench.get("read_ops", 0) + bench.get("write_ops", 0)
                probe.append({"cell": f"{m}/{c}/d1", "rep": i,
                              "cycles_per_op": (cyc / ops) if cyc and ops
                              else None})
    gates.persist(manifest | {"runs_total": len(cells) + 12},
                  runs_total=len(cells) + 12)
    (sd / "notes.md").write_text(
        f"# {session_id} — notes\n\nvalidation subset: 3 modules x "
        f"{{4K,64K,1M}} x {{d1,d4}} x 2 reps = {len(cells)} runs + 12-run\n"
        "cycles stability probe.\n")
    cycles_negative = any(
        p["cycles_per_op"] is not None and p["cycles_per_op"] < 0
        for p in probe)
    print(f"validate: {len(cells) + 12} runs, "
          f"{len(gates.errors)} gate errors")
    probe_label = ("NEGATIVE VALUES -> DEMOTED" if cycles_negative
                   else "no negative values -> upgrade candidate")
    print(f"cycles probe: {probe_label}")
    if len(gates.errors) > 0:
        sys.exit("validation FAILED (gate errors present)")


def cmd_sweep(session_id: str) -> None:
    manifest = {"base": "master (post-#266)", "kind": "sweep",
                "data_src_sha256": sha256_file(DATA_DIR / "src.bin"),
                "rounds": ROUNDS, "chunks": CHUNKS, "depths": DEPTHS,
                "modules": MODULES, "file_bytes": FILE_BYTES}
    sd = new_session(session_id, "frozen chunk x depth x module sweep",
                     manifest)
    gates = Gates(sd)
    cells = [(m, c, d) for c in CHUNKS for d in DEPTHS for m in MODULES]
    total = 0
    for rnd in range(1, ROUNDS + 1):
        order = cells[:]
        rng = random.Random(PREREG_SEED + rnd)
        rng.shuffle(order)
        run_matrix(sd, gates, manifest, order, f"r{rnd}")
        total += len(order)
        print(f"round {rnd}/{ROUNDS} done ({len(gates.errors)} gate errors "
              f"so far)", flush=True)
    gates.persist(manifest | {"runs_total": total}, runs_total=total)
    print(f"sweep: {total} runs, {len(gates.errors)} gate errors")
    if len(gates.errors) > 0:
        sys.exit("sweep FAILED (gate errors present)")


def load_runs(session_id: str) -> list[dict]:
    raw = RESULTS / session_id / "raw" / "runs.jsonl"
    runs = []
    for line in raw.read_text().splitlines():
        if line.strip():
            runs.append(json.loads(line))
    return runs


def cells_stats(runs: list[dict]) -> dict:
    """Per (module, chunk, depth): median/MAD over reps of key metrics."""
    by_cell: dict[tuple, list[dict]] = {}
    for r in runs:
        if not r.get("ok"):
            continue
        key = (r["module"], r["chunk"], r["depth"])
        by_cell.setdefault(key, []).append(r)
    stats = {}
    for (module, chunk, depth), rs in by_cell.items():
        tot = [r["total_ns"] for r in rs]
        eng = [r.get("bench", {}).get("engine_ns", 0) for r in rs]
        con = [r.get("bench", {}).get("construct_ns", 0) for r in rs]
        ins = [r.get("perf", {}).get("instructions:u", 0) for r in rs]
        cpu = [(r.get("bench", {}).get("utime_us", 0) +
                r.get("bench", {}).get("stime_us", 0)) for r in rs]
        chunks = rs[0].get("bench", {}).get("chunks", 1)
        med_t = median(tot)
        med_i = median(ins)
        stats[(module, chunk, depth)] = {
            "n": len(rs),
            "total_ns_median": med_t,
            "total_ns_mad": mad(tot, med_t),
            "engine_ns_median": median(eng),
            "construct_ns_median": median(con),
            "mibps_median": (FILE_BYTES / med_t * 1e9) if med_t else 0,
            "wall_per_chunk_ns_median": med_t / chunks if chunks else 0,
            "instructions_median": med_i,
            "instructions_per_byte": med_i / FILE_BYTES if med_i else 0,
            "instructions_per_chunk": med_i / chunks if chunks else 0,
            "cpu_us_median": median(cpu),
        }
    return stats


def material(stats: dict, module_n: str, module_a: str, chunk: int,
             depth: int) -> bool:
    n = stats.get((module_n, chunk, depth))
    a = stats.get((module_a, chunk, depth))
    if not n or not a or n["n"] < 2 or a["n"] < 2:
        return False
    med_n = n["total_ns_median"]
    med_a = a["total_ns_median"]
    if med_n / med_a < MATERIAL_RATIO:
        return False
    return (med_a + MATERIAL_MAD_SCALE * a["total_ns_mad"] <
            med_n - MATERIAL_MAD_SCALE * n["total_ns_mad"])


def two_segment_knee(xs: list[float], ys: list[float]):
    """Deterministic two-segment LS fit on (x=log2 chunk, y=MiB/s median).
    Breakpoint = interior point with >=2 points per side minimizing SSE.
    Returns (breakpoint_index, sse_reduction_vs_single_line)."""
    n = len(xs)
    if n < 5:
        return None, 0.0
    xm = sum(xs) / n
    ym = sum(ys) / n
    sxx = sum((x - xm) ** 2 for x in xs)
    sxy = sum((x - xm) * (y - ym) for x, y in zip(xs, ys))
    b1 = sxy / sxx if sxx else 0
    a1 = ym - b1 * xm
    sse_single = sum((y - (a1 + b1 * x)) ** 2 for x, y in zip(xs, ys))
    best = None
    for k in range(2, n - 2):
        xl, yl = xs[:k], ys[:k]
        xr, yr = xs[k:], ys[k:]
        sse = 0.0
        for x, y in zip(xl, yl):
            m = sum(yl) / len(yl)
            a = sum((x - m) * (y - m) for x, y in zip(xl, yl))
            b = sum((x - m) ** 2 for x in xl)
            slope = a / b if b else 0
            inter = m - slope * m
            sse += (y - (inter + slope * x)) ** 2
        for x, y in zip(xr, yr):
            m = sum(yr) / len(yr)
            a = sum((x - m) * (y - m) for x, y in zip(xr, yr))
            b = sum((x - m) ** 2 for x in xr)
            slope = a / b if b else 0
            inter = m - slope * m
            sse += (y - (inter + slope * x)) ** 2
        if best is None or sse < best[1]:
            best = (k, sse)
    if best is None:
        return None, 0.0
    k, sse = best
    reduction = (sse_single - sse) / sse_single if sse_single else 0
    return k, reduction


def analyze(session_id: str) -> dict:
    runs = load_runs(session_id)
    stats = cells_stats(runs)
    by_mod_depth: dict[tuple, dict[int, float]] = {}
    for (m, c, d), s in stats.items():
        by_mod_depth.setdefault((m, d), {})[c] = s["mibps_median"]

    def sweet(mod: str, depth: int) -> dict:
        cp = by_mod_depth.get((mod, depth), {})
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
        return {
            "peak_chunk": peak_c,
            "peak_mibps": peak_v,
            "p95_chunk": p95_c,
            "p95_threshold_mibps": p95_v,
            "knee_chunk": chunks_sorted[knee_i] if knee_i is not None else None,
            "knee_sse_reduction": round(reduction, 4),
            "knee_label": ("KNEE" if knee_i is not None and reduction >= 0.10
                           else "NO KNEE (flat)"),
        }

    sweet_spots = {m: {d: sweet(m, d) for d in DEPTHS} for m in MODULES}

    materiality = {}
    for c in CHUNKS:
        depths = [d for d in DEPTHS
                  if material(stats, "replica-natural", "replica-aligned",
                              c, d)]
        materiality[str(c)] = {"depths": depths, "m": len(depths)}

    primary = [c for c in CHUNKS if c != 1048576]
    # STABLE_MATERIAL_REGIME: longest contiguous primary-chunk interval
    # with M(c) >= 2.
    regime = []
    cur = []
    for c in primary:
        if materiality[str(c)]["m"] >= 2:
            cur.append(c)
        else:
            if len(cur) > len(regime):
                regime = cur
            cur = []
    if len(cur) > len(regime):
        regime = cur
    # Crossover: smallest c* with M(c') == 0 for every primary chunk c' >= c*.
    crossover = None
    for i, c in enumerate(primary):
        if all(materiality[str(c2)]["m"] == 0 for c2 in primary[i:]):
            crossover = {"chunk": c, "q": f"materiality disappears from "
                                          f"{c} onward"}
            break

    # Engine fidelity (prereg §11): 40 cells, 0.98..1.02 ratio.
    fid_cells = [(c, d) for c in CHUNKS for d in DEPTHS]
    fid_ok = []
    for c, d in fid_cells:
        e = stats.get(("engine", c, d))
        n = stats.get(("replica-natural", c, d))
        if not e or not n or not e["total_ns_median"]:
            continue
        ratio = e["total_ns_median"] / n["total_ns_median"]
        fid_ok.append((c, d, round(ratio, 4), 0.98 <= ratio <= 1.02))
    fid_frac = (sum(1 for _, _, _, ok in fid_ok if ok) / len(fid_ok)
                if fid_ok else 0)

    # Verdict (prereg §12, priority order).
    chunks_material = {c for c in primary if materiality[str(c)]["m"] >= 1}
    depths_material = {d for d in DEPTHS
                       if any(material(stats, "replica-natural",
                                       "replica-aligned", c, d)
                              for c in primary)}
    broad = (len(chunks_material) >= 6 and len(depths_material) >= 3)
    any_material = any(materiality[str(c)]["m"] > 0 for c in primary)
    if broad:
        verdict = "APP-MATERIAL — SMALL/MEDIUM REGIME"
    elif regime and crossover:
        verdict = "REGIME-SPECIFIC — CROSSOVER LOCATED"
    elif not any_material:
        verdict = "MICROBENCH-ONLY — NOT APPLICATION MATERIAL"
    else:
        verdict = "MIXED — NEED ONE TARGETED DIAGNOSTIC"

    return {
        "sessions": [session_id],
        "rounds": ROUNDS,
        "gates": {
            "gate_errors": json.loads((RESULTS / session_id /
                                       "gates.json").read_text())
            .get("gate_errors", -1),
        },
        "sweet_spots": sweet_spots,
        "materiality": materiality,
        "stable_material_regime": regime,
        "alignment_materiality_crossover": crossover,
        "engine_fidelity": {
            "cells": len(fid_ok),
            "held": sum(1 for _, _, _, ok in fid_ok if ok),
            "fraction": round(fid_frac, 4),
            "conclusion": fid_frac >= 0.75 and len(fid_ok) >= 36,
            "detail": fid_ok,
        },
        "verdict": verdict,
    }


def cmd_summarize(session_id: str) -> None:
    sd = RESULTS / session_id
    runs = load_runs(session_id)
    gates = json.loads((sd / "gates.json").read_text())
    stats = cells_stats(runs)
    rows = []
    for (module, chunk, depth), s in sorted(stats.items()):
        rows.append({
            "module": module, "chunk": chunk,
            "chunk_kib": round(chunk / 1024, 1), "depth": depth, **s})
    write_summary(sd, rows)
    analysis = analyze(session_id)
    analysis["gate_errors"] = gates["gate_errors"]
    (sd / "analysis.json").write_text(json.dumps(analysis, indent=1) + "\n")
    print(json.dumps({
        "runs": len(runs),
        "gate_errors": gates["gate_errors"],
        "verdict": analysis["verdict"],
        "stable_regime": analysis["stable_material_regime"],
        "crossover": analysis["alignment_materiality_crossover"],
        "engine_fidelity": analysis["engine_fidelity"]["conclusion"],
    }, indent=1))


def write_summary(session_dir: Path, rows: list[dict]) -> None:
    cols = ["module", "chunk", "chunk_kib", "depth", "n",
            "total_ns_median", "total_ns_mad", "engine_ns_median",
            "construct_ns_median", "mibps_median", "wall_per_chunk_ns_median",
            "instructions_median", "instructions_per_byte",
            "instructions_per_chunk", "cpu_us_median"]
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
          f"bare-metal={env['vm']}")


def usage() -> None:
    print(__doc__)
    sys.exit(1)


def main() -> None:
    if len(sys.argv) < 2:
        usage()
    cmd = sys.argv[1]
    if cmd == "status":
        cmd_status()
    elif cmd == "generate" and len(sys.argv) == 3:
        cmd_generate(sys.argv[2])
    elif cmd in ("validate", "sweep", "summarize") and len(sys.argv) == 3:
        {"validate": cmd_validate, "sweep": cmd_sweep,
         "summarize": cmd_summarize}[cmd](sys.argv[2])
    else:
        usage()


if __name__ == "__main__":
    main()