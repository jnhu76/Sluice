#!/usr/bin/env python3
"""#255 TAX-0 router-fix shootout campaign validator (fail-closed).

Validates the official campaign artifacts (kinds `tax0routermicro` and
`tax0routershootout`, produced by scripts/bench/perf-attribution.py) and
MECHANICALLY recomputes every derived quantity the fix-selection report
relies on:

  - seed-derived execution order (must equal the recorded rounds AND the
    actual row sequence, cell-by-cell);
  - SEALED matrix completeness: the validator embeds the externally
    frozen campaign matrix (candidates, patterns, geometries, windows,
    request size, total bytes, reps, warmup, seed, exact session set) and
    requires each artifact's declared cell set to EQUAL it — deleting a
    cell, a candidate, a geometry, or a whole session fails closed even
    when rows and derived values are tampered consistently;
  - same-work (ops/bytes/word-sums identical across candidates within each
    session);
  - semantic_validation flags (fail-closed: any False fails the campaign);
  - per-cell medians and normalized-vs-r0 ratios for BOTH axes
    (instructions AND cycles), each recomputed and cross-checked against
    the recorded derived values;
  - campaign-aggregate geometric means (overall + per fs + per op),
    worst regressions, +5% guardrail eligibility;
  - the pairwise practical-tie relation (2% band, both GM axes and both
    worst-cell tails);
  - the frozen §25 winner selection (mechanical; the human report may
    only quote it, not re-derive it differently).

Mutation testing: `--self-test` applies each documented tamper (candidate
label tampering, row removal, order tampering, normalized-metric tampering,
GM tampering — instructions AND cycles, worst-regression tampering,
semantic-validity tampering, whole-cell deletion with consistent derived
resync, whole-candidate deletion, whole-session deletion, unknown session)
to synthetic fixtures and asserts the validator FAILS on every one. The
validator never repairs; it only accepts or exits non-zero.

Usage:
  tax0router-validate.py --micro micro.json --shootout a.json [b.json ...]
  tax0router-validate.py --self-test
"""
from __future__ import annotations

import argparse
import copy
import json
import statistics
import sys
from pathlib import Path

GUARDRAIL = 0.05
TIE = 0.02
SIMPLICITY = {"r1": 0, "r2": 1, "r3": 2}
CANDIDATES = ["r0", "r1", "r2", "r3"]
MICRO_KIND = "tax0routermicro"
SHOOTOUT_KIND = "tax0routershootout"

# Externally frozen campaign matrix (SHOOTOUT-FREEZE d45f620; recorded
# verbatim in the official artifacts' params). The validator holds these
# as INDEPENDENT facts and forces every artifact to match — an artifact
# cannot redefine its own matrix.
FROZEN_MICRO = {
    "experiment": "TAX-0-ROUTER-SHOOTOUT-A",
    "candidates": ["r0", "r1", "r2", "r3"],
    "patterns": ["P0", "P1", "P2"],
    "geometries": ["D=8,C=8", "D=8,C=32", "D=8,C=128", "D=8,C=512",
                   "D=32,C=32", "D=32,C=128", "D=32,C=512",
                   "D=128,C=128", "D=128,C=512"],
    "windows": 20000,
    "reps": 9,
    "seed": 0x52545253,  # == 1381257811 ("RTRS")
}
FROZEN_SHOOTOUT = {
    "experiment": "TAX-0-ROUTER-SHOOTOUT-B",
    "candidates": ["r0", "r1", "r2", "r3"],
    "geometries": ["D=8,C=8", "D=8,C=32", "D=8,C=128", "D=8,C=512",
                   "D=32,C=32", "D=32,C=128", "D=32,C=512"],
    "request_size": 4096,
    "total_bytes": 134217728,  # 128 MiB per process/cell
    "reps": 9,
    "warmup_rounds": 2,
    "seed": 0x52545253,
}
# The campaign is exactly these four fs x op sessions — no more, no fewer.
FROZEN_SESSIONS = [("read", "tmpfs"), ("write", "tmpfs"),
                   ("read", "btrfs"), ("write", "btrfs")]
# Domain-level frozen facts independent of any spec override.
ALLOWED_OPS = ("read", "write")
ALLOWED_FS = ("tmpfs", "btrfs")


class Invalid(Exception):
    pass


def _med(vals: list[float]) -> float:
    return statistics.median(vals)


def _gm(vals: list[float]) -> float:
    prod = 1.0
    for v in vals:
        prod *= v
    return prod ** (1.0 / len(vals))


def _rng_order(cells: list[tuple], reps: int, seed: int) -> list[list[tuple]]:
    import random
    rng = random.Random(seed)
    norm = sorted(cells)
    return [rng.sample(norm, k=len(norm)) for _ in range(reps)]


def _check_tracked(cond: bool, msg: str, errs: list[str]) -> None:
    if not cond:
        errs.append(msg)


def _parse_cell(s: str) -> tuple:
    cand, rest = s.split("|")
    d, c = rest[2:].split(",C=")
    return (cand, int(d), int(c))


def _expected_micro_cells(spec: dict) -> list[tuple]:
    """Mirror of the runner's tax0routermicro_cells over the frozen spec."""
    cells = []
    for pat in spec["patterns"]:
        for g in spec["geometries"]:
            _, d, c = _parse_cell(f"x|{g}")
            if pat == "P2" and d != c:
                continue  # P2 is the full-occupancy pattern: D == C only
            for cand in spec["candidates"]:
                cells.append((cand, pat, d, c))
    return sorted(cells)


def _check_micro_params(art: dict, spec: dict, errs: list[str]) -> None:
    p = art.get("params") or {}
    for key, want in (("experiment", spec["experiment"]),
                      ("candidates", spec["candidates"]),
                      ("patterns", spec["patterns"]),
                      ("geometries", spec["geometries"]),
                      ("windows", spec["windows"]),
                      ("reps", spec["reps"]),
                      ("seed", spec["seed"])):
        got = p.get(key)
        if got != want:
            errs.append(f"micro params.{key}: sealed matrix violation "
                        f"(frozen {want!r}, artifact {got!r})")


