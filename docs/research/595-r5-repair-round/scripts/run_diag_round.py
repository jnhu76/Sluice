#!/usr/bin/env python3
"""#595 R5-repair diagnostic round: fixed-instrument (v2) on/off design.

Per PREREGISTERED.md §7, shapes W03 d16 (main) + W03 d4 (control). 12 blocks
per shape; per block each arm {A0, R, C} runs once with trace ON
(SLUICE_595_R5_TAIL_TRACE=1 + dump dir) and once OFF (env unset, no dump
dir), order balanced: block even -> on pass first, odd -> off pass first;
arm order rotates by block index. Binaries: diag-original (c0cdc3ea) for
A0/R, diag-cleanup for C. Warmup 1 / measured 1 (diagnostic pass is the
instrumented latency pass of the single measured rep), stats 0.

Runs in its own dual-lock session AFTER clean timing completes; v2 dumps
analyzed by scripts/analyze_tail_trace_v2.py. Never mixed with clean data.
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, "/home/jnhu/sluice-595-e5/r5-repair-measure/harness")
import measure3 as m3  # noqa: E402

DIAG_ORIG_BIN = "/home/jnhu/sluice-595-e5/r5-repair-measure/worktree-diag/build/linux/x86_64/release/bench_w03_external_loop"
DIAG_CLEAN_BIN = "/home/jnhu/sluice-595-e5/r5-repair-measure/worktree-diag-cleanup/build/linux/x86_64/release/bench_w03_external_loop"
DUMP_ROOT = "/home/jnhu/sluice-595-e5/r5-repair-measure/raw/diag-dumps"
PROGRESS = os.path.join(m3.RAW, "DIAG_PROGRESS.json")
BLOCKS = 12


def diag_bin(arm):
    return DIAG_ORIG_BIN if arm in ("A0", "R") else DIAG_CLEAN_BIN


def run_diag_invocation(shape, case, env_extra, arm, mode, block, position, attempt):
    binary = diag_bin(arm)
    sha = m3.mp.sha256_file(binary)
    seg = f"diag-{case}"
    out_dir = os.path.join(m3.RAW, seg)
    os.makedirs(os.path.join(out_dir, "logs"), exist_ok=True)
    os.makedirs(os.path.join(out_dir, "jsonl"), exist_ok=True)
    tag = f"{case}-b{block:02d}-p{position}-{arm}-{mode}-a{attempt}"
    jpath = os.path.join(out_dir, "jsonl", f"{tag}.jsonl")
    opath = os.path.join(out_dir, "logs", f"{tag}.log")
    dump_dir = os.path.join(DUMP_ROOT, tag)
    env = {k: v for k, v in os.environ.items() if not k.startswith("SLUICE_")}
    env.update({
        "SLUICE_BENCH_WARMUP": "1",
        "SLUICE_BENCH_MEASURED": "1",
        "SLUICE_BENCH_CASE_FILTER": "=" + case,
        "SLUICE_BENCH_STATS": "0",
        "SLUICE_BENCH_JSONL": jpath,
        "SLUICE_BENCH_TMP_DIR": m3.TMPDIR_BENCH,
        m3.ARM_VAR: "A0" if arm == "A0" else "R",
    })
    env.update(env_extra)
    if mode == "on":
        os.makedirs(dump_dir, exist_ok=True)
        env["SLUICE_595_R5_TAIL_TRACE"] = "1"
        env["SLUICE_595_R5_TAIL_TRACE_DUMP_DIR"] = dump_dir

    before = m3.mp.snapshot(binary, sha, case, env[m3.ARM_VAR], m3.BENCH_CPU, m3.MON_CPUS)
    cmd = [m3.mp.TASKSET, "-c", m3.BENCH_CPU, binary]
    fa = [(os.POSIX_SPAWN_OPEN, 1, opath, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644),
          (os.POSIX_SPAWN_DUP2, 1, 2)]
    t0 = time.monotonic()
    pid = os.posix_spawn(m3.mp.TASKSET, cmd, env, file_actions=fa)
    sampler = m3.mp.Sampler(pid)
    sampler.start()
    _, status, ru = os.wait4(pid, 0)
    wall = time.monotonic() - t0
    sampler.stop_ev.set()
    sampler.join(timeout=2.0)
    exit_code = os.waitstatus_to_exitcode(status)
    after = m3.mp.snapshot(binary, sha, case, env[m3.ARM_VAR], m3.BENCH_CPU, m3.MON_CPUS)
    measured, warmup_n = m3.mp.parse_jsonl(jpath, case)
    rec = {
        "cell": f"DIAG-{case}", "case": case, "seg": "diag", "block": block,
        "position": position, "arm": arm, "mode": mode,
        "binary": "diag-original" if arm in ("A0", "R") else "diag-cleanup",
        "binary_sha256": sha, "attempt": attempt, "ts_utc": before["ts_utc"],
        "exit": exit_code, "wall_s": wall,
        "ru_utime": ru.ru_utime, "ru_stime": ru.ru_stime,
        "dump_dir": dump_dir if mode == "on" else None,
        "warmup_records": warmup_n, "measured_records": len(measured),
        "buckets": m3.mp.summarize_sampler(sampler, ru.ru_utime + ru.ru_stime, pid),
        "env": {k: env[k] for k in sorted(env) if k.startswith("SLUICE_")},
    }
    invalid = []
    if exit_code != 0:
        invalid.append(f"exit={exit_code}")
    if len(measured) != 1 or warmup_n != 1:
        invalid.append(f"records={warmup_n}+{len(measured)}")
    if not before["binary_sha_ok"]:
        invalid.append("binary_sha_mismatch")
    for r in measured:
        if not r.get("ok"):
            invalid.append("bench_failure")
    if measured:
        r0 = measured[0]
        rec["metrics"] = {
            "cpu_ns_per_op": 1e9 * r0["cpu_s"] / r0["ops"],
            "ops_per_sec": r0["ops_per_sec"],
            "p95_ns": r0["lat_p95_ns"], "p99_ns": r0["lat_p99_ns"],
            "wall_s": r0["wall_s"],
        }
    rec["valid"] = not invalid
    rec["invalid_reasons"] = invalid
    with open(os.path.join(out_dir, "results.jsonl"), "a") as f:
        f.write(json.dumps(rec) + "\n")
    m = rec.get("metrics", {})
    print(f"[diag {case}] b{block:02d} p{position} {arm}/{mode} a{attempt} "
          f"exit={exit_code} valid={rec['valid']} cpu/op={m.get('cpu_ns_per_op')} "
          f"p99={m.get('p99_ns')}", flush=True)
    return rec


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--shapes", default="both")
    args = ap.parse_args()
    shapes = [
        ("W03-uring-4k-d16", {}),
        ("W03-uring-4k-d4", {}),
    ] if args.shapes == "both" else [("W03-uring-4k-" + s, {}) for s in args.shapes.split(",")]
    prog = {"done": []}
    if os.path.exists(PROGRESS):
        prog = json.load(open(PROGRESS))
    arms = ["A0", "R", "C"]
    with m3.DualLockSession("r5rm-diagnostic", [
        "run_diag_round: fixed-instrument on/off, 12 blocks x 3 arms x 2 modes "
        "per shape (W03 d16 + d4), dump dirs under raw/diag-dumps/",
    ]):
        ok, why = m3.env_ok()
        if not ok:
            raise RuntimeError(f"env gate: {why}")
        for case, env_extra in shapes:
            for b in range(BLOCKS):
                arm_order = arms[b % 3:] + arms[:b % 3]
                mode_order = ["on", "off"] if b % 2 == 0 else ["off", "on"]
                position = 0
                for mode in mode_order:
                    for arm in arm_order:
                        k = f"{case}/{b:02d}-{position}-{arm}-{mode}"
                        if k in prog["done"]:
                            position += 1
                            continue
                        for attempt in (1, 2, 3):
                            rec = run_diag_invocation(None, case, env_extra, arm,
                                                      mode, b, position, attempt)
                            if rec["valid"]:
                                break
                        prog["done"].append(k)
                        position += 1
                        tmp = PROGRESS + ".tmp"
                        json.dump(prog, open(tmp, "w"), indent=1)
                        os.replace(tmp, PROGRESS)
    print("[diag] complete", flush=True)


if __name__ == "__main__":
    main()
