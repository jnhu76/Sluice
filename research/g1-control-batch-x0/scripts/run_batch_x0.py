#!/usr/bin/env python3
# run_batch_x0.py — BATCH-X0 formal driver (research-only).
#
# Drives the perf matrix (prereg §7) and the A/A qualification gate (§9):
#   qualify — two consecutive full-matrix passes of B1 and B2; per matched
#             cell |median1 − median2|/min ≤ 5% required on ≥ 90% of cells
#   matrix  — the full 2 ops × 2 sizes × 6 N × 5 arms grid, 7 reps, one row
#             per rep in rows.jsonl (cell order seeded-interleaved)
#   semantic — runs the bench `semantic` mode once and copies rows
#
# Every bench invocation is wrapped under strace -c -e trace=io_uring_enter
# for the kernel-enter counter (M6 evidence). Environment is recorded per run.
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
WORK = Path("/tmp/batch-x0")
FILE_SIZE = 8 << 20
ARMS = ["B0", "B1", "B2", "MB1", "MB3"]
OPS = ["read", "write"]
SIZES = [4096, 65536]
NS = [1, 2, 4, 8, 16, 32]
REPS = 7


def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def env_record():
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
        "dirty_tracked": sh(["git", "-C", str(ROOT), "status",
                          "--porcelain", "--untracked-files=no"]).stdout.strip() != "",
        "commit": sh(["git", "-C", str(ROOT), "rev-parse", "HEAD"]).stdout.strip(),
        "binary_sha256": sh(["sha256sum", str(BENCH)]).stdout.split()[0],
        "ts": time.strftime("%Y-%m-%dT%H:%M:%S"),
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


def run_cell(arm, op, size, n, cpu, write_buf, with_strace=True, reps=REPS):
    data = WORK / "data.bin"
    if op == "write":
        target = WORK / f"scratch-{arm}.bin"
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


def do_matrix(outdir, cpu):
    outdir.mkdir(parents=True, exist_ok=True)
    write_src = WORK / "data.bin"
    env = env_record()
    (outdir / "environment.json").write_text(json.dumps(env, indent=2))
    rng = random.Random(42)
    all_rows = []
    cells = cell_grid()
    strace_enter_total = {}
    for idx, (op, size, n) in enumerate(cells):
        for arm in arm_order(rng):
            # timed rows are collected WITHOUT ptrace (disclosed amendment:
            # strace-wrapped timing amplifies run-to-run variance on this
            # host; the prereg defines strace as the enter COUNTER, not the
            # timing harness). Enter counts come from a separate 1-rep
            # strace-wrapped pass below.
            rows, _, rc, err = run_cell(arm, op, size, n, cpu, write_src,
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
    # separate enter-counter pass: 1 strace-wrapped rep per arm x cell
    # (untimed; M6 evidence)
    for idx, (op, size, n) in enumerate(cells):
        for arm in arm_order(rng):
            _, enters, rc, err = run_cell(arm, op, size, n, cpu, write_src,
                                          with_strace=True, reps=1)
            if rc != 0:
                print(f"STRACE FAIL arm={arm} op={op} size={size} n={n}\n{err[-300:]}",
                      file=sys.stderr)
                return None
            strace_enter_total[arm] = strace_enter_total.get(arm, 0) + (enters or 0)
            print(f"[enters {idx+1}] {arm} {op} {size} n={n} enters={enters}")
    (outdir / "strace-enter-totals.json").write_text(
        json.dumps(strace_enter_total, indent=2))
    return all_rows


def cell_medians(rows):
    from collections import defaultdict
    cells = defaultdict(list)
    for r in rows:
        cells[(r["arm"], r["op"], r["size"], r["n"])].append(r["wall_per_op_ns"])
    return {k: sorted(v)[len(v) // 2] for k, v in cells.items()}


def do_qualify(cpu):
    qdir = WORK / "qualify"
    if qdir.exists():
        shutil.rmtree(qdir)
    qdir.mkdir(parents=True)
    env = env_record()
    (qdir / "environment.json").write_text(json.dumps(env, indent=2))
    write_src = WORK / "data.bin"
    passes = []
    for p in (1, 2):
        rows = []
        for op in OPS:
            for size in SIZES:
                for n in NS:
                    for arm in ("B1", "B2"):
                        rs, _, rc, err = run_cell(arm, op, size, n, cpu, write_src,
                                                  with_strace=False)
                        if rc != 0 or not rs:
                            print(f"qualify FAIL {arm} {op} {size} n={n}: {err[-300:]}",
                                  file=sys.stderr)
                            return None
                        rows.extend(rs)
        passes.append(rows)
        (qdir / f"rows-pass{p}.jsonl").write_text(
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
    (qdir / "verdicts.json").write_text(json.dumps(verdict, indent=2))
    print(json.dumps(verdict, indent=2))
    return verdict["pass"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=["qualify", "matrix", "semantic", "enters"])
    ap.add_argument("--out", required=True)
    ap.add_argument("--cpu", type=int, default=2)
    args = ap.parse_args()
    if not BENCH.exists():
        print(f"bench missing: {BENCH}", file=sys.stderr)
        return 2
    WORK.mkdir(parents=True, exist_ok=True)
    if not (WORK / "data.bin").exists():
        sh([str(BENCH), "prepare", "--file", str(WORK / "data.bin"),
            "--size", str(FILE_SIZE)])
    if args.mode == "semantic":
        out = Path(args.out)
        out.mkdir(parents=True, exist_ok=True)
        r = sh([str(BENCH), "semantic", "--file", str(WORK / "data.bin")])
        (out / "fixtures.jsonl").write_text(r.stdout)
        ok = all(json.loads(l).get("result") in (None, "PASS")
                 for l in r.stdout.splitlines()
                 if l.startswith('{"kind":"fixture"'))
        (out / "gates.json").write_text(json.dumps({"fixtures_pass": ok}, indent=2))
        print(f"semantic fixtures pass={ok}")
        return 0 if ok else 9
    if args.mode == "qualify":
        return 0 if do_qualify(args.cpu) else 10
    if args.mode == "enters":
        # enter-counter pass only (M6); rewrites strace-enter-totals.json
        out = Path(args.out)
        write_src = WORK / "data.bin"
        rng = random.Random(43)
        totals = {}
        cells = cell_grid()
        for idx, (op, size, n) in enumerate(cells):
            for arm in arm_order(rng):
                _, enters, rc, err = run_cell(arm, op, size, n, args.cpu,
                                              write_src, with_strace=True,
                                              reps=1)
                if rc != 0 or enters is None:
                    print(f"enters FAIL {arm} {op} {size} n={n}: {err[-200:]}",
                          file=sys.stderr)
                    return 12
                totals[arm] = totals.get(arm, 0) + enters
                if idx % 6 == 0:
                    print(f"[enters {idx+1}/24 cells] {arm} {op} {size} n={n} "
                          f"enters={enters}")
        (out / "strace-enter-totals.json").write_text(json.dumps(totals, indent=2))
        print(json.dumps(totals, indent=2))
        return 0
    if args.mode == "matrix":
        out = Path(args.out)
        if (out / "rows.jsonl").exists():
            (out / "rows.jsonl").unlink()
        rows = do_matrix(out, args.cpu)
        return 0 if rows else 11
    return 2


if __name__ == "__main__":
    sys.exit(main())
