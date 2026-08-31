#!/usr/bin/env python3
"""TAX-0 COPY-AB-1 runner — application-level copy A/B (R0 vs R1 router).

Drives bench/tax0_copy_ab_bench (the REAL sluice-copy engine with an
injected backend) under user-mode `perf stat`, one process per measured
candidate run, blocked-randomized rounds from a predeclared seed.

Measurement discipline (frozen in research/tax0/TAX0-COPY-AB1-DESIGN.md):
  * The measured region is the bench process (whole-process perf counters,
    identical fixed costs across candidates — recorded, never subtracted).
  * Source generation and content verification are OUTSIDE any measured
    region: `generate` runs before the session; after EVERY measured
    process the runner verifies destination == source with a full byte
    comparison (`cmp`), unmeasured. A verification failure fails the
    campaign closed.
  * Real-ring rule: every uring row must come from a process whose bench
    reported real_uring=true (the bench itself fail-closes otherwise).
  * Environment/provenance helpers are REUSED from perf-attribution.py
    via importlib (single source of truth for env fingerprint / perf
    parsing / MAD).
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import math
import random
import statistics
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
BENCH = REPO / "build/linux/x86_64/release/tax0_copy_ab_bench"
SCHEMA = 2
EXPERIMENT = "TAX-0-COPY-AB-1"
SEED = 0x434F5059  # "COPY" — frozen in the design doc

_spec = importlib.util.spec_from_file_location(
    "perf_attribution", REPO / "scripts/bench/perf-attribution.py")
pa = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(pa)

# Primary + secondary user-mode events; instructions/cycles REQUIRED
# (fail-closed) — the same event set as the TAX-0 router campaigns.
PERF_EVENTS = pa.TAX0_PERF_EVENTS

# Frozen A/A cells (one session per fs): (buffer, depth, capacity).
AA_CELLS = [(4096, 8, 32), (4096, 8, 512), (1048576, 8, 512)]
AA_LABELS = ["r0a", "r0b"]  # both execute the IDENTICAL production_baseline
AA_REPS = 11

# Frozen primary matrix (one session per fs): B x P x C.
BUFFERS = [4096, 65536, 1048576]
DEPTHS = [1, 8, 32]
C_NEAR = {1: 2, 8: 9, 32: 33}  # peak accepted = P (code trace + smoke);
C_FIXED = [128, 512]           # C_near keeps the smallest safe margin (+1)
QUEUE_DEPTH = 64
LABELS = ["r0", "r1"]  # r0 -> production_baseline, r1 -> reverse_scan
CAMPAIGN_REPS = 9
WARMUP_ROUNDS = 1
FILE_BYTES = 256 * 1024 * 1024

CONTROL_LABEL = "threadpool"
CONTROL_CELL = (4096, 8, 0)  # buffer, depth; no C — production default backend

BACKEND_OF = {"r0a": "uring-r0", "r0b": "uring-r0",
              "r0": "uring-r0", "r1": "uring-r1",
              "threadpool": "threadpool"}

METRICS = ("instructions_user", "cycles_user", "copy_wall_ns")


def log(msg: str) -> None:
    print(f"[copy-ab1] {msg}", file=sys.stderr)


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(8 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def generate_source(src: str, total: int) -> str:
    """Deterministic source bytes (splitmix64 master block), unmeasured."""
    log(f"generating {total} bytes -> {src}")
    subprocess.run([str(BENCH), "--generate", "--src", src,
                    "--expected-bytes", str(total)], check=True)
    return sha256_file(src)


def bench_argv(label: str, cell, src: str, dst: str, total: int) -> list[str]:
    backend = BACKEND_OF[label]
    argv = [str(BENCH), "--backend", backend,
            "--buffer-size", str(cell[0]), "--pipeline-depth", str(cell[1])]
    if backend != "threadpool":
        argv += ["--request-capacity", str(cell[2]),
                 "--queue-depth", str(QUEUE_DEPTH)]
    return argv + ["--workers", "1", "--src", src, "--dst", dst,
                   "--expected-bytes", str(total), "--reps", "1"]


def measured_run(argv: list[str], taskset: str | None, label: str):
    """One bench process under perf stat. Returns (bench_json, counters)."""
    full = (["taskset", "-c", taskset] if taskset else []) + \
        ["perf", "stat", "-x,", "-e", ",".join(PERF_EVENTS), "--"] + argv
    proc = subprocess.run(full, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.exit(f"{label}: bench/perf failed rc={proc.returncode}\n"
                 f"{proc.stderr[-2000:]}")
    try:
        bench = json.loads(proc.stdout)
    except json.JSONDecodeError as e:
        sys.exit(f"{label}: cannot parse bench JSON: {e}\n{proc.stdout[:800]}")
    counters = pa.parse_perf_stat(proc.stderr)
    for req in ("instructions", "cycles"):
        if not counters.get(req):
            sys.exit(f"{label}: perf stat returned no {req} counter — "
                     f"fail closed")
    return bench, counters


def verify_content(src: str, dst: str, label: str) -> None:
    """Unmeasured full byte comparison — a wrong copy invalidates the row."""
    proc = subprocess.run(["cmp", "-s", src, dst])
    if proc.returncode != 0:
        sys.exit(f"{label}: CONTENT VERIFICATION FAILED (cmp rc="
                 f"{proc.returncode}) — destination differs from source")


def environment(args) -> None:
    if not BENCH.exists():
        sys.exit(f"missing {BENCH} (xmake f -m release --toolchain=clang "
                 f"--with-liburing=true; xmake build tax0_copy_ab_bench)")
    env_fp = pa.env_fingerprint(input_path=args.src, output_path=args.dst)
    env_id_src = {k: v for k, v in env_fp.items() if k != "time"}
    args._environment_id = hashlib.sha256(json.dumps(
        env_id_src, sort_keys=True).encode()).hexdigest()[:16]
    args._env_fp = env_fp
    args._env_extra = pa._tax0_environment_extra(args.taskset, args.src)


def blocked_rounds(entries: list[tuple], reps: int, seed: int) -> list[list]:
    """Same generator convention as the TAX-0 router campaigns: per round a
    fresh permutation of ALL (label, cell) entries — the candidate position
    within a cell is therefore randomized per round, closing any positional
    (order) bias. Generated BEFORE measurement from the predeclared seed;
    stored verbatim in the artifact."""
    rng = random.Random(seed)
    return [rng.sample(entries, k=len(entries)) for _ in range(reps)]


def run_rounds(args, rounds, src, dst, total, src_sha) -> list[dict]:
    rows = []
    for ridx, rnd in enumerate(rounds):
        for label, cell in rnd:
            idx = len(rows)
            where = (f"round {ridx + 1}/{len(rounds)} {label} "
                     f"B={cell[0]} P={cell[1]}"
                     + (f" C={cell[2]}" if cell[2] else ""))
            bench, counters = measured_run(
                bench_argv(label, cell, src, dst, total),
                args.taskset, f"row {idx} ({where})")
            verify_content(src, dst, f"row {idx} ({where})")
            rep = bench["reps_out"][0]
            backend = bench["backend"]
            row = {
                "experiment": EXPERIMENT,
                "git_sha": pa.git_sha(),
                "candidate": label,
                "backend": backend,
                "real_uring": bool(bench.get("real_uring")),
                "filesystem": args.fs_label,
                "buffer_size": cell[0],
                "pipeline_depth": cell[1],
                "request_capacity": cell[2],
                "queue_depth": bench.get("queue_depth"),
                "workers": bench.get("workers"),
                "file_bytes": total,
                "source_sha256": src_sha,
                "bytes_copied": rep["bytes_copied"],
                "read_ops": rep["read_ops"],
                "write_ops": rep["write_ops"],
                "short_writes": rep["short_writes"],
                "chunks": bench.get("chunks"),
                "round": ridx,
                "execution_order_index": idx,
                "copy_wall_ns": rep["wall_ns"],
                "user_ns": bench.get("user_ns"),
                "sys_ns": bench.get("sys_ns"),
                "instructions_user": counters.get("instructions"),
                "cycles_user": counters.get("cycles"),
                "branches_user": counters.get("branches"),
                "branch_misses_user": counters.get("branch-misses"),
                "cache_misses_user": counters.get("cache-misses"),
                "same_work": bool(bench.get("same_work")),
                "content_verification": "cmp full byte comparison",
                "content_verified": True,
                "environment_id": args._environment_id,
            }
            # Fail-closed per-row checks (same-work witness).
            if not row["same_work"]:
                sys.exit(f"row {idx}: bench reported same_work=false")
            if row["bytes_copied"] != total:
                sys.exit(f"row {idx}: bytes_copied "
                         f"{row['bytes_copied']} != {total}")
            if backend.startswith("uring"):
                if not row["real_uring"]:
                    sys.exit(f"row {idx}: uring row without a real ring")
                if row["queue_depth"] != QUEUE_DEPTH:
                    sys.exit(f"row {idx}: queue_depth "
                             f"{row['queue_depth']} != {QUEUE_DEPTH}")
                if row["request_capacity"] < cell[1] + 1:
                    sys.exit(f"row {idx}: C < P+1 is outside the frozen "
                             f"envelope")
            rows.append(row)
            if not args.no_perf:
                log(f"{idx + 1}/{sum(len(r) for r in rounds)} {where}: "
                    f"instr/byte={row['instructions_user'] / total:.3f} "
                    f"wall/byte={row['copy_wall_ns'] / total:.3f}")
    return rows


def derived_per_cell(rows: list[dict], labels: list[str]) -> dict:
    """Per-cell medians + same-cell normalized ratios (labels[1] over
    labels[0]); recomputed independently by the validator."""
    by_cell: dict[tuple, dict] = {}
    for r in rows:
        key = (r["candidate"], r["buffer_size"], r["pipeline_depth"],
               r["request_capacity"], r["file_bytes"])
        slot = by_cell.setdefault(key, {m: [] for m in METRICS})
        for m in METRICS:
            slot[m].append(r[m])
    cells = {}
    for key, slot in sorted(by_cell.items()):
        med = {m: statistics.median(slot[m]) for m in METRICS}
        cells["|".join(map(str, key))] = {
            "candidate": key[0], "buffer_size": key[1],
            "pipeline_depth": key[2], "request_capacity": key[3],
            "file_bytes": key[4], "reps": len(slot["instructions_user"]),
            "median_instr": med["instructions_user"],
            "median_cycles": med["cycles_user"],
            "median_wall_ns": med["copy_wall_ns"],
            "instr_per_byte": med["instructions_user"] / key[4],
            "cycles_per_byte": med["cycles_user"] / key[4],
            "wall_ns_per_byte": med["copy_wall_ns"] / key[4],
        }
    ratios = {}
    by_geom: dict[tuple, dict] = {}
    for k, v in cells.items():
        by_geom.setdefault(tuple(k.split("|")[1:]), {})[v["candidate"]] = v
    for geom, cand in sorted(by_geom.items()):
        if len(cand) != len(labels):
            continue
        a, b = cand[labels[0]], cand[labels[1]]
        ratios["|".join(geom)] = {
            "buffer_size": a["buffer_size"],
            "pipeline_depth": a["pipeline_depth"],
            "request_capacity": a["request_capacity"],
            "file_bytes": a["file_bytes"],
            "normalized_instr": b["instr_per_byte"] / a["instr_per_byte"],
            "normalized_cycles": b["cycles_per_byte"] / a["cycles_per_byte"],
            "normalized_wall": b["wall_ns_per_byte"] / a["wall_ns_per_byte"],
        }
    return {"cells": cells, "ratios": ratios}


def gm(values: list[float]) -> float:
    prod = 1.0
    for v in values:
        prod *= v
    return prod ** (1.0 / len(values))


def paired_effects(rows: list[dict], labels: list[str],
                   per_cell: bool = False) -> dict:
    """Blocked paired design: per (round, cell) log2 ratio of the candidate
    pair. Round/cell clustering is preserved (one pair per round per cell);
    rows are NEVER treated as IID."""
    by_rc: dict[tuple, dict] = {}
    for r in rows:
        key = (r["round"], r["buffer_size"], r["pipeline_depth"],
               r["request_capacity"], r["file_bytes"])
        by_rc.setdefault(key, {})[r["candidate"]] = r

    def _effects(ds: list[float]) -> dict | None:
        if not ds:
            return None
        p90 = sorted(abs(v) for v in ds)[max(0, int(0.9 * len(ds)) - 1)]
        return {
            "n_pairs": len(ds),
            "paired_median_log2": statistics.median(ds),
            "paired_mean_log2": sum(ds) / len(ds),
            "gm_ratio": 2 ** (sum(ds) / len(ds)),
            "mad_log2": pa._mad(ds),
            "p90_abs_log2": p90,
        }

    pooled = {m: [] for m in METRICS}
    per_cell_out: dict[str, dict] = {}
    for key, slot in sorted(by_rc.items()):
        if len(slot) != len(labels):
            continue
        cell_ds = {m: [math.log2(slot[labels[1]][m] / slot[labels[0]][m])]
                   for m in METRICS}
        for m in METRICS:
            pooled[m].extend(cell_ds[m])
        if per_cell:
            name = f"B={key[1]},P={key[2]},C={key[3]}"
            per_cell_out[name] = {
                m: _effects(cell_ds[m]) for m in METRICS}
    out = {m: _effects(pooled[m]) for m in METRICS}
    if per_cell:
        out["per_cell"] = per_cell_out
    return out


def artifact(args, params, order_meta, rows, derived, extra=None) -> dict:
    # Top-level provenance note: required by perf-evidence-validate.py
    # whenever the measurement ran on a dirty tree (pre-freeze calibration
    # sessions) — states exactly what was dirty and why it is admissible.
    note = params.get("note", "") + (
        " | Measured on branch " + args._env_fp["git"].get("branch", "?") +
        " @ " + args._env_fp["git"].get("sha", "?")[:12] +
        " with uncommitted research-only files (bench/runner/validator/"
        "artifact output); the measured binaries are the Release+liburing "
        "build of tax0_copy_ab_bench and the unmodified production "
        "libraries at that tree (binary sha256 in `binary`)."
        if args._env_fp["git"].get("dirty") else "")
    art = {
        "schema": SCHEMA,
        "kind": args.kind,
        "binary": pa.binary_provenance(str(BENCH)),
        "env": args._env_fp,
        "note": note.strip(" |"),
        "params": params,
        "execution_order": order_meta,
        "environment_extra": args._env_extra,
        "environment_id": args._environment_id,
        "rows": rows,
        "same_work": {
            "bytes_expected": params["file_bytes"],
            "bytes_observed": sorted({r["bytes_copied"] for r in rows}),
            "content_verification": "cmp full byte comparison (unmeasured)",
            "all_verified": all(r["content_verified"] for r in rows),
            "real_uring_all": all(r["real_uring"] for r in rows
                                  if r["backend"].startswith("uring")),
        },
        "derived": derived,
    }
    if extra:
        art.update(extra)
    return art


def cmd_generate(args) -> None:
    sha = generate_source(args.src, args.bytes)
    print(json.dumps({"src": args.src, "bytes": args.bytes,
                      "sha256": sha}, indent=2))


def cmd_aa(args) -> None:
    environment(args)
    src_sha = generate_source(args.src, FILE_BYTES)
    entries = [(lab, cell) for cell in AA_CELLS for lab in AA_LABELS]
    rounds = blocked_rounds(entries, args.reps, SEED)
    rows = run_rounds(args, rounds, args.src, args.dst,
                      FILE_BYTES, src_sha)
    derived = derived_per_cell(rows, AA_LABELS)
    paired = paired_effects(rows, AA_LABELS, per_cell=True)
    art = artifact(args, {
        "experiment": EXPERIMENT + "-AA",
        "labels": AA_LABELS,
        "cells": [f"B={b},P={p},C={c}" for (b, p, c) in AA_CELLS],
        "file_bytes": FILE_BYTES,
        "reps": args.reps,
        "seed": SEED,
        "fs_label": args.fs_label,
        "taskset": args.taskset,
        "queue_depth": QUEUE_DEPTH,
        "workers": 1,
        "note": "Both labels execute the IDENTICAL production_baseline "
                "router mode; the pair measures harness noise.",
    }, {
        "generator": "random.Random(seed).sample(sorted(entries)) per round "
                     "over (label, B, P, C) — generated before measurement; "
                     "candidate order within a cell is randomized per round",
        "seed": SEED, "reps": args.reps,
        "cells": [f"{lab}|B={b},P={p},C={c}" for (b, p, c) in AA_CELLS
                  for lab in AA_LABELS],
        "rounds": [[f"{lab}|B={b},P={p},C={c}" for (lab, (b, p, c)) in rnd]
                   for rnd in rounds],
    }, rows, derived, {"paired_effects": paired})
    Path(args.out).write_text(json.dumps(art, indent=1))
    log(f"wrote {args.out} ({len(rows)} rows)")


def cmd_campaign(args) -> None:
    cells = [(b, p, c) for b in BUFFERS for p in DEPTHS
             for c in [C_NEAR[p]] + C_FIXED]
    environment(args)
    src_sha = generate_source(args.src, FILE_BYTES)
    entries = [(lab, cell) for cell in cells for lab in LABELS]
    rounds = blocked_rounds(entries, args.reps, SEED)

    # Unmeasured warmup round over every (label, cell) — also establishes the
    # recorded warm-page-cache policy for the btrfs group; no drop_caches.
    if not args.no_perf:
        for (label, (b, p, c)) in sorted(entries):
            subprocess.run((["taskset", "-c", args.taskset]
                            if args.taskset else []) +
                           bench_argv(label, (b, p, c), args.src,
                                      args.dst, FILE_BYTES),
                           capture_output=True)
        log(f"warmup done ({len(entries)} label-cells, unmeasured)")

    rows = run_rounds(args, rounds, args.src, args.dst,
                      FILE_BYTES, src_sha)
    derived = derived_per_cell(rows, LABELS)
    paired = paired_effects(rows, LABELS)
    ratios = derived["ratios"]
    gm_all = {
        "gm_normalized_instr": gm([v["normalized_instr"]
                                   for v in ratios.values()]),
        "gm_normalized_cycles": gm([v["normalized_cycles"]
                                    for v in ratios.values()]),
        "gm_normalized_wall": gm([v["normalized_wall"]
                                  for v in ratios.values()]),
    }
    art = artifact(args, {
        "experiment": EXPERIMENT,
        "labels": LABELS,
        "cells": [f"B={b},P={p},C={c}" for (b, p, c) in cells],
        "file_bytes": FILE_BYTES,
        "reps": args.reps,
        "warmup_rounds": 0 if args.no_perf else WARMUP_ROUNDS,
        "seed": SEED,
        "fs_label": args.fs_label,
        "taskset": args.taskset,
        "queue_depth": QUEUE_DEPTH,
        "workers": 1,
        "cache_policy": "warm page cache (unmeasured warmup round; no "
                        "drop_caches)",
    }, {
        "generator": "random.Random(seed).sample(sorted(entries)) per round "
                     "over (label, B, P, C) — generated before measurement; "
                     "candidate order within a cell is randomized per round",
        "seed": SEED, "reps": args.reps,
        "cells": [f"{lab}|B={b},P={p},C={c}" for (b, p, c) in cells
                  for lab in LABELS],
        "rounds": [[f"{lab}|B={b},P={p},C={c}" for (lab, (b, p, c)) in rnd]
                   for rnd in rounds],
    }, rows, derived, {"paired_effects": paired, "gm": gm_all})
    Path(args.out).write_text(json.dumps(art, indent=1))
    log(f"wrote {args.out} ({len(rows)} rows)")


def cmd_control(args) -> None:
    """ThreadPool control: SAME copy algorithm, production backend. Shows
    the harness itself cannot fake a capacity-dependent router effect (no
    router, no request_capacity parameter)."""
    environment(args)
    src_sha = generate_source(args.src, FILE_BYTES)
    rounds = blocked_rounds([(CONTROL_LABEL, CONTROL_CELL)], args.reps, SEED)
    rows = run_rounds(args, rounds, args.src, args.dst, FILE_BYTES, src_sha)
    derived = derived_per_cell(rows, [CONTROL_LABEL])
    art = artifact(args, {
        "experiment": EXPERIMENT + "-CONTROL",
        "labels": [CONTROL_LABEL],
        "cells": [f"B={CONTROL_CELL[0]},P={CONTROL_CELL[1]},"
                  f"backend=threadpool"],
        "file_bytes": FILE_BYTES,
        "reps": args.reps,
        "seed": SEED,
        "fs_label": args.fs_label,
        "taskset": args.taskset,
        "workers": 1,
        "note": "Production ThreadPoolBackend default construction; no "
                "router, no request_capacity parameter.",
    }, {
        "generator": "single cell, blocked by round",
        "seed": SEED, "reps": args.reps, "cells": ["threadpool"],
        "rounds": [["threadpool"] for _ in range(args.reps)],
    }, rows, derived)
    Path(args.out).write_text(json.dumps(art, indent=1))
    log(f"wrote {args.out} ({len(rows)} rows)")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)

    g = sub.add_parser("generate")
    g.add_argument("--src", required=True)
    g.add_argument("--bytes", type=int, required=True)
    g.set_defaults(fn=cmd_generate, kind="generate")

    def common(p, default_out: str, default_reps: int):
        p.add_argument("--fs-label", required=True)
        p.add_argument("--src", required=True)
        p.add_argument("--dst", required=True)
        p.add_argument("--out", default=default_out)
        p.add_argument("--taskset", default="0,2,4,6")
        p.add_argument("--reps", type=int, default=default_reps)
        p.add_argument("--no-perf", action="store_true",
                       help="diagnostic only (no counters; never official)")

    a = sub.add_parser("aa")
    common(a, "docs/results/performance-attribution/tax0-copy-ab1-aa.json",
           AA_REPS)
    a.set_defaults(fn=cmd_aa, kind="tax0copyab-aa")

    c = sub.add_parser("campaign")
    common(c, "docs/results/performance-attribution/tax0-copy-ab1.json",
           CAMPAIGN_REPS)
    c.set_defaults(fn=cmd_campaign, kind="tax0copyab")

    t = sub.add_parser("control")
    common(t,
           "docs/results/performance-attribution/tax0-copy-ab1-control.json",
           CAMPAIGN_REPS)
    t.set_defaults(fn=cmd_control, kind="tax0copyab-control")

    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
