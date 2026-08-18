#!/usr/bin/env python3
"""Sluice performance-attribution runner (docs/verification/
performance-attribution.md).

Reproducible, machine-readable benchmark driver for the attribution
campaign. Every command records the environment fingerprint (git SHA,
compiler, kernel, CPU, filesystem) next to the measurements so a result is
always attributable to a build + machine + workload, never hand-copied.

Commands:
  ladder    Run grep_attribution_bench (L0..L4) over the workload matrix.
  cli       Run the L6 CLI matrix: sluice-grep vs GNU grep vs ripgrep on
            generated workload files (plus output byte-equality check).
  perf      Run one command under `perf stat`, capture hardware counters.
  compare   Side-by-side before/after table from two result JSON files.
  env       Print the environment fingerprint only.

Results are environment-sensitive; the JSON is the evidence, not a
universal performance claim.

Examples:
  perf-attribution.py ladder --bytes 268435456 --output out.json
  perf-attribution.py cli --bytes 1073741824 --output cli.json
  perf-attribution.py perf -- /path/to/sluice-grep pat file
  perf-attribution.py compare baseline.json optimized.json
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import io
import json
import os
import platform
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BENCH_BIN = REPO / "build" / "linux" / "x86_64" / "release" / "grep_attribution_bench"
GEN_BIN = REPO / "build" / "linux" / "x86_64" / "release" / "grep_workload_gen"
GREP_APPS = {
    "sluice-grep": REPO / "build" / "linux" / "x86_64" / "release" / "sluice-grep",
}
GNU_GREP = "/usr/bin/grep"  # bypass any shell alias (e.g. ugrep wrappers)
RIPGREP = shutil.which("rg") or ""

PERF_EVENTS = [
    "task-clock",
    "cycles",
    "instructions",
    "branches",
    "branch-misses",
    "cache-misses",
    "context-switches",
    "cpu-migrations",
    "page-faults",
]

# Pattern per CLI workload name (kept in sync with grep_workloads.hpp).
CLI_PATTERNS = {
    "normal__p_qz9__d_s": "qz9",
    "normal__p_the__d_s": "the",
    "normal__p_the__d_all": "the",
    "normal__p_1b_e__d_0": "e",
    "normal__p_16b__d_s": "QuiCk_br0wn_f0x!",
    "long__p_the__d_s": "the",
    "binary__p_the__d_s": "the",
}


def run(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, check=True, **kw)


def git_sha() -> dict:
    try:
        sha = subprocess.run(["git", "rev-parse", "HEAD"], cwd=REPO,
                             capture_output=True, text=True, check=True).stdout.strip()
        dirty = bool(subprocess.run(["git", "status", "--short"], cwd=REPO,
                                    capture_output=True, text=True,
                                    check=True).stdout.strip())
        branch = subprocess.run(["git", "rev-parse", "--abbrev-ref", "HEAD"],
                                cwd=REPO, capture_output=True, text=True,
                                check=True).stdout.strip()
        return {"sha": sha, "dirty": dirty, "branch": branch}
    except (subprocess.CalledProcessError, FileNotFoundError):
        return {"sha": "unknown", "dirty": None, "branch": "unknown"}


def env_fingerprint() -> dict:
    cpu = ""
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                cpu = line.split(":", 1)[1].strip()
                break
    except OSError:
        pass
    clang = ""
    try:
        clang = subprocess.run(["clang++", "--version"], capture_output=True,
                               text=True, check=True).stdout.splitlines()[0]
    except (subprocess.CalledProcessError, FileNotFoundError):
        pass
    fs = ""
    try:
        st = os.statvfs(tempfile.gettempdir())
        fs = f"tmpdir on {tempfile.gettempdir()}"
    except OSError:
        pass
    return {
        "time": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "git": git_sha(),
        "kernel": platform.release(),
        "cpu": cpu,
        "nproc": os.cpu_count(),
        "compiler": clang,
        "build_mode": "release",
        "glibc": platform.libc_ver(),
        "workdir_fs": fs,
    }


# ---------------------------------------------------------------------------
# ladder


def cmd_ladder(args) -> dict:
    if not BENCH_BIN.exists():
        sys.exit(f"missing {BENCH_BIN} (xmake build grep_attribution_bench)")
    cmd = [str(BENCH_BIN), "--bytes", str(args.bytes), "--iters", str(args.iters),
           "--warmup", str(args.warmup)]
    if args.stages:
        cmd += ["--stages", args.stages]
    if args.workloads:
        cmd += ["--workloads", args.workloads]
    out = run(cmd)
    rows = list(csv.DictReader(io.StringIO(out.stdout)))
    for r in rows:
        for k in ("bytes", "iters", "ns_min", "ns_med", "ns_max", "matches"):
            r[k] = int(r[k])
        r["gbps_med"] = float(r["gbps_med"])
    return {"kind": "ladder", "rows": rows}


# ---------------------------------------------------------------------------
# cli


def time_cli(cmd: list[str], stdout_to: str) -> tuple[float, int, str]:
    with open(stdout_to, "wb") as devnull:
        t0 = time.perf_counter_ns()
        rc = subprocess.run(cmd, stdout=devnull, stderr=subprocess.DEVNULL).returncode
        t1 = time.perf_counter_ns()
    return (t1 - t0) / 1e9, rc, ""


def cmd_cli(args) -> dict:
    if not GEN_BIN.exists():
        sys.exit(f"missing {GEN_BIN} (xmake build grep_workload_gen)")
    sluice = GREP_APPS["sluice-grep"]
    if not sluice.exists():
        sys.exit(f"missing {sluice} (xmake build sluice-grep)")

    workdir = Path(tempfile.mkdtemp(prefix="sluice-perf-cli-"))
    rows = []

    def md5_file(path: Path) -> str:
        h = hashlib.md5()
        with open(path, "rb") as f:
            for block in iter(lambda: f.read(1 << 20), b""):
                h.update(block)
        return h.hexdigest()

    try:
        for wl, pattern in CLI_PATTERNS.items():
            data = workdir / f"{wl}.bin"
            run([str(GEN_BIN), wl, str(data), "--bytes", str(args.bytes)])
            out_paths = {name: workdir / f"{wl}.{name}.out"
                         for name in ["sluice-grep", "gnugrep", "rg"]
                         if (name != "rg" or RIPGREP)}
            tools = {
                "sluice-grep": [str(sluice), pattern, str(data)],
                "gnugrep": [GNU_GREP, "-F", pattern, str(data)],
            }
            if RIPGREP:
                tools["rg"] = [RIPGREP, "-F", "--no-unicode", pattern, str(data)]
            hashes = {}
            for name, cmd in tools.items():
                times = []
                rcs = set()
                for i in range(args.warmup + args.iters):
                    secs, rc, _ = time_cli(cmd, str(out_paths[name]))
                    if i >= args.warmup:
                        times.append(secs)
                    rcs.add(rc)
                size = data.stat().st_size
                # Hash + free this tool's captured output immediately: a
                # dense workload's three outputs would otherwise exceed
                # tmpfs headroom.
                hashes[name] = md5_file(out_paths[name])
                out_paths[name].unlink(missing_ok=True)
                rows.append({
                    "tool": name, "workload": wl, "pattern": pattern,
                    "bytes": size,
                    "s_min": min(times), "s_med": statistics.median(times),
                    "s_max": max(times),
                    "gbps_med": size / 1e9 / statistics.median(times),
                    "exit_codes": sorted(rcs),
                    "output_md5": hashes[name],
                })
            # Differential check: byte-identical stdout across tools on the
            # FINAL run's captured outputs (literal patterns, single file).
            eq = len(set(hashes.values())) == 1
            for r in reversed(rows):
                if r["workload"] == wl:
                    r["outputs_equal"] = eq
                else:
                    break
            data.unlink(missing_ok=True)
    finally:
        if not args.keep_files:
            shutil.rmtree(workdir, ignore_errors=True)
        else:
            print(f"workdir kept: {workdir}", file=sys.stderr)
    return {"kind": "cli", "rows": rows}


# ---------------------------------------------------------------------------
# perf stat


def cmd_perf(args) -> dict:
    perf = shutil.which("perf")
    if perf is None:
        sys.exit("perf not found in PATH")
    cmd = [perf, "stat", "-x,", "-e", ",".join(PERF_EVENTS), "--"] + args.cmd
    out = subprocess.run(cmd, capture_output=True, text=True, check=True)
    counters = {}
    for line in out.stderr.splitlines():
        fields = [f for f in line.split(",")]
        if len(fields) < 3 or fields[2] not in PERF_EVENTS:
            continue
        try:
            counters[fields[2]] = float(fields[0].replace(",", ""))
        except ValueError:
            pass
    return {"kind": "perf", "cmd": args.cmd, "counters": counters,
            "raw": out.stderr.strip()}


# ---------------------------------------------------------------------------
# compare


def cmd_compare(args) -> dict:
    def load(p):
        return json.loads(Path(p).read_text())

    a, b = load(args.baseline), load(args.optimized)
    key_cols = ("stage", "workload") if a.get("kind") == "ladder" else ("tool", "workload")
    med_col = "ns_med" if a.get("kind") == "ladder" else "s_med"
    ia = {tuple(r[c] for c in key_cols): r for r in a["rows"]}
    ib = {tuple(r[c] for c in key_cols): r for r in b["rows"]}
    print(f"baseline : {a['env']['git']['sha']} {'(dirty)' if a['env']['git']['dirty'] else ''}")
    print(f"optimized: {b['env']['git']['sha']} {'(dirty)' if b['env']['git']['dirty'] else ''}")
    hdr = key_cols + ("before", "after", "ratio(after/before)",)
    print("\t".join(hdr))
    for k in sorted(set(ia) & set(ib)):
        va, vb = ia[k][med_col], ib[k][med_col]
        ratio = vb / va if va else float("inf")
        print("\t".join(map(str, k)) + f"\t{va}\t{vb}\t{ratio:.3f}")
    for k in sorted(set(ia) - set(ib)):
        print("\t".join(map(str, k)) + "\tONLY-IN-BASELINE")
    for k in sorted(set(ib) - set(ia)):
        print("\t".join(map(str, k)) + "\tONLY-IN-OPTIMIZED")
    return {"kind": "compare"}


# ---------------------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="command", required=True)

    p = sub.add_parser("ladder", help="run grep_attribution_bench L0..L4")
    p.add_argument("--bytes", type=int, default=256 << 20)
    p.add_argument("--iters", type=int, default=5)
    p.add_argument("--warmup", type=int, default=1)
    p.add_argument("--stages", default="")
    p.add_argument("--workloads", default="")
    p.add_argument("--output", default="")

    p = sub.add_parser("cli", help="L6 CLI matrix vs GNU grep / rg")
    p.add_argument("--bytes", type=int, default=1 << 30)
    p.add_argument("--iters", type=int, default=5)
    p.add_argument("--warmup", type=int, default=1)
    p.add_argument("--output", default="")
    p.add_argument("--keep-files", action="store_true")

    p = sub.add_parser("perf", help="one command under perf stat")
    p.add_argument("--output", default="")
    p.add_argument("cmd", nargs=argparse.REMAINDER)

    p = sub.add_parser("compare", help="before/after table")
    p.add_argument("baseline")
    p.add_argument("optimized")

    sub.add_parser("env", help="print environment fingerprint")

    args = ap.parse_args()
    if args.command == "env":
        print(json.dumps(env_fingerprint(), indent=2))
        return 0

    result = {"env": env_fingerprint()}
    if args.command == "ladder":
        result.update(cmd_ladder(args))
    elif args.command == "cli":
        result.update(cmd_cli(args))
    elif args.command == "perf":
        if args.cmd and args.cmd[0] == "--":
            args.cmd = args.cmd[1:]
        result.update(cmd_perf(args))
    elif args.command == "compare":
        cmd_compare(args)
        return 0

    text = json.dumps(result, indent=2)
    if getattr(args, "output", ""):
        Path(args.output).write_text(text + "\n")
        print(f"wrote {args.output}", file=sys.stderr)
    else:
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
