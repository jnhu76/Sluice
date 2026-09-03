#!/usr/bin/env python3
"""COPY-X0 session driver (prereg: research/g1-control-copy-x0/COPY-X0-PREREGISTRATION.md).

Drives the research bench into immutable session directories under
research/g1-control-copy-x0/results/. Fail-closed: refuses label/substrate
mismatch (C0 Corrective-2 lesson), refuses to overwrite a session, records
full provenance in environment.json. The validator (validate_copy_x0.py) is
the derivation authority; this script only executes and records.

Usage:
  run_copy_x0.py probe     --session <name> --tmp-root P --ext-root P
  run_copy_x0.py qualify   --session <name> --tmp-root P --ext-root P
  run_copy_x0.py semantic  --session <name> --tmp-root P --ext-root P \
                            --qualified <qualify-session-name>
  run_copy_x0.py perf      --session <name> --tmp-root P --ext-root P \
                            --qualified <qualify-session-name>
"""

import argparse
import hashlib
import json
import os
import random
import shutil
import statfs_helper  # local: statfs fstype helper
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
# COPY_X0_RESULTS_ROOT overrides the results root (smoke runs only; formal
# sessions always live in the repository results/ directory).
RESULTS = Path(os.environ.get("COPY_X0_RESULTS_ROOT") or
               (REPO / "research" / "g1-control-copy-x0" / "results"))

SIZES = [4 * 1024, 64 * 1024, 1024 * 1024, 64 * 1024 * 1024]
SIZE_NAMES = {4 * 1024: "4K", 64 * 1024: "64K", 1024 * 1024: "1M", 64 * 1024 * 1024: "64M"}
ARMS = ["B0", "B1", "B2", "B3"]
CHUNK_CANDIDATES = [8 * 1024, 64 * 1024, 256 * 1024, 1024 * 1024, 4 * 1024 * 1024]
QUALIFY_SIZE = 64 * 1024 * 1024
QUALIFY_REPS = 5
ROUNDS = 9
SESSION_SEED = 202609030


def fail(msg: str) -> None:
    print(f"run_copy_x0: FAIL-CLOSED: {msg}", file=sys.stderr)
    sys.exit(1)


def sh(cmd: list[str]) -> str:
    return subprocess.run(cmd, capture_output=True, text=True, check=True).stdout.strip()


def statfs_fstype(path: str) -> str:
    return statfs_helper.fstype(path)


def canonical(label: str) -> str:
    return {"tmpfs": "tmpfs", "ext4": "ext4"}[label]


def bench_bin(args_bench: str) -> str:
    p = Path(args_bench)
    if not p.is_file():
        fail(f"bench binary not found: {p}")
    return str(p.resolve())


def capture_environment(session_dir: Path, args, bench: str, extra: dict) -> dict:
    head = sh(["git", "-C", str(REPO), "rev-parse", "HEAD"])
    por = subprocess.run(["git", "-C", str(REPO), "status", "--porcelain", "-uno"],
                         capture_output=True, text=True, check=True).stdout.strip()
    dirty_tracked = bool(por)
    env = {
        "session": session_dir.name,
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "git_head": head,
        "dirty_tracked": dirty_tracked,
        "dirty_detail": por.splitlines(),
        "branch": sh(["git", "-C", str(REPO), "rev-parse", "--abbrev-ref", "HEAD"]),
        "bench_sha256": hashlib.sha256(Path(bench).read_bytes()).hexdigest(),
        "compiler": sh(["clang++", "--version"]).splitlines()[0],
        "build_mode": "release",
        "kernel": sh(["uname", "-r"]),
        "libc": sh(["ldd", "--version"]).splitlines()[0],
        "cpu": next(
            (l.split(":", 1)[1].strip() for l in Path("/proc/cpuinfo").read_text().splitlines()
             if l.startswith("model name")), "unknown"),
        "session_seed": SESSION_SEED,
        "roots": {
            "tmpfs": {"path": str(Path(args.tmp_root).resolve()),
                      "fstype": statfs_fstype(args.tmp_root)},
            "ext4": {"path": str(Path(args.ext_root).resolve()),
                     "fstype": statfs_fstype(args.ext_root)},
        },
        "mount": {
            "tmpfs": sh(["findmnt", "-T", args.tmp_root, "-no", "SOURCE,FSTYPE"]),
            "ext4": sh(["findmnt", "-T", args.ext_root, "-no", "SOURCE,FSTYPE"]),
        },
        "params": {"sizes": SIZES, "arms": ARMS, "rounds": ROUNDS, **extra},
        "commands": [],
    }
    # Substrate gate: label must resolve to its canonical fstype (C0 lesson).
    for label in ("tmpfs", "ext4"):
        got = env["roots"][label]["fstype"]
        if not got.startswith(canonical(label)):
            fail(f"substrate gate: label {label} root {env['roots'][label]['path']} "
                 f"resolves to {got!r}, expected {canonical(label)!r}")
    return env


