#!/usr/bin/env python3
# run_batch_x0.py — BATCH-X0 formal driver (research-only).
#
# Drives the perf matrix (prereg §7) and the A/A qualification gate (§9):
#   qualify  — two consecutive full-matrix passes of B1 and B2; per matched
#              cell |median1 − median2|/min ≤ 5% required on ≥ 90% of cells
#   matrix   — the full 2 ops × 2 sizes × 6 N × 5 arms grid: 7 timed reps
#              per cell (strace-free, Amendment 1) + a separate untimed
#              1-rep enter-counter pass recorded PER CELL in
#              strace-enter-rows.jsonl (strace-enter-totals.json is kept as
#              a convenience aggregate only)
#   enters   — standalone enter-counter pass (same per-cell row format)
#   semantic — runs the bench `semantic` mode once and copies rows
#
# Corrective-1 (P1-1): the frozen §9 substrate rule ("one pre-created
# regular file on the host filesystem (ext4)") is enforced mechanically.
# qualify/matrix/enters REQUIRE --work-dir on a VERIFIED ext4 filesystem;
# the gate runs findmnt BEFORE any measurement and fails closed on any
# other filesystem (no tmpfs/btrfs substitution is authorized). matrix
# also requires --qualification: an existing PASSING qualification session
# that admits the formal matrix; the binding is recorded in
# environment.json and re-verified by the validator from raw rows.
# `semantic` keeps the historical /tmp default: §9 governs perf measurement
# validity, not the scripted functional fixtures.
import argparse
import json
import os
import random
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
BENCH = ROOT / "build/linux/x86_64/release/g1_control_batch_x0_bench"
DEFAULT_SEMANTIC_WORK = Path("/tmp/batch-x0")
FILE_SIZE = 8 << 20
ARMS = ["B0", "B1", "B2", "MB1", "MB3"]
OPS = ["read", "write"]
SIZES = [4096, 65536]
NS = [1, 2, 4, 8, 16, 32]
REPS = 7


def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def filesystem_record(workdir):
    """Mechanical substrate identification (Corrective-1 P1-1): findmnt on
    the resolved path; no value is invented or assumed. (record, err)."""
    workdir = workdir.resolve()
    r = sh(["findmnt", "-T", str(workdir), "-n", "-o", "FSTYPE,SOURCE,TARGET"])
    if r.returncode != 0:
        return None, f"findmnt failed for {workdir}: {r.stderr.strip()}"
    parts = r.stdout.split()
    if len(parts) != 3:
        return None, f"unparseable findmnt output: {r.stdout.strip()!r}"
    return {"work_dir": str(workdir),
            "filesystem_type": parts[0],
            "filesystem_source": parts[1],
            "mount_target": parts[2],
            "filesystem_probe": "findmnt -T <work_dir> -n -o FSTYPE,SOURCE,TARGET"}, None


def require_ext4(workdir):
    """Fail-closed §9 substrate gate for formal modes: ext4 or refuse,
    before any file creation or measurement (Corrective-1 §21: no
    xfs/btrfs/tmpfs substitution, no post-hoc prereg amendment)."""
    workdir.mkdir(parents=True, exist_ok=True)
    rec, err = filesystem_record(workdir)
    if rec is None:
        print(f"SUBSTRATE GATE ERROR: {err}", file=sys.stderr)
        sys.exit(13)
    if rec["filesystem_type"] != "ext4":
        print("SUBSTRATE GATE REFUSED (fail-closed, prereg §9): formal "
              "measurement requires an ext4 host filesystem; "
              f"work_dir={rec['work_dir']} is {rec['filesystem_type']} "
              f"({rec['filesystem_source']}). Substituting another "
              "filesystem is not authorized (Corrective-1 §21).",
              file=sys.stderr)
        sys.exit(13)
    return rec


