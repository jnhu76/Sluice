#!/usr/bin/env python3
"""check_g1_control_c0_analysis.py — fail-closed analysis validator (#279).

Re-derives the campaign analysis from the immutable raw evidence and fails
if ANY preregistered cell is missing a valid run, if any same-work, causal,
or threaded-gate field is violated, if the materiality computation does not
match the frozen rule, or if the stored campaign verdict does not equal the
verdict independently re-derived from the raw cells (prereg §13/§13.1).

Corrective-1 hardening (P1-3): the validator previously re-computed
medians/MAD/ratios/per-cell directions but only checked the stored campaign
verdict against the frozen VOCABULARY — a wrong-but-well-spelled verdict
would have passed. It now derives the READ/WRITE verdicts from the raw
cells and requires stored == derived. `--self-test` proves by in-memory
mutation that a falsified stored verdict is rejected.

Modes:
  <session-id>                    single-session validation (scope read
                                  from the session manifest: "full" or
                                  "threaded-corrective"). A pre-corrective
                                  full-matrix session FAILS by design: its
                                  threaded runs lack the corrective gate
                                  fields (prereg §5 was violated); use
                                  --composite for the campaign-level
                                  certification.
  --composite <sid1> <sid2>       Corrective-1 campaign certification:
                                  F0/F1 from the single-thread session,
                                  F0-T/F1-T from the threaded-corrective
                                  session; the native-1 threaded subset is
                                  required to be the superseded shape and
                                  is excluded from every derived number.
  --self-test                     in-memory mutation self-test; never
                                  touches session files.

Usage:
  python3 check_g1_control_c0_analysis.py <session-id>
  python3 check_g1_control_c0_analysis.py --composite <sid-single> <sid-threaded>
  python3 check_g1_control_c0_analysis.py --self-test
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
RESULTS = REPO / "research/g1-control-c0/results"

OPS = ["READ", "WRITE"]
SIZES = [4096, 65536, 2097152]
DEPTHS_BY_SIZE = {4096: [1, 8, 32], 65536: [1], 2097152: [1]}
FS = ["tmpfs", "btrfs"]
ARMS = ["F0", "F1", "F0-T", "F1-T"]
THREAD_ARMS = ["F0-T", "F1-T"]
SINGLE_ARMS = ["F0", "F1"]
ROUNDS = 7
THREADED_WORKERS = 4
FILE_BYTES = {4096: 512 * 1024 * 1024, 65536: 1 << 30, 2097152: 1 << 30}
MATERIAL_RATIO = 1.03
MATERIAL_MAD_K = 1.5
PRIMARY_DEPTHS = DEPTHS_BY_SIZE[4096]

PERF_VERDICTS = {
    "FIXED-FILE PERFORMANCE BENEFIT ESTABLISHED",
    "REGIME-LOCAL BENEFIT ESTABLISHED",
    "FIXED-FILE PERFORMANCE REGRESSION",
    "FIXED-FILE PERFORMANCE BENEFIT NOT ESTABLISHED",
    "BLOCKED", "INVALID",
}


def median(vals):
    if not vals:
        return 0.0
    s = sorted(vals)
    return s[len(s) // 2]


def mad(vals, med):
    if not vals:
        return 0.0
    return median([abs(v - med) for v in vals])


# ---- frozen rule, re-implemented independently of the driver ------------

def direction_of(f0_vals, f1_vals) -> str:
    """Frozen materiality (prereg §13): F1 faster if ratio > 1."""
    if not f0_vals or not f1_vals:
        return "NONE"
    m0, m1 = median(f0_vals), median(f1_vals)
    mad0, mad1 = mad(f0_vals, m0), mad(f1_vals, m1)
    ratio = m0 / m1 if m1 else float("inf")
    benefit = ratio >= MATERIAL_RATIO and \
        m1 + MATERIAL_MAD_K * mad1 < m0 - MATERIAL_MAD_K * mad0
    regression = ratio <= 1.0 / MATERIAL_RATIO and \
        m0 + MATERIAL_MAD_K * mad0 < m1 - MATERIAL_MAD_K * mad1
    return "F1_FASTER" if benefit else "F1_SLOWER" if regression else "NONE"


def neighbor_share(directions: dict) -> dict:
    """prereg §13 neighbor consistency: neighbors of a primary 4 KiB tmpfs
    cell are the other 4 KiB depths (tmpfs), the 64 KiB cell (tmpfs), and
    the same cell on btrfs. True when >= 1 neighbor shares the direction."""
    share = {}
    for op in OPS:
        for d in PRIMARY_DEPTHS:
            cell = f"{op}_4096_{d}_tmpfs"
            own = directions.get(cell)
            if own is None:
                share[cell] = False
                continue
            neighbor_dirs = [directions.get(f"{op}_4096_{d2}_tmpfs")
                             for d2 in PRIMARY_DEPTHS if d2 != d]
            neighbor_dirs.append(directions.get(f"{op}_65536_1_tmpfs"))
            neighbor_dirs.append(directions.get(f"{op}_4096_{d}_btrfs"))
            share[cell] = any(x == own for x in neighbor_dirs if x)
    return share


def derive_verdicts(directions: dict, share: dict) -> dict:
    """prereg §13.1 campaign verdict derivation from primary-cell
    directions. Independent of the driver's copy by construction."""
    verdicts = {}
    for op in OPS:
        cells = [f"{op}_4096_{d}_tmpfs" for d in PRIMARY_DEPTHS]
        dirs = [directions.get(c) for c in cells]
        benefit_supported = any(d == "F1_FASTER" and share.get(c)
                                for c, d in zip(cells, dirs))
        regress_supported = any(d == "F1_SLOWER" and share.get(c)
                                for c, d in zip(cells, dirs))
        if benefit_supported:
            verdicts[op] = "FIXED-FILE PERFORMANCE BENEFIT ESTABLISHED"
        elif regress_supported:
            verdicts[op] = "FIXED-FILE PERFORMANCE REGRESSION"
        elif any(d == "F1_FASTER" for d in dirs):
            verdicts[op] = "REGIME-LOCAL BENEFIT ESTABLISHED"
        else:
            # isolated regression is a per-cell observation; the frozen
            # vocabulary has no isolated-regression campaign verdict
            verdicts[op] = "FIXED-FILE PERFORMANCE BENEFIT NOT ESTABLISHED"
    return verdicts