def _check_shootout_params(art: dict, spec: dict, errs: list[str]) -> None:
    p = art.get("params") or {}
    for key, want in (("experiment", spec["experiment"]),
                      ("candidates", spec["candidates"]),
                      ("geometries", spec["geometries"]),
                      ("request_size", spec["request_size"]),
                      ("total_bytes", spec["total_bytes"]),
                      ("reps", spec["reps"]),
                      ("warmup_rounds", spec["warmup_rounds"]),
                      ("seed", spec["seed"])):
        got = p.get(key)
        if got != want:
            errs.append(f"shootout params.{key}: sealed matrix violation "
                        f"(frozen {want!r}, artifact {got!r})")
    if p.get("op") not in ALLOWED_OPS:
        errs.append(f"shootout params.op: {p.get('op')!r} not in "
                    f"{ALLOWED_OPS}")
    if p.get("fs_label") not in ALLOWED_FS:
        errs.append(f"shootout params.fs_label: {p.get('fs_label')!r} "
                    f"not in {ALLOWED_FS}")


def validate_micro(art: dict, errs: list[str],
                   spec: dict | None = None) -> dict:
    spec = spec or FROZEN_MICRO
    if art.get("kind") != MICRO_KIND:
        errs.append(f"micro artifact kind is {art.get('kind')!r}")
        return {}
    rows = art.get("rows") or []
    params = art.get("params") or {}
    if not rows:
        errs.append("micro artifact has no rows")
        return {}
    seed = params.get("seed")
    reps = params.get("reps")
    if "execution_order" not in art or "cells" not in art["execution_order"]:
        errs.append("micro: missing execution_order.cells")
        return {}
    _check_micro_params(art, spec, errs)
    # SEALED matrix: the declared cell set must EQUAL the frozen set —
    # missing AND extra cells both fail.
    cells_spec = art["execution_order"]["cells"]
    declared: list[tuple] = []
    for s in cells_spec:
        try:
            cand, pat, dc = s.split("|")
            d, c = dc[2:].split(",C=")
            declared.append((cand, pat, int(d), int(c)))
        except (ValueError, IndexError):
            errs.append(f"micro: unparsable cell {s!r}")
            return {}
    expected = _expected_micro_cells(spec)
    missing = sorted(set(expected) - set(declared))
    extra = sorted(set(declared) - set(expected))
    for cell in missing:
        errs.append(f"micro: SEALED matrix cell absent: {cell}")
    for cell in extra:
        errs.append(f"micro: SEALED matrix extra cell: {cell}")
    if missing or extra:
        return {}
    # Recompute the seed-derived order over the (now sealed) cell set.
    cells = declared
    want = _rng_order(cells, reps, seed)
    got = art["execution_order"]["rounds"]
    if [[f"{a}|{b}|D={d},C={c}" for a, b, d, c in r] for r in want] != got:
        errs.append("micro: seed-derived execution order != recorded rounds")
    # Row sequence must follow the recorded rounds exactly.
    seq = [f"{r['candidate']}|{r['pattern']}|D={r['depth']},"
           f"C={r['request_capacity']}" for r in rows]
    flat = [c for rnd in got for c in rnd]
    if seq != flat:
        errs.append("micro: row sequence != recorded execution order")
    # Rep count per cell (the sealed set already guarantees the cell list;
    # this pins the rep count on the row side).
    counts: dict[str, int] = {}
    for s in seq:
        counts[s] = counts.get(s, 0) + 1
    if len(counts) != len(cells):
        errs.append(f"micro: row-side cell count {len(counts)} != sealed "
                    f"{len(cells)}")
    for s, n in counts.items():
        if n != reps:
            errs.append(f"micro: cell {s} has {n} reps (want {reps})")
    # Same-work / semantic flags / structural gates.
    for i, r in enumerate(rows):
        _check_tracked(r.get("semantic_validation") is True,
                       f"micro row {i}: semantic_validation not True", errs)
        _check_tracked(r.get("same_work") is True,
                       f"micro row {i}: same_work not True", errs)
        _check_tracked(r.get("steady_allocations_during_trace") == 0,
                       f"micro row {i}: steady-state allocation observed "
                       f"during trace", errs)
        _check_tracked(r.get("steady_allocations_per_op") == 0,
                       f"micro row {i}: steady allocations != 0", errs)
        _check_tracked(r.get("ops") == r.get("lifecycle_ops"),
                       f"micro row {i}: ops != lifecycle_ops", errs)
        if r["candidate"] == "r3":
            _check_tracked(r.get("table_insert_probes_total", 0) > 0
                           and r.get("table_erase_probes_total", 0) > 0,
                           f"micro row {i}: r3 without table accounting", errs)
        else:
            _check_tracked(r.get("table_insert_probes_total") == 0
                           and r.get("table_erase_probes_total") == 0,
                           f"micro row {i}: non-r3 touched the table", errs)
    # Same ops within a (pattern, D, C) across candidates.
    ops_by_cell: dict[tuple, set] = {}
    for r in rows:
        key = (r["pattern"], r["depth"], r["request_capacity"])
        ops_by_cell.setdefault(key, set()).add(r["ops"])
    for key, v in ops_by_cell.items():
        if len(v) != 1:
            errs.append(f"micro: same-work violation at {key}: ops {v}")
    # Recompute medians + normalized + GM for BOTH axes.
    by_cell: dict[tuple, list[dict]] = {}
    for r in rows:
        by_cell.setdefault((r["candidate"], r["pattern"], r["depth"],
                            r["request_capacity"]), []).append(r)
    med_instr: dict[tuple, float] = {}
    med_cycles: dict[tuple, float] = {}
    for cell, rs in by_cell.items():
        vals_i = [r["instructions_user"] / r["ops"] for r in rs
                  if r.get("instructions_user")]
        vals_c = [r["cycles_user"] / r["ops"] for r in rs
                  if r.get("cycles_user")]
        if len(vals_i) != len(rs) or len(vals_c) != len(rs):
            errs.append(f"micro: cell {cell} rows missing counters")
        else:
            med_instr[cell] = _med(vals_i)
            med_cycles[cell] = _med(vals_c)
    normalized: dict[str, dict[str, float]] = {}
    for cell, v in med_instr.items():
        cand, pat, d, c = cell
        base_i = med_instr.get(("r0", pat, d, c))
        base_c = med_cycles.get(("r0", pat, d, c))
        if cand != "r0" and base_i:
            entry: dict = {"instr": v / base_i}
            if base_c:
                entry["cycles"] = med_cycles[cell] / base_c
            normalized[f"{cand}|{pat}|D={d},C={c}"] = entry
    gm: dict[str, dict[str, float]] = {}
    for cand in sorted({r["candidate"] for r in rows}):
        ri = [v["instr"] for k, v in normalized.items()
              if k.startswith(cand + "|")]
        rc = [v["cycles"] for k, v in normalized.items()
              if k.startswith(cand + "|") and "cycles" in v]
        if ri:
            gm[cand] = {"gm_instr": _gm(ri), "n_cells": len(ri)}
            if rc:
                gm[cand]["gm_cycles"] = _gm(rc)
    # Recorded derived must match the recomputation (both axes).
    rec_gm = (art.get("derived") or {}).get("gm_per_candidate") or {}
    for cand, d in gm.items():
        rec = (rec_gm.get(cand) or {}).get("gm_instr")
        v = d["gm_instr"]
        if rec is None or abs(rec - v) > 1e-9:
            errs.append(f"micro: recorded GM_instr {cand} tampered "
                        f"(recorded {rec}, recomputed {v})")
        if "gm_cycles" in d:
            rec_c = (rec_gm.get(cand) or {}).get("gm_cycles")
            if rec_c is None or abs(rec_c - d["gm_cycles"]) > 1e-9:
                errs.append(f"micro: recorded GM_cycles {cand} tampered "
                            f"(recorded {rec_c}, recomputed "
                            f"{d['gm_cycles']})")
    rec_norm = (art.get("derived") or {}).get("normalized_vs_r0") or {}
    for k, v in normalized.items():
        rec = (rec_norm.get(k) or {}).get("instr")
        if rec is None or abs(rec - v["instr"]) > 1e-9:
            errs.append(f"micro: recorded normalized {k} tampered "
                        f"(recorded {rec}, recomputed {v['instr']})")
        if "cycles" in v:
            rec_c = (rec_norm.get(k) or {}).get("cycles")
            if rec_c is None or abs(rec_c - v["cycles"]) > 1e-9:
                errs.append(f"micro: recorded normalized cycles {k} tampered "
                            f"(recorded {rec_c}, recomputed {v['cycles']})")
    return {"med_instr": med_instr, "med_cycles": med_cycles,
            "normalized": normalized, "gm": gm, "cells": by_cell}


