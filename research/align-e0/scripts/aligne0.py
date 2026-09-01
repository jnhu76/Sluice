#!/usr/bin/env python3
"""ALIGN-E0 (#265) session driver.

Runs the frozen ALIGN-E0 matrix (research/align-e0/ALIGN-E0-PREREGISTRATION.md):

  micro ladder   arms a0..a7 x dirs {read,write} x sizes {4K,8K,16K,64K,1M}
                 x depths {1,2,4,8,16,32} (synchronous batch, workers=1)
  micro offset   PAGE-OFFSET-E0: offsets {0,16,32,64,128,256,512,1024,2048}
                 x dirs x sizes (gated diagnostic)
  micro threaded SECONDARY TOPOLOGY DIAGNOSTIC: workers {2,4,8,16,32} x
                 dirs x sizes x arms {a0,a7}
  amp           application amplifier: depth {1,2,4,8,16} x arms
                 {engine,natural,best,4096} at 1 MiB, 512 MiB copy

each cell-arm as an R7/R14 process pair wrapped in
  perf stat -x, -e instructions:u,cycles:u
with per-op attribution by double-difference
  (total(R14) - total(R7)) / 7 / ops_per_rep
(the TAX-0-line normalization) plus in-process wall statistics from the
bench JSON (median/MAD over reps). On WSL2, cycles:u is recorded as
UNRELIABLE (virtualized non-monotonic counter, BUF-E0 finding); the
quantitative pair is instructions:u + in-process wall.

Fail-closed same-work gates per cell: identical ops/bytes across arms and
R-pairs; all_reps_ok everywhere; exit code 0. WRITE runs additionally have
the runner hash the target file post-run (== expected deterministic hash);
the amp runner hashes src vs dst post-exit.

Sessions are immutable: written once under
research/align-e0/results/<session-id>/, never edited afterwards.

Usage:
  python3 research/align-e0/scripts/aligne0.py validate --session <id>
  python3 research/align-e0/scripts/aligne0.py run --session <id> --kind ladder
  python3 research/align-e0/scripts/aligne0.py run --session <id> --kind offset
  python3 research/align-e0/scripts/aligne0.py run --session <id> --kind threaded
  python3 research/align-e0/scripts/aligne0.py amp --session <id> --best-align <bytes>
  python3 research/align-e0/scripts/aligne0.py report --session <id>
"""

from __future__ import annotations

import argparse
import csv
import datetime
import hashlib
import json
import os
import platform
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
RESULTS = REPO / "research/align-e0/results"
DATA_DIR = REPO / "build/aligne0-data"
DATA_SRC = DATA_DIR / "src.bin"
DATA_DST = DATA_DIR / "dst.bin"
DATA_BYTES = 256 * 1024 * 1024
# Amplifier uses its own 512 MiB src/dst pair (distinct from the 256 MiB
# microbench data files) so the two matrices never collide on file size.
AMP_SRC = DATA_DIR / "amp-src.bin"
AMP_DST = DATA_DIR / "amp-dst.bin"
AMP_BYTES = 512 * 1024 * 1024
BENCH = REPO / "build/linux/x86_64/release/align_e0_bench"
AMP = REPO / "build/linux/x86_64/release/align_e0_amp_bench"