def compare_verdicts(stored: dict, derived: dict, label: str,
                     fails: list) -> None:
    """The single verdict gate used by every mode (and by the self-test):
    the stored verdict must be in the frozen vocabulary AND equal the
    verdict re-derived from raw cells."""
    for op in OPS:
        v = stored.get(op)
        if v not in PERF_VERDICTS:
            fails.append(f"{label}[{op}]={v} not in frozen vocabulary")
        elif v != derived[op]:
            fails.append(f"{label}[{op}]: stored '{v}' != re-derived "
                         f"'{derived[op]}'")


def gate_field_problems(run, problems: list) -> None:
    """Corrective-1 threaded gate fields (prereg §5 frozen condition):
    workers ready before the span, released only after it. Fail-closed."""
    b = run.get("bench") or {}
    rid = run["run_id"]
    if (b.get("threads_spawned") != THREADED_WORKERS or
            b.get("threads_io_ok") != THREADED_WORKERS or
            b.get("threads_joined") != THREADED_WORKERS):
        problems.append(f"{rid}: threaded counters != {THREADED_WORKERS}")
    if "threads_ready" not in b or "threads_released" not in b or \
            "thread_gate_ready" not in b or \
            "thread_gate_release_after_transfer" not in b:
        problems.append(
            f"{rid}: corrective threaded gate fields missing "
            f"(threads_ready/threads_released/thread_gate_ready/"
            f"thread_gate_release_after_transfer) — pre-corrective shape")
        return
    if (b.get("threads_ready") != THREADED_WORKERS or
            b.get("threads_released") != THREADED_WORKERS):
        problems.append(f"{rid}: threaded gate counts != {THREADED_WORKERS}")
    if b.get("thread_gate_ready") is not True or \
            b.get("thread_gate_release_after_transfer") is not True:
        problems.append(f"{rid}: threaded gate causality flags not true")