def validate_shootout(art: dict, errs: list[str],
                      spec: dict | None = None) -> dict:
    spec = spec or FROZEN_SHOOTOUT
    if art.get("kind") != SHOOTOUT_KIND:
        errs.append(f"shootout artifact kind is {art.get('kind')!r}")
        return {}
    rows = art.get("rows") or []
    params = art.get("params") or {}
    if not rows:
        errs.append("shootout artifact has no rows")
        return {}
    seed, reps = params.get("seed"), params.get("reps")
    if "execution_order" not in art or "cells" not in art["execution_order"]:
        errs.append("shootout: missing execution_order.cells")
        return {}
    _check_shootout_params(art, spec, errs)
    # SEALED matrix: declared cell set must EQUAL the frozen set.
    cells_spec = art["execution_order"]["cells"]
    declared: list[tuple] = []
    for s in cells_spec:
        try:
            cand, dc = s.split("|")
            d, c = dc[2:].split(",C=")
            declared.append((cand, int(d), int(c)))
        except (ValueError, IndexError):
            errs.append(f"shootout: unparsable cell {s!r}")
            return {}
    expected: list[tuple] = []
    for g in spec["geometries"]:
        _, d, c = _parse_cell(f"x|{g}")
        for cand in spec["candidates"]:
            expected.append((cand, d, c))
    expected.sort()
    missing = sorted(set(expected) - set(declared))
    extra = sorted(set(declared) - set(expected))
    for cell in missing:
        errs.append(f"shootout: SEALED matrix cell absent: {cell}")
    for cell in extra:
        errs.append(f"shootout: SEALED matrix extra cell: {cell}")
    if missing or extra:
        return {}
    cells = declared
    want = _rng_order(cells, reps, seed)
    got = art["execution_order"]["rounds"]
    if [[f"{a}|D={d},C={c}" for a, d, c in r] for r in want] != got:
        errs.append("shootout: seed-derived execution order != recorded "
                    "rounds")
    seq = [f"{r['candidate']}|D={r['active_depth']},"
           f"C={r['request_capacity']}" for r in rows]
    flat = [c for rnd in got for c in rnd]
    if seq != flat:
        errs.append("shootout: row sequence != recorded execution order")
    counts: dict[str, int] = {}
    for s in seq:
        counts[s] = counts.get(s, 0) + 1
    if len(counts) != len(cells):
        errs.append(f"shootout: row-side cell count {len(counts)} != sealed "
                    f"{len(cells)}")
    for s, n in counts.items():
        if n != reps:
            errs.append(f"shootout: cell {s} has {n} reps (want {reps})")
    for i, r in enumerate(rows):
        _check_tracked(r.get("semantic_validation") is True,
                       f"shootout row {i}: semantic_validation not True",
                       errs)
        _check_tracked(r.get("same_work") is True,
                       f"shootout row {i}: same_work not True", errs)
        _check_tracked(r.get("real_uring") is True,
                       f"shootout row {i}: not a real uring row", errs)
        _check_tracked(r.get("uring_queue_depth") == r.get("active_depth"),
                       f"shootout row {i}: Q != D (frozen primary matrix)",
                       errs)
        _check_tracked(r.get("op_cookie_lookup_calls") == r.get("ops"),
                       f"shootout row {i}: lookup calls != ops", errs)
        _check_tracked(r.get("lookup_hits") == r.get("ops")
                       and r.get("lookup_misses") == 0,
                       f"shootout row {i}: unexpected misses", errs)
        _check_tracked(r.get("control_cookie_lookup_calls") == 0
                       and r.get("transport_cookie_lookup_calls") == 0,
                       f"shootout row {i}: control/transport contamination",
                       errs)
        if r["candidate"] == "r3":
            _check_tracked(r.get("table_insert_calls") == r.get("ops")
                           and r.get("table_erase_calls") == r.get("ops"),
                           f"shootout row {i}: r3 table accounting", errs)
        else:
            _check_tracked(r.get("table_insert_calls") == 0
                           and r.get("table_erase_calls") == 0,
                           f"shootout row {i}: non-r3 touched the table",
                           errs)
    # Same-work across candidates within the session.
    ops_set = {r["ops"] for r in rows}
    if len(ops_set) != 1:
        errs.append(f"shootout: same-work violation (ops {ops_set})")
    ws = {r["word_sum"] for r in rows if r.get("word_sum") is not None}
    if len(ws) > 1:
        errs.append(f"shootout: word_sum mismatch {ws}")
    # Per-cell medians.
    by_cell: dict[tuple, list[dict]] = {}
    for r in rows:
        by_cell.setdefault((r["candidate"], r["active_depth"],
                            r["request_capacity"]), []).append(r)
    med_instr: dict[tuple, float] = {}
    med_cycles: dict[tuple, float] = {}
    for cell, rs in by_cell.items():
        vi = [r["instructions_user"] / r["ops"] for r in rs
              if r.get("instructions_user")]
        vc = [r["cycles_user"] / r["ops"] for r in rs
              if r.get("cycles_user")]
        if len(vi) != len(rs) or len(vc) != len(rs):
            errs.append(f"shootout: cell {cell} rows missing counters")
            continue
        med_instr[cell] = _med(vi)
        med_cycles[cell] = _med(vc)
    # Recompute the recorded derived envelope (normalized ratios, GMs,
    # worst regressions) and cross-check — tampered derived values fail.
    rec_env = (art.get("derived") or {}).get("envelope_vs_r0") or {}
    for cand in sorted({r["candidate"] for r in rows}):
        if cand == "r0":
            continue
        ni, nc = [], []
        for (cd, d, c), mi in med_instr.items():
            if cd != cand:
                continue
            base_i = med_instr.get(("r0", d, c))
            base_c = med_cycles.get(("r0", d, c))
            if not base_i or not base_c:
                errs.append(f"shootout: missing r0 baseline for D={d} C={c}")
                continue
            ni.append(mi / base_i)
            nc.append(med_cycles[(cd, d, c)] / base_c)
        rec = rec_env.get(cand) or {}
        if not ni or not nc:
            if rec:
                errs.append(f"shootout: derived envelope has {cand} with no "
                            f"recomputable cells")
            continue
        for key, v in (("gm_instr", _gm(ni)), ("gm_cycles", _gm(nc)),
                       ("worst_cell_instr", max(ni)),
                       ("worst_cell_cycles", max(nc))):
            rv = rec.get(key)
            if rv is None or abs(rv - v) > 1e-9:
                errs.append(f"shootout: recorded {cand}.{key} tampered "
                            f"(recorded {rv}, recomputed {v})")
    return {"med_instr": med_instr, "med_cycles": med_cycles,
            "cells": by_cell,
            "fs": params.get("fs_label"), "op": params.get("op"),
            "candidates": params.get("candidates")}