ARMS = ["a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7"]
SIZES = [4096, 8192, 16384, 65536, 1048576]
DEPTHS = [1, 2, 4, 8, 16, 32]
DIRS = ["read", "write"]
OFFSETS = [0, 16, 32, 64, 128, 256, 512, 1024, 2048]
THREADED_WORKERS = [2, 4, 8, 16, 32]
REP_LO, REP_HI = 7, 14
PERF_EVENTS = ["instructions:u", "cycles:u"]
AMP_DEPTHS = [1, 2, 4, 8, 16]
AMP_ARMS = ["engine", "natural", "best", "4096"]


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
    return {
        "timestamp_utc": datetime.datetime.now(datetime.timezone.utc)
        .isoformat(),
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
            "perf_event_paranoid": Path(
                "/proc/sys/kernel/perf_event_paranoid").read_text().strip(),
        },
        "filesystem": {
            "data_src": str(DATA_SRC),
            "data_dst": str(DATA_DST),
            "amp_src": str(AMP_SRC),
            "amp_dst": str(AMP_DST),
            "data_fs": "ext4 (WSL2 virtual block device)" if wsl2()
            else "native",
            "data_bytes": DATA_BYTES,
            "amp_bytes": AMP_BYTES,
            "cache_regime": "warm (untimed warm sweep before formal reps)",
        },
        "virtualization": {
            "wsl2": wsl2(),
            "classification": "QUALIFIED_BUT_VIRTUALIZED",
            "claim_limit": ("ENVIRONMENT-LIMITED: same-host causal comparison "
                            "and harness validation only; no native Linux "
                            "verdict; final Phase-3 verdict requires native"),
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
            "amp_binary": str(AMP),
            "amp_binary_sha256": sha256_file(AMP),
        },
        "protocol": {
            "perf_events": PERF_EVENTS,
            "rep_pair": [REP_LO, REP_HI],
            "normalization": ("double-difference (total(R14)-total(R7))/7"
                              "/ops_per_rep"),
            "cycles_note": ("cycles:u UNRELIABLE on WSL2 (virtualized "
                            "non-monotonic counter, BUF-E0 finding); "
                            "quantitative pair = instructions:u + wall"),
        },
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

def micro_cmd(mode, d, arm, offset, size, depth, workers, reps, label):
    cmd = [str(BENCH), "--dir", d, "--arm", arm]
    if mode == "offset":
        cmd = [str(BENCH), "--dir", d, "--offset", str(offset)]
    if mode == "threaded":
        cmd = [str(BENCH), "--dir", d, "--mode", "threaded",
               "--workers", str(workers), "--arm", arm]
    cmd += ["--size", str(size), "--reps", str(reps), "--label", label,
            "--file", str(DATA_SRC), "--wfile", str(DATA_DST)]
    return cmd


def micro_tag(mode, d, size, depth, workers, arm, offset, reps):
    sz = f"{size // 1024}k"
    if mode == "offset":
        return f"{d}-{sz}-o{offset}-d{depth}-R{reps}"
    if mode == "threaded":
        return f"{d}-{sz}-w{workers}-{arm}-R{reps}"
    return f"{d}-{sz}-d{depth}-{arm}-R{reps}"


def run_one_micro(mode, d, size, depth, workers, arm, offset, reps,
                  raw_dir: Path):
    tag = micro_tag(mode, d, size, depth, workers, arm, offset, reps)
    cmd = micro_cmd(mode, d, arm, offset, size, depth, workers, reps, tag)
    full = ["perf", "stat", "-x,", "-e", ",".join(PERF_EVENTS), "--"] + cmd
    env = dict(os.environ, LC_ALL="C")
    proc = subprocess.run(full, capture_output=True, text=True, env=env,
                          cwd=REPO)
    (raw_dir / f"{tag}.perf.txt").write_text(proc.stderr)
    bench = None
    if proc.stdout.strip():
        try:
            bench = json.loads(proc.stdout)
        except ValueError:
            try:
                bench = json.loads(proc.stdout.strip().splitlines()[-1])
            except (ValueError, IndexError):
                bench = None
    if bench is not None:
        (raw_dir / f"{tag}.bench.json").write_text(
            json.dumps(bench, indent=1) + "\n")
    return {
        "tag": tag, "mode": mode, "dir": d, "arm": arm, "offset": offset,
        "size": size, "depth": depth, "workers": workers, "reps": reps,
        "cmd": cmd, "returncode": proc.returncode,
        "perf_counters": parse_perf_stat(proc.stderr), "bench": bench,
    }