def check_same_work(run, problems: list) -> None:
    """Per-run same-work + causal re-verification from raw."""
    b = run.get("bench")
    rid = run["run_id"]
    if b is None:
        problems.append(f"{rid}: bench json missing")
        return
    if b.get("canceled", 1) != 0 or b.get("errors", 1) != 0 or \
            b.get("short_reads", 1) != 0 or b.get("short_writes", 1) != 0:
        problems.append(f"{rid}: unexpected terminal/short I/O")
    fb = FILE_BYTES[run["size"]]
    if b.get("bytes_read") != (fb if run["op"] == "READ" else 0) or \
            b.get("bytes_written") != (fb if run["op"] == "WRITE" else 0):
        problems.append(f"{rid}: byte accounting")
    if b.get("cqe_count") != b.get("chunks"):
        problems.append(f"{rid}: cqe accounting")
    if b.get("align_remainder") != 0 or b.get("slot_stride") != run["size"]:
        problems.append(f"{rid}: causal isolation storage")
    if run["arm"] in ("F1", "F1-T") and b.get("registered_files") != 1:
        problems.append(f"{rid}: registration table")


def check_write_hash(run, manifest, problems: list) -> None:
    if run["op"] == "WRITE":
        exp = manifest.get(f"expected_dst_sha256_{run['size']}")
        if not exp:
            problems.append(f"{run['run_id']}: expected dst hash not frozen")
        elif run.get("dst_sha256") != exp:
            problems.append(f"{run['run_id']}: dst hash mismatch")


def load_session(sid: str):
    sd = RESULTS / sid
    manifest = json.loads((sd / "manifest.json").read_text())
    gates = json.loads((sd / "gates.json").read_text())
    raw = sd / "raw" / "runs.jsonl"
    runs = [json.loads(l) for l in raw.read_text().splitlines() if l.strip()]
    runs = [r for r in runs if not r["run_id"].startswith("q0-")]
    return sd, manifest, gates, runs


def scope_arms(scope: str) -> list:
    return ARMS if scope == "full" else THREAD_ARMS


def values_by_cell(runs):
    """ok runs -> {(op,size,depth,fs,arm): [wall_per_op_ns, ...]}"""
    out: dict = {}
    for r in runs:
        if r.get("ok") and r.get("bench"):
            out.setdefault((r["op"], r["size"], r["depth"], r["fs"],
                            r["arm"]), []).append(
                r["bench"]["wall_per_op_ns"])
    return out


def directions_from(values: dict, arm0: str, arm1: str) -> dict:
    """All-cell directions for one arm pair."""
    d = {}
    for op in OPS:
        for size in SIZES:
            for depth in DEPTHS_BY_SIZE[size]:
                for fs in FS:
                    d[f"{op}_{size}_{depth}_{fs}"] = direction_of(
                        values.get((op, size, depth, fs, arm0), []),
                        values.get((op, size, depth, fs, arm1), []))
    return d


# ---- single-session mode -------------------------------------------------