def campaign_aggregate(micro_v: dict, shootouts: list[dict],
                       errs: list[str],
                       sessions: list[tuple] | None = None) -> dict:
    """Campaign-level envelope across ALL shootout sessions (production
    selection authority) + the frozen §25 mechanical winner.

    The session SET itself is sealed: it must equal the frozen four
    (fs x op) sessions exactly — a dropped session, a duplicated session,
    or an unknown session fails closed."""
    frozen = sessions if sessions is not None else FROZEN_SESSIONS
    got_sessions = sorted((sv["op"], sv["fs"]) for sv in shootouts
                          if sv.get("op") and sv.get("fs"))
    for s in sorted(frozen):
        if s not in got_sessions:
            errs.append(f"campaign: SEALED session absent: {s[0]}/{s[1]}")
    for s in got_sessions:
        if s not in sorted(frozen):
            errs.append(f"campaign: unknown session (not in frozen set): "
                        f"{s[0]}/{s[1]}")
    all_med_i: dict[tuple, float] = {}
    all_med_c: dict[tuple, float] = {}
    # Sessions share the SAME (cand, D, C) grid by design — the fs/op
    # dimension is what distinguishes them — so the global cell key is
    # (cand, D, C, fs, op). A repeated 5-tuple means the same session
    # was passed twice (fail closed).
    for sv in shootouts:
        fs, op = sv["fs"], sv["op"]
        for (cd, d, c), v in sv["med_instr"].items():
            key = (cd, d, c, fs, op)
            if key in all_med_i:
                errs.append(f"campaign: duplicate session cell {key}")
            all_med_i[key] = v
            all_med_c[key] = sv["med_cycles"][(cd, d, c)]
    candidates = sorted({k[0] for k in all_med_i})
    if "r0" not in candidates:
        errs.append("campaign: no r0 baseline rows")

    env: dict[str, dict] = {}
    per_fs: dict[str, dict[str, list[float]]] = {}
    per_op: dict[str, dict[str, list[float]]] = {}
    for cand in candidates:
        if cand == "r0":
            continue
        ni, nc = [], []
        for (cd, d, c, fs, op), mi in all_med_i.items():
            if cd != cand:
                continue
            base_i = all_med_i.get(("r0", d, c, fs, op))
            base_c = all_med_c.get(("r0", d, c, fs, op))
            if not base_i or not base_c:
                errs.append(f"campaign: missing r0 baseline for "
                            f"D={d} C={c} {fs}|{op}")
                continue
            ri, rc = mi / base_i, all_med_c[(cd, d, c, fs, op)] / base_c
            ni.append(ri)
            nc.append(rc)
            per_fs.setdefault(fs, {}).setdefault(cand, []).append(ri)
            per_op.setdefault(op, {}).setdefault(cand, []).append(ri)
        env[cand] = {
            "n_cells": len(ni),
            "gm_instr": _gm(ni) if ni else None,
            "gm_cycles": _gm(nc) if nc else None,
            "worst_cell_instr": max(ni) if ni else None,
            "worst_cell_cycles": max(nc) if nc else None,
            "guardrail_pass": bool(ni) and bool(nc)
            and max(ni) <= 1 + GUARDRAIL and max(nc) <= 1 + GUARDRAIL,
        }
    gm_fs = {fs: {cand: _gm(v) for cand, v in cands.items() if v}
             for fs, cands in per_fs.items()}
    gm_op = {op: {cand: _gm(v) for cand, v in cands.items() if v}
             for op, cands in per_op.items()}

    # Frozen §25 mechanical selection.
    eligible = [c for c, e in env.items() if e["guardrail_pass"]]
    verdict = None
    selected = None
    tie_set: list[str] = []
    beats = [c for c in eligible
             if env[c]["gm_instr"] is not None
             and env[c]["gm_instr"] <= 1 - TIE]
    if not eligible or not beats:
        verdict = ("ROUTER SHOOTOUT PASS - NO CANDIDATE ROBUSTLY BEATS "
                   "BASELINE")
    else:
        best = min(eligible, key=lambda c: env[c]["gm_instr"])
        others = [c for c in eligible if c != best]
        best_cycles = min(eligible, key=lambda c: env[c]["gm_cycles"])
        leads = all(env[best]["gm_instr"] <=
                    env[o]["gm_instr"] - TIE for o in others if o in SIMPLICITY)
        # Direction fixed per corrective review 5063072823: "worse" is a
        # LARGER ratio, so the instruction winner must be no more than the
        # tie band above the cycles winner (the old `>= min - TIE` form was
        # satisfied by almost anything).
        cycles_ok = env[best]["gm_cycles"] <= \
            env[best_cycles]["gm_cycles"] + TIE
        # Tail symmetry: the instruction winner must not carry a worst-cell
        # regression worse than any other candidate by more than the tie
        # band — on EITHER axis (instructions AND cycles).
        worst_ok = all(
            env[best]["worst_cell_instr"] <=
            env[o]["worst_cell_instr"] + TIE
            and env[best]["worst_cell_cycles"] <=
            env[o]["worst_cell_cycles"] + TIE
            for o in others if o in SIMPLICITY)
        if leads and cycles_ok and worst_ok:
            selected, verdict = best, f"ROUTER SHOOTOUT PASS - " \
                                      f"{best.upper()} SELECTED"
        else:
            # Practical-tie set: within the 2% band of the GM leader on
            # both axes and no >2pp worst-regression disadvantage on
            # either axis.
            tie_set = [c for c in eligible
                       if abs(env[c]["gm_instr"] -
                              env[best]["gm_instr"]) < TIE
                       and abs(env[c]["gm_cycles"] -
                               env[best]["gm_cycles"]) < TIE
                       and env[c]["worst_cell_instr"] <=
                       env[best]["worst_cell_instr"] + TIE
                       and env[c]["worst_cell_cycles"] <=
                       env[best]["worst_cell_cycles"] + TIE]
            if len(tie_set) <= 1:
                tie_set = [best]
            selected = min(tie_set, key=lambda c: SIMPLICITY.get(c, 99))
            verdict = ("ROUTER SHOOTOUT PASS - PRACTICAL TIE, SIMPLEST "
                       f"CANDIDATE SELECTED ({selected})")
    return {"envelope": env, "gm_by_fs": gm_fs, "gm_by_op": gm_op,
            "eligible": eligible, "tie_set": tie_set,
            "selected": selected, "verdict": verdict,
            "rules": {"guardrail": GUARDRAIL, "tie": TIE,
                      "simplicity_order": SIMPLICITY}}