def env_record(fs, cpu, qualification=None):
    load = open("/proc/loadavg").read().split()[0]
    gov_files = list(Path("/sys/devices/system/cpu/cpu0/cpufreq/").glob("scaling_governor"))
    governor = gov_files[0].read_text().strip() if gov_files else "n/a"
    model = ""
    for line in Path("/proc/cpuinfo").read_text().splitlines():
        if line.startswith("model name"):
            model = line.split(":", 1)[1].strip()
            break
    return {
        "kernel": os.uname().release,
        "cpu_model": model,
        "cpus": os.cpu_count(),
        "loadavg_start": load,
        "governor": governor,
        # pinning authority is the bench's own sched_setaffinity (it fails
        # closed if affinity cannot be set); cpu_pin records the requested
        # core. nice is NOT modified by this driver (recorded as observed).
        "cpu_pin": cpu,
        "pin_enforcement": "bench sched_setaffinity, fails closed",
        "nice": os.nice(0),
        "nice_protocol": "driver does not renice; recorded as observed",
        "dirty_tracked": sh(["git", "-C", str(ROOT), "status",
                          "--porcelain", "--untracked-files=no"]).stdout.strip() != "",
        "commit": sh(["git", "-C", str(ROOT), "rev-parse", "HEAD"]).stdout.strip(),
        "binary_sha256": sh(["sha256sum", str(BENCH)]).stdout.split()[0],
        "qualification_session": qualification,
        "ts": time.strftime("%Y-%m-%dT%H:%M:%S"),
        **fs,
    }


def cell_grid():
    cells = [(op, size, n) for op in OPS for size in SIZES for n in NS]
    rng = random.Random(20260904)
    rng.shuffle(cells)
    return cells


def arm_order(rng):
    arms = ARMS[:]
    rng.shuffle(arms)
    return arms


def prep_work(work):
    work.mkdir(parents=True, exist_ok=True)
    if not (work / "data.bin").exists():
        sh([str(BENCH), "prepare", "--file", str(work / "data.bin"),
            "--size", str(FILE_SIZE)])


def run_cell(arm, op, size, n, cpu, work, write_buf, with_strace=True, reps=REPS):
    data = work / "data.bin"
    if op == "write":
        target = work / f"scratch-{arm}.bin"
        if not target.exists():
            shutil.copy(write_buf, target)
        data = target
    cmd = (["strace", "-c", "-e", "trace=io_uring_enter"] if with_strace else []) + [
           str(BENCH), "perf", "--arm", arm, "--op", op, "--size", str(size),
           "--n", str(n), "--reps", str(reps), "--cpu", str(cpu),
           "--file", str(data)]
    r = sh(cmd)
    rows, enters = [], None
    for line in r.stdout.splitlines():
        line = line.strip()
        if line.startswith('{"kind":"perf"'):
            rows.append(json.loads(line))
    # parse strace -c summary: "% time seconds usecs/call calls [errors] syscall"
    # calls is field index 3; errors column present only when nonzero
    for line in r.stderr.splitlines():
        parts = line.split()
        if parts and parts[-1].startswith("io_uring_enter") and len(parts) >= 5:
            try:
                enters = int(parts[3])
            except ValueError:
                pass
            break
    return rows, enters, r.returncode, r.stderr


def enter_row(arm, op, size, n, enters, bench_rows):
    """Per-cell M6 evidence row (Corrective-1 P1-5): the authority for the
    transport-topology derivation. rounds/ops come from the same bench run
    the counter was taken over."""
    return {"arm": arm, "op": op, "size": size, "n": n,
            "rounds": bench_rows[0]["rounds"], "ops": bench_rows[0]["ops"],
            "io_uring_enter": enters, "counter_reps": 1}


def run_counter_pass(cells, cpu, work, write_src):
    """Untimed strace-wrapped enter-counter pass: 1 rep per arm×cell.
    Returns (enter_rows, totals, None) or (None, None, errmsg)."""
    rng = random.Random(43)
    rows_out, totals = [], {}
    total = len(cells) * len(ARMS)
    done = 0
    for (op, size, n) in cells:
        for arm in arm_order(rng):
            rows, enters, rc, err = run_cell(arm, op, size, n, cpu, work,
                                             write_src, with_strace=True,
                                             reps=1)
            done += 1
            if rc != 0 or enters is None or not rows:
                print(f"counter FAIL arm={arm} op={op} size={size} n={n} "
                      f"rc={rc} enters={enters}\n{err[-300:]}", file=sys.stderr)
                return None, None, f"enter-counter pass failed at {arm}/{op}/{size}/{n}"
            rows_out.append(enter_row(arm, op, size, n, enters, rows))
            totals[arm] = totals.get(arm, 0) + enters
            if done % 12 == 0 or done == total:
                print(f"[enters {done}/{total}] {arm} {op} {size} n={n} "
                      f"enters={enters}")
    return rows_out, totals, None