def validate_single(sid: str) -> None:
    fails: list = []
    sd, manifest, gates, runs = load_session(sid)
    scope = manifest.get("scope", "full")
    expected_arms = scope_arms(scope)

    errs = gates.get("errors", [])
    if errs:
        fails.append(f"{len(errs)} gate errors in gates.json")
    for size in SIZES:
        if not manifest.get(f"expected_dst_sha256_{size}"):
            fails.append(f"expected_dst_sha256_{size} not frozen")

    ok_runs = [r for r in runs if r.get("ok")]
    by_cell: dict = {}
    for r in ok_runs:
        by_cell.setdefault((r["op"], r["size"], r["depth"], r["fs"],
                            r["arm"]), []).append(r)

    # scope discipline + exact cell coverage
    for r in runs:
        if r["arm"] not in expected_arms:
            fails.append(f"{r['run_id']}: arm {r['arm']} outside "
                         f"scope '{scope}'")
    for op in OPS:
        for size in SIZES:
            for depth in DEPTHS_BY_SIZE[size]:
                for fs in FS:
                    for arm in expected_arms:
                        n = len(by_cell.get((op, size, depth, fs, arm), []))
                        if n != ROUNDS:
                            fails.append(f"{(op, size, depth, fs, arm)}: "
                                         f"{n} valid runs (expected "
                                         f"{ROUNDS})")

    ids = [r["run_id"] for r in runs]
    if len(ids) != len(set(ids)):
        fails.append("duplicate run ids")

    for r in ok_runs:
        check_same_work(r, fails)
        if r["arm"] in THREAD_ARMS:
            gate_field_problems(r, fails)
        check_write_hash(r, manifest, fails)

    # re-derive verdicts from raw and require stored == derived
    summary = json.loads((sd / "summary.json").read_text())
    values = values_by_cell(ok_runs)
    if scope == "full":
        dirs = directions_from(values, "F0", "F1")
        primary = {f"{op}_4096_{d}_tmpfs": dirs[f"{op}_4096_{d}_tmpfs"]
                   for op in OPS for d in PRIMARY_DEPTHS}
        derived = derive_verdicts(primary, neighbor_share(primary))
        # per-cell direction + ratio re-checks
        for op in OPS:
            for size in SIZES:
                for depth in DEPTHS_BY_SIZE[size]:
                    for fs in FS:
                        cell = f"{op}_{size}_{depth}_{fs}"
                        stored = summary["per_cell"][cell]["f0"]["direction"]
                        if stored != dirs[cell]:
                            fails.append(f"{cell}: stored direction {stored} "
                                         f"!= re-derived {dirs[cell]}")
                        m0 = median(values.get(
                            (op, size, depth, fs, "F0"), []))
                        m1 = median(values.get(
                            (op, size, depth, fs, "F1"), []))
                        stored_ratio = summary["per_cell"][cell]["f0"][
                            "ratio"]
                        ratio = (m0 / m1) if m1 else float("inf")
                        if stored_ratio is None or \
                                abs(stored_ratio - ratio) > 5e-5:
                            fails.append(f"{cell}: ratio mismatch")
        compare_verdicts(summary["verdicts"], derived, "verdict", fails)
    else:
        dirs = directions_from(values, "F0-T", "F1-T")
        primary = {f"{op}_4096_{d}_tmpfs": dirs[f"{op}_4096_{d}_tmpfs"]
                   for op in OPS for d in PRIMARY_DEPTHS}
        derived = derive_verdicts(primary, neighbor_share(primary))
        compare_verdicts(summary.get("threaded_verdicts", {}), derived,
                         "threaded_verdicts", fails)

    report(f"ANALYSIS", fails, sid=sid, scope=scope, ok=len(ok_runs),
           note=("pre-corrective sessions fail on threaded gate fields by "
                 "design — use --composite" if scope == "full" else None))


# ---- composite mode -------------------------------------------------------