# ---------------------------------------------------------------------------
# Mutation self-test (every documented tamper must FAIL validation).
# ---------------------------------------------------------------------------

# Tiny specs for the self-test fixtures: the validators take the sealed
# spec as a parameter (the official CLI passes nothing and gets the frozen
# campaign matrix), so the self-test proves the sealing MECHANISM against
# fixtures that deliberately deviate from the frozen campaign sizes.
SELF_MICRO_SPEC = {
    "experiment": "TAX-0-ROUTER-SHOOTOUT-A",
    "candidates": ["r0", "r1", "r3"],
    "patterns": ["P0", "P1"],
    "geometries": ["D=8,C=8", "D=8,C=32"],
    "windows": 20000,
    "reps": 2,
    "seed": 0x52545253,
}
SELF_SHOOT_SPEC = {
    "experiment": "TAX-0-ROUTER-SHOOTOUT-B",
    "candidates": ["r0", "r1"],
    "geometries": ["D=8,C=8", "D=8,C=32"],
    "request_size": 4096,
    "total_bytes": 134217728,
    "reps": 2,
    "warmup_rounds": 2,
    "seed": 0x52545253,
}
SELF_SESSIONS = [("read", "tmpfs"), ("write", "btrfs")]


def _fixture() -> dict:
    """Tiny but structurally valid shootout artifact (synthetic numbers)."""
    cands = list(SELF_SHOOT_SPEC["candidates"])
    geo = [(8, 8), (8, 32)]
    reps = SELF_SHOOT_SPEC["reps"]
    seed = SELF_SHOOT_SPEC["seed"]
    cells = sorted((c, d, cc) for c in cands for d, cc in geo)
    rounds = _rng_order(cells, reps, seed)
    rows = []
    base_instr = {(8, 8): 5000.0, (8, 32): 6000.0}
    for rnd in rounds:
        for (cand, d, cc) in rnd:
            f = 1.0 if cand == "r0" else (0.95 if cc == 8 else 0.90)
            rows.append({
                "candidate": cand, "pattern": None, "depth": d,
                "active_depth": d, "request_capacity": cc,
                "ops": 100, "lifecycle_ops": 100, "rep": 0,
                "instructions_user": base_instr[(d, cc)] * f * 100,
                "cycles_user": base_instr[(d, cc)] * f * 100 * 0.9,
                "semantic_validation": True, "same_work": True,
                "real_uring": True, "uring_queue_depth": d,
                "op_cookie_lookup_calls": 100, "lookup_hits": 100,
                "lookup_misses": 0, "control_cookie_lookup_calls": 0,
                "transport_cookie_lookup_calls": 0,
                "table_insert_calls": 0, "table_erase_calls": 0,
                "word_sum": 42,
            })
    art = {
        "kind": SHOOTOUT_KIND,
        "params": {"experiment": SELF_SHOOT_SPEC["experiment"],
                   "seed": seed, "reps": reps, "fs_label": "tmpfs",
                   "op": "read", "candidates": cands,
                   "geometries": list(SELF_SHOOT_SPEC["geometries"]),
                   "request_size": SELF_SHOOT_SPEC["request_size"],
                   "total_bytes": SELF_SHOOT_SPEC["total_bytes"],
                   "warmup_rounds": SELF_SHOOT_SPEC["warmup_rounds"]},
        "execution_order": {
            "cells": [f"{c}|D={d},C={cc}" for c, d, cc in cells],
            "rounds": [[f"{c}|D={d},C={cc}" for c, d, cc in rnd]
                       for rnd in rounds],
        },
        "rows": rows,
        "derived": {},
    }
    _embed_shootout_envelope(art)
    return art


