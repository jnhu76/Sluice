#!/usr/bin/env python3
"""TAX-0 clean-tree reproducibility closure driver (#250 / PR #260).

PURPOSE
-------
Verify that the COMMITTED research implementation at PR #260 HEAD
reproduces the same control-plane tax structure from a CLEAN working tree
as the original canonical session (`tax0b-zladder-wsl2-formal4`, measured
from a DIRTY research tree at 867ac94, binary sha256 recorded).

This is NOT a re-run of TAX-0 and NOT a replacement of canonical data.
It is a bounded representative subset using the SAME formal protocol
machinery imported from `tax0z.py` (double-difference perf normalization,
runner-side write verification, cross-arm same-work fail-closed) so the
measurement protocol is identical.

Representative matrix (per closure plan):
  READ  : {4K d1, 4K d32, 4K d64, 64K d8, 1M d8} x {z1, z1b, z2, z3w1}
  WRITE : {4K d1, 4K d32, 64K d8} x {z1, z1b, z2, z3w1}
          (if OBS-1 / write-runtime flake reappears: record the affected
           cell as BLOCKED and stop the remaining write cells; READ closure
           remains the primary evidence and must not be blocked by it.)

Primary metrics: instructions/op, cycles/op (secondary: wall/op).

Same-work gate stays mandatory per cell: same op count, same bytes, same
offsets, same word/checksum result, same completion count, same depth,
same outer-loop semantics (fail-closed in the harness per rep, cross-arm
equality checked here, runner-side byte verification for writes). A
closure comparison that fails same-work is INVALID and is not used.
"""

import argparse
import csv
import datetime
import json
import os
import pathlib
import subprocess
import sys

# The driver may run from outside the repo (so the repo tree stays literally
# `git status`-clean during a clean-tree closure measurement). Resolve the
# repo root from SLUICE_REPO or by walking up from this file, and make the
# in-repo tax0z.py importable regardless of cwd.
_default_repo = None
for _cand in [pathlib.Path(__file__).resolve(), *pathlib.Path(__file__).resolve().parents]:
    if (_cand / "xmake.lua").exists() and (_cand / ".git").exists():
        _default_repo = _cand
        break
REPO = pathlib.Path(os.environ.get("SLUICE_REPO", str(_default_repo or pathlib.Path.cwd())))
sys.path.insert(0, str(REPO / "research/tax0/scripts"))
import tax0z

RESULTS_ROOT = REPO / "research/tax0/results"
ORIGINAL_SESSION = "tax0b-zladder-wsl2-formal4"

# Representative closure cells (subset of the P1 canonical matrix).
READ_CELLS = [(4096, 1), (4096, 32), (4096, 64), (65536, 8), (1048576, 8)]
WRITE_CELLS = [(4096, 1), (4096, 32), (65536, 8)]
CLOSURE_ARMS = ["z1", "z1b", "z2", ("z3", 1)]  # z3w1; z1bw/z3w4 out of scope


def arm_label(arm):
    return tax0z.arm_label(arm)