def run_micro_matrix(session_dir: Path, mode, dirs, arms, sizes, depths,
                     workers_list, offsets, reps_pair=True):
    raw_dir = session_dir / "raw"
    raw_dir.mkdir(parents=True, exist_ok=True)
    records = []
    for d in dirs:
        if mode == "offset":
            cells = [(sz, 1, off) for sz in sizes for off in offsets]
        elif mode == "threaded":
            for sz in sizes:
                for w in workers_list:
                    for arm in arms:
                        for reps in (REP_LO, REP_HI) if reps_pair else (REP_LO,):
                            rec = run_one_micro(mode, d, sz, 1, w, arm, 0,
                                                reps, raw_dir)
                            if rec["returncode"] != 0 or rec["bench"] is None:
                                print(f"FATAL: {rec['tag']} "
                                      f"rc={rec['returncode']}",
                                      file=sys.stderr)
                                sys.exit(3)
                            records.append(rec)
            continue
        else:
            cells = [(sz, dep, 0) for sz in sizes for dep in depths]
        for sz, dep, off in cells:
            for arm in arms:
                pair = {}
                for reps in (REP_LO, REP_HI) if reps_pair else (REP_LO,):
                    rec = run_one_micro(mode, d, sz, dep, 1, arm, off, reps,
                                        raw_dir)
                    if rec["returncode"] != 0 or rec["bench"] is None:
                        print(f"FATAL: {rec['tag']} rc={rec['returncode']}",
                              file=sys.stderr)
                        sys.exit(3)
                    pair[reps] = rec
                    records.append(rec)
    return records