def _embed_shootout_envelope(art: dict) -> None:
    """Recompute and embed the CORRECT derived envelope from the artifact's
    rows (standalone math, independent of the sealed validator — the
    drop-cell mutations produce artifacts the validator refuses before the
    derived stage). Real artifacts always carry this block; the mutations
    then tamper it, and absence itself is invalid."""
    by_cell: dict[tuple, list[dict]] = {}
    for r in art["rows"]:
        by_cell.setdefault((r["candidate"], r["active_depth"],
                            r["request_capacity"]), []).append(r)
    med_i = {k: _med([r["instructions_user"] / r["ops"] for r in rs])
             for k, rs in by_cell.items()}
    med_c = {k: _med([r["cycles_user"] / r["ops"] for r in rs])
             for k, rs in by_cell.items()}
    ratios: dict[str, tuple[list, list]] = {}
    for (cd, d, c) in by_cell:
        if cd == "r0":
            continue
        ni = med_i[(cd, d, c)] / med_i[("r0", d, c)]
        nc = med_c[(cd, d, c)] / med_c[("r0", d, c)]
        ni_l, nc_l = ratios.setdefault(cd, ([], []))
        ni_l.append(ni)
        nc_l.append(nc)
    env = {cd: {"gm_instr": _gm(v[0]), "gm_cycles": _gm(v[1]),
                "worst_cell_instr": max(v[0]),
                "worst_cell_cycles": max(v[1])}
           for cd, v in ratios.items()}
    art["derived"] = {"envelope_vs_r0": env}


MUTATIONS = [
    "relabel-candidate", "drop-row", "tamper-order", "tamper-semantic",
    "tamper-gm", "tamper-normalized", "tamper-worst-regression",
    "drop-cell-synced", "drop-candidate-synced",
]


def _drop_shootout_cell(base: dict, cand: str, d: int, c: int) -> dict:
    """Delete one whole cell AND resync rounds/rows/derived — the tamper a
    derived-consistency check alone cannot catch; only the sealed matrix
    can."""
    art = copy.deepcopy(base)
    cell = f"{cand}|D={d},C={c}"
    art["execution_order"]["cells"] = [s for s in
                                       art["execution_order"]["cells"]
                                       if s != cell]
    art["execution_order"]["rounds"] = [[s for s in rnd if s != cell]
                                        for rnd in
                                        art["execution_order"]["rounds"]]
    art["rows"] = [r for r in art["rows"]
                   if not (r["candidate"] == cand
                           and r["active_depth"] == d
                           and r["request_capacity"] == c)]
    _embed_shootout_envelope(art)
    return art


def _mutate(base: dict, which: str) -> dict:
    art = copy.deepcopy(base)
    if which == "relabel-candidate":
        art["rows"][0]["candidate"] = "r9"
    elif which == "drop-row":
        art["rows"].pop(3)
    elif which == "tamper-order":
        art["rows"][0], art["rows"][1] = art["rows"][1], art["rows"][0]
    elif which == "tamper-semantic":
        art["rows"][2]["semantic_validation"] = False
    elif which == "tamper-gm":
        art["derived"] = {"gm_per_candidate": {"r1": {"gm_instr": 0.5}}}
    elif which == "tamper-normalized":
        art["derived"] = {"normalized_vs_r0": {
            "r1|P0|D=8,C=8": {"instr": 0.5}}}
    elif which == "tamper-worst-regression":
        art["derived"] = {"envelope_vs_r0": {
            "r1": {"worst_cell_instr": 0.7}}}
    elif which == "drop-cell-synced":
        art = _drop_shootout_cell(base, "r1", 8, 32)
    elif which == "drop-candidate-synced":
        for d, c in [(8, 8), (8, 32)]:
            art = _drop_shootout_cell(art, "r1", d, c)
    return art