def write_counter_artifacts(outdir, enter_rows, totals):
    (outdir / "strace-enter-rows.jsonl").write_text(
        "\n".join(json.dumps(r) for r in enter_rows) + "\n")
    # convenience aggregate only; the per-cell rows are the M6 authority
    (outdir / "strace-enter-totals.json").write_text(
        json.dumps(totals, indent=2))


def do_matrix(outdir, cpu, work, fs, qualification):
    outdir.mkdir(parents=True, exist_ok=True)
    write_src = work / "data.bin"
    env = env_record(fs, cpu, qualification=qualification.name)
    (outdir / "environment.json").write_text(json.dumps(env, indent=2))
    rng = random.Random(42)
    all_rows = []
    cells = cell_grid()
    for idx, (op, size, n) in enumerate(cells):
        for arm in arm_order(rng):
            # timed rows are collected WITHOUT ptrace (disclosed amendment:
            # strace-wrapped timing amplifies run-to-run variance on this
            # host; the prereg defines strace as the enter COUNTER, not the
            # timing harness). Enter counts come from the separate untimed
            # pass below.
            rows, _, rc, err = run_cell(arm, op, size, n, cpu, work, write_src,
                                        with_strace=False)
            if rc != 0 or not rows:
                print(f"FAIL arm={arm} op={op} size={size} n={n} rc={rc}\n{err[-500:]}",
                      file=sys.stderr)
                return None
            all_rows.extend(rows)
            print(f"[{idx+1}/{len(cells)*len(ARMS)}] {arm} {op} {size} n={n} "
                  f"median={sorted(r['wall_per_op_ns'] for r in rows)[len(rows)//2]:.0f} ns/op")
            (outdir / "rows.jsonl").open("a").write(
                "\n".join(json.dumps(r) for r in rows) + "\n")
    # separate untimed enter-counter pass: 1 strace-wrapped rep per arm×cell
    # (M6 evidence, per-cell authority: strace-enter-rows.jsonl)
    enter_rows, totals, err = run_counter_pass(cells, cpu, work, write_src)
    if err is not None:
        print(err, file=sys.stderr)
        return None
    write_counter_artifacts(outdir, enter_rows, totals)
    return all_rows


