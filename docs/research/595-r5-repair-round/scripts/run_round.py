#!/usr/bin/env python3
"""#595 R5-repair round driver: A/A, three-arm matched blocks, A0o/A0c
calibration, per cell, all under the dual measurement lock. Resumable via
raw/PROGRESS.json; the frozen schedule sha must match across restarts.
"""
import argparse
import json
import os
import sys
import time

sys.path.insert(0, "/home/jnhu/sluice-595-e5/r5-repair-measure/harness")
import measure3 as m3  # noqa: E402

PROGRESS = os.path.join(m3.RAW, "PROGRESS.json")


def load_progress():
    if os.path.exists(PROGRESS):
        with open(PROGRESS) as f:
            return json.load(f)
    return {"schedule_sha": m3.schedule_sha(), "done": []}


def save_progress(prog):
    tmp = PROGRESS + ".tmp"
    with open(tmp, "w") as f:
        json.dump(prog, f, indent=1)
    os.replace(tmp, PROGRESS)


def wait_env(max_s=1800, poll=30):
    waited = 0
    while waited < max_s:
        ok, why = m3.env_ok()
        if ok:
            return True
        print(f"[env] not ready: {why}; wait {poll}s", flush=True)
        time.sleep(poll)
        waited += poll
    return False


def run_cell(cell, prog):
    name = cell["name"]
    kind = cell["kind"]
    done = set(prog["done"])

    def key(seg, block, position):
        return f"{name}/{seg}/{block:02d}-{position}"

    with m3.DualLockSession(f"r5rm-{name}", [
        f"run_round: {name} A/A x{cell['aa_pairs']} pairs, "
        f"three-arm x{m3.BLOCKS} matched blocks, "
        f"A0o/A0c calibration x{cell['calib_pairs']} pairs",
        f"schedule_sha={prog['schedule_sha'][:16]}",
    ]):
        if not wait_env():
            raise RuntimeError("environment gate never became ready")
        # A/A: 24 pairs, both members A0 on the original binary
        for i in range(cell["aa_pairs"]):
            k0, k1 = key("aa", i, 0), key("aa", i, 1)
            if k0 in done and k1 in done:
                continue
            bkey, arm_env = m3.arm_bin("A0", kind)
            if k0 not in done:
                m3.run_slot(cell, "A0", bkey, arm_env, i, 0, "aa")
                done.add(k0)
                save_progress(prog)
            if k1 not in done:
                m3.run_slot(cell, "A0", bkey, arm_env, i, 1, "aa")
                done.add(k1)
                prog["done"] = sorted(done)
                save_progress(prog)
        # three-arm matched blocks
        orders = m3.block_orders()
        for j in range(m3.BLOCKS):
            order = orders[j]
            for position, arm in enumerate(order):
                k = key("main", j, position)
                if k in done:
                    continue
                bkey, arm_env = m3.arm_bin(arm, kind)
                m3.run_slot(cell, arm, bkey, arm_env, j, position, "main")
                done.add(k)
                prog["done"] = sorted(done)
                save_progress(prog)
        # A0(original) vs A0(cleanup) calibration; even pair -> orig first
        for i in range(cell["calib_pairs"]):
            first_is_orig = (i % 2 == 0)
            for position, which in enumerate(("orig", "cleanupb")):
                k = key("calib", i, position)
                if k in done:
                    continue
                if (which == "orig") == first_is_orig:
                    bkey = f"{kind}_original"
                    arm, arm_env = "A0o", "A0"
                else:
                    bkey = f"{kind}_cleanup"
                    arm, arm_env = "A0c", "A0"
                m3.run_slot(cell, arm, bkey, arm_env, i, position, "calib")
                done.add(k)
                prog["done"] = sorted(done)
                save_progress(prog)
    print(f"[cell-done] {name}", flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cells", default="all")
    args = ap.parse_args()
    prog = load_progress()
    want = m3.cells() if args.cells == "all" else \
        [c for c in m3.cells() if c["name"] in args.cells.split(",")]
    print(f"[run_round] schedule_sha={prog['schedule_sha']}", flush=True)
    for cell in want:
        run_cell(cell, prog)
    print("[run_round] all requested cells complete", flush=True)


if __name__ == "__main__":
    main()