# ---------------------------------------------------------------------------
# Same-work gates + aggregation (micro)
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
        if not b["same_work"].get("ok", False):
            errors.append(f"{r['tag']}: same_work.ok false")
        key = (r["mode"], r["dir"], r["size"], r["depth"], r["workers"])
        groups.setdefault(key, []).append(r)
    for key, recs in sorted(groups.items()):
        per_rep = {x["bench"]["same_work"]["ops"] // x["reps"] for x in recs}
        byts = {x["bench"]["same_work"]["bytes"] // x["reps"] for x in recs}
        if len(per_rep) != 1:
            errors.append(f"{key}: ops/rep differs across runs: {per_rep}")
        if len(byts) != 1:
            errors.append(f"{key}: bytes/rep differs: {byts}")
        kcs = {x["bench"].get("kc") for x in recs}
        wins = {x["bench"].get("window_bytes") for x in recs}
        if len(kcs) != 1 or len(wins) != 1:
            errors.append(f"{key}: kc/window differ: {kcs} {wins}")
    return errors


def summarize_micro(records):
    groups = {}
    for r in records:
        groups.setdefault((r["mode"], r["dir"], r["arm"], r["size"],
                           r["depth"], r["workers"], r["offset"]),
                          {})[r["reps"]] = r
    rows = []
    for key in sorted(groups):
        mode, d, arm, size, depth, workers, offset = key
        pair = groups[key]
        hi = pair.get(REP_HI) or pair.get(REP_LO)
        lo = pair.get(REP_LO)
        if hi is None:
            continue
        b = hi["bench"]
        per_rep = b["same_work"]["ops"] // hi["reps"]
        opr = per_rep
        row = {
            "mode": mode, "dir": d, "arm": arm, "offset": offset,
            "size": size, "depth": depth, "workers": workers,
            "alignment": b.get("alignment"), "page_offset": b.get("page_offset"),
            "kc": b.get("kc"), "ops_per_rep": per_rep,
        }
        vals = [r["io_ns"] for r in hi["bench"]["reps_detail"]]
        m = median(vals)
        row["io_ns_median"] = m
        row["io_ns_mad"] = mad(vals, m)
        if opr:
            row["wall_ns_per_op"] = m / opr
        flt = [r["minflt_io"] for r in hi["bench"]["reps_detail"]]
        row["minflt_io_median"] = median(flt)
        if lo is not None and opr:
            for ev in ("instructions", "cycles"):
                try:
                    hi_v = hi["perf_counters"][ev]
                    lo_v = lo["perf_counters"][ev]
                    row[f"{ev}_per_op"] = (hi_v - lo_v) / \
                        (REP_HI - REP_LO) / opr
                except (KeyError, TypeError):
                    row[f"{ev}_per_op"] = None
        if mode == "threaded":
            top = hi["bench"].get("thread_op_ns")
            if isinstance(top, dict):
                row["thread_op_ns_median"] = top.get("median")
        rows.append(row)
    return rows


# ---------------------------------------------------------------------------
# Amplifier
# ---------------------------------------------------------------------------

def amp_cmd(arm, align, depth, reps, label):
    cmd = [str(AMP), "--arm", arm, "--depth", str(depth), "--reps", str(reps),
           "--file-bytes", str(AMP_BYTES), "--src", str(AMP_SRC),
           "--dst", str(AMP_DST), "--label", label]
    if arm == "aligned":
        cmd += ["--align", str(align)]
    return cmd


def run_amp_matrix(session_dir: Path, best_align: int):
    raw_dir = session_dir / "raw"
    raw_dir.mkdir(parents=True, exist_ok=True)
    records = []
    # prereg §9: if best == 4096, keep only {natural, 4096} for replicas
    arms = AMP_ARMS[:]
    if best_align >= 4096 and "best" in arms:
        arms.remove("best")
    for depth in AMP_DEPTHS:
        for arm in arms:
            # engine has no replica alignment; natural = base+16; best =
            # best_align; 4096 = aligned with --align 4096
            if arm == "engine":
                bench_arm, align = "engine", 0
            elif arm == "natural":
                bench_arm, align = "natural", 16
            elif arm == "best":
                bench_arm, align = "aligned", best_align
            else:  # "4096"
                bench_arm, align = "aligned", 4096
            label = f"amp-d{depth}-{arm}"
            for reps in (REP_LO, REP_HI):
                tag = f"d{depth}-{arm}-R{reps}"
                cmd = amp_cmd(bench_arm, align, depth, reps, label)
                full = ["perf", "stat", "-x,", "-e",
                        ",".join(PERF_EVENTS), "--"] + cmd
                env = dict(os.environ, LC_ALL="C")
                proc = subprocess.run(full, capture_output=True, text=True,
                                      env=env, cwd=REPO)
                (raw_dir / f"{tag}.perf.txt").write_text(proc.stderr)
                bench = None
                if proc.stdout.strip():
                    try:
                        bench = json.loads(proc.stdout)
                    except ValueError:
                        try:
                            bench = json.loads(
                                proc.stdout.strip().splitlines()[-1])
                        except (ValueError, IndexError):
                            bench = None
                if bench is not None:
                    (raw_dir / f"{tag}.bench.json").write_text(
                        json.dumps(bench, indent=1) + "\n")
                if proc.returncode != 0 or bench is None:
                    print(f"FATAL amp: {tag} rc={proc.returncode}",
                          file=sys.stderr)
                    sys.exit(3)
                records.append({
                    "tag": tag, "depth": depth, "arm": arm, "align": align,
                    "reps": reps, "returncode": proc.returncode,
                    "perf_counters": parse_perf_stat(proc.stderr),
                    "bench": bench,
                })
    return records


def amp_gates(records) -> list[str]:
    errors = []
    groups = {}
    for r in records:
        if r["returncode"] != 0:
            errors.append(f"{r['tag']}: rc={r['returncode']}")
        b = r["bench"]
        if b is None:
            errors.append(f"{r['tag']}: no bench JSON")
            continue
        if not all(x["ok"] for x in b["reps_detail"]):
            errors.append(f"{r['tag']}: rep ok false")
        key = r["depth"]
        groups.setdefault(key, []).append(r)
    return errors


def summarize_amp(records):
    groups = {}
    for r in records:
        groups.setdefault((r["depth"], r["arm"]), {})[r["reps"]] = r
    rows = []
    for key in sorted(groups):
        depth, arm = key
        pair = groups[key]
        hi = pair.get(REP_HI) or pair.get(REP_LO)
        lo = pair.get(REP_LO)
        if hi is None:
            continue
        b = hi["bench"]
        en = [x["engine_ns"] for x in b["reps_detail"]]
        tot = [x["engine_ns"] + x["construct_ns"] for x in b["reps_detail"]]
        med_en = median(en)
        row = {
            "depth": depth, "arm": arm, "align": hi["align"],
            "engine_ns_median": med_en,
            "engine_ns_mad": mad(en, med_en),
            "total_ns_median": median(tot),
            "chunks": b["chunks"],
            "bytes_copied": b["reps_detail"][0]["bytes_copied"],
            "reps": hi["reps"],
        }
        if lo is not None:
            for ev in ("instructions", "cycles"):
                try:
                    hi_v = hi["perf_counters"][ev]
                    lo_v = lo["perf_counters"][ev]
                    row[f"{ev}_per_engine_call"] = (hi_v - lo_v) / \
                        (REP_HI - REP_LO)
                except (KeyError, TypeError):
                    row[f"{ev}_per_engine_call"] = None
        rows.append(row)
    return rows


# ---------------------------------------------------------------------------
# Session IO
# ---------------------------------------------------------------------------

def write_summary(session_dir: Path, rows, skips, cols):
    with open(session_dir / "summary.json", "w") as f:
        json.dump({"rows": rows, "skipped": skips}, f, indent=1)
    with open(session_dir / "summary.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols, extrasaction="ignore")
        w.writeheader()
        for r in rows:
            w.writerow(r)


def new_session(session_id: str, purpose: str, manifest: dict):
    session_dir = RESULTS / session_id
    if session_dir.exists():
        print(f"session {session_dir} already exists (immutable)",
              file=sys.stderr)
        sys.exit(2)
    if not BENCH.exists() or not AMP.exists():
        print("bench binaries missing — build first (xmake build "
              "align_e0_bench align_e0_amp_bench)", file=sys.stderr)
        sys.exit(2)
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    if not DATA_SRC.exists() or DATA_SRC.stat().st_size != DATA_BYTES or \
       not DATA_DST.exists() or DATA_DST.stat().st_size != DATA_BYTES:
        subprocess.run([str(BENCH), "--generate", "--file", str(DATA_SRC),
                        "--wfile", str(DATA_DST), "--generate-bytes",
                        str(DATA_BYTES)], check=True)
    env = environment_json()
    env["purpose"] = purpose
    env["data_src_sha256"] = sha256_file(DATA_SRC)
    env["data_dst_sha256"] = sha256_file(DATA_DST)
    session_dir.mkdir(parents=True)
    (session_dir / "environment.json").write_text(
        json.dumps(env, indent=1) + "\n")
    (session_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=1) + "\n")
    (session_dir / "commands.md").write_text(
        "commands.md placeholder — see raw/<tag>.perf.txt and this file\n")
    return session_dir


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------

def cmd_validate(args):
    session_dir = new_session(
        args.session,
        "HARNESS VALIDATION (WSL2 development-only subset): all 8 arms x "
        "dirs {read,write} x sizes {4K,1M} x depths {1,8} — proves the "
        "harness end-to-end (same-work gates, JSON, R-pairs, perf wiring).",
        {"purpose": "validate", "preregistration":
         "research/align-e0/ALIGN-E0-PREREGISTRATION.md",
         "matrix": {"mode": "sync", "arms": ARMS,
                    "dirs": DIRS, "sizes": [4096, 1048576],
                    "depths": [1, 8], "rep_pair": [REP_LO, REP_HI]}})
    records = run_micro_matrix(session_dir, "sync", DIRS, ARMS,
                               [4096, 1048576], [1, 8], [], [0])
    errors = same_work_gate(records)
    (session_dir / "gates.json").write_text(json.dumps(
        {"same_work_errors": errors, "skipped": []}, indent=1) + "\n")
    rows = summarize_micro(records)
    write_summary(session_dir, rows, [],
                  ["mode", "dir", "arm", "offset", "size", "depth", "workers",
                   "alignment", "page_offset", "kc", "ops_per_rep",
                   "io_ns_median", "io_ns_mad", "wall_ns_per_op",
                   "minflt_io_median", "instructions_per_op", "cycles_per_op"])
    print(f"session {args.session}: {len(records)} runs, {len(rows)} rows, "
          f"gate errors: {len(errors)}")
    if errors:
        for e in errors:
            print("GATE:", e, file=sys.stderr)
        sys.exit(3)


def cmd_run(args):
    kind = args.kind
    if kind == "ladder":
        sizes, depths = SIZES, DEPTHS
        arms = ARMS
        manifest = {"purpose": "frozen ALIGN-E0 alignment ladder",
                    "preregistration":
                    "research/align-e0/ALIGN-E0-PREREGISTRATION.md",
                    "matrix": {"mode": "sync", "arms": arms,
                               "dirs": DIRS, "sizes": sizes,
                               "depths": depths,
                               "rep_pair": [REP_LO, REP_HI]}}
    elif kind == "offset":
        sizes, depths = [4096, 65536, 1048576], [1]
        arms = ["a0"]
        manifest = {"purpose": "PAGE-OFFSET-E0 diagnostic (gated)",
                    "preregistration":
                    "research/align-e0/ALIGN-E0-PREREGISTRATION.md",
                    "matrix": {"mode": "offset", "offsets": OFFSETS,
                               "dirs": DIRS, "sizes": sizes,
                               "rep_pair": [REP_LO, REP_HI]}}
    elif kind == "threaded":
        sizes, depths = [65536, 1048576], [1]
        arms = ["a0", "a7"]
        manifest = {"purpose": "SECONDARY TOPOLOGY DIAGNOSTIC (d1/d8 "
                    "overlap adjudication)",
                    "preregistration":
                    "research/align-e0/ALIGN-E0-PREREGISTRATION.md",
                    "matrix": {"mode": "threaded",
                               "workers": THREADED_WORKERS,
                               "arms": arms, "dirs": DIRS,
                               "sizes": sizes,
                               "rep_pair": [REP_LO, REP_HI]}}
    else:
        print("unknown --kind", file=sys.stderr)
        sys.exit(2)
    session_dir = new_session(args.session, f"ALIGN-E0 {kind} matrix", manifest)
    records = run_micro_matrix(session_dir, kind, DIRS, arms, sizes, depths,
                               THREADED_WORKERS if kind == "threaded" else [],
                               OFFSETS if kind == "offset" else [0])
    errors = same_work_gate(records)
    (session_dir / "gates.json").write_text(json.dumps(
        {"same_work_errors": errors, "skipped": []}, indent=1) + "\n")
    rows = summarize_micro(records)
    write_summary(session_dir, rows, [],
                  ["mode", "dir", "arm", "offset", "size", "depth", "workers",
                   "alignment", "page_offset", "kc", "ops_per_rep",
                   "io_ns_median", "io_ns_mad", "wall_ns_per_op",
                   "minflt_io_median", "instructions_per_op", "cycles_per_op",
                   "thread_op_ns_median"])
    print(f"session {args.session}: {len(records)} runs, {len(rows)} rows, "
          f"gate errors: {len(errors)}")
    if errors:
        for e in errors:
            print("GATE:", e, file=sys.stderr)
        sys.exit(3)


def cmd_amp(args):
    session_dir = new_session(
        args.session,
        "ALIGN-E0 application amplifier (prereg §9): 1 MiB x depth "
        f"{AMP_DEPTHS} x arms {AMP_ARMS}, 512 MiB copy, best-align="
        f"{args.best_align}. If best-align >= 4096 the 'best' arm is "
        "dropped (natural + 4096 only). Runner verifies src/dst hashes.",
        {"purpose": "amplifier", "preregistration":
         "research/align-e0/ALIGN-E0-PREREGISTRATION.md",
         "best_align": args.best_align, "matrix": {
             "buffer_size": 1048576, "depths": AMP_DEPTHS,
             "arms": AMP_ARMS, "file_bytes": 512 << 20,
             "rep_pair": [REP_LO, REP_HI]}})
    if not AMP_SRC.exists() or AMP_SRC.stat().st_size != AMP_BYTES:
        subprocess.run([str(AMP), "--generate", "--src", str(AMP_SRC),
                        "--file-bytes", str(AMP_BYTES)], check=True)
        env = json.loads((session_dir / "environment.json").read_text())
        env["data_src_sha256"] = sha256_file(AMP_SRC)
        (session_dir / "environment.json").write_text(
            json.dumps(env, indent=1) + "\n")
    records = run_amp_matrix(session_dir, args.best_align)
    errors = amp_gates(records)
    # runner-side hash verification (prereg §9 / §4): every dst must equal
    # the deterministic src hash
    src_hash = sha256_file(AMP_SRC)
    if AMP_DST.exists():
        dst_hash = sha256_file(AMP_DST)
        if dst_hash != src_hash:
            errors.append(f"dst hash mismatch: {dst_hash} != {src_hash}")
    else:
        errors.append("dst file missing after amp runs")
    (session_dir / "gates.json").write_text(json.dumps(
        {"same_work_errors": errors, "skipped": [],
         "src_sha256": src_hash}, indent=1) + "\n")
    rows = summarize_amp(records)
    write_summary(session_dir, rows, [],
                  ["depth", "arm", "align", "engine_ns_median",
                   "engine_ns_mad", "total_ns_median", "chunks",
                   "bytes_copied", "reps"])
    print(f"session {args.session}: {len(records)} runs, {len(rows)} rows, "
          f"gate errors: {len(errors)}")
    if errors:
        for e in errors:
            print("GATE:", e, file=sys.stderr)
        sys.exit(3)


def cmd_report(args):
    session_dir = RESULTS / args.session
    if not session_dir.exists():
        print(f"no such session: {args.session}", file=sys.stderr)
        sys.exit(2)
    print(f"# {args.session}")
    env = json.loads((session_dir / "environment.json").read_text())
    print(json.dumps(env["git"], indent=1))
    print(f"wsl2={env['virtualization']['wsl2']} "
          f"({env['virtualization']['classification']})")
    gates = json.loads((session_dir / "gates.json").read_text())
    print(f"gate errors: {len(gates['same_work_errors'])}")
    for e in gates["same_work_errors"][:20]:
        print("  GATE:", e)
    summary = json.loads((session_dir / "summary.json").read_text())
    print(f"rows: {len(summary['rows'])}")


def cmd_summarize(args):
    """Regenerate derived summary.json/summary.csv/gates.json from the
    immutable raw/ evidence (bench JSON + perf text). Raw evidence is never
    touched; only the derived aggregation is recomputed."""
    session_dir = RESULTS / args.session
    if not session_dir.exists():
        print(f"no such session: {args.session}", file=sys.stderr)
        sys.exit(2)
    raw_dir = session_dir / "raw"
    if not raw_dir.exists():
        print(f"no raw dir in {args.session}", file=sys.stderr)
        sys.exit(2)
    bench_jsons = sorted(raw_dir.glob("*.bench.json"))
    if not bench_jsons:
        print(f"no bench json in {args.session}/raw", file=sys.stderr)
        sys.exit(2)
    first = json.loads(bench_jsons[0].read_text())
    if first.get("bench") == "align_e0_bench":
        records = []
        for bf in bench_jsons:
            tag = bf.name[: -len(".bench.json")]
            b = json.loads(bf.read_text())
            perf = parse_perf_stat(
                (raw_dir / f"{tag}.perf.txt").read_text())
            records.append({
                "tag": tag, "mode": b["mode"], "dir": b["dir"],
                "arm": b["arm"], "offset": b.get("offset", 0),
                "size": b["size"], "depth": b["depth"],
                "workers": b["workers"], "reps": b["reps"],
                "returncode": 0, "perf_counters": perf, "bench": b,
            })
        errors = same_work_gate(records)
        (session_dir / "gates.json").write_text(json.dumps(
            {"same_work_errors": errors, "skipped": []}, indent=1) + "\n")
        rows = summarize_micro(records)
        write_summary(session_dir, rows, [],
                      ["mode", "dir", "arm", "offset", "size", "depth",
                       "workers", "alignment", "page_offset", "kc",
                       "ops_per_rep", "io_ns_median", "io_ns_mad",
                       "wall_ns_per_op", "minflt_io_median",
                       "instructions_per_op", "cycles_per_op",
                       "thread_op_ns_median"])
    elif first.get("bench") == "align_e0_amp_bench":
        records = []
        for bf in bench_jsons:
            tag = bf.name[: -len(".bench.json")]
            b = json.loads(bf.read_text())
            perf = parse_perf_stat(
                (raw_dir / f"{tag}.perf.txt").read_text())
            # recover the original arm name from the immutable tag
            # (d{depth}-{arm}-R{reps}); the bench JSON collapses best/4096
            # into arm="aligned" with differing alignment values
            prefix = f"d{b['depth']}-"
            suffix = f"-R{b['reps']}"
            arm = tag[len(prefix):len(tag) - len(suffix)] if \
                tag.startswith(prefix) and tag.endswith(suffix) else b["arm"]
            records.append({
                "tag": tag, "depth": b["depth"], "arm": arm,
                "align": b["alignment"], "reps": b["reps"],
                "returncode": 0, "perf_counters": perf, "bench": b,
            })
        errors = amp_gates(records)
        (session_dir / "gates.json").write_text(json.dumps(
            {"same_work_errors": errors, "skipped": []}, indent=1) + "\n")
        rows = summarize_amp(records)
        write_summary(session_dir, rows, [],
                      ["depth", "arm", "align", "engine_ns_median",
                       "engine_ns_mad", "total_ns_median", "chunks",
                       "bytes_copied", "reps", "instructions_per_engine_call",
                       "cycles_per_engine_call"])
    else:
        print(f"unknown bench type: {first.get('bench')}", file=sys.stderr)
        sys.exit(2)
    print(f"session {args.session}: {len(records)} runs, {len(rows)} rows, "
          f"gate errors: {len(errors)}")
    if errors:
        for e in errors:
            print("GATE:", e, file=sys.stderr)
        sys.exit(3)


def main():
    ap = argparse.ArgumentParser(prog="aligne0.py")
    sub = ap.add_subparsers(dest="cmd", required=True)
    v = sub.add_parser("validate")
    v.add_argument("--session", required=True)
    r = sub.add_parser("run")
    r.add_argument("--session", required=True)
    r.add_argument("--kind", choices=["ladder", "offset", "threaded"],
                   required=True)
    a = sub.add_parser("amp")
    a.add_argument("--session", required=True)
    a.add_argument("--best-align", type=int, required=True)
    rep = sub.add_parser("report")
    rep.add_argument("--session", required=True)
    sm = sub.add_parser("summarize")
    sm.add_argument("--session", required=True)
    args = ap.parse_args()
    if args.cmd == "validate":
        cmd_validate(args)
    elif args.cmd == "run":
        cmd_run(args)
    elif args.cmd == "amp":
        cmd_amp(args)
    elif args.cmd == "summarize":
        cmd_summarize(args)
    else:
        cmd_report(args)


if __name__ == "__main__":
    main()
