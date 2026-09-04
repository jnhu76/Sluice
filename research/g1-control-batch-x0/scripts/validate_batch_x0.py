#!/usr/bin/env python3
# validate_batch_x0.py — BATCH-X0 mechanical validator (prereg §11).
#
# Parses rows.jsonl, re-derives per-cell medians and ALL §10 verdicts,
# recomputes the A/A qualification gate from RAW rows (Corrective-1 P1-2;
# a recorded verdicts.json is cross-checked, never trusted), verifies real
# git freeze ancestry (P1-3), enforces the verified ext4 substrate record
# (P1-1), checks per-cell enter-counter evidence and derives the transport
# verdict from it (P1-5), re-checks same-work witnesses, identity witnesses
# (M7), the MB-region design budget (M8), and the semantic fixture record
# (S1..S10 + S9 verdict). Blocking gates are resolved BEFORE the
# value/G1/PROMOTION derivation so a BLOCKED upstream gate can never coexist
# with a positive promotion (P1-4).
#
# Self-test mode validates itself against deliberately corrupted synthetic
# sessions; each test is enumerated by what it actually proves (P2: no
# "N/N corrupted evidence rejected" aggregates).
#
# Usage:
#   validate_batch_x0.py <matrix-results-dir> --qualification <qual-dir>
#   validate_batch_x0.py --selftest
import argparse
import json
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

RESEARCH = Path(__file__).resolve().parents[1]
ROOT = RESEARCH.parents[1]
BENCH_CPP = ROOT / "bench/g1_control_batch_x0_bench.cpp"
FREEZE = "c0da5db505d24bb8f7c576459b74eaee8ef68ad5"

ARMS = ["B0", "B1", "B2", "MB1", "MB3"]
OPS = ["read", "write"]
SIZES = [4096, 65536]
NS = [1, 2, 4, 8, 16, 32]
REPS = 7
MATERIAL = 0.05          # prereg §10 materiality threshold
MATERIAL_CELLS = 4       # of 6 N-cells
M8_BUDGET_LINES = 480    # MB-region design budget (prereg §5 M8)
ANCHOR_BOUND = 0.30      # S-9 substrate comparability bound (prereg §10)


class Fail(Exception):
    pass


def check(cond, msg):
    if not cond:
        raise Fail(msg)