def validate_composite(sid_single: str, sid_threaded: str) -> None:
    fails: list = []
    sd1, man1, gates1, runs1 = load_session(sid_single)
    sd2, man2, gates2, runs2 = load_session(sid_threaded)

    for sid, gates in ((sid_single, gates1), (sid_threaded, gates2)):
        if gates.get("errors"):
            fails.append(f"{sid}: {len(gates['errors'])} gate errors")
    if man2.get("scope") != "threaded-corrective":
        fails.append(f"{sid_threaded}: manifest scope is not "
                     f"threaded-corrective")

    ok1 = [r for r in runs1 if r.get("ok")]
    ok2 = [r for r in runs2 if r.get("ok")]
    cells1: dict = {}
    for r in ok1:
        cells1.setdefault((r["op"], r["size"], r["depth"], r["fs"],
                           r["arm"]), []).append(r)
    cells2: dict = {}
    for r in ok2:
        cells2.setdefault((r["op"], r["size"], r["depth"], r["fs"],
                           r["arm"]), []).append(r)

    for op in OPS:
        for size in SIZES:
            for depth in DEPTHS_BY_SIZE[size]:
                for fs in FS:
                    for arm in SINGLE_ARMS:
                        n = len(cells1.get((op, size, depth, fs, arm), []))
                        if n != ROUNDS:
                            fails.append(f"{sid_single} {op}/{size}/{depth}/"
                                         f"{fs}/{arm}: {n} valid runs "
                                         f"(expected {ROUNDS})")
                    for arm in THREAD_ARMS:
                        n = len(cells2.get((op, size, depth, fs, arm), []))
                        if n != ROUNDS:
                            fails.append(f"{sid_threaded} {op}/{size}/"
                                         f"{depth}/{fs}/{arm}: {n} valid "
                                         f"runs (expected {ROUNDS})")

    # superseded-shape discipline on native-1's threaded subset
    superseded = [r for r in runs1 if r["arm"] in THREAD_ARMS]
    if len(superseded) != 280:
        fails.append(f"{sid_single}: expected 280 superseded threaded runs, "
                     f"found {len(superseded)}")
    for r in superseded:
        if "threads_ready" in (r.get("bench") or {}):
            fails.append(f"{sid_single} {r['run_id']}: superseded threaded "
                         f"run unexpectedly carries corrective gate fields")
    # scope discipline on native-2
    for r in runs2:
        if r["arm"] not in THREAD_ARMS:
            fails.append(f"{sid_threaded} {r['run_id']}: arm outside "
                         f"threaded-corrective scope")

    for r in ok1:
        if r["arm"] in SINGLE_ARMS:
            check_same_work(r, fails)
            check_write_hash(r, man1, fails)
    for r in ok2:
        check_same_work(r, fails)
        gate_field_problems(r, fails)
        check_write_hash(r, man2, fails)

    # re-derive everything from raw and compare to composite-summary.json
    comp_path = sd2 / "composite-summary.json"
    if not comp_path.is_file():
        fails.append(f"{sid_threaded}: composite-summary.json missing "
                     f"(run 'g1_control_c0.py composite {sid_single} "
                     f"{sid_threaded}' first)")
    else:
        comp = json.loads(comp_path.read_text())
        vals1 = values_by_cell(ok1)
        vals2 = values_by_cell(ok2)
        dirs_single = directions_from(vals1, "F0", "F1")
        dirs_threaded = directions_from(vals2, "F0-T", "F1-T")
        prim_single = {f"{op}_4096_{d}_tmpfs":
                       dirs_single[f"{op}_4096_{d}_tmpfs"]
                       for op in OPS for d in PRIMARY_DEPTHS}
        prim_threaded = {f"{op}_4096_{d}_tmpfs":
                         dirs_threaded[f"{op}_4096_{d}_tmpfs"]
                         for op in OPS for d in PRIMARY_DEPTHS}
        derived_v = derive_verdicts(prim_single,
                                    neighbor_share(prim_single))
        derived_tv = derive_verdicts(prim_threaded,
                                     neighbor_share(prim_threaded))
        compare_verdicts(comp.get("verdicts", {}), derived_v,
                         "composite verdicts", fails)
        compare_verdicts(comp.get("threaded_verdicts", {}), derived_tv,
                         "composite threaded_verdicts", fails)
        sup = comp.get("superseded", {})
        if sup.get("session") != sid_single or \
                set(sup.get("arms", [])) != set(THREAD_ARMS) or \
                sup.get("runs") != len(superseded):
            fails.append(f"composite superseded provenance mismatch: {sup}")
        for key, sid in (("single_thread_source", sid_single),
                         ("threaded_source", sid_threaded)):
            src = comp.get(key, {})
            if src.get("session") != sid:
                fails.append(f"composite {key}.session != {sid}")
            if not src.get("git_head") or not src.get("bench_binary_sha256"):
                fails.append(f"composite {key}: provenance incomplete")

    report(f"COMPOSITE ANALYSIS", fails, sid=f"{sid_single} + "
           f"{sid_threaded}", scope="composite",
           ok=len(ok1) + len(ok2), note=None)


# ---- self-test ------------------------------------------------------------