def cell_medians(rows):
    from collections import defaultdict
    cells = defaultdict(list)
    for r in rows:
        cells[(r["arm"], r["op"], r["size"], r["n"])].append(r["wall_per_op_ns"])
    return {k: sorted(v)[len(v) // 2] for k, v in cells.items()}


def do_qualify(outdir, cpu, work, fs):
    outdir.mkdir(parents=True, exist_ok=True)
    if (outdir / "rows-pass1.jsonl").exists():
        print(f"REFUSED: {outdir} already holds qualification rows "
              "(no silent overwrite of evidence)", file=sys.stderr)
        return False
    write_src = work / "data.bin"
    env = env_record(fs, cpu)
    (outdir / "environment.json").write_text(json.dumps(env, indent=2))
    passes = []
    for p in (1, 2):
        rows = []
        for op in OPS:
            for size in SIZES:
                for n in NS:
                    for arm in ("B1", "B2"):
                        rs, _, rc, err = run_cell(arm, op, size, n, cpu, work,
                                                  write_src, with_strace=False)
                        if rc != 0 or not rs:
                            print(f"qualify FAIL {arm} {op} {size} n={n}: {err[-300:]}",
                                  file=sys.stderr)
                            return False
                        rows.extend(rs)
        passes.append(rows)
        (outdir / f"rows-pass{p}.jsonl").write_text(
            "\n".join(json.dumps(r) for r in rows) + "\n")
        print(f"qualify pass {p}: {len(rows)} rows")
    m1, m2 = cell_medians(passes[0]), cell_medians(passes[1])
    ok_cells, total, ratios = 0, 0, []
    violators = []
    for k in m1:
        a, b = m1[k], m2[k]
        ratio = abs(a - b) / min(a, b)
        if ratio > 0.05:
            violators.append({"cell": list(k), "m1": a, "m2": b, "ratio": ratio})
    for k in m1:
        total += 1
        a, b = m1[k], m2[k]
        ratio = abs(a - b) / min(a, b)
        ratios.append(ratio)
        if ratio <= 0.05:
            ok_cells += 1
    frac = ok_cells / total if total else 0
    verdict = {"gate": "A/A", "cells": total, "cells_within_5pct": ok_cells,
               "fraction_within_5pct": frac,
               "pass": frac >= 0.90,
               "max_ratio": max(ratios) if ratios else None,
               "median_ratio": sorted(ratios)[len(ratios)//2] if ratios else None,
               "violators": sorted(violators, key=lambda v: -v["ratio"])}
    (outdir / "verdicts.json").write_text(json.dumps(verdict, indent=2))
    print(json.dumps(verdict, indent=2))
    return verdict["pass"]


def check_qualification_binding(qdir):
    """Driver-side admission check for matrix mode (defense in depth; the
    validator recomputes the gate from raw rows): the bound qualification
    session must exist and record a PASSING A/A verdict."""
    if not qdir.is_dir():
        print(f"qualification session missing: {qdir}", file=sys.stderr)
        sys.exit(14)
    vpath = qdir / "verdicts.json"
    if not vpath.exists():
        print(f"qualification session has no verdicts.json: {qdir}", file=sys.stderr)
        sys.exit(14)
    v = json.loads(vpath.read_text())
    if not v.get("pass"):
        print(f"qualification session did not PASS the A/A gate: {qdir}",
              file=sys.stderr)
        sys.exit(14)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["qualify", "matrix", "semantic", "enters"])
    ap.add_argument("--out", required=True)
    ap.add_argument("--cpu", type=int, default=2)
    ap.add_argument("--work-dir", type=Path, default=None,
                    help="work directory; REQUIRED on a verified ext4 "
                         "filesystem for qualify/matrix/enters (prereg §9)")
    ap.add_argument("--qualification", type=Path, default=None,
                    help="existing PASSING qualification session; REQUIRED "
                         "for matrix (admission binding)")
    args = ap.parse_args()
    if not BENCH.exists():
        print(f"bench missing: {BENCH}", file=sys.stderr)
        return 2
    if args.mode == "semantic":
        work = args.work_dir or DEFAULT_SEMANTIC_WORK
        prep_work(work)
        out = Path(args.out)
        out.mkdir(parents=True, exist_ok=True)
        r = sh([str(BENCH), "semantic", "--file", str(work / "data.bin")])
        (out / "fixtures.jsonl").write_text(r.stdout)
        ok = all(json.loads(l).get("result") in (None, "PASS")
                 for l in r.stdout.splitlines()
                 if l.startswith('{"kind":"fixture"'))
        (out / "gates.json").write_text(json.dumps({"fixtures_pass": ok}, indent=2))
        print(f"semantic fixtures pass={ok}")
        return 0 if ok else 9
    # formal modes: substrate gate first, before anything else
    if not args.work_dir:
        print(f"--work-dir is REQUIRED for {args.mode} (prereg §9 ext4 "
              "substrate; Corrective-1 P1-1)", file=sys.stderr)
        return 2
    fs = require_ext4(args.work_dir)
    prep_work(args.work_dir)
    if args.mode == "qualify":
        return 0 if do_qualify(Path(args.out), args.cpu, args.work_dir, fs) else 10
    if args.mode == "enters":
        out = Path(args.out)
        out.mkdir(parents=True, exist_ok=True)
        env = env_record(fs, args.cpu)
        (out / "environment.json").write_text(json.dumps(env, indent=2))
        enter_rows, totals, err = run_counter_pass(cell_grid(), args.cpu,
                                                   args.work_dir,
                                                   args.work_dir / "data.bin")
        if err is not None:
            print(err, file=sys.stderr)
            return 12
        write_counter_artifacts(out, enter_rows, totals)
        print(json.dumps(totals, indent=2))
        return 0
    # matrix
    if not args.qualification:
        print("--qualification is REQUIRED for matrix (the formal matrix is "
              "admitted by a PASSING A/A session; Corrective-1 P1-2)",
              file=sys.stderr)
        return 14
    check_qualification_binding(args.qualification)
    out = Path(args.out)
    if (out / "rows.jsonl").exists():
        (out / "rows.jsonl").unlink()
    rows = do_matrix(out, args.cpu, args.work_dir, fs, args.qualification)
    return 0 if rows else 11


if __name__ == "__main__":
    sys.exit(main())
