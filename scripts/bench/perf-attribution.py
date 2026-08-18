#!/usr/bin/env python3
"""Sluice performance-attribution runner (docs/verification/
performance-attribution.md).

Reproducible, machine-readable benchmark driver for the attribution
campaign. Every command records the environment fingerprint (git SHA,
build, kernel, CPU, WSL state, filesystem mounts, tool versions) next to
the measurements so a result is always attributable to a build + machine +
workload, never hand-copied. Committed evidence artifacts are additionally
validated by scripts/bench/perf-evidence-validate.py.

Commands:
  ladder    Run grep_attribution_bench (L0..L4) over the workload matrix.
  cli       Run the L6 CLI matrix: sluice-grep vs GNU grep vs ripgrep on
            generated workload files (plus output byte-equality check).
  perf      Run one command under `perf stat`, capture hardware counters.
  compare   Side-by-side before/after table from two result JSON files,
            with environment-compatibility warnings.
  env       Print the environment fingerprint only.
  self-test Hermetic unit tests for this runner's pure logic (no builds).

Tool exit-code semantics (identical for sluice-grep, GNU grep, ripgrep):
0 = match found, 1 = no match (a legitimate benchmark outcome, NOT an
infrastructure failure), 2 = tool error (recorded as tool_error; the row
is invalid as performance evidence). The runner never treats a no-match
exit as a benchmark failure.

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
import unittest
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

# Exit-code classification for every timed competitor (grep family): 0/1 are
# legitimate data (match/no-match), 2 (or anything else) is a tool error.
VALID_DATA_EXIT_CODES = {0, 1}

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

LADDER_CORE_PAIR = ("L4_sluice", "L3_pread_matcher")


def run(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, check=True, **kw)


# ---------------------------------------------------------------------------
# Environment fingerprint. Optional metadata degrades to null/"unknown";
# the runner must never crash because one probe is unavailable.
# ---------------------------------------------------------------------------


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


def _first_line(cmd: list[str]) -> str | None:
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, check=True)
        return out.stdout.splitlines()[0].strip() if out.stdout else None
    except (subprocess.CalledProcessError, FileNotFoundError, IndexError):
        return None


def detect_wsl() -> str:
    """Classify WSL2 | WSL1 | native | unknown from kernel release/version."""
    try:
        rel = Path("/proc/sys/kernel/osrelease").read_text().lower()
        ver = Path("/proc/version").read_text().lower()
        text = rel + " " + ver
        if "wsl2" in text or ("microsoft" in text and "-standard-" in rel):
            return "WSL2"
        if "microsoft" in text:
            return "WSL1"
        return "native"
    except OSError:
        return "unknown"


def parse_mountinfo(text: str, path: str) -> dict | None:
    """Longest-mount-point-prefix match for `path` in /proc/self/mountinfo.

    Returns {"mount_point":..., "type":...} or None. Field layout (see
    proc_pid_mountinfo(5)): fields 0..3 ids/root, field 4 mount point; after
    the " - " separator the first field is the filesystem type. Octal
    escapes (\\040 space, \\011 tab, \\012 newline, \\134 backslash) are
    decoded before matching.
    """
    best = None
    norm = os.path.normpath(path)
    for line in text.splitlines():
        if " - " not in line:
            continue
        left, right = line.split(" - ", 1)
        fields = left.split()
        if len(fields) < 5 or not right.split():
            continue
        mp = (fields[4].replace("\\040", " ").replace("\\011", "\t")
                      .replace("\\012", "\n").replace("\\134", "\\"))
        fstype = right.split()[0]
        if norm == mp or norm.startswith(mp.rstrip("/") + "/"):
            if best is None or len(mp) > len(best["mount_point"]):
                best = {"mount_point": mp, "type": fstype}
    return best


def mount_info(path: str | None) -> dict | None:
    """Mount record for `path` via /proc/self/mountinfo (preferred) or None."""
    if not path:
        return None
    try:
        text = Path("/proc/self/mountinfo").read_text()
        resolved = str(Path(path).resolve())
        found = parse_mountinfo(text, resolved)
        if found is None:
            return None
        return {"path": path, "mount_point": found["mount_point"],
                "type": found["type"]}
    except OSError:
        return None


def _cpu_model() -> str | None:
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return None


def _tool_version(cmd: list[str]) -> str | None:
    return _first_line(cmd + ["--version"])


def _glibc_ver() -> str | None:
    name, ver = platform.libc_ver()
    return f"{name} {ver}" if name and ver else None


def env_fingerprint(input_path: str | None = None,
                    output_path: str | None = None) -> dict:
    # Record the invoked driver plus its full version banner; a bare number
    # would drop distribution context ("Ubuntu clang version 21.1.8").
    compiler_banner = _first_line(["clang++", "--version"])
    return {
        "time": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "git": git_sha(),
        "build": {
            "mode": "release",
            "compiler": "clang++",
            "compiler_version": compiler_banner,
        },
        "system": {
            "kernel": platform.release() or None,
            "platform": platform.machine() or None,
            "cpu": _cpu_model(),
            "logical_cpus": os.cpu_count(),
            "glibc": _glibc_ver(),
            "python": platform.python_version(),
        },
        "environment": {
            "wsl": detect_wsl(),
        },
        "filesystem": {
            # Where benchmark data files live (workload bytes in / out).
            "input": mount_info(input_path),
            # Where captured tool stdout goes (null when suppressed).
            "output": mount_info(output_path),
        },
        "tools": {
            "gnu_grep": _tool_version([GNU_GREP]),
            "ripgrep": _tool_version([RIPGREP]) if RIPGREP else None,
        },
    }


# ---------------------------------------------------------------------------
# Derived metrics (docs/verification/performance-engineering.md §ratios).
# ---------------------------------------------------------------------------


def median(samples: list[float]) -> float:
    """True mathematical median (mean of middle pair for even n)."""
    return statistics.median(samples)


def ladder_derived(rows: list[dict]) -> list[dict]:
    """Per-workload Core increment metrics from L4/L3 rows.

    L4 - L3 is the AGGREGATE Core increment for the measured
    workload/backend/buffer configuration; it is not decomposed among
    runtime lifecycle, admission, submit, handoff, syscall interaction,
    wait/wake, reap, or Fiber resume (that requires the Core Cost
    Decomposition experiment).
    """
    by_key = {(r["stage"], r["workload"]): r for r in rows}
    out = []
    for r in rows:
        if r["stage"] != LADDER_CORE_PAIR[0]:
            continue
        wl = r["workload"]
        l3 = by_key.get((LADDER_CORE_PAIR[1], wl))
        if l3 is None:
            continue
        inc = r["ns_med"] - l3["ns_med"]
        gib = r["bytes"] / (1 << 30)
        out.append({
            "workload": wl,
            "bytes": r["bytes"],
            "core_increment_ns": inc,
            "core_increment_ms_per_gib": (inc / 1e6 / gib) if gib else None,
            "core_overhead_ratio": (inc / l3["ns_med"]) if l3["ns_med"] else None,
            "core_share": (inc / r["ns_med"]) if r["ns_med"] else None,
        })
    return out


# ---------------------------------------------------------------------------
# ladder
# ---------------------------------------------------------------------------


def cmd_ladder(args) -> dict:
    if not BENCH_BIN.exists():
        sys.exit(f"missing {BENCH_BIN} (xmake build grep_attribution_bench)")
    cmd = [str(BENCH_BIN), "--bytes", str(args.bytes), "--iters", str(args.iters),
           "--warmup", str(args.warmup), "--buffer-size", str(args.buffer_size)]
    if args.stages:
        cmd += ["--stages", args.stages]
    if args.workloads:
        cmd += ["--workloads", args.workloads]
    out = run(cmd)
    rows = list(csv.DictReader(io.StringIO(out.stdout)))
    for r in rows:
        for k in ("bytes", "iters", "ns_min", "ns_max", "matches"):
            r[k] = int(r[k])
        r["ns_med"] = float(r["ns_med"])
        r["gbps_med"] = float(r["gbps_med"])
        r["ns_samples"] = [int(s) for s in r["ns_samples"].split(";") if s]
    return {
        "kind": "ladder",
        "params": {
            "bytes": args.bytes, "iters": args.iters,
            "warmup": args.warmup, "buffer_size": args.buffer_size,
            # Recorded so a filtered (subset) artifact can never pose as a
            # full-matrix run.
            "stages": args.stages or None,
            "workloads": args.workloads or None,
        },
        "rows": rows,
        "derived": ladder_derived(rows),
    }


# ---------------------------------------------------------------------------
# cli
# ---------------------------------------------------------------------------


def classify_exit_codes(rcs: list[int]) -> dict:
    """Split recorded exit codes into legitimate data vs tool errors."""
    bad = sorted(set(rcs) - VALID_DATA_EXIT_CODES)
    return {"exit_codes": sorted(set(rcs)), "tool_error": bool(bad)}


def time_cli(cmd: list[str], stdout_to: str) -> tuple[float, int]:
    # "wb" truncates on every open: each iteration measures a fresh capture,
    # never stale appended output.
    with open(stdout_to, "wb") as out:
        t0 = time.perf_counter_ns()
        rc = subprocess.run(cmd, stdout=out, stderr=subprocess.DEVNULL).returncode
        t1 = time.perf_counter_ns()
    return (t1 - t0) / 1e9, rc


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
                # Output is symmetric: every competitor's stdout goes to its
                # own captured file on the same filesystem, including
                # sluice-grep. Nobody gets /dev/null while others pay for I/O.
                "sluice-grep": [str(sluice), pattern, str(data)],
                "gnugrep": [GNU_GREP, "-F", pattern, str(data)],
            }
            if RIPGREP:
                tools["rg"] = [RIPGREP, "-F", "--no-unicode", pattern, str(data)]
            hashes = {}
            for name, cmd in tools.items():
                times = []
                rcs = []
                for i in range(args.warmup + args.iters):
                    secs, rc = time_cli(cmd, str(out_paths[name]))
                    if i >= args.warmup:
                        times.append(secs)
                    rcs.append(rc)
                size = data.stat().st_size
                classification = classify_exit_codes(rcs)
                if classification["tool_error"]:
                    print(f"WARNING: {name} on {wl} exited with tool-error "
                          f"codes {classification['exit_codes']}; row is "
                          f"invalid as evidence", file=sys.stderr)
                # Hash + free this tool's captured output immediately: a
                # dense workload's three outputs would otherwise exceed
                # tmpfs headroom. output_bytes records how much of the run
                # was output/materialization work (dense-output visibility).
                hashes[name] = md5_file(out_paths[name])
                out_bytes = out_paths[name].stat().st_size
                out_paths[name].unlink(missing_ok=True)
                rows.append({
                    "tool": name, "workload": wl, "pattern": pattern,
                    "bytes": size,
                    "s_min": min(times), "s_med": median(times),
                    "s_max": max(times),
                    "s_samples": times,
                    "gbps_med": size / 1e9 / median(times),
                    "exit_codes": classification["exit_codes"],
                    "tool_error": classification["tool_error"],
                    "output_md5": hashes[name],
                    "output_bytes": out_bytes,
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
    return {
        "kind": "cli",
        "params": {"bytes": args.bytes, "iters": args.iters,
                   "warmup": args.warmup},
        "rows": rows,
    }


# ---------------------------------------------------------------------------
# perf stat
# ---------------------------------------------------------------------------


def parse_perf_stat(stderr_text: str) -> dict:
    counters = {}
    for line in stderr_text.splitlines():
        fields = line.split(",")
        # Field layout is value[,unit],event,runtime,... — but the value may
        # itself carry thousands separators ("4,567"), so split-then-replace
        # is wrong. Locate the first field that is an event name; everything
        # before the preceding unit field is the value. Event names may
        # carry a modifier suffix (":u" when perf_event_paranoid forces
        # user-space-only counting, e.g. under WSL2); strip it for both the
        # match and the canonical key.
        idx = next((i for i, f in enumerate(fields)
                    if f.split(":", 1)[0] in PERF_EVENTS), None)
        if idx is None or idx < 2:
            continue
        name = fields[idx].split(":", 1)[0]
        try:
            counters[name] = float("".join(fields[: idx - 1]))
        except ValueError:
            pass
    return counters


def cmd_perf(args) -> dict:
    perf = shutil.which("perf")
    if perf is None:
        sys.exit("perf not found in PATH")
    cmd = [perf, "stat", "-x,", "-e", ",".join(PERF_EVENTS), "--"] + args.cmd
    # LC_ALL=C pins the CSV number format: a comma-decimal locale would make
    # perf emit "596,24" for 596.24, indistinguishable from thousands
    # separators at parse time. perf propagates the CHILD's exit status, and
    # a no-match grep legitimately exits 1 — record it instead of raising.
    env = dict(os.environ, LC_ALL="C")
    out = subprocess.run(cmd, capture_output=True, text=True, env=env)
    counters = parse_perf_stat(out.stderr)
    result = {"kind": "perf", "cmd": args.cmd,
              "child_exit_code": out.returncode,
              "counters": counters,
              "raw": out.stderr.strip()}
    if out.returncode not in (0,) and out.returncode not in VALID_DATA_EXIT_CODES:
        print(f"WARNING: measured command exited {out.returncode} (tool "
              f"error); counters may not be valid evidence", file=sys.stderr)
    # Derived normalized ratios (ns/request, cycles/request, ...) exist only
    # when the caller states the request count; the divisor is recorded so
    # the ratios stay recomputable from the artifact. PMU counters are
    # diagnostic evidence, not optimization authorization.
    if args.requests and args.requests > 0:
        result["params"] = {"requests": args.requests}
        result["derived"] = {f"{k}_per_request": v / args.requests
                             for k, v in counters.items()}
    return result


# ---------------------------------------------------------------------------
# compare
# ---------------------------------------------------------------------------

# Fingerprint fields that make two artifacts materially non-comparable.
_COMPAT_FIELDS = [
    ("system.cpu", ("env", "system", "cpu")),
    ("system.kernel", ("env", "system", "kernel")),
    ("system.logical_cpus", ("env", "system", "logical_cpus")),
    ("system.glibc", ("env", "system", "glibc")),
    ("build.mode", ("env", "build", "mode")),
    ("build.compiler_version", ("env", "build", "compiler_version")),
    ("environment.wsl", ("env", "environment", "wsl")),
    ("filesystem.input.type", ("env", "filesystem", "input", "type")),
    ("filesystem.output.type", ("env", "filesystem", "output", "type")),
    ("tools.gnu_grep", ("env", "tools", "gnu_grep")),
    ("tools.ripgrep", ("env", "tools", "ripgrep")),
]


def _dig(d: dict, path: tuple) -> object:
    cur = d
    for k in path:
        if not isinstance(cur, dict) or k not in cur:
            return None
        cur = cur[k]
    return cur


def compat_warnings(a: dict, b: dict) -> list[str]:
    """Material environment/config differences between two artifacts.

    Different CPU, filesystem, build mode, compiler, WSL/native state,
    workload size, or iteration count means the numbers are not directly
    comparable; warn instead of silently presenting them as such.
    """
    warns = []
    for label, path in _COMPAT_FIELDS:
        va, vb = _dig(a, path), _dig(b, path)
        if va is not None and vb is not None and va != vb:
            warns.append(f"{label}: {va!r} vs {vb!r}")
    for k in ("bytes", "iters", "buffer_size"):
        pa, pb = _dig(a, ("params", k)), _dig(b, ("params", k))
        if pa is not None and pb is not None and pa != pb:
            warns.append(f"params.{k}: {pa!r} vs {pb!r}")
    da, db = _dig(a, ("env", "git", "dirty")), _dig(b, ("env", "git", "dirty"))
    if da != db:
        warns.append(f"git.dirty: {da!r} vs {db!r} "
                     f"(one side measured with a dirty tree)")
    return warns


def cmd_compare(args) -> dict:
    def load(p):
        return json.loads(Path(p).read_text())

    a, b = load(args.baseline), load(args.optimized)
    for w in compat_warnings(a, b):
        print(f"WARNING (not directly comparable): {w}", file=sys.stderr)
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
    # Aggregate Core increment (L4-L3) before/after, when both sides are
    # ladders: the number a Core-side change must actually move.
    if a.get("kind") == "ladder" and b.get("kind") == "ladder":
        da = {d["workload"]: d for d in a.get("derived", [])}
        db_ = {d["workload"]: d for d in b.get("derived", [])}
        if da and db_:
            print("\naggregate core increment L4-L3 (ms/GiB)")
            for wl in sorted(set(da) & set(db_)):
                va = da[wl]["core_increment_ms_per_gib"]
                vb = db_[wl]["core_increment_ms_per_gib"]
                print(f"{wl}\t{va:.2f}\t{vb:.2f}")
    return {"kind": "compare"}


# ---------------------------------------------------------------------------
# self-test (hermetic; exercises this file's pure logic without building)
# ---------------------------------------------------------------------------


def _positive_int(s: str) -> int:
    v = int(s)
    if v < 1:
        raise argparse.ArgumentTypeError(f"must be >= 1, got {v}")
    return v


def _nonneg_int(s: str) -> int:
    v = int(s)
    if v < 0:
        raise argparse.ArgumentTypeError(f"must be >= 0, got {v}")
    return v


class RunnerSelfTest(unittest.TestCase):
    def test_median_even_and_odd(self):
        self.assertEqual(median([3.0, 1.0, 2.0]), 2.0)
        self.assertEqual(median([4.0, 1.0, 3.0, 2.0]), 2.5)

    def test_classify_exit_codes(self):
        c = classify_exit_codes([0, 1, 0])
        self.assertEqual(c["exit_codes"], [0, 1])
        self.assertFalse(c["tool_error"])  # grep no-match (1) is data, not error
        c = classify_exit_codes([0, 2])
        self.assertTrue(c["tool_error"])

    MOUNTINFO = (
        "36 35 0:53 / /tmp rw - tmpfs tmpfs rw\n"
        "28 27 0:53 /repositories/perf /home/hoo/Projects/perf-performance-attribution rw - ext4 /dev/sdc2 rw\n"
    )

    def test_parse_mountinfo(self):
        got = parse_mountinfo(self.MOUNTINFO, "/tmp/sluice_bench/x.bin")
        self.assertEqual(got, {"mount_point": "/tmp", "type": "tmpfs"})
        got = parse_mountinfo(self.MOUNTINFO,
                              "/home/hoo/Projects/perf-performance-attribution/bench")
        self.assertEqual(got["mount_point"],
                         "/home/hoo/Projects/perf-performance-attribution")
        self.assertEqual(got["type"], "ext4")
        self.assertIsNone(parse_mountinfo(self.MOUNTINFO, "/nonexistent-elsewhere"))

    def test_ladder_derived(self):
        rows = [
            {"stage": "L3_pread_matcher", "workload": "w", "bytes": 1 << 30,
             "ns_med": 1.0e8},
            {"stage": "L4_sluice", "workload": "w", "bytes": 1 << 30,
             "ns_med": 1.4e8},
        ]
        d = ladder_derived(rows)
        self.assertEqual(len(d), 1)
        self.assertAlmostEqual(d[0]["core_increment_ns"], 4.0e7)
        self.assertAlmostEqual(d[0]["core_increment_ms_per_gib"], 40.0)
        self.assertAlmostEqual(d[0]["core_overhead_ratio"], 0.4)
        self.assertAlmostEqual(d[0]["core_share"], 4.0e7 / 1.4e8)

    def test_compat_warnings(self):
        base = {"env": {"system": {"cpu": "x", "kernel": "k"},
                        "build": {"mode": "release", "compiler_version": "21"},
                        "environment": {"wsl": "WSL2"},
                        "filesystem": {"input": {"type": "tmpfs"}},
                        "tools": {"gnu_grep": "grep 3.11"},
                        "git": {"dirty": False}},
                "params": {"bytes": 1, "iters": 5}}
        same = json.loads(json.dumps(base))
        self.assertEqual(compat_warnings(base, same), [])
        other = json.loads(json.dumps(base))
        other["env"]["system"]["cpu"] = "y"
        other["params"]["iters"] = 4
        other["env"]["git"]["dirty"] = True
        warns = compat_warnings(base, other)
        self.assertTrue(any("system.cpu" in w for w in warns))
        self.assertTrue(any("params.iters" in w for w in warns))
        self.assertTrue(any("git.dirty" in w for w in warns))

    def test_parse_perf_stat(self):
        # Values may carry thousands separators ("4,567") in the CSV value
        # field; naive split-then-check-fields[2] drops such lines entirely.
        # Event names may carry a ":u" modifier (user-space-only counters
        # under perf_event_paranoid, e.g. WSL2) — the canonical unmodified
        # name must still be recognized and used as the key.
        text = ("1.23,seconds,task-clock,100.0\n"
                "4,567,,instructions,50.0\n"
                "389384,,cycles:u,514668,100.00,,\n"
                "garbage-line\n")
        c = parse_perf_stat(text)
        self.assertEqual(c, {"task-clock": 1.23, "instructions": 4567.0,
                             "cycles": 389384.0})


def cmd_self_test(_args) -> int:
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(RunnerSelfTest)
    result = unittest.TextTestRunner(verbosity=1).run(suite)
    return 0 if result.wasSuccessful() else 1


# ---------------------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="command", required=True)

    p = sub.add_parser("ladder", help="run grep_attribution_bench L0..L4")
    p.add_argument("--bytes", type=int, default=256 << 20)
    p.add_argument("--iters", type=_positive_int, default=5)
    p.add_argument("--warmup", type=_nonneg_int, default=1)
    p.add_argument("--buffer-size", type=int, default=1 << 20)
    p.add_argument("--stages", default="")
    p.add_argument("--workloads", default="")
    p.add_argument("--output", default="")
    p.add_argument("--note", default="",
                   help="provenance note embedded in the artifact")

    p = sub.add_parser("cli", help="L6 CLI matrix vs GNU grep / rg")
    p.add_argument("--bytes", type=int, default=1 << 30)
    p.add_argument("--iters", type=_positive_int, default=5)
    p.add_argument("--warmup", type=_nonneg_int, default=1)
    p.add_argument("--output", default="")
    p.add_argument("--keep-files", action="store_true")
    p.add_argument("--note", default="",
                   help="provenance note embedded in the artifact")

    p = sub.add_parser("perf", help="one command under perf stat")
    p.add_argument("--output", default="")
    p.add_argument("--requests", type=int, default=0,
                   help="request count for derived per-request ratios")
    p.add_argument("--note", default="",
                   help="provenance note embedded in the artifact")
    p.add_argument("cmd", nargs=argparse.REMAINDER)

    p = sub.add_parser("compare", help="before/after table")
    p.add_argument("baseline")
    p.add_argument("optimized")

    sub.add_parser("env", help="print environment fingerprint")

    sub.add_parser("self-test", help="hermetic unit tests for runner logic")

    args = ap.parse_args()
    if args.command == "env":
        print(json.dumps(env_fingerprint(input_path=os.environ.get("TMPDIR", "/tmp")),
                         indent=2))
        return 0
    if args.command == "self-test":
        return cmd_self_test(args)

    # CLI captures competitor stdout under the temp dir; the ladder writes
    # suppressed output only, and `perf` measures an arbitrary user command
    # whose output filesystem we cannot know — record null there instead of
    # guessing.
    tmpdir = os.environ.get("TMPDIR", "/tmp")
    out_fs = tmpdir if args.command == "cli" else None
    result = {"env": env_fingerprint(input_path=tmpdir, output_path=out_fs)}
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

    if getattr(args, "note", ""):
        result["note"] = args.note

    text = json.dumps(result, indent=2)
    if getattr(args, "output", ""):
        Path(args.output).write_text(text + "\n")
        print(f"wrote {args.output}", file=sys.stderr)
    else:
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