def run_bench(env: dict, bench: str, out: Path, args: list[str]) -> list[dict]:
    cmd = [bench] + args
    env["commands"].append(" ".join(cmd))
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        fail(f"bench failed ({proc.returncode}): {' '.join(cmd)}\n{proc.stderr}")
    rows = []
    with out.open("a") as f:
        for line in proc.stdout.splitlines():
            line = line.strip()
            if not line:
                continue
            row = json.loads(line)  # invalid JSON fails closed here
            f.write(line + "\n")
            rows.append(row)
    return rows


def new_session(name: str) -> Path:
    d = RESULTS / name
    if d.exists():
        fail(f"session dir already exists (immutable): {d}")
    d.mkdir(parents=True)
    return d


def work_file(session_dir: Path, label: str, name: str) -> str:
    d = session_dir / "work" / label
    d.mkdir(parents=True, exist_ok=True)
    return str(d / name)


def cmd_probe(args) -> None:
    bench = bench_bin(args.bench)
    d = new_session(args.session)
    env = capture_environment(d, args, bench, {})
    rows = run_bench(env, bench, d / "rows.jsonl", ["selftest"])
    if not all(r.get("pass") for r in rows):
        fail("bench selftest failed")
    run_bench(env, bench, d / "rows.jsonl",
              ["probe", env["roots"]["tmpfs"]["path"], env["roots"]["ext4"]["path"]])
    (d / "environment.json").write_text(json.dumps(env, indent=2) + "\n")
    print(f"probe session written: {d}")