def _micro_fixture() -> dict:
    """Minimal valid tax0routermicro artifact exercising the micro
    validator: sealed-param/cell checks, seed-order recompute, per-cell
    rep counts, semantic / same-work / steady-alloc / table accounting,
    and the recorded-derived cross-check on BOTH axes (instructions and
    cycles). Multipliers make r1/r3 faster than r0 deterministically."""
    seed = SELF_MICRO_SPEC["seed"]
    reps = SELF_MICRO_SPEC["reps"]
    cands = list(SELF_MICRO_SPEC["candidates"])
    geo = [(8, 8), (8, 32)]
    cells = [(cd, p, d, cc)
             for cd in cands for p in SELF_MICRO_SPEC["patterns"]
             for (d, cc) in geo]
    rounds = _rng_order(cells, reps, seed)
    speed = {"r0": 1.0, "r1": 0.5, "r3": 0.4}
    rows = []
    seen: dict[tuple, int] = {}
    for rnd in rounds:
        for (cand, pat, d, c) in rnd:
            rep = seen.get((cand, pat, d, c), 0)
            seen[(cand, pat, d, c)] = rep + 1
            base = 1000.0 + 40 * c + (0 if pat == "P0" else 100)
            instr = base * speed[cand] * 100
            rows.append({
                "candidate": cand, "pattern": pat, "depth": d,
                "request_capacity": c, "ops": 100, "lifecycle_ops": 100,
                "instructions_user": instr, "cycles_user": instr * 0.9,
                "semantic_validation": True, "same_work": True,
                "steady_allocations_during_trace": 0,
                "steady_allocations_per_op": 0,
                "table_insert_probes_total": 100 if cand == "r3" else 0,
                "table_erase_probes_total": 100 if cand == "r3" else 0,
            })
    art = {
        "kind": MICRO_KIND,
        "params": {"experiment": SELF_MICRO_SPEC["experiment"],
                   "seed": seed, "reps": reps, "candidates": cands,
                   "patterns": list(SELF_MICRO_SPEC["patterns"]),
                   "geometries": list(SELF_MICRO_SPEC["geometries"]),
                   "windows": SELF_MICRO_SPEC["windows"]},
        "execution_order": {
            "cells": [f"{c}|{p}|D={d},C={cc}" for c, p, d, cc in cells],
            "rounds": [[f"{c}|{p}|D={d},C={cc}"
                        for c, p, d, cc in rnd] for rnd in rounds],
        },
        "rows": rows,
        "derived": {},
    }
    _embed_micro_derived(art)
    return art


def _embed_micro_derived(art: dict) -> None:
    """Recompute and embed the correct micro derived block (both axes),
    standalone from the rows (see _embed_shootout_envelope)."""
    by_cell: dict[tuple, list[dict]] = {}
    for r in art["rows"]:
        by_cell.setdefault((r["candidate"], r["pattern"], r["depth"],
                            r["request_capacity"]), []).append(r)
    med_i = {k: _med([r["instructions_user"] / r["ops"] for r in rs])
             for k, rs in by_cell.items()}
    med_c = {k: _med([r["cycles_user"] / r["ops"] for r in rs])
             for k, rs in by_cell.items()}
    normalized: dict[str, dict[str, float]] = {}
    for (cd, p, d, c) in sorted(by_cell):
        if cd == "r0":
            continue
        normalized[f"{cd}|{p}|D={d},C={c}"] = {
            "instr": med_i[(cd, p, d, c)] / med_i[("r0", p, d, c)],
            "cycles": med_c[(cd, p, d, c)] / med_c[("r0", p, d, c)],
        }
    gm: dict[str, dict[str, float]] = {}
    for cd in sorted({k[0] for k in by_cell}):
        ri = [v["instr"] for k, v in normalized.items()
              if k.startswith(cd + "|")]
        rc = [normalized[k]["cycles"] for k in normalized
              if k.startswith(cd + "|")]
        if ri:
            gm[cd] = {"gm_instr": _gm(ri), "n_cells": len(ri)}
            if rc:
                gm[cd]["gm_cycles"] = _gm(rc)
    art["derived"] = {"gm_per_candidate": gm,
                      "normalized_vs_r0": normalized}


def _drop_micro_cell(base: dict, cand: str, pat: str, d: int,
                     c: int) -> dict:
    """Delete one whole micro cell AND resync rounds/rows/derived."""
    art = json.loads(json.dumps(base))
    cell = f"{cand}|{pat}|D={d},C={c}"
    art["execution_order"]["cells"] = [s for s in
                                       art["execution_order"]["cells"]
                                       if s != cell]
    art["execution_order"]["rounds"] = [[s for s in rnd if s != cell]
                                        for rnd in
                                        art["execution_order"]["rounds"]]
    art["rows"] = [r for r in art["rows"]
                   if not (r["candidate"] == cand and r["pattern"] == pat
                           and r["depth"] == d
                           and r["request_capacity"] == c)]
    _embed_micro_derived(art)
    return art


MICRO_MUTATIONS = [
    "micro-tamper-order", "micro-tamper-gm", "micro-tamper-gm-cycles",
    "micro-tamper-normalized-cycles", "micro-tamper-semantic",
    "micro-nonr3-table", "micro-tamper-alloc",
    "micro-drop-cell-synced", "micro-drop-candidate-synced",
]


def _mutate_micro(art: dict, which: str) -> dict:
    art = json.loads(json.dumps(art))
    if which == "micro-tamper-order":
        art["execution_order"]["rounds"][0][0], \
            art["execution_order"]["rounds"][0][1] = \
            art["execution_order"]["rounds"][0][1], \
            art["execution_order"]["rounds"][0][0]
    elif which == "micro-tamper-gm":
        art["derived"]["gm_per_candidate"]["r1"]["gm_instr"] = 0.7
    elif which == "micro-tamper-gm-cycles":
        art["derived"]["gm_per_candidate"]["r1"]["gm_cycles"] = 0.7
    elif which == "micro-tamper-normalized-cycles":
        k = next(k for k in art["derived"]["normalized_vs_r0"]
                 if k.startswith("r1|"))
        art["derived"]["normalized_vs_r0"][k]["cycles"] = 0.7
    elif which == "micro-tamper-semantic":
        art["rows"][1]["semantic_validation"] = False
    elif which == "micro-nonr3-table":
        for r in art["rows"]:
            if r["candidate"] == "r3":
                r["table_insert_probes_total"] = 0
                break
    elif which == "micro-tamper-alloc":
        art["rows"][0]["steady_allocations_per_op"] = 0.5
        art["rows"][0]["steady_allocations_during_trace"] = 50
    elif which == "micro-drop-cell-synced":
        art = _drop_micro_cell(art, "r1", "P0", 8, 32)
    elif which == "micro-drop-candidate-synced":
        for pat in list(SELF_MICRO_SPEC["patterns"]):
            for d, c in [(8, 8), (8, 32)]:
                art = _drop_micro_cell(art, "r1", pat, d, c)
    return art