def median(xs):
    xs = sorted(xs)
    n = len(xs)
    return xs[n // 2] if n % 2 else 0.5 * (xs[n // 2 - 1] + xs[n // 2])


def load_rows(path):
    rows = []
    for line in Path(path).read_text().splitlines():
        line = line.strip()
        if line.startswith("{"):
            rows.append(json.loads(line))
    return rows


def perf_rows(rows):
    return [r for r in rows if r.get("kind") == "perf"]


def cell_medians(rows, field="wall_per_op_ns"):
    cells = defaultdict(list)
    for r in rows:
        cells[(r["arm"], r["op"], r["size"], r["n"])].append(r[field])
    return {k: median(v) for k, v in cells.items()}


# --- git ancestry (Corrective-1 P1-3) ---------------------------------------

def git(*args):
    return subprocess.run(["git", "-C", str(ROOT), *args],
                          capture_output=True, text=True)


def is_descendant(commit):
    """True iff `commit` exists as a git commit object and is the freeze
    commit itself or a real descendant of it (git merge-base --is-ancestor).
    Fails for invalid SHAs, pre-freeze ancestors, and foreign lineages."""
    if git("cat-file", "-e", f"{commit}^{{commit}}").returncode != 0:
        return False
    return git("merge-base", "--is-ancestor", FREEZE, commit).returncode == 0


# --- evidence-section validators ---------------------------------------------

def validate_semantic_dir(sdir):
    """S1..S10 all PASS; S9 verdict recorded and one of DIVERGENCE/NO_DIVERGENCE.
    S9's execution PASS is carried by fixtures["S9"]; its verdict field is the
    separate semantic observation."""
    text = (sdir / "fixtures.jsonl").read_text()
    fixtures = {}
    s9 = None
    for line in text.splitlines():
        if not line.startswith("{"):
            continue
        d = json.loads(line)
        if d.get("kind") == "fixture":
            if d["fixture"] == "S9" and "verdict" in d:
                s9 = d["verdict"]
            else:
                fixtures[d["fixture"]] = d.get("result")
    for name in ["S1", "S2", "S3", "S4", "S5", "S6", "S7", "S8", "S9", "S10"]:
        check(fixtures.get(name) == "PASS", f"fixture {name} not PASS: {fixtures.get(name)}")
    check(s9 in ("DIVERGENCE", "NO_DIVERGENCE"), f"S9 verdict missing: {s9}")
    return {"s9_verdict": s9}


def check_session_env(env, what):
    """Commit pinning + verified substrate shared by matrix and qualification
    sessions (Corrective-1 P1-1/P1-3)."""
    check(env["dirty_tracked"] is False, f"{what}: dirty_tracked=true")
    check(is_descendant(env["commit"]),
          f"{what}: commit is not the freeze or a git descendant of it: {env['commit']}")
    check(env.get("filesystem_type") == "ext4",
          f"{what}: substrate not a VERIFIED ext4 work dir "
          f"(filesystem_type={env.get('filesystem_type')!r}; prereg §9)")
    check(env.get("work_dir"), f"{what}: environment record missing work_dir")


def validate_aa(qdir):
    """Recompute the frozen §9 A/A gate from RAW qualification rows
    (Corrective-1 P1-2). Two passes, arms B1/B2 only, full 48-cell grid at
    REPS each, per matched cell ratio ≤ 5% on ≥ 90% of cells. Fails closed
    on any missing evidence."""
    check(qdir.is_dir(), f"qualification session missing: {qdir}")
    check((qdir / "environment.json").exists(),
          f"qualification session missing environment.json: {qdir}")
    check_session_env(json.loads((qdir / "environment.json").read_text()),
                      "qualification")
    medians = []
    for p in (1, 2):
        path = qdir / f"rows-pass{p}.jsonl"
        check(path.exists(), f"qualification pass {p} rows missing")
        rows = perf_rows(load_rows(path))
        check(rows, f"qualification pass {p}: no perf rows")
        cells = defaultdict(list)
        for r in rows:
            check(r["arm"] in ("B1", "B2"),
                  f"qualification pass {p}: arm outside A/A pair: {r['arm']}")
            check(r.get("work") == "ok", f"qualification pass {p}: work row not ok")
            cells[(r["arm"], r["op"], r["size"], r["n"])].append(r["wall_per_op_ns"])
        expected = 2 * len(OPS) * len(SIZES) * len(NS)
        check(len(cells) == expected,
              f"qualification pass {p}: grid has {len(cells)} cells (want {expected})")
        for k, v in cells.items():
            check(len(v) == REPS, f"qualification pass {p}: cell {k} has {len(v)} reps")
        medians.append({k: median(v) for k, v in cells.items()})
    check(set(medians[0]) == set(medians[1]), "qualification pass grids differ")
    ratios = {k: abs(a - medians[1][k]) / min(a, medians[1][k])
              for k, a in medians[0].items()}
    ok = sum(1 for r in ratios.values() if r <= MATERIAL)
    frac = ok / len(ratios)
    passed = frac >= 0.90
    rec_path = qdir / "verdicts.json"
    if rec_path.exists():
        rec = json.loads(rec_path.read_text())
        check(rec.get("pass") == passed,
              "recorded A/A verdict disagrees with recomputation from raw rows")
    check(passed, f"A/A qualification FAILED on recomputation: {ok}/{len(ratios)} "
          f"cells within 5% (need ≥ 90%); worst {max(ratios.values()):.3f}")
    violators = sorted(({ "cell": list(k), "ratio": r }
                        for k, r in ratios.items() if r > MATERIAL),
                       key=lambda v: -v["ratio"])
    return {"cells": len(ratios), "within_5pct": ok, "fraction": round(frac, 4),
            "pass": True, "violators": violators[:5]}


def validate_enter_rows(mdir):
    """Per-cell enter-counter evidence (Corrective-1 P1-5): exactly one row
    per arm×op×size×N (120 cells), valid grid, counts ≥ 0, work accounting
    consistent. Fails closed on missing/duplicate/invalid cells."""
    path = mdir / "strace-enter-rows.jsonl"
    check(path.exists(),
          "strace-enter-rows.jsonl missing (per-cell M6 evidence is the "
          "transport authority; arm-total aggregates are not sufficient)")
    seen = {}
    for r in load_rows(path):
        arm, op, size, n = r.get("arm"), r.get("op"), r.get("size"), r.get("n")
        check(arm in ARMS and op in OPS and size in SIZES and n in NS,
              f"enter row outside the frozen grid: {r}")
        key = (arm, op, size, n)
        check(key not in seen, f"duplicate enter-counter cell: {key}")
        ent = r.get("io_uring_enter")
        check(isinstance(ent, int) and not isinstance(ent, bool) and ent >= 0,
              f"bad io_uring_enter count: {r}")
        check(isinstance(r.get("rounds"), int) and r["rounds"] > 0
              and r.get("ops") == r["rounds"] * n,
              f"enter row work accounting inconsistent: {r}")
        check(r.get("counter_reps") == 1, f"enter row counter_reps != 1: {r}")
        seen[key] = r
    expected = len(ARMS) * len(OPS) * len(SIZES) * len(NS)
    check(len(seen) == expected,
          f"enter-counter grid incomplete: {len(seen)} cells (want {expected})")
    return seen


def derive_transport(cells):
    """Frozen rule (prereg §10): B1 vs B2 kernel_enters/ops identical
    (±1 episode boundary) and B0 ≈ 1/N of B1 already ⇒ ALREADY OBTAINED BY
    PRIMITIVE SUBMITS; B2 materially fewer enters than B1 ⇒ ADDITIONAL
    TRANSPORT AMORTIZATION ESTABLISHED.

    Conservative mechanical reading, disclosed in Corrective-1 BEFORE any
    compliant-substrate evidence exists (no new positive criterion):
      per-op enter rate r = io_uring_enter / ops of the counter cell;
      B1↔B2 equivalence per cell: |r_b1 − r_b2| ≤ max(0.05 · max(r), 2/ops)
        (5% relative or ±1 episode boundary over the cell's op count);
      B0 ≈ 1/N per cell: |r_b0 · n − r_b1| ≤ 0.25 · r_b1;
      "B2 materially fewer": r_b2 ≤ 0.95 · r_b1 on ≥ 4 of 6 N-cells per
        size, in BOTH op classes (the frozen §10 materiality shape).
    Evidence satisfying neither frozen outcome maps to BLOCKED — never to a
    positive claim."""
    rates = {arm: {} for arm in ARMS}
    for (arm, op, size, n), row in cells.items():
        rates[arm][(op, size, n)] = row["io_uring_enter"] / row["ops"]
    eq_fail, b0_fail = [], []
    for key, r1 in rates["B1"].items():
        ops = cells[("B1",) + key]["ops"]
        r2 = rates["B2"][key]
        if abs(r1 - r2) > max(0.05 * max(r1, r2), 2.0 / ops):
            eq_fail.append(f"B1/B2@{key}: {r1:.4f} vs {r2:.4f}")
        rb0 = rates["B0"][key]
        if abs(rb0 * key[2] - r1) > 0.25 * r1:
            b0_fail.append(f"B0@{key}: n·{rb0:.4f} vs {r1:.4f}")
    fewer = defaultdict(list)
    for (op, size, n), r2 in rates["B2"].items():
        if r2 <= 0.95 * rates["B1"][(op, size, n)]:
            fewer[(op, size)].append(n)
    material_fewer = all(len(fewer.get((op, size), [])) >= MATERIAL_CELLS
                         for op in OPS for size in SIZES)
    if material_fewer:
        verdict = "ADDITIONAL TRANSPORT AMORTIZATION ESTABLISHED"
    elif not eq_fail and not b0_fail:
        verdict = "ALREADY OBTAINED BY PRIMITIVE SUBMITS"
    else:
        verdict = "BLOCKED"
    detail = {"b1_b2_equivalence_failures": eq_fail[:8],
              "b0_amortization_failures": b0_fail[:8],
              "b2_fewer_enter_cells": {f"{op}|{size}": ns
                                       for (op, size), ns in fewer.items()}}
    return verdict, detail


def validate_matrix(mdir, semantic, qdir):
    rows = perf_rows(load_rows(mdir / "rows.jsonl"))
    check(rows, "no perf rows")
    # --- same work / accounting sanity -----------------------------------
    seen_cells = defaultdict(int)
    for r in rows:
        check(r.get("work") == "ok", f"work row not ok: {r}")
        check(r["arm"] in ARMS and r["op"] in OPS, f"bad arm/op {r}")
        check(r["rounds"] > 0 and r["ops"] == r["rounds"] * r["n"], "ops mismatch")
        check(r["wall_per_op_ns"] > 0 and r["cpu_per_op_ns"] >= 0, "bad timing")
        seen_cells[(r["arm"], r["op"], r["size"], r["n"])] += 1
    for k, cnt in seen_cells.items():
        check(cnt == REPS, f"cell {k} has {cnt} reps (want {REPS})")
    check(len(seen_cells) == len(ARMS) * len(OPS) * len(SIZES) * len(NS),
          f"grid incomplete: {len(seen_cells)} cells")

    # --- environment pinning: real ancestry + verified ext4 substrate -----
    env = json.loads((mdir / "environment.json").read_text())
    check_session_env(env, "matrix")
    session_qual = env.get("qualification_session")
    check(session_qual,
          "matrix environment lacks its qualification_session binding "
          "(Corrective-1 P1-2)")
    check(session_qual == qdir.name,
          f"qualification binding mismatch: session declares {session_qual!r}, "
          f"validator was bound to {qdir.name!r}")

    # --- A/A gate RECOMPUTED from raw qualification rows (P1-2) -----------
    aa = validate_aa(qdir)

    # --- M6: per-cell enter-counter evidence + transport derivation (P1-5)
    enter_cells = validate_enter_rows(mdir)
    transport, transport_detail = derive_transport(enter_cells)

    # --- M7: identity witnesses on MB rows -------------------------------
    for r in rows:
        if r["arm"] in ("MB1", "MB3"):
            check(r["identity_entries"] >= r["rounds"] * r["n"],
                  f"MB identity witness missing: {r['arm']} rep {r['rep']}")
            check(r["distinct_slots_per_round"] > 0 and
                  r["distinct_slots_per_round"] <= max(r["n"], 64),
                  f"MB distinct-slot witness bad: {r['distinct_slots_per_round']}")

    # --- counters distinguish MB1 vs MB3 shapes --------------------------
    shapes = defaultdict(list)
    for r in rows:
        if r["arm"] in ("MB1", "MB3"):
            shapes[(r["arm"], r["op"], r["size"], r["n"])].append(
                r["admission_sections"])
    for (op, size, n) in [(o, s, n) for o in OPS for s in SIZES for n in NS]:
        mb1 = median(shapes.get(("MB1", op, size, n), []))
        mb3 = median(shapes.get(("MB3", op, size, n), []))
        if mb1 and mb3:
            # MB1 = 2 sections/op; MB3 = 1 section/batch (episodes have
            # extra drives, but the ratio must be unmistakably different)
            check(mb1 > mb3 * max(1.0, 0.5 * n),
                  f"admission sections do not distinguish MB1/MB3 at {op}/{size}/{n}")

    # --- M8: MB-region design budget --------------------------------------
    src = BENCH_CPP.read_text()
    mb_region = src.split("[MB-BEGIN]")[1].split("[MB-END]")[0] if "[MB-BEGIN]" in src else ""
    mb_lines = len([l for l in mb_region.splitlines() if l.strip()])
    # word-boundary match; "MiniBatchBackend" (the research class) is legal,
    # a generic "BatchBackend" hierarchy token is not
    forbidden = [t for t in ["OperationStorage", "BatchBackend", "BatchPlanner",
                             "CapabilityRegistry", "BulkRequest"]
                 if re.search(r"(?<![A-Za-z])" + t + r"(?![A-Za-z])", mb_region)]
    check(not forbidden, f"M8 forbidden tokens in MB region: {forbidden}")
    check(mb_lines <= M8_BUDGET_LINES, f"M8 budget exceeded: {mb_lines} > {M8_BUDGET_LINES}")
    minimality = "THIN FLOOR SUFFICIENT"

    # --- medians + materiality --------------------------------------------
    med = cell_medians(rows)

    def gain(arm_a, arm_b, op):
        """median (b − a)/b per (size, n) cell, signed; a faster than b > 0."""
        out = {}
        for size in SIZES:
            for n in NS:
                b = med[(arm_b, op, size, n)]
                a = med[(arm_a, op, size, n)]
                out[(size, n)] = (b - a) / b
        return out

    def material(g):
        """MATERIAL iff >= MATERIAL on >= MATERIAL_CELLS of 6 N-cells per size,
        both sizes, sign consistent (prereg §10)."""
        ok_sizes = 0
        for size in SIZES:
            cells = [g[(size, n)] for n in NS]
            hit = sum(1 for x in cells if x >= MATERIAL)
            if hit >= MATERIAL_CELLS:
                ok_sizes += 1
        return ok_sizes == len(SIZES)

    wrapper = gain("B1", "B2", "read")   # positive = B1 faster (B2 adds wall cost)
    wrapper_w = gain("B1", "B2", "write")
    fusion = {op: gain("MB1", "MB3", op) for op in OPS}
    raw_gap = {op: gain("MB1", "B1", op) for op in OPS}   # substrate anchor

    # --- SEMANTIC GRANT (frozen disposition §6 + S9 witness) --------------
    s9 = semantic["s9_verdict"]
    if s9 == "DIVERGENCE":
        grant = "CURRENT BATCH DOES NOT GRANT GROUP ADMISSION"
    else:
        grant = "SEMANTICALLY AMBIGUOUS — BLOCKED"  # no contract grant either way

    # --- S-9 substrate anchor: a BLOCKING gate, resolved BEFORE value/G1/
    #     promotion (Corrective-1 P1-4). BLOCKED ≠ NOT MATERIAL (§14).
    anchor = raw_gap["read"].get((4096, 8), 0)
    substrate_ok = abs(anchor) <= ANCHOR_BOUND
    blocked_reasons = []
    if not substrate_ok:
        blocked_reasons.append(
            f"S-9 substrate anchor |MB1−B1|/B1 = {anchor:.3f} > {ANCHOR_BOUND}")

    # --- CONTROL ----------------------------------------------------------
    fusion_material = material(fusion["read"]) and material(fusion["write"])
    if not substrate_ok:
        control = "BLOCKED"
    elif fusion_material:
        control = "COST EXISTS BUT FUSION NOT SEMANTICALLY LEGAL" \
            if grant != "GROUP EXECUTION GRANT SUPPORTED" \
            else "MATERIAL AMORTIZATION LEGALLY AVAILABLE"
    else:
        control = "NOT MATERIAL"

    # --- PERFORMANCE: a LEGAL arm materially faster than B1 ----------------
    wrapper_material = material(wrapper) and material(wrapper_w)
    if wrapper_material and med[("B2", "read", 4096, 8)] < med[("B1", "read", 4096, 8)]:
        performance = "MATERIAL"
    else:
        performance = "NOT MATERIAL"

    # --- SLUICE-SPECIFIC-VALUE / G1-CONTROL / PROMOTION (LAST; P1-4) -------
    b0_competitive = all(
        med[("B0", op, size, n)] <= med[("B1", op, size, n)] * 1.05
        for op in OPS for size in SIZES for n in NS if n >= 8)
    value = "NOT ESTABLISHED"
    if blocked_reasons:
        value = "NOT ESTABLISHED"          # blocking gates cap everything
    elif grant == "GROUP EXECUTION GRANT SUPPORTED" and fusion_material and \
            control == "MATERIAL AMORTIZATION LEGALLY AVAILABLE" and not b0_competitive:
        value = "ESTABLISHED"
    elif not fusion_material and not wrapper_material:
        value = "PORTABLE THIN-BASELINE VALUE ONLY" if b0_competitive else "NOT ESTABLISHED"
    g1 = "POSITIVE CANDIDATE" if value == "ESTABLISHED" else "NOT ESTABLISHED"
    promotion = "PROMOTE-CONSIDER" if g1 == "POSITIVE CANDIDATE" else "STOP — NO C1"

    return {
        "cells": {f"{k[0]}|{k[1]}|{k[2]}|{k[3]}": round(v, 1) for k, v in med.items()},
        "wrapper_gain_read": {f"{s}|{n}": round(v, 4) for (s, n), v in wrapper.items()},
        "wrapper_gain_write": {f"{s}|{n}": round(v, 4) for (s, n), v in wrapper_w.items()},
        "fusion_gain_read": {f"{s}|{n}": round(v, 4) for (s, n), v in fusion["read"].items()},
        "fusion_gain_write": {f"{s}|{n}": round(v, 4) for (s, n), v in fusion["write"].items()},
        "substrate_anchor_read_4k_n8": round(anchor, 4),
        "substrate_anchor_ok": substrate_ok,
        "qualification_session": session_qual,
        "aa_gate": aa,
        "substrate": {k: env.get(k) for k in
                      ("work_dir", "filesystem_type", "filesystem_source",
                       "mount_target")},
        "enter_cells": len(enter_cells),
        "transport_topology": transport_detail,
        "blocked_reasons": blocked_reasons,
        "mb_region_lines": mb_lines,
        "verdicts": {
            "BATCH-X0-SEMANTIC-GRANT": grant,
            "BATCH-X0-TRANSPORT-AMORTIZATION": transport,
            "BATCH-X0-CONTROL-AMORTIZATION": control,
            "BATCH-X0-PERFORMANCE": performance,
            "BATCH-X0-MINIMALITY": minimality,
            "BATCH-X0-SLUICE-SPECIFIC-VALUE": value,
            "BATCH-X0-G1-CONTROL": g1,
            "PROMOTION": promotion,
        },
    }


# --- self-test ---------------------------------------------------------------

def _perf_row(arm, op, size, n, rep, base):
    return {"kind": "perf", "arm": arm, "op": op, "size": size, "n": n,
            "rep": rep, "rounds": 10, "ops": 10 * n,
            "wall_ns": base * 10 * n, "wall_per_op_ns": base,
            "cpu_per_op_ns": base, "submits": 10, "drive_episodes": 10,
            "admission_sections": 20 * n if arm == "MB1" else 10,
            "flush_calls": 10, "wait_enters": 10,
            "distinct_slots_per_round": min(n, 64),
            "identity_entries": 10 * n, "work": "ok"}


def _env(commit=None, fs="ext4", qualification="qual-ok", dirty=False):
    return {"dirty_tracked": dirty, "commit": commit or FREEZE,
            "filesystem_type": fs, "work_dir": "/work/ext4-x",
            "filesystem_source": "/dev/xyz", "mount_target": "/mnt/ext4-x",
            "qualification_session": qualification}


def _build_matrix(mdir, base_fn, env, rate_fn=None):
    """Synthetic matrix session. base_fn(arm, op, size, n) → wall_per_op_ns;
    rate_fn(arm, op, size, n) → per-op io_uring_enter rate (default 1.0 for
    B1/B2/MB*, 1/n for B0 — the equivalent-topology shape)."""
    mdir.mkdir(parents=True, exist_ok=True)
    rows = []
    for arm in ARMS:
        for op in OPS:
            for size in SIZES:
                for n in NS:
                    for rep in range(REPS):
                        rows.append(_perf_row(arm, op, size, n, rep,
                                              base_fn(arm, op, size, n)))
    (mdir / "rows.jsonl").write_text("\n".join(json.dumps(r) for r in rows) + "\n")
    (mdir / "environment.json").write_text(json.dumps(env))
    if rate_fn is None:
        def rate_fn(arm, op, size, n):
            return 1.0 / n if arm == "B0" else 1.0
    enters = []
    for arm in ARMS:
        for op in OPS:
            for size in SIZES:
                for n in NS:
                    ops = 10 * n
                    enters.append({"arm": arm, "op": op, "size": size, "n": n,
                                   "rounds": 10, "ops": ops,
                                   "io_uring_enter": round(rate_fn(arm, op, size, n) * ops),
                                   "counter_reps": 1})
    (mdir / "strace-enter-rows.jsonl").write_text(
        "\n".join(json.dumps(r) for r in enters) + "\n")
    return mdir


def _build_qual(qdir, delta_fn=None):
    """Synthetic A/A qualification session: 48-cell grid × REPS × 2 passes;
    delta_fn(arm, op, size, n) → pass-2 fractional shift (default 1%)."""
    qdir.mkdir(parents=True, exist_ok=True)
    (qdir / "environment.json").write_text(json.dumps(_env()))
    for p in (1, 2):
        rows = []
        for arm in ("B1", "B2"):
            for op in OPS:
                for size in SIZES:
                    for n in NS:
                        base = 1000.0
                        if p == 2:
                            d = delta_fn(arm, op, size, n) if delta_fn else 0.01
                            base *= 1.0 + d
                        for rep in range(REPS):
                            rows.append(_perf_row(arm, op, size, n, rep, base))
        (qdir / f"rows-pass{p}.jsonl").write_text(
            "\n".join(json.dumps(r) for r in rows) + "\n")
    return qdir


def _expect_fail(fn, label, bad_counter):
    try:
        fn()
        print(f"SELFTEST FAIL: {label} not rejected")
        return bad_counter + 1
    except Fail as e:
        print(f"SELFTEST OK: {label} rejected ({e})")
        return bad_counter


def selftest():
    """Every test states exactly what it proves (Corrective-1 P2: no
    aggregate 'N/N corrupted evidence rejected' wording)."""
    import shutil
    import tempfile
    tmp = Path(tempfile.mkdtemp())
    bad = 0
    try:
        head = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "HEAD"],
                              capture_output=True, text=True).stdout.strip()
        pre_freeze = "39f9d984e562a6396b58ebbe733d89513dd7242a"  # master before the campaign branch point; NOT a freeze descendant
        semantic = validate_semantic_dir(_build_semantic(tmp / "semantic"))

        # -- valid end-to-end session --------------------------------------
        qdir = _build_qual(tmp / "qual-ok")
        mdir = _build_matrix(tmp / "matrix", lambda *a: 1000.0, _env(commit=head))
        res = validate_matrix(mdir, semantic, qdir)
        v = res["verdicts"]
        assert v["BATCH-X0-SEMANTIC-GRANT"] == "CURRENT BATCH DOES NOT GRANT GROUP ADMISSION", v
        # M6-C: valid EQUIVALENT enter topology → primitive submits suffice
        assert v["BATCH-X0-TRANSPORT-AMORTIZATION"] == "ALREADY OBTAINED BY PRIMITIVE SUBMITS", v
        assert v["BATCH-X0-CONTROL-AMORTIZATION"] == "NOT MATERIAL", v
        assert v["BATCH-X0-PERFORMANCE"] == "NOT MATERIAL", v
        assert v["BATCH-X0-SLUICE-SPECIFIC-VALUE"] == "PORTABLE THIN-BASELINE VALUE ONLY", v
        assert v["PROMOTION"] == "STOP — NO C1", v
        assert res["substrate_anchor_ok"] and not res["blocked_reasons"], res
        print("SELFTEST OK: valid session accepted; M6-C equivalent enter "
              "topology → ALREADY OBTAINED BY PRIMITIVE SUBMITS")
        print(f"  (validator derived verdicts on synthetic evidence: {v['PROMOTION']})")

        # -- M7: identity-witness corruption REJECTED ----------------------
        def m7_corrupt():
            d = _build_matrix(tmp / "m7", lambda *a: 1000.0, _env(commit=head))
            rows = [json.loads(l) for l in (d / "rows.jsonl").read_text().splitlines()]
            for r in rows:
                if r["arm"] == "MB3":
                    r["identity_entries"] = 0
            (d / "rows.jsonl").write_text("\n".join(json.dumps(r) for r in rows) + "\n")
            validate_matrix(d, semantic, qdir)
        bad = _expect_fail(m7_corrupt, "M7 identity-witness corruption", bad)

        # -- M6-A: malformed enter evidence REJECTED -----------------------
        def m6_missing():
            d = _build_matrix(tmp / "m6a", lambda *a: 1000.0, _env(commit=head))
            rows = (d / "strace-enter-rows.jsonl").read_text().splitlines()
            (d / "strace-enter-rows.jsonl").write_text("\n".join(rows[:-1]) + "\n")
            validate_matrix(d, semantic, qdir)
        bad = _expect_fail(m6_missing, "M6-A missing enter-counter cell", bad)

        def m6_duplicate():
            d = _build_matrix(tmp / "m6d", lambda *a: 1000.0, _env(commit=head))
            rows = (d / "strace-enter-rows.jsonl").read_text().splitlines()
            (d / "strace-enter-rows.jsonl").write_text(
                "\n".join(rows + [rows[0]]) + "\n")
            validate_matrix(d, semantic, qdir)
        bad = _expect_fail(m6_duplicate, "M6-A duplicate enter-counter cell", bad)

        def m6_negative():
            d = _build_matrix(tmp / "m6n", lambda *a: 1000.0, _env(commit=head))
            rows = [json.loads(l) for l in (d / "strace-enter-rows.jsonl").read_text().splitlines()]
            rows[0]["io_uring_enter"] = -5
            (d / "strace-enter-rows.jsonl").write_text(
                "\n".join(json.dumps(r) for r in rows) + "\n")
            validate_matrix(d, semantic, qdir)
        bad = _expect_fail(m6_negative, "M6-A negative enter count", bad)

        # -- M6-B: valid evidence with DIFFERENT topology → verdict
        #    recomputed (a different lawful verdict, NOT a rejection) ------
        d = _build_matrix(tmp / "m6b", lambda *a: 1000.0, _env(commit=head),
                          rate_fn=lambda arm, op, size, n:
                          (0.4 if arm == "B2" else 1.0 / n if arm == "B0" else 1.0))
        v2 = validate_matrix(d, semantic, qdir)["verdicts"]
        assert v2["BATCH-X0-TRANSPORT-AMORTIZATION"] == \
            "ADDITIONAL TRANSPORT AMORTIZATION ESTABLISHED", v2
        print("SELFTEST OK: M6-B valid per-cell evidence with B2 at 0.4× "
              "B1 enter rate → transport verdict recomputed to "
              "ADDITIONAL TRANSPORT AMORTIZATION ESTABLISHED")

        # -- conservative branch: equivalence violated without B2 fewer ---
        d = _build_matrix(tmp / "m6e", lambda *a: 1000.0, _env(commit=head),
                          rate_fn=lambda arm, op, size, n:
                          (1.5 if arm == "B2" else 1.0 / n if arm == "B0" else 1.0))
        v3 = validate_matrix(d, semantic, qdir)["verdicts"]
        assert v3["BATCH-X0-TRANSPORT-AMORTIZATION"] == "BLOCKED", v3
        print("SELFTEST OK: enter topology neither equivalent nor B2-fewer "
              "→ transport BLOCKED (fail-closed, not a positive claim)")

        # -- missing perf-grid cell REJECTED --------------------------------
        def missing_cell():
            d = _build_matrix(tmp / "grid", lambda *a: 1000.0, _env(commit=head))
            rows = [json.loads(l) for l in (d / "rows.jsonl").read_text().splitlines()]
            rows = [r for r in rows if not (r["arm"] == "B0" and r["n"] == 32)]
            (d / "rows.jsonl").write_text("\n".join(json.dumps(r) for r in rows) + "\n")
            validate_matrix(d, semantic, qdir)
        bad = _expect_fail(missing_cell, "missing perf-grid cell", bad)

        # -- semantic fixture FAIL REJECTED ---------------------------------
        def fixture_fail():
            s = tmp / "sem2"
            s.mkdir()
            (s / "fixtures.jsonl").write_text(
                json.dumps({"kind": "fixture", "fixture": "S2", "result": "FAIL"}) + "\n")
            validate_semantic_dir(s)
        bad = _expect_fail(fixture_fail, "S2 fixture FAIL", bad)

        # -- A/A qualification gates (P1-2) ---------------------------------
        validate_aa(qdir)  # valid session recomputes to pass
        print("SELFTEST OK: valid A/A qualification recomputed from raw rows")

        def aa_violation():
            _build_qual(tmp / "qual-bad",
                        delta_fn=lambda a, o, s, n: 0.25 if n == 32 else 0.01)
            validate_aa(tmp / "qual-bad")
        bad = _expect_fail(aa_violation,
                           "A/A failure (40/48 cells within 5% < 90%)", bad)

        def aa_missing_pass():
            q = _build_qual(tmp / "qual-miss")
            (q / "rows-pass2.jsonl").unlink()
            validate_aa(q)
        bad = _expect_fail(aa_missing_pass, "A/A missing pass-2 rows", bad)

        def aa_bad_substrate():
            q = _build_qual(tmp / "qual-fs")
            (q / "environment.json").write_text(json.dumps(_env(fs="tmpfs")))
            validate_aa(q)
        bad = _expect_fail(aa_bad_substrate,
                           "qualification session on unverified (tmpfs) substrate", bad)

        def aa_verdict_disagreement():
            q = _build_qual(tmp / "qual-rec")
            (q / "verdicts.json").write_text(json.dumps({"gate": "A/A", "pass": False}))
            validate_aa(q)  # raw rows PASS; recorded verdict says FAIL
        bad = _expect_fail(aa_verdict_disagreement,
                           "recorded A/A verdict contradicting the raw-row recomputation", bad)

        # -- substrate gates (P1-1) ------------------------------------------
        def matrix_tmpfs():
            d = _build_matrix(tmp / "fs1", lambda *a: 1000.0, _env(fs="tmpfs"))
            validate_matrix(d, semantic, qdir)
        bad = _expect_fail(matrix_tmpfs, "formal matrix on tmpfs substrate", bad)

        def matrix_no_fs_record():
            d = _build_matrix(tmp / "fs2", lambda *a: 1000.0,
                              {"dirty_tracked": False, "commit": head})
            validate_matrix(d, semantic, qdir)
        bad = _expect_fail(matrix_no_fs_record,
                           "formal matrix without a filesystem record", bad)

        # -- git ancestry (P1-3): real git semantics -------------------------
        assert is_descendant(FREEZE), "freeze itself must be accepted"
        assert is_descendant(head), "HEAD must be accepted"
        assert not is_descendant(pre_freeze), "pre-freeze ancestor must be rejected"
        assert not is_descendant("deadbeefdeadbeefdeadbeefdeadbeefdeadbeef"), \
            "garbage SHA must be rejected"
        print("SELFTEST OK: git ancestry — freeze/HEAD accepted, pre-freeze "
              "ancestor and garbage SHA rejected (real merge-base check)")

        def matrix_pre_freeze_commit():
            d = _build_matrix(tmp / "anc", lambda *a: 1000.0,
                              _env(commit=pre_freeze))
            validate_matrix(d, semantic, qdir)
        bad = _expect_fail(matrix_pre_freeze_commit,
                           "matrix session pinned to a pre-freeze commit", bad)

        # -- A/A binding mismatch (P1-2) --------------------------------------
        def binding_mismatch():
            d = _build_matrix(tmp / "bind", lambda *a: 1000.0,
                              _env(commit=head, qualification="qual-other"))
            validate_matrix(d, semantic, qdir)
        bad = _expect_fail(binding_mismatch,
                           "qualification_session binding mismatch", bad)

        # -- P1-4: substrate-anchor STOP propagates over favorable evidence --
        # gain(a, b) > 0 ⇔ a is faster. B2 is 10% SLOWER than B1 everywhere
        # (wrapper material, hits fire) EXCEPT read/4K/N=8 where B2 is
        # faster, so PERFORMANCE=MATERIAL; MB1 is 50% slower at read/4K/N=8
        # so the S-9 anchor is far outside the comparability bound.
        def favorable_but_bad_anchor(arm, op, size, n):
            if arm == "B1" or arm == "MB3":
                return 1000.0
            if arm == "MB1" and (op, size, n) == ("read", 4096, 8):
                return 1500.0      # |anchor| = 0.5 > 0.30
            if arm == "B2" and (op, size, n) == ("read", 4096, 8):
                return 900.0       # B2 faster at the anchor cell
            return 1100.0          # B2 10% slower elsewhere (material hits)
        d = _build_matrix(tmp / "p14", favorable_but_bad_anchor,
                          _env(commit=head),
                          rate_fn=lambda arm, op, size, n:
                          (1.0 / n if arm == "B0" else 1.0))
        res4 = validate_matrix(d, semantic, qdir)
        v4 = res4["verdicts"]
        assert not res4["substrate_anchor_ok"], res4
        assert res4["blocked_reasons"], res4
        assert v4["BATCH-X0-CONTROL-AMORTIZATION"] == "BLOCKED", v4
        assert v4["BATCH-X0-PERFORMANCE"] == "MATERIAL", v4   # favorable, yet capped below
        assert v4["BATCH-X0-SLUICE-SPECIFIC-VALUE"] == "NOT ESTABLISHED", v4
        assert v4["BATCH-X0-G1-CONTROL"] == "NOT ESTABLISHED", v4
        assert v4["PROMOTION"] == "STOP — NO C1", v4
        print("SELFTEST OK: P1-4 substrate-anchor failure → CONTROL=BLOCKED, "
              "G1=NOT ESTABLISHED, PROMOTION=STOP even with otherwise "
              "favorable (MATERIAL) synthetic measurements")

        # -- Corrective-2: final validation status follows blocked_reasons --
        # The verdict artifact above (CONTROL=BLOCKED / PROMOTION=STOP) is a
        # legitimate derived record for VALID evidence; the final CLI status
        # must distinguish it from PASS without turning it into a Fail.
        assert not res["blocked_reasons"], res
        assert validation_status(res) == "PASS", res
        assert res4["blocked_reasons"], res4
        assert validation_status(res4) == "BLOCKED" \
            and validation_status(res4) != "PASS", res4
        print("SELFTEST OK: final validation status — structurally valid "
              "session → PASS; anchor-blocked valid session → BLOCKED "
              "(never PASS)")
    finally:
        shutil.rmtree(tmp)
    if bad:
        print(f"SELFTEST RESULT: {bad} test(s) FAILED")
        return 1
    print("SELFTEST RESULT: all tests passed")
    return 0