def self_test() -> None:
    """In-memory mutation self-test (Corrective-1 P1-3): proves the verdict
    derivation rejects a falsified stored verdict. Touches no files."""
    def vals(center_f0, center_f1, spread0=2.0, spread1=1.0, n=7):
        # deterministic value sets: F0/ F1 medians at the given centers
        return ([center_f0 + (i % 3 - 1) * spread0 for i in range(n)],
                [center_f1 + (i % 3 - 1) * spread1 for i in range(n)])

    def synthetic_directions(f1_faster: bool) -> dict:
        d = {}
        for op in OPS:
            d[f"{op}_4096_1_tmpfs"], d[f"{op}_4096_8_tmpfs"], \
                d[f"{op}_4096_32_tmpfs"] = (
                    ("F1_FASTER", "F1_FASTER", "F1_FASTER") if f1_faster
                    else ("NONE", "NONE", "NONE"))
            d[f"{op}_65536_1_tmpfs"] = "F1_FASTER" if f1_faster else "NONE"
            d[f"{op}_4096_1_btrfs"] = "F1_FASTER" if f1_faster else "NONE"
            d[f"{op}_4096_8_btrfs"] = "F1_FASTER" if f1_faster else "NONE"
            d[f"{op}_4096_32_btrfs"] = "F1_FASTER" if f1_faster else "NONE"
        return d

    failures = []

    # scenario A: robust material F1-faster everywhere -> BENEFIT ESTABLISHED
    dirs_a = synthetic_directions(f1_faster=True)
    share_a = neighbor_share(dirs_a)
    derived_a = derive_verdicts(dirs_a, share_a)
    if any(v != "FIXED-FILE PERFORMANCE BENEFIT ESTABLISHED"
           for v in derived_a.values()):
        failures.append(f"scenario A derivation wrong: {derived_a}")
    # mutation (benefit erasure): store NOT ESTABLISHED instead -> the
    # validator's verdict gate must flag it
    mutated_a = dict(derived_a)
    mutated_a["READ"] = "FIXED-FILE PERFORMANCE BENEFIT NOT ESTABLISHED"
    flagged: list = []
    compare_verdicts(mutated_a, derived_a, "mutated", flagged)
    if not flagged:
        failures.append("scenario A mutation undetected")

    # scenario B: all NONE -> NOT ESTABLISHED; mutate to BENEFIT ESTABLISHED
    # (benefit fabrication) -> the verdict gate must flag it
    dirs_b = synthetic_directions(f1_faster=False)
    share_b = neighbor_share(dirs_b)
    derived_b = derive_verdicts(dirs_b, share_b)
    if any(v != "FIXED-FILE PERFORMANCE BENEFIT NOT ESTABLISHED"
           for v in derived_b.values()):
        failures.append(f"scenario B derivation wrong: {derived_b}")
    mutated_b = dict(derived_b)
    mutated_b["READ"] = "FIXED-FILE PERFORMANCE BENEFIT ESTABLISHED"
    flagged = []
    compare_verdicts(mutated_b, derived_b, "mutated", flagged)
    if not flagged:
        failures.append("scenario B mutation undetected")

    # the direction rule itself, on real numeric value sets
    f0, f1 = vals(center_f0=1000.0, center_f1=900.0)   # ratio 1.111, robust
    if direction_of(f0, f1) != "F1_FASTER":
        failures.append("direction rule: robust benefit not detected")
    g0, g1 = vals(center_f0=1000.0, center_f1=985.0)   # ratio 1.0152 < 1.03
    if direction_of(g0, g1) != "NONE":
        failures.append("direction rule: sub-threshold ratio flagged")
    h0, h1 = vals(center_f0=1000.0, center_f1=960.0, spread1=30.0)
    # ratio 1.0417 >= 1.03 but 1.5*MAD separation must fail (wide spread)
    if direction_of(h0, h1) != "NONE":
        failures.append("direction rule: separation bypass detected")

    if failures:
        print(f"SELF-TEST FAIL ({len(failures)}):")
        for f in failures:
            print(f"  - {f}")
        sys.exit(1)
    print("SELF-TEST PASS: verdict derivation re-derives both scenarios "
          "correctly and detects falsified stored verdicts "
          "(benefit-erasure and benefit-fabrication mutations).")


# ---- report ----------------------------------------------------------------

def report(kind: str, fails: list, sid: str, scope: str, ok: int,
           note) -> None:
    if fails:
        print(f"{kind} FAIL ({len(fails)}):")
        for f in fails:
            print(f"  - {f}")
        if note:
            print(f"  note: {note}")
        sys.exit(1)
    print(f"{kind} PASS: session {sid} (scope {scope}), {ok} valid runs, "
          f"verdicts independently re-derived from raw and equal to stored.")
    if note:
        print(f"note: {note}")


def main() -> None:
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        sys.exit(2)
    if args[0] == "--self-test":
        self_test()
    elif args[0] == "--composite":
        if len(args) != 3:
            print("usage: check_g1_control_c0_analysis.py --composite "
                  "<sid-single> <sid-threaded>", file=sys.stderr)
            sys.exit(2)
        validate_composite(args[1], args[2])
    elif args[0].startswith("--"):
        print(f"unknown option {args[0]}", file=sys.stderr)
        sys.exit(2)
    else:
        if len(args) != 1:
            print("usage: check_g1_control_c0_analysis.py <session-id>",
                  file=sys.stderr)
            sys.exit(2)
        validate_single(args[0])


if __name__ == "__main__":
    main()