def self_test() -> int:
    failures = 0

    def run_muts(label: str, base: dict, muts: list[str],
                 mutate_fn, validate_fn, spec) -> None:
        nonlocal failures
        errs: list[str] = []
        validate_fn(base, errs, spec=spec)
        if errs:
            print(f"SELF-TEST {label} FIXTURE INVALID: {errs}")
            failures += 1
            return
        for which in muts:
            errs = []
            validate_fn(mutate_fn(base, which), errs, spec=spec)
            if errs:
                print(f"mutation {which}: rejected as expected "
                      f"({errs[0]})")
            else:
                print(f"mutation {which}: NOT DETECTED (validator bug)")
                failures += 1

    # Shootout: clean fixture + every mutation must fail.
    base = _fixture()
    run_muts("SHOOTOUT", base, MUTATIONS, _mutate, validate_shootout,
             SELF_SHOOT_SPEC)

    # Micro: clean fixture + every mutation must fail.
    mbase = _micro_fixture()
    run_muts("MICRO", mbase, MICRO_MUTATIONS, _mutate_micro, validate_micro,
             SELF_MICRO_SPEC)

    # Campaign aggregate sanity on the clean fixture (tiny session set).
    sv = validate_shootout(base, [], spec=SELF_SHOOT_SPEC)
    agg = campaign_aggregate({}, [sv], [], sessions=[("read", "tmpfs")])
    if agg["selected"] != "r1" or not agg["verdict"]:
        print(f"SELF-TEST aggregate unexpected: {agg['verdict']} "
              f"{agg['selected']}")
        failures += 1
    else:
        print(f"aggregate: {agg['verdict']} (gm_instr "
              f"{agg['envelope']['r1']['gm_instr']:.4f})")

    # Multi-session aggregate: a second session with a different fs|op
    # shares the SAME (cand, D, C) grid by design; r1 is faster there, so
    # the campaign envelope must pool 2x cells, not flag duplicates.
    other = json.loads(json.dumps(base))
    other["params"]["fs_label"] = "btrfs"
    other["params"]["op"] = "write"
    for r in other["rows"]:
        if r["candidate"] == "r1":
            r["instructions_user"] *= 0.2
            r["cycles_user"] *= 0.2
    _embed_shootout_envelope(other)
    sv2 = validate_shootout(other, [], spec=SELF_SHOOT_SPEC)
    agg2 = campaign_aggregate({}, [sv, sv2], [], sessions=SELF_SESSIONS)
    if agg2["envelope"]["r1"]["n_cells"] != \
            2 * agg["envelope"]["r1"]["n_cells"] or not agg2["verdict"]:
        print(f"SELF-TEST multi-session aggregate wrong: "
              f"n_cells={agg2['envelope']['r1']['n_cells']} "
              f"{agg2['verdict']}")
        failures += 1
    else:
        print(f"multi-session aggregate: {agg2['verdict']} "
              f"(n_cells {agg2['envelope']['r1']['n_cells']})")

    # Passing the SAME session twice must fail closed (duplicate cells).
    errs = []
    campaign_aggregate({}, [sv, sv], errs, sessions=[("read", "tmpfs")])
    if any("duplicate session cell" in e for e in errs):
        print("duplicate-session: rejected as expected")
    else:
        print("duplicate-session: NOT DETECTED (validator bug)")
        failures += 1

    # Sealed session set: dropping one of the frozen sessions must fail.
    errs = []
    campaign_aggregate({}, [sv, sv2], errs)  # frozen set wants 4 sessions
    if any("SEALED session absent" in e for e in errs):
        print("drop-session: rejected as expected")
    else:
        print("drop-session: NOT DETECTED (validator bug)")
        failures += 1

    # Unknown session (not in the frozen set) must fail.
    zfs = json.loads(json.dumps(base))
    zfs["params"]["fs_label"] = "zfs"
    _embed_shootout_envelope(zfs)
    errs = []
    campaign_aggregate({}, [validate_shootout(zfs, [], spec=SELF_SHOOT_SPEC)],
                       errs)
    if any("unknown session" in e for e in errs) or \
            any("fs_label" in e for e in errs):
        print("unknown-session: rejected as expected")
    else:
        print("unknown-session: NOT DETECTED (validator bug)")
        failures += 1

    print("SELF-TEST " + ("OK" if failures == 0 else f"FAILED ({failures})"))
    return 1 if failures else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--micro", help="tax0routermicro artifact (optional for "
                                    "shootout-only validation)")
    ap.add_argument("--shootout", nargs="*", default=[],
                    help="tax0routershootout artifacts (>=1)")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if not args.shootout:
        print("validator: --shootout requires at least one artifact "
              "(fail closed)", file=sys.stderr)
        return 1
    errs: list[str] = []
    micro_v: dict = {}
    for path in ([args.micro] if args.micro else []):
        art = json.loads(Path(path).read_text())
        micro_v = validate_micro(art, errs)
    shoot_vs = []
    for path in args.shootout:
        art = json.loads(Path(path).read_text())
        shoot_vs.append(validate_shootout(art, errs))
    agg = campaign_aggregate(micro_v, shoot_vs, errs)
    if errs:
        print("VALIDATION FAILED (fail closed):")
        for e in errs:
            print(f"  - {e}")
        return 1
    print("VALIDATION PASSED")
    print(json.dumps({
        "campaign": {"selected": agg["selected"],
                     "verdict": agg["verdict"],
                     "eligible": agg["eligible"],
                     "tie_set": agg["tie_set"]},
        "envelope": agg["envelope"],
        "gm_by_fs": agg["gm_by_fs"], "gm_by_op": agg["gm_by_op"],
    }, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