def cmd_qualify(args) -> None:
    bench = bench_bin(args.bench)
    d = new_session(args.session)
    env = capture_environment(d, args, bench,
                              {"chunk_candidates": CHUNK_CANDIDATES,
                               "qualify_reps": QUALIFY_REPS})
    rows = run_bench(env, bench, d / "rows.jsonl", ["selftest"])
    if not all(r.get("pass") for r in rows):
        fail("bench selftest failed")
    medians = {}
    for label in ("tmpfs", "ext4"):
        root = env["roots"][label]["path"]
        src = work_file(d, label, "q-src.bin")
        run_bench(env, bench, d / "rows.jsonl", ["mk", src, str(QUALIFY_SIZE),
                                                 str(SESSION_SEED + 7)])
        for chunk in CHUNK_CANDIDATES:
            walls = []
            for rep in range(QUALIFY_REPS):
                rid = f"qualify|{label}|{chunk}|r{rep}"
                out_rows = run_bench(env, bench, d / "rows.jsonl",
                                     ["bench", "B0", src, work_file(d, label, "q-dst.bin"),
                                      str(QUALIFY_SIZE), str(chunk), rid])
                r = out_rows[0]
                if not r.get("ok") or not r.get("bytes_ok"):
                    fail(f"qualify run invalid: {rid}")
                walls.append(r["wall_sec"])
            walls.sort()
            medians[(label, chunk)] = walls[len(walls) // 2]
    # FROZEN selection rule (prereg §10.3): minimize max(median_tmpfs,
    # median_ext4); tie -> smaller chunk.
    def key(chunk):
        return (max(medians[("tmpfs", chunk)], medians[("ext4", chunk)]), chunk)
    chosen = min(CHUNK_CANDIDATES, key=key)
    qual = {
        "rule": "minimize max(median_tmpfs, median_ext4) at 64MiB; tie -> smaller chunk",
        "medians": {f"{l}|{c}": medians[(l, c)] for l in ("tmpfs", "ext4")
                    for c in CHUNK_CANDIDATES},
        "qualified_chunk": chosen,
    }
    (d / "qualified.json").write_text(json.dumps(qual, indent=2) + "\n")
    # M5 control demonstration (labeled control; excluded from formal corpora
    # by the validator): pathological 16-byte chunk at 1 MiB.
    src = work_file(d, "tmpfs", "m5-src.bin")
    run_bench(env, bench, d / "rows.jsonl", ["mk", src, str(1024 * 1024),
                                             str(SESSION_SEED + 8)])
    run_bench(env, bench, d / "rows.jsonl",
              ["bench", "B0", src, work_file(d, "tmpfs", "m5-dst.bin"),
               str(1024 * 1024), "16", "control|m5-weak-baseline|16B"])
    (d / "environment.json").write_text(json.dumps(env, indent=2) + "\n")
    print(f"qualify session written: {d} (qualified chunk {chosen})")


def load_qualified(name: str) -> int:
    p = RESULTS / name / "qualified.json"
    if not p.is_file():
        fail(f"qualified.json not found in session {name}")
    return json.loads(p.read_text())["qualified_chunk"]


def cmd_semantic(args) -> None:
    bench = bench_bin(args.bench)
    chunk = load_qualified(args.qualified)
    d = new_session(args.session)
    env = capture_environment(d, args, bench, {"qualified_chunk": chunk,
                                               "qualified_from": args.qualified})
    if env["dirty_tracked"]:
        fail("formal semantic session requires clean tracked worktree (commit-pinned)")
    rows = run_bench(env, bench, d / "rows.jsonl", ["selftest"])
    if not all(r.get("pass") for r in rows):
        fail("bench selftest failed")
    seed = SESSION_SEED
    for label in ("tmpfs", "ext4"):
        root = env["roots"][label]["path"]
        for fx in ("S1", "S2", "S3", "S4", "S5", "S7", "S8"):
            run_bench(env, bench, d / "rows.jsonl",
                      ["fixture", fx, root, label, str(seed), "--chunk", str(chunk)])
    run_bench(env, bench, d / "rows.jsonl",
              ["s6", env["roots"]["tmpfs"]["path"], env["roots"]["ext4"]["path"],
               str(seed), "--chunk", str(chunk)])
    (d / "environment.json").write_text(json.dumps(env, indent=2) + "\n")
    print(f"semantic session written: {d}")


def cmd_perf(args) -> None:
    bench = bench_bin(args.bench)
    chunk = load_qualified(args.qualified)
    d = new_session(args.session)
    env = capture_environment(d, args, bench, {"qualified_chunk": chunk,
                                               "qualified_from": args.qualified})
    if env["dirty_tracked"]:
        fail("formal perf session requires clean tracked worktree (commit-pinned)")
    rows = run_bench(env, bench, d / "rows.jsonl", ["selftest"])
    if not all(r.get("pass") for r in rows):
        fail("bench selftest failed")
    out = d / "rows.jsonl"
    # Phase 0: A/A noise calibration (B0 twice, shuffled labels).
    for label in ("tmpfs", "ext4"):
        src = work_file(d, label, "aa-src.bin")
        run_bench(env, bench, out, ["mk", src, str(64 * 1024 * 1024),
                                    str(SESSION_SEED + 11)])
        for cell_size in (64 * 1024, 64 * 1024 * 1024):
            rng = random.Random(SESSION_SEED ^ hash_cell(label, cell_size) ^ 0xAA)
            for rnd in range(1, ROUNDS + 1):
                labels = ["A", "B"]
                rng.shuffle(labels)
                for lab in labels:
                    run_bench(env, bench, out,
                              ["bench", "B0", src, work_file(d, label, "aa-dst.bin"),
                               str(cell_size), str(chunk),
                               f"aa|{label}|{cell_size}|{lab}|r{rnd}"])
    # Formal matrix.
    for label in ("tmpfs", "ext4"):
        src = work_file(d, label, "src.bin")
        run_bench(env, bench, out, ["mk", src, str(64 * 1024 * 1024),
                                    str(SESSION_SEED + 12)])
        for cell_size in SIZES:
            rng = random.Random(SESSION_SEED ^ hash_cell(label, cell_size))
            for rnd in range(1, ROUNDS + 1):
                arms = list(ARMS)
                rng.shuffle(arms)
                for arm in arms:
                    rid = f"perf|{label}|{cell_size}|{arm}|r{rnd}"
                    rs = run_bench(env, bench, out,
                                   ["bench", arm, src, work_file(d, label, "dst.bin"),
                                    str(cell_size), str(chunk), rid])
                    r = rs[0]
                    if r.get("bytes_ok") is False or r.get("size_ok") is False:
                        fail(f"fail-closed same-work violation: {rid}")
    (d / "environment.json").write_text(json.dumps(env, indent=2) + "\n")
    print(f"perf session written: {d}")


def hash_cell(label: str, size: int) -> int:
    return (size * 1_000_003) ^ (sum(ord(c) for c in label) * 1_000_033)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["probe", "qualify", "semantic", "perf"])
    ap.add_argument("--session", required=True)
    ap.add_argument("--tmp-root", required=True)
    ap.add_argument("--ext-root", required=True)
    ap.add_argument("--bench",
                    default=str(REPO / "build/linux/x86_64/release/g1_control_copy_x0_bench"))
    ap.add_argument("--qualified", default=None)
    args = ap.parse_args()
    if args.cmd in ("semantic", "perf") and not args.qualified:
        fail(f"{args.cmd} requires --qualified <qualify session name>")
    {"probe": cmd_probe, "qualify": cmd_qualify,
     "semantic": cmd_semantic, "perf": cmd_perf}[args.cmd](args)


if __name__ == "__main__":
    main()