def validation_status(res):
    """Final CLI status (Corrective-2). blocked_reasons is the explicit
    experiment-level gate authority: nonempty means the evidence is
    structurally valid and fully derived, but a preregistered
    qualification gate (S-9 substrate anchor) stops the downstream
    claims — the session reports VALIDATION BLOCKED, never
    VALIDATION PASS. Domain verdicts alone (e.g. TRANSPORT=BLOCKED from
    valid measurements) do not decide the status."""
    return "BLOCKED" if res["blocked_reasons"] else "PASS"


def _build_semantic(sdir):
    sdir.mkdir(parents=True, exist_ok=True)
    (sdir / "fixtures.jsonl").write_text(
        '\n'.join(json.dumps({"kind": "fixture", "fixture": f"S{i}",
                              "result": "PASS"}) for i in range(1, 11)) + "\n" +
        json.dumps({"kind": "fixture", "fixture": "S9", "verdict": "DIVERGENCE"}) + "\n")
    return sdir


def main():
    if len(sys.argv) == 2 and sys.argv[1] == "--selftest":
        return selftest()
    ap = argparse.ArgumentParser()
    ap.add_argument("matrix_dir", type=Path)
    ap.add_argument("--qualification", type=Path, required=True,
                    help="qualification session the matrix was admitted by "
                         "(A/A is recomputed from its raw rows; the session's "
                         "qualification_session field must name this dir)")
    args = ap.parse_args()
    mdir = args.matrix_dir
    try:
        check((mdir / "rows.jsonl").exists(), f"no rows.jsonl in {mdir}")
        sem_root = mdir.parent
        sem_dirs = sorted(sem_root.glob("batch-x0-semantic-*"))
        check(sem_dirs, "no semantic results dir found beside the matrix dir")
        print(f"semantic evidence: {sem_dirs[-1].name}")
        semantic = validate_semantic_dir(sem_dirs[-1])
        res = validate_matrix(mdir, semantic, args.qualification)
    except Fail as e:
        print(f"VALIDATION FAIL: {e}", file=sys.stderr)
        return 1
    print(json.dumps(res["verdicts"], indent=2, ensure_ascii=False))
    print(f"aa_gate: {res['aa_gate']['within_5pct']}/{res['aa_gate']['cells']} "
          f"cells within 5% ({res['aa_gate']['fraction']:.1%})")
    print(f"substrate: {res['substrate']}")
    print(f"enter cells: {res['enter_cells']}; blocked_reasons: {res['blocked_reasons']}")
    (mdir / "verdicts.json").write_text(
        json.dumps(res, indent=2, ensure_ascii=False))
    if validation_status(res) == "BLOCKED":
        print("VALIDATION BLOCKED")
        return 2
    print("VALIDATION PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
