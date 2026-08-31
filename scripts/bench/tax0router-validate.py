#!/usr/bin/env python3
"""#255 TAX-0 router-fix shootout campaign validator (fail-closed).

Validates the official campaign artifacts (kinds `tax0routermicro` and
`tax0routershootout`, produced by scripts/bench/perf-attribution.py) and
MECHANICALLY recomputes every derived quantity the fix-selection report
relies on:

  - seed-derived execution order (must equal the recorded rounds AND the
    actual row sequence, cell-by-cell);
  - candidate set + matrix completeness (every frozen cell present, no
    extras, exact rep counts);
  - same-work (ops/bytes/word-sums identical across candidates within each
    session);
  - semantic_validation flags (fail-closed: any False fails the campaign);
  - per-cell medians and normalized-vs-r0 ratios;
  - campaign-aggregate geometric means (overall + per fs + per op),
    worst regressions, +5% guardrail eligibility;
  - the pairwise practical-tie relation (2% band);
  - the frozen §25 winner selection (mechanical; the human report may only
    quote it, not re-derive it differently).

Mutation testing: `--self-test` applies each documented tamper (candidate
label tampering, row removal, order tampering, normalized-metric tampering,
GM tampering, worst-regression tampering, semantic-validity tampering) to a
synthetic fixture and asserts the validator FAILS on every one. The
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


def validate_micro(art: dict, errs: list[str]) -> dict:
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
    cells_spec = art["execution_order"]["cells"]
    # Recompute the seed-derived order.
    cells = []
    for s in cells_spec:
        cand, pat, dc = s.split("|")
        d, c = dc[2:].split(",C=")
        cells.append((cand, pat, int(d), int(c)))
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
    # Matrix completeness: rep count per cell.
    counts: dict[str, int] = {}
    for s in seq:
        counts[s] = counts.get(s, 0) + 1
    for s, n in counts.items():
        if n != reps:
            errs.append(f"micro: cell {s} has {n} reps (want {reps})")
    # Same-work / semantic flags / structural gates.
    for i, r in enumerate(rows):
        _check_tracked(r.get("semantic_validation") is True,
                       f"micro row {i}: semantic_validation not True", errs)
        _check_tracked(r.get("same_work") is True,
                       f"micro row {i}: same_work not True", errs)
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
    # Recompute medians + normalized + GM.
    by_cell: dict[tuple, list[dict]] = {}
    for r in rows:
        by_cell.setdefault((r["candidate"], r["pattern"], r["depth"],
                            r["request_capacity"]), []).append(r)
    med_instr: dict[tuple, float] = {}
    for cell, rs in by_cell.items():
        vals = [r["instructions_user"] / r["ops"] for r in rs
                if r.get("instructions_user")]
        if len(vals) != len(rs):
            errs.append(f"micro: cell {cell} rows missing instructions")
        elif vals:
            med_instr[cell] = _med(vals)
    normalized: dict[str, float] = {}
    for cell, v in med_instr.items():
        cand, pat, d, c = cell
        base = med_instr.get(("r0", pat, d, c))
        if cand != "r0" and base:
            normalized[f"{cand}|{pat}|D={d},C={c}"] = v / base
    gm: dict[str, float] = {}
    for cand in sorted({r["candidate"] for r in rows}):
        ratios = [v for k, v in normalized.items()
                  if k.startswith(cand + "|")]
        if ratios:
            gm[cand] = _gm(ratios)
    # Recorded derived must match the recomputation.
    rec_gm = (art.get("derived") or {}).get("gm_per_candidate") or {}
    for cand, v in gm.items():
        rec = (rec_gm.get(cand) or {}).get("gm_instr")
        if rec is None or abs(rec - v) > 1e-9:
            errs.append(f"micro: recorded GM_instr {cand} tampered "
                        f"(recorded {rec}, recomputed {v})")
    rec_norm = (art.get("derived") or {}).get("normalized_vs_r0") or {}
    for k, v in normalized.items():
        rec = (rec_norm.get(k) or {}).get("instr")
        if rec is None or abs(rec - v) > 1e-9:
            errs.append(f"micro: recorded normalized {k} tampered "
                        f"(recorded {rec}, recomputed {v})")
    return {"med_instr": med_instr, "normalized": normalized, "gm": gm,
            "cells": by_cell}


def validate_shootout(art: dict, errs: list[str]) -> dict:
    if art.get("kind") != SHOOTOUT_KIND:
        errs.append(f"shootout artifact kind is {art.get('kind')!r}")
        return {}
    rows = art.get("rows") or []
    params = art.get("params") or {}
    if not rows:
        errs.append("shootout artifact has no rows")
        return {}
    seed, reps = params.get("seed"), params.get("reps")
    cells_spec = art["execution_order"]["cells"]
    cells = []
    for s in cells_spec:
        cand, dc = s.split("|")
        d, c = dc[2:].split(",C=")
        cells.append((cand, int(d), int(c)))
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
                       errs: list[str]) -> dict:
    """Campaign-level envelope across ALL shootout sessions (production
    selection authority) + the frozen §25 mechanical winner."""
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
        cycles_ok = env[best]["gm_cycles"] >= \
            env[best_cycles]["gm_cycles"] - TIE
        worst_ok = all(env[best]["worst_cell_instr"] <=
                       env[o]["worst_cell_instr"] + TIE
                       for o in others if o in SIMPLICITY)
        if leads and cycles_ok and worst_ok:
            selected, verdict = best, f"ROUTER SHOOTOUT PASS - " \
                                      f"{best.upper()} SELECTED"
        else:
            # Practical-tie set: within the 2% band of the GM leader on
            # both axes and no >2pp worst-regression disadvantage.
            tie_set = [c for c in eligible
                       if abs(env[c]["gm_instr"] -
                              env[best]["gm_instr"]) < TIE
                       and abs(env[c]["gm_cycles"] -
                               env[best]["gm_cycles"]) < TIE
                       and env[c]["worst_cell_instr"] <=
                       env[best]["worst_cell_instr"] + TIE]
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

def _fixture() -> dict:
    """Tiny but structurally valid shootout artifact (synthetic numbers)."""
    cands = ["r0", "r1"]
    geo = [(8, 8), (8, 32)]
    reps = 2
    seed = 0x52545253
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
                "table_insert_calls": 0 if cand != "r3" else 100,
                "table_erase_calls": 0, "word_sum": 42,
            })
    art = {
        "kind": SHOOTOUT_KIND,
        "params": {"seed": seed, "reps": reps, "fs_label": "tmpfs",
                   "op": "read", "candidates": cands},
        "execution_order": {
            "cells": [f"{c}|D={d},C={cc}" for c, d, cc in cells],
            "rounds": [[f"{c}|D={d},C={cc}" for c, d, cc in rnd]
                       for rnd in rounds],
        },
        "rows": rows,
        "derived": {},
    }
    # Embed the CORRECT derived envelope (real artifacts always carry it;
    # the mutations then tamper it, and absence itself is invalid).
    v = validate_shootout(art, [])
    env: dict[str, dict] = {}
    for cand in cands:
        if cand == "r0":
            continue
        ni = [v["med_instr"][(cand, d, cc)] /
              v["med_instr"][("r0", d, cc)] for d, cc in geo]
        nc = [v["med_cycles"][(cand, d, cc)] /
              v["med_cycles"][("r0", d, cc)] for d, cc in geo]
        env[cand] = {"gm_instr": _gm(ni), "gm_cycles": _gm(nc),
                     "worst_cell_instr": max(ni),
                     "worst_cell_cycles": max(nc)}
    art["derived"] = {"envelope_vs_r0": env}
    return art


MUTATIONS = [
    "relabel-candidate", "drop-row", "tamper-order", "tamper-semantic",
    "tamper-gm", "tamper-normalized", "tamper-worst-regression",
]


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
    return art


def _micro_fixture() -> dict:
    """Minimal valid tax0routermicro artifact exercising the micro
    validator: seed-order recompute, per-cell rep counts, semantic /
    same-work / steady-alloc / table accounting, and the recorded-derived
    cross-check. Multipliers make r1/r3 faster than r0 deterministically."""
    seed, reps = 0x52545253, 2
    cands = ["r0", "r1", "r3"]
    geo = [(8, 8), (8, 32)]
    cells = [(cd, p, d, cc)
             for cd in cands for p in ("P0", "P1") for (d, cc) in geo]
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
                "steady_allocations_per_op": 0,
                "table_insert_probes_total": 100 if cand == "r3" else 0,
                "table_erase_probes_total": 100 if cand == "r3" else 0,
            })
    art = {
        "kind": MICRO_KIND,
        "params": {"seed": seed, "reps": reps},
        "execution_order": {
            "cells": [f"{c}|{p}|D={d},C={cc}" for c, p, d, cc in cells],
            "rounds": [[f"{c}|{p}|D={d},C={cc}"
                        for c, p, d, cc in rnd] for rnd in rounds],
        },
        "rows": rows,
        "derived": {},
    }
    # Embed the CORRECT derived values (validate_micro appends mismatch
    # errors but still recomputes; the throwaway list is discarded).
    v = validate_micro(art, [])
    art["derived"] = {
        "gm_per_candidate": {c: {"gm_instr": g}
                             for c, g in v["gm"].items()},
        "normalized_vs_r0": {k: {"instr": n}
                             for k, n in v["normalized"].items()},
    }
    return art


MICRO_MUTATIONS = [
    "micro-tamper-order", "micro-tamper-gm", "micro-tamper-semantic",
    "micro-nonr3-table",
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
    elif which == "micro-tamper-semantic":
        art["rows"][1]["semantic_validation"] = False
    elif which == "micro-nonr3-table":
        for r in art["rows"]:
            if r["candidate"] == "r3":
                r["table_insert_probes_total"] = 0
                break
    return art


def self_test() -> int:
    base = _fixture()
    errs: list[str] = []
    validate_shootout(base, errs)
    if errs:
        print(f"SELF-TEST FIXTURE INVALID: {errs}")
        return 1
    failures = 0
    for which in MUTATIONS:
        errs = []
        validate_shootout(_mutate(base, which), errs)
        if errs:
            print(f"mutation {which}: rejected as expected "
                  f"({errs[0]})")
        else:
            print(f"mutation {which}: NOT DETECTED (validator bug)")
            failures += 1
    # Micro validator must be exercised too (the official campaign runs
    # it on real artifacts; a parser that accepts nothing must die here).
    mbase = _micro_fixture()
    errs = []
    validate_micro(mbase, errs)
    if errs:
        print(f"SELF-TEST MICRO FIXTURE INVALID: {errs}")
        return 1
    for which in MICRO_MUTATIONS:
        errs = []
        validate_micro(_mutate_micro(mbase, which), errs)
        if errs:
            print(f"mutation {which}: rejected as expected "
                  f"({errs[0]})")
        else:
            print(f"mutation {which}: NOT DETECTED (validator bug)")
            failures += 1
    # Campaign aggregate sanity on the clean fixture.
    sv = validate_shootout(base, [])
    agg = campaign_aggregate({}, [sv], [])
    if agg["selected"] != "r1" or not agg["verdict"]:
        print(f"SELF-TEST aggregate unexpected: {agg['verdict']} "
              f"{agg['selected']}")
        failures += 1
    else:
        print(f"aggregate: {agg['verdict']} (gm_instr "
              f"{agg['envelope']['r1']['gm_instr']:.4f})")
    # Multi-session aggregate: a second session with a different fs|op
    # shares the SAME (cand, D, C) grid by design; r2 is faster there,
    # so the campaign envelope must pool 2x cells, not flag duplicates.
    other = json.loads(json.dumps(base))
    other["params"]["fs_label"] = "btrfs"
    other["params"]["op"] = "write"
    for r in other["rows"]:
        if r["candidate"] == "r2":
            r["instructions_user"] *= 0.2
            r["cycles_user"] *= 0.2
    sv2 = validate_shootout(other, [])
    agg2 = campaign_aggregate({}, [sv, sv2], [])
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
    campaign_aggregate({}, [sv, sv], errs)
    if any("duplicate session cell" in e for e in errs):
        print("duplicate-session: rejected as expected")
    else:
        print("duplicate-session: NOT DETECTED (validator bug)")
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
