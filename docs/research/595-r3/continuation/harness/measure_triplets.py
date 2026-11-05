#!/usr/bin/env python3
"""#595 R3 continuation triplet runner (frozen prereg §6.2(b) order).

Implements exactly:
  1. orders = list(itertools.permutations(['A0','A1','B']))   (6 permutations)
  2. random.Random(5950).shuffle(orders)                      (shuffled once)
  3. the 6-permutation sequence repeated 4 rounds -> 24 triplets
  4. invocation k of triplet j runs arm orders[j % 6][k]

Each invocation reuses measure_pairs_cont.run_slot (same validation, sampler,
provenance snapshots, retry policy).  Resampling unit downstream is the
triplet.  The runner must be launched with the shared flock already held on
fd 9.
"""
import argparse
import itertools
import json
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import measure_pairs_cont as mp


def frozen_orders():
    orders = list(itertools.permutations(["A0", "A1", "B"]))
    random.Random(5950).shuffle(orders)
    return orders


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--phase", required=True)
    ap.add_argument("--case", required=True)
    ap.add_argument("--triplets", type=int, default=24)
    ap.add_argument("--rounds", type=int, default=4)
    ap.add_argument("--seed", type=int, default=5950)
    ap.add_argument("--binary", required=True)
    ap.add_argument("--expect-sha", default=None)
    ap.add_argument("--cpu", default="2")
    ap.add_argument("--cpus", default="2,12")
    ap.add_argument("--arm-var", default="SLUICE_595_R3_ARM")
    ap.add_argument("--env", action="append", default=[])
    ap.add_argument("--warmup", type=int, default=1)
    ap.add_argument("--measured", type=int, default=5)
    ap.add_argument("--stats", type=int, default=0)
    ap.add_argument("--expected-ops", type=int, default=20000)
    ap.add_argument("--max-attempts", type=int, default=3)
    ap.add_argument("--tmp-dir", default="/tmp/sluice-595-r3flip")
    ap.add_argument("--phase-dir", required=True)
    ap.add_argument("--lock-file", default="/home/jnhu/sluice-595-e5/r3/MEASUREMENT.lock")
    args = ap.parse_args()

    assert args.seed == 5950, "frozen seed is 5950"
    assert args.triplets == args.rounds * 6

    if not os.access(mp.TASKSET, os.X_OK):
        sys.exit(f"taskset not executable: {mp.TASKSET}")
    if not os.access(args.binary, os.X_OK):
        sys.exit(f"bench binary not executable: {args.binary}")
    os.makedirs(args.phase_dir, exist_ok=True)

    orders = frozen_orders()
    schedule = [orders[j % 6] for j in range(args.triplets)]

    binary_sha = mp.sha256_file(args.binary)
    cpus = args.cpus.split(",")
    header = {
        "phase": args.phase,
        "case": args.case,
        "triplets": args.triplets,
        "order_rule": ("permutations(['A0','A1','B']) shuffled once with "
                       "random.Random(5950); triplet j position k runs "
                       "orders[j%6][k] (prereg 6.2b)"),
        "frozen_orders": [list(o) for o in orders],
        "schedule": [list(o) for o in schedule],
        "balance_check": {
            "per_position_counts": {
                k: {arm: [o[k] for o in schedule].count(arm) for arm in ("A0", "A1", "B")}
                for k in range(3)
            },
            "balanced": all(
                [o[k] for o in schedule].count(arm) == args.rounds
                for k in range(3) for arm in ("A0", "A1", "B")
            ),
        },
        "arm_var": args.arm_var,
        "cpu_set": args.cpu,
        "cpus_monitored": cpus,
        "binary": os.path.abspath(args.binary),
        "binary_sha256": binary_sha,
        "expect_sha": args.expect_sha,
        "env_overrides": args.env,
        "warmup": args.warmup,
        "measured": args.measured,
        "stats": args.stats,
        "expected_ops": args.expected_ops,
        "tmp_dir": args.tmp_dir,
        "lock": mp.lock_holder(args.lock_file),
        "started_utc": mp.iso_now(),
        "loadavg_start": list(os.getloadavg()),
        "tmp_start": mp.tmp_df(),
        "cpufreq_start": mp.cpufreq_state(cpus),
        "binary_sha_ok_start": (args.expect_sha is None or binary_sha == args.expect_sha),
        "host_kernel": os.uname().release,
        "runner_argv": sys.argv,
        "runner_sha256": mp.sha256_file(os.path.abspath(__file__)),
        "pair_module": os.path.abspath(mp.__file__),
        "pair_module_sha256": mp.sha256_file(os.path.abspath(mp.__file__)),
    }
    with open(os.path.join(args.phase_dir, "header.json"), "w") as f:
        json.dump(header, f, indent=1)

    results_path = os.path.join(args.phase_dir, "results.jsonl")
    if os.path.exists(results_path):
        os.unlink(results_path)
    attempts_path = os.path.join(args.phase_dir, "attempts.jsonl")
    if os.path.exists(attempts_path):
        os.unlink(attempts_path)

    fixture_path = None
    fixture_sha_start = None
    valid_triplets = 0
    for j, order in enumerate(schedule):
        label = "".join(order)
        ok = True
        for k, arm in enumerate(order):
            rec = mp.run_slot(args, args.case, arm, arm, j, k, label, binary_sha)
            if rec is None:
                ok = False
        if ok:
            valid_triplets += 1
        if fixture_path is None:
            candidate = os.path.join(args.tmp_dir,
                                     "external.bin" if "W03" in args.case else "pipeline.bin")
            if os.path.exists(candidate):
                fixture_path = candidate
                fixture_sha_start = mp.sha256_file(candidate)

    if fixture_path is not None and os.path.exists(fixture_path):
        fixture_sha_end = mp.sha256_file(fixture_path)
    else:
        fixture_sha_end = None
    end = {
        "phase": args.phase,
        "ended_utc": mp.iso_now(),
        "valid_triplets": valid_triplets,
        "expected_triplets": args.triplets,
        "loadavg_end": list(os.getloadavg()),
        "tmp_end": mp.tmp_df(),
        "cpufreq_end": mp.cpufreq_state(cpus),
        "binary_sha256_end": mp.sha256_file(args.binary),
        "fixture_path": fixture_path,
        "fixture_sha256_start": fixture_sha_start,
        "fixture_sha256_end": fixture_sha_end,
        "fixture_unchanged": (fixture_sha_start is not None
                              and fixture_sha_start == fixture_sha_end),
        "lock_end": mp.lock_holder(args.lock_file),
    }
    with open(os.path.join(args.phase_dir, "block_end.json"), "w") as f:
        json.dump(end, f, indent=1)
    print(f"[{args.phase}] done: valid triplets {valid_triplets}/{args.triplets}",
          flush=True)
    return 0 if valid_triplets == args.triplets else 3


if __name__ == "__main__":
    sys.exit(main())