def run_closure(session, cells_by_op, out_root=None):
    sdir = (out_root or RESULTS_ROOT) / session
    if sdir.exists():
        sys.exit(f"refusing to overwrite session dir: {sdir}")
    (sdir / "raw").mkdir(parents=True)
    tax0z.capture_environment(sdir)
    tax0z.prepare_data_files(READ_CELLS + WRITE_CELLS)

    manifest = {
        "purpose": ("clean-tree reproducibility closure only; NOT a re-run "
                    "of TAX-0, NOT a replacement of canonical data"),
        "supersedes": "nothing",
        "original_canonical_session": str(RESULTS_ROOT / ORIGINAL_SESSION),
        "tree_state": "clean",
        "protocol": {
            "warmup_reps_formal": 0,
            "formal_rep_pairs": [tax0z.FORMAL_REPS_A, tax0z.FORMAL_REPS_B],
            "normalization": "double-difference (total(R14)-total(R7))/7/ops",
            "write_verification": "runner-side post-exit byte compare",
            "primary_counters": tax0z.SESSION_PERF_EVENTS,
        },
        "cells": {
            "read": READ_CELLS,
            "write": WRITE_CELLS,
        },
        "arms": [arm_label(a) for a in CLOSURE_ARMS],
        "runs": [],
    }

    rows = []
    blocked_write = None
    for op, cells in cells_by_op.items():
        for rs, d in cells:
            if blocked_write is not None:
                print(f"[BLOCKED] {op} r{rs} d{d}: write cells stopped after "
                      f"{blocked_write} (OBS-1 flake)")
                continue
            for arm in CLOSURE_ARMS:
                label = arm_label(arm)
                tag_a = f"closure-{op}-r{rs}-d{d}-{label}-R{tax0z.FORMAL_REPS_A}"
                tag_b = f"closure-{op}-r{rs}-d{d}-{label}-R{tax0z.FORMAL_REPS_B}"
                # Bounded retry (documented, never silent), matching the
                # formal session policy for the intermittent OBS-1 write
                # flake (spurious canceled terminal / teardown abort).
                retries = 0
                while True:
                    rec_a = tax0z.run_one(arm, rs, d, op, tax0z.FORMAL_REPS_A,
                                          sdir / "raw", tag_a)
                    rec_b = tax0z.run_one(arm, rs, d, op, tax0z.FORMAL_REPS_B,
                                          sdir / "raw", tag_b)
                    manifest["runs"] += [tag_a, tag_b]
                    if rec_a["returncode"] == 0 and rec_b["returncode"] == 0:
                        break
                    retries += 1
                    if retries > 2:
                        break
                    manifest.setdefault("combo_retries", []).append(
                        {"op": op, "request_size": rs, "depth": d,
                         "arm": label, "retries": retries})
                row = tax0z.aggregate_combo([rec_a, rec_b])
                if row is None:
                    print(f"[FAIL] {op} r{rs} d{d} {label}: aggregation failed")
                    if op == "write":
                        blocked_write = f"r{rs} d{d} {label}"
                        print(f"[BLOCKED] write cell {blocked_write} failed "
                              f"(OBS-1); stopping remaining write cells")
                    continue
                if op == "write":
                    row["write_runner_verified"] = \
                        tax0z.runner_verify_write(arm, rs, op)
                    if not row["write_runner_verified"]:
                        print(f"[FAIL] {op} r{rs} d{d} {label}: "
                              f"runner write verification mismatch")
                        row["ok"] = False
                rows.append(row)
                subprocess.run(["sync"], check=False)
                subprocess.run(["sleep", "0.3"], check=False)
                ins = row.get("instructions_u_per_op")
                cyc = row.get("cycles_u_per_op")
                ins_s = f"{ins:.0f}" if ins else "n/a"
                cyc_s = f"{cyc:.0f}" if cyc else "n/a"
                print(f"[{'OK' if row['ok'] else 'FAIL'}] {op} r{rs} d{d} "
                      f"{label}: instr/op={ins_s} cycles/op={cyc_s} "
                      f"wall/op={row['wall_ns_per_op']:.0f}ns")
                if op == "write" and not row["ok"]:
                    blocked_write = f"r{rs} d{d} {label}"
                    print(f"[BLOCKED] write cell {blocked_write} failed "
                          f"(OBS-1); stopping remaining write cells")

    problems = tax0z.cross_arm_same_work(rows)
    for p in problems:
        print(f"SAME-WORK PROBLEM: {p}")

    (sdir / "summary.json").write_text(json.dumps(rows, indent=1) + "\n")
    fieldnames = sorted({k for r in rows for k in r})
    with open(sdir / "summary.csv", "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fieldnames, extrasaction="ignore")
        w.writeheader()
        w.writerows(rows)
    (sdir / "manifest.json").write_text(json.dumps(manifest, indent=1) + "\n")
    print(f"\nsession: {sdir}")
    return sdir


def compare(session, original=ORIGINAL_SESSION):
    """Same-work + tax-structure comparison vs the original canonical session."""
    closure = json.loads((RESULTS_ROOT / session / "summary.json").read_text())
    orig = json.loads((RESULTS_ROOT / original / "summary.json").read_text())
    orig_by = {(r["op"], r["request_size"], r["depth"], r["arm"]): r
               for r in orig}
    verdicts = []
    for r in closure:
        key = (r["op"], r["request_size"], r["depth"], r["arm"])
        o = orig_by.get(key)
        if o is None:
            verdicts.append((key, "NO_ORIGINAL_CELL", None))
            continue
        sw_ok = (o["ops"] == r["ops"] and o["word_sum"] == r["word_sum"]
                 and o["ok"] and r["ok"])
        if r["op"] == "write":
            sw_ok = sw_ok and r.get("write_runner_verified")
        verdicts.append((key, "PASS" if sw_ok else "FAIL", o))
    return closure, orig, verdicts


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("action", choices=["run", "compare", "report"],
                    default="run", nargs="?")
    ap.add_argument("--session", default=None,
                    help="session id (default: tax0-clean-tree-closure-<date>)")
    ap.add_argument("--out-dir", default=None,
                    help="parent dir for the session (default: "
                         "research/tax0/results; pass an OUT-OF-REPO dir to "
                         "keep the repo tree clean during measurement)")
    args = ap.parse_args()

    if args.action == "run":
        session = args.session or \
            f"tax0-clean-tree-closure-{datetime.datetime.now():%Y%m%d-%H%M%S}"
        cells_by_op = {"read": READ_CELLS, "write": WRITE_CELLS}
        out_root = pathlib.Path(args.out_dir) if args.out_dir else None
        run_closure(session, cells_by_op, out_root)
        print(f"compare with: python3 {__file__} compare --session {session}")
        return

    if args.action in ("compare", "report"):
        if not args.session:
            sys.exit("--session required for compare/report")
        closure, orig, verdicts = compare(args.session)
        sw_fail = [v for v in verdicts if v[1] != "PASS"]
        print(f"same-work verdicts ({len(verdicts)} closure cells):")
        for key, v, _ in sorted(verdicts):
            print(f"  {key[0]:5} r{key[1]} d{key[2]:<3} {key[3]:<4} -> {v}")
        print(f"\nSAME-WORK: {'PASS' if not sw_fail else 'FAIL ' + str(sw_fail)}")
        if args.action == "report":
            report_table(closure, orig)
        return


def report_table(closure, orig):
    """Delta table: original vs clean (instructions/op) per read cell."""
    c = {(r["op"], r["request_size"], r["depth"], r["arm"]): r for r in closure}
    o = {(r["op"], r["request_size"], r["depth"], r["arm"]): r for r in orig}
    print("\nread cells (instructions/op, original -> clean, delta):")
    for rs, d in READ_CELLS:
        def val(m, a):
            r = m.get(("read", rs, d, a))
            return r.get("instructions_u_per_op") if r else None
        arms = {a: (val(o, a), val(c, a)) for a in ("z1", "z1b", "z2", "z3w1")}

        def show(a):
            ov, cv = arms[a]
            if ov is None or cv is None:
                return f"{a}=orig {ov} / clean {cv}"
            return f"{a}={ov:.0f}->{cv:.0f} ({cv - ov:+.0f})"
        print(f"  {rs // 1024}K d{d}: " + ", ".join(show(a) for a in arms))

        def delta(a, b):
            ov, cv = arms[a]
            ov2, cv2 = arms[b]
            if None in (ov, cv, ov2, cv2):
                return None
            return (cv2 - cv, ov2 - ov)
        z1z1b = delta("z1", "z1b")
        z1bz2 = delta("z1b", "z2")
        z2z3 = delta("z2", "z3w1")
        def fmt(t):
            if t is None:
                return "n/a"
            return f"clean {t[0]:+.0f} (orig {t[1]:+.0f})"
        print(f"       Z1b-Z1: {fmt(z1z1b)} | Z2-Z1b: {fmt(z1bz2)} | "
              f"Z3w1-Z2: {fmt(z2z3)}")


if __name__ == "__main__":
    main()
