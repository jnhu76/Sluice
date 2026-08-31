#!/usr/bin/env python3
"""TAX-0 COPY-AB-1 validator — external frozen manifest + full recomputation.

The validator holds the campaign matrix and the A/A-derived noise envelope
as INDEPENDENT facts and forces every artifact to match them: an artifact
cannot redefine its own matrix, its derived section, or its materiality
classification. Every derived quantity (per-cell medians, normalized
ratios, GM, paired effects, noise envelope) is RECOMPUTED from the raw rows
and compared against the artifact; any mismatch is a validation failure.

Usage:
  tax0-copy-ab1-validate.py --aa AA_TMPFS.json AA_BTRFS.json \
      --campaign TMPFS.json BTRFS.json --control CTRL_TMPFS.json CTRL_BTRFS.json
  tax0-copy-ab1-validate.py --self-test
"""

from __future__ import annotations

import argparse
import copy
import json
import math
import random
import statistics
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# FROZEN manifest — COPY-AB1-FREEZE. Matrix per
# research/tax0/TAX0-COPY-AB1-DESIGN.md; noise envelope per the official
# A/A sessions (tmpfs + btrfs, 11 reps, seed 0x434F5059). Do not edit after
# the freeze commit; a changed envelope means a NEW freeze and a rerun.
# ---------------------------------------------------------------------------
SEED = 0x434F5059  # "COPY"
QUEUE_DEPTH = 64
FILE_BYTES = 256 * 1024 * 1024
BUFFERS = [4096, 65536, 1048576]
DEPTHS = [1, 8, 32]
DEPTH_SET = DEPTHS
C_NEAR = {1: 2, 8: 9, 32: 33}
C_FIXED = [128, 512]
LABELS = ["r0", "r1"]
CAMPAIGN_REPS = 9
AA_CELLS = [(4096, 8, 32), (4096, 8, 512), (1048576, 8, 512)]
AA_LABELS = ["r0a", "r0b"]
AA_REPS = 11
CONTROL_CELL = (4096, 8, 0)
CONTROL_REPS = 9
WORKERS = 1
ALLOWED_FS = ("tmpfs", "btrfs")

# A/A noise envelope (log2 |paired ratio| p90, max over A/A cells across
# BOTH official A/A sessions - tmpfs + btrfs, 11 reps, seed 0x434F5059) and
# the frozen materiality rule: an R1/R0 improvement is MATERIAL for metric m
# iff  median_log2(ratio) <= -max(2 * NOISE[m], MATERIALITY_FLOOR_LOG2)
# i.e. the reduction must exceed BOTH twice the A/A noise envelope AND 2%.
# FROZEN at COPY-AB1-FREEZE from the official A/A artifacts
# (tax0-copy-ab1-aa-{tmpfs,btrfs}.json); see TAX0-COPY-AB1-DESIGN.md section 7.
#   instr:  0.002212 (the 2% floor dominates the rule)
#   cycles: 0.146368 (2x noise = 0.2927 log2 = ratio 0.8164 dominates)
#   wall:   0.411414 (2x noise = 0.8228 log2 = ratio 0.5653 dominates)
NOISE = {"instr": 0.0022117266327279866,
         "cycles": 0.14636767500596964,
         "wall": 0.411413782561839}
MATERIALITY_FLOOR_LOG2 = math.log2(1 / 0.98)
METRIC_KEY = {"instr": "instructions_user", "cycles": "cycles_user",
              "wall": "copy_wall_ns"}


class Invalid(Exception):
    pass


def _med(vals):
    return statistics.median(vals)


def _gm(vals) -> float:
    prod = 1.0
    for v in vals:
        prod *= v
    return prod ** (1.0 / len(vals))


def _expected_entries(cells, labels) -> list[tuple]:
    # MUST mirror scripts/bench/tax0-copy-ab1-run.py exactly (declared cell
    # order, label-major inner loop) so the frozen-seed order regenerates.
    return [(lab, cell) for cell in cells for lab in labels]


def _regen_rounds(entries, reps, seed):
    rng = random.Random(seed)
    return [rng.sample(entries, k=len(entries)) for _ in range(reps)]


def _check(cond, msg, errs) -> None:
    if not cond:
        errs.append(msg)


def _check_row_common(art: dict, i: int, row: dict, errs: list[str],
                      expect_uring: bool) -> None:
    _check(row.get("experiment") in
           ("TAX-0-COPY-AB-1", "TAX-0-COPY-AB-1-AA",
            "TAX-0-COPY-AB-1-CONTROL"),
           f"row {i}: wrong experiment", errs)
    sha = (row.get("git_sha") or {}).get("sha") if \
        isinstance(row.get("git_sha"), dict) else row.get("git_sha")
    _check(isinstance(sha, str) and len(sha) == 40,
           f"row {i}: git_sha", errs)
    _check(row.get("bytes_copied") == FILE_BYTES,
           f"row {i}: bytes_copied != file size", errs)
    _check(row.get("same_work") is True, f"row {i}: same_work", errs)
    _check(row.get("content_verified") is True,
           f"row {i}: content_verified", errs)
    _check(row.get("content_verification") == "cmp full byte comparison",
           f"row {i}: verification method", errs)
    _check(row.get("workers") == WORKERS, f"row {i}: workers", errs)
    _check(isinstance(row.get("instructions_user"), (int, float)) and
           row["instructions_user"] > 0, f"row {i}: instructions", errs)
    _check(isinstance(row.get("cycles_user"), (int, float)) and
           row["cycles_user"] > 0, f"row {i}: cycles", errs)
    _check(isinstance(row.get("copy_wall_ns"), (int, float)) and
           row["copy_wall_ns"] > 0, f"row {i}: wall", errs)
    _check(row.get("short_writes") == 0, f"row {i}: short_writes", errs)
    chunks = row.get("chunks")
    if expect_uring:
        _check(row.get("real_uring") is True, f"row {i}: real_uring", errs)
        _check(row.get("queue_depth") == QUEUE_DEPTH,
               f"row {i}: queue_depth", errs)
        _check(row.get("request_capacity", 0) >= row["pipeline_depth"] + 1,
               f"row {i}: C < P+1 outside frozen envelope", errs)
    else:
        _check(row.get("real_uring") is False,
               f"row {i}: non-uring row claims real_uring", errs)
    if chunks:
        _check(row.get("write_ops") == chunks,
               f"row {i}: write_ops != chunks", errs)
        _check(chunks <= row.get("read_ops", 0) <= chunks +
               row["pipeline_depth"],
               f"row {i}: read_ops outside [chunks, chunks+P]", errs)
        _check(row["buffer_size"] * chunks >= FILE_BYTES,
               f"row {i}: chunk math", errs)
    _check(row.get("source_sha256"), f"row {i}: source hash missing", errs)


def _check_row_matrix(art: dict, i: int, row: dict, cells: set,
                      errs: list[str]) -> None:
    key = (row["buffer_size"], row["pipeline_depth"], row["request_capacity"])
    _check(key in cells, f"row {i}: cell {key} not in frozen matrix", errs)


def _check_order(art: dict, entries, reps, errs) -> None:
    """The recorded execution order must regenerate from the frozen seed and
    match the actual row sequence (kills 'tamper order')."""
    order_meta = art.get("execution_order", {})
    _check(order_meta.get("seed") == SEED, "order: seed", errs)
    _check(order_meta.get("reps") == reps, "order: reps", errs)
    rng_order = _regen_rounds(entries, reps, SEED)
    want = [[f"{lab}|B={c[0]},P={c[1]},C={c[2]}" for (lab, c) in rnd]
            for rnd in rng_order]
    got = order_meta.get("rounds")
    _check(got == want, "order: recorded rounds do not regenerate from the "
                        "frozen seed", errs)
    rows = art.get("rows", [])
    _check(len(rows) == reps * len(entries),
           f"rows: expected {reps * len(entries)}, got {len(rows)}", errs)
    flat = [f"{lab}|B={c[0]},P={c[1]},C={c[2]}" for rnd in rng_order
            for (lab, c) in rnd]
    for i, (row, want_key) in enumerate(zip(rows, flat)):
        got_key = (f"{row['candidate']}|B={row['buffer_size']},"
                   f"P={row['pipeline_depth']},C={row['request_capacity']}")
        _check(got_key == want_key,
               f"row {i}: executed order {got_key} != frozen order "
               f"{want_key}", errs)
        _check(row.get("execution_order_index") == i,
               f"row {i}: execution_order_index", errs)
        _check(row.get("round") == i // len(entries),
               f"row {i}: round index", errs)


def _check_coverage(art: dict, entries, reps, errs) -> None:
    """Exact cell x label x round coverage — no missing/extra/duplicated."""
    rows = art.get("rows", [])
    seen: dict[tuple, int] = {}
    for r in rows:
        key = (r["round"], r["candidate"], r["buffer_size"],
               r["pipeline_depth"], r["request_capacity"])
        seen[key] = seen.get(key, 0) + 1
    want_keys = [(rnd, lab, c[0], c[1], c[2])
                 for rnd in range(reps) for (lab, c) in entries]
    for k in want_keys:
        _check(seen.get(k, 0) == 1, f"coverage: {k} appears "
               f"{seen.get(k, 0)} times (expected exactly 1)", errs)
    extra = set(seen) - set(want_keys)
    _check(not extra, f"coverage: unexpected rows {sorted(extra)[:3]}", errs)


def _check_same_source(art: dict, errs) -> None:
    hashes = {r.get("source_sha256") for r in art.get("rows", [])}
    _check(len(hashes) == 1 and None not in hashes,
           "source identity: rows bind different/missing source hashes", errs)
    shas = {r["git_sha"].get("sha") if isinstance(r.get("git_sha"), dict)
            else r.get("git_sha") for r in art["rows"]}
    _check(len(shas) == 1, "git_sha differs across rows", errs)


def _recompute(art: dict) -> dict:
    """Recompute per-cell medians from RAW rows only."""
    by_cell: dict[tuple, dict] = {}
    metrics = ("instructions_user", "cycles_user", "copy_wall_ns")
    for r in art["rows"]:
        key = (r["candidate"], r["buffer_size"], r["pipeline_depth"],
               r["request_capacity"], r["file_bytes"])
        slot = by_cell.setdefault(key, {m: [] for m in metrics})
        for m in metrics:
            slot[m].append(r[m])
    cells = {}
    for key, slot in sorted(by_cell.items()):
        cells["|".join(map(str, key))] = {m: _med(slot[m]) for m in metrics}
    by_geom: dict[tuple, dict] = {}
    for k, v in cells.items():
        by_geom.setdefault(tuple(k.split("|")[1:]), {})[k.split("|")[0]] = v
    return {"cells": cells, "by_geom": by_geom}


def _ratios(recomputed: dict) -> dict:
    """labels[1]/labels[0] normalized per-byte ratios from cell medians."""
    out = {}
    for geom, cand in sorted(recomputed["by_geom"].items()):
        if set(cand) != set(LABELS):
            continue
        ka, kb = cand["r0"], cand["r1"]
        fb = int(geom[3])

        def ratio(metric: str) -> float:
            return ((kb[metric] / fb) / (ka[metric] / fb))

        out["|".join(geom)] = {
            "normalized_instr": ratio("instructions_user"),
            "normalized_cycles": ratio("cycles_user"),
            "normalized_wall": ratio("copy_wall_ns"),
        }
    return out


def validate_aa(art: dict, errs: list[str]) -> None:
    params = art.get("params", {})
    _check(params.get("labels") == AA_LABELS, "aa params: labels", errs)
    _check(params.get("seed") == SEED, "aa params: seed", errs)
    _check(params.get("reps") == AA_REPS, "aa params: reps", errs)
    _check(params.get("file_bytes") == FILE_BYTES, "aa params: file size",
           errs)
    _check(params.get("fs_label") in ALLOWED_FS, "aa params: fs", errs)
    _check(params.get("queue_depth") == QUEUE_DEPTH, "aa params: Q", errs)
    cells = set(AA_CELLS)
    entries = _expected_entries(AA_CELLS, AA_LABELS)
    _check_order(art, entries, AA_REPS, errs)
    _check_coverage(art, entries, AA_REPS, errs)
    _check_same_source(art, errs)
    for i, row in enumerate(art.get("rows", [])):
        _check_row_common(art, i, row, errs, expect_uring=True)
        _check_row_matrix(art, i, row, cells, errs)
        _check(row["candidate"] in AA_LABELS,
               f"row {i}: candidate label", errs)
    # Paired effects must reproduce (kills tamper-A/A-noise).
    recomputed = _aa_pooled_p90(art)
    got = (art.get("paired_effects") or {})
    for m, key in (("instr", "instructions_user"),
                   ("cycles", "cycles_user"), ("wall", "copy_wall_ns")):
        want = recomputed[key]
        have = got.get(key, {}).get("p90_abs_log2")
        _check(isinstance(have, (int, float)) and
               abs(have - want) < 1e-9,
               f"aa paired_effects[{key}].p90 tampered "
               f"(artifact {have} != recomputed {want})", errs)


def _aa_pooled_p90(art: dict) -> dict:
    """Per-metric pooled p90|log2 paired ratio| recomputed from raw rows
    (pooling ALL cell pairs in this session)."""
    labels = AA_LABELS
    by_rc: dict[tuple, dict] = {}
    for r in art["rows"]:
        key = (r["round"], r["buffer_size"], r["pipeline_depth"],
               r["request_capacity"])
        by_rc.setdefault(key, {})[r["candidate"]] = r
    ds = {m: [] for m in METRIC_KEY.values()}
    for key, slot in sorted(by_rc.items()):
        if len(slot) != len(labels):
            continue
        for m in METRIC_KEY.values():
            ds[m].append(math.log2(slot[labels[1]][m] / slot[labels[0]][m]))
    out = {}
    for m, vals in ds.items():
        vals_abs = sorted(abs(v) for v in vals)
        out[m] = vals_abs[max(0, int(0.9 * len(vals_abs)) - 1)]
    return out


def validate_campaign(art: dict, errs: list[str]) -> dict:
    params = art.get("params", {})
    _check(params.get("labels") == LABELS, "params: labels", errs)
    _check(params.get("seed") == SEED, "params: seed", errs)
    _check(params.get("reps") == CAMPAIGN_REPS, "params: reps", errs)
    _check(params.get("file_bytes") == FILE_BYTES, "params: file size", errs)
    _check(params.get("fs_label") in ALLOWED_FS, "params: fs", errs)
    _check(params.get("queue_depth") == QUEUE_DEPTH, "params: Q", errs)
    _check(params.get("warmup_rounds") == 1, "params: warmup", errs)
    cells = {(b, p, c) for b in BUFFERS for p in DEPTH_SET
             for c in [C_NEAR[p]] + C_FIXED}
    ordered_cells = [(b, p, c) for b in BUFFERS for p in DEPTH_SET
                     for c in [C_NEAR[p]] + C_FIXED]  # runner order
    entries = _expected_entries(ordered_cells, LABELS)
    _check_order(art, entries, CAMPAIGN_REPS, errs)
    _check_coverage(art, entries, CAMPAIGN_REPS, errs)
    _check_same_source(art, errs)
    for i, row in enumerate(art.get("rows", [])):
        _check_row_common(art, i, row, errs, expect_uring=True)
        _check_row_matrix(art, i, row, cells, errs)
        _check(row["candidate"] in LABELS, f"row {i}: candidate label", errs)

    # Recompute per-cell medians + normalized ratios from raw rows and
    # compare with the artifact's derived section.
    my_ratios = _ratios(_recompute(art))
    if not my_ratios:
        errs.append("derived: no comparable r0/r1 cell pairs "
                    "(candidate or cells missing)")
        return {"ratios": {}, "gm": {}, "classification": {}}
    derived = art.get("derived", {})
    got_ratios = derived.get("ratios", {})
    _check(set(got_ratios) == set(my_ratios),
           "derived: ratio cell set mismatch", errs)
    for k, want in my_ratios.items():
        got = got_ratios.get(k)
        _check(isinstance(got, dict), f"derived: missing ratio {k}", errs)
        if not isinstance(got, dict):
            continue
        for field, w in want.items():
            g = got.get(field)
            _check(isinstance(g, (int, float)) and
                   abs(g - w) <= 1e-9 * max(1.0, abs(w)),
                   f"derived: {k}.{field} tampered ({g} != {w})", errs)
    # GM must reproduce.
    my_gm = {f"gm_normalized_{m}": _gm([v[f"normalized_{m}"] for v in
                                        my_ratios.values()])
             for m in ("instr", "cycles", "wall")}
    got_gm = art.get("gm", {})
    for k, w in my_gm.items():
        g = got_gm.get(k)
        _check(isinstance(g, (int, float)) and abs(g - w) <= 1e-9,
               f"gm: {k} tampered ({g} != {w})", errs)
    # Materiality classification recomputed per cell (frozen rule).
    classification = {}
    for k, v in my_ratios.items():
        cell_cls = {}
        for m in ("instr", "cycles", "wall"):
            d = math.log2(v[f"normalized_{m}"])
            thr = max(2.0 * NOISE[m], MATERIALITY_FLOOR_LOG2)
            cell_cls[m] = ("material_improvement" if d <= -thr
                           else "material_regression" if d >= thr
                           else "within_noise")
        classification[k] = cell_cls
    return {"ratios": my_ratios, "gm": my_gm,
            "classification": classification}


def _ratios(recomputed: dict) -> dict:
    """labels[1]/labels[0] normalized ratios from recomputed cell medians."""
    out = {}
    by_geom = recomputed["by_geom"]
    for geom, cand in sorted(by_geom.items()):
        if set(cand) != set(LABELS):
            continue
        ka, kb = cand["r0"], cand["r1"]
        fb = int(geom[3])
        out["|".join(geom)] = {
            "normalized_instr": ((kb["instructions_user"] / fb) /
                                 (ka["instructions_user"] / fb)),
            "normalized_cycles": ((kb["cycles_user"] / fb) /
                                  (ka["cycles_user"] / fb)),
            "normalized_wall": ((kb["copy_wall_ns"] / fb) /
                                (ka["copy_wall_ns"] / fb)),
        }
    return out


def validate_control(art: dict, errs: list[str]) -> None:
    params = art.get("params", {})
    _check(params.get("labels") == ["threadpool"], "control: labels", errs)
    _check(params.get("seed") == SEED, "control: seed", errs)
    _check(params.get("reps") == CONTROL_REPS, "control: reps", errs)
    _check(params.get("file_bytes") == FILE_BYTES, "control: file size", errs)
    entries = [("threadpool", CONTROL_CELL)]
    _check_order(art, entries, CONTROL_REPS, errs)
    _check_coverage(art, entries, CONTROL_REPS, errs)
    _check_same_source(art, errs)
    for i, row in enumerate(art.get("rows", [])):
        _check_row_common(art, i, row, errs, expect_uring=False)
        _check(row["backend"] == "threadpool", f"row {i}: backend", errs)
        _check((row["buffer_size"], row["pipeline_depth"],
                row["request_capacity"]) == CONTROL_CELL,
               f"row {i}: control cell", errs)


def validate(aa_paths: list[str], campaign_paths: list[str],
             control_paths: list[str]) -> int:
    errs: list[str] = []
    for p in aa_paths:
        art = json.loads(Path(p).read_text())
        validate_aa(art, errs)
    results = []
    for p in campaign_paths:
        art = json.loads(Path(p).read_text())
        results.append((p, validate_campaign(art, errs)))
    for p in control_paths:
        art = json.loads(Path(p).read_text())
        validate_control(art, errs)

    if errs:
        print("VALIDATION FAILED — fail closed:")
        for e in errs:
            print(f"  - {e}")
        return 1

    print("VALIDATION PASS")
    for p, r in results:
        fs = json.loads(Path(p).read_text())["params"]["fs_label"]
        print(f"\n[{fs}] {p}")
        gm = r["gm"]
        print(f"  GM normalized: instr={gm['gm_normalized_instr']:.4f} "
              f"cycles={gm['gm_normalized_cycles']:.4f} "
              f"wall={gm['gm_normalized_wall']:.4f}")
        mat = r["classification"]
        n_instr = sum(1 for v in mat.values()
                      if v["instr"] == "material_improvement")
        n_wall = sum(1 for v in mat.values()
                     if v["wall"] == "material_improvement")
        print(f"  material instr improvements: {n_instr}/{len(mat)} "
              f"(frozen rule: > max(2x{NOISE['instr']:.4f}, "
              f"{MATERIALITY_FLOOR_LOG2:.4f}) log2 reduction)")
        print(f"  material wall improvements: {n_wall}/{len(mat)}")
    return 0


# ---------------------------------------------------------------------------
# Self test — clean fixture must PASS, every mutation must FAIL CLOSED.
# ---------------------------------------------------------------------------
def _fixture_rows(entries, reps, seed, fs="tmpfs"):
    rng_rounds = _regen_rounds(entries, reps, seed)
    rows = []
    idx = 0
    for rnd_i, rnd in enumerate(rng_rounds):
        for (lab, (b, p, c)) in rnd:
            chunks = (FILE_BYTES + b - 1) // b
            base_instr = {4096: 1.2e9, 65536: 2.5e8, 1048576: 5.1e7}[b]
            jitter = 1.0 + 0.001 * ((idx * 37) % 11)
            rows.append({
                "experiment": "TAX-0-COPY-AB-1",
                "git_sha": "a" * 40,
                "candidate": lab,
                "backend": ("threadpool" if lab == "threadpool"
                            else ("uring-r1" if lab == "r1" else "uring-r0")),
                "real_uring": lab != "threadpool",
                "filesystem": fs,
                "buffer_size": b, "pipeline_depth": p,
                "request_capacity": c, "queue_depth": QUEUE_DEPTH,
                "workers": 1,
                "file_bytes": FILE_BYTES,
                "source_sha256": "d" * 64,
                "bytes_copied": FILE_BYTES,
                "read_ops": chunks + 1, "write_ops": chunks,
                "short_writes": 0, "chunks": chunks,
                "round": rnd_i, "execution_order_index": idx,
                "copy_wall_ns": int(base_instr / 40),
                "user_ns": 1, "sys_ns": 1,
                "instructions_user": int(base_instr * jitter),
                "cycles_user": int(base_instr / 3 * jitter),
                "branches_user": 1, "branch_misses_user": 1,
                "cache_misses_user": 1,
                "same_work": True,
                "content_verification": "cmp full byte comparison",
                "content_verified": True,
                "environment_id": "x",
            })
            idx += 1
    return rows, rng_rounds


def _fixture(entry_cells, labels, reps, kind, fs="tmpfs", experiment=None):
    entries = _expected_entries(entry_cells, labels)
    rows, rounds = _fixture_rows(entries, reps, SEED, fs=fs)
    art = {
        "schema": 2,
        "kind": kind,
        "binary": {"sha256": "b" * 64},
        "params": {
            "experiment": experiment or "TAX-0-COPY-AB-1",
            "labels": labels,
            "cells": [f"B={b},P={p},C={c}" for (b, p, c) in entry_cells],
            "file_bytes": FILE_BYTES,
            "reps": reps,
            "warmup_rounds": 1 if kind == "tax0copyab" else 0,
            "seed": SEED,
            "fs_label": fs,
            "taskset": "0,2,4,6",
            "queue_depth": QUEUE_DEPTH,
            "workers": 1,
        },
        "execution_order": {
            "seed": SEED, "reps": reps,
            "cells": [f"{lab}|B={c[0]},P={c[1]},C={c[2]}"
                      for (lab, c) in entries],
            "rounds": [[f"{lab}|B={c[0]},P={c[1]},C={c[2]}"
                        for (lab, c) in rnd] for rnd in rounds],
        },
        "rows": rows,
        "same_work": {},
        "derived": {},
    }
    if kind == "tax0copyab":
        _embed_derived(art)
    return art


def _derived_key(d: dict) -> str:
    return (f"{d['buffer_size']}|{d['pipeline_depth']}|"
            f"{d['request_capacity']}|{d['file_bytes']}")


def _embed_derived(art: dict) -> None:
    """Embed the RECOMPUTED derived/gm in the runner's exact shape — the
    fixture must look like an honest artifact; tampering is detected by
    the validator's independent recomputation."""
    if art["kind"] != "tax0copyab":
        return
    my_ratios = _ratios(_recompute(art))
    art["derived"] = {"ratios": {
        k: {"buffer_size": int(k.split("|")[0]),
            "pipeline_depth": int(k.split("|")[1]),
            "request_capacity": int(k.split("|")[2]),
            "file_bytes": int(k.split("|")[3]),
            "normalized_instr": v["normalized_instr"],
            "normalized_cycles": v["normalized_cycles"],
            "normalized_wall": v["normalized_wall"]}
        for k, v in my_ratios.items()}}
    art["gm"] = {f"gm_normalized_{m}": _gm(
        [v[f"normalized_{m}"] for v in my_ratios.values()])
        for m in ("instr", "cycles", "wall")}


def _aa_embed_effects(art: dict) -> None:
    pooled = _aa_pooled_p90(art)
    art["paired_effects"] = {m: {"p90_abs_log2": v}
                             for m, v in pooled.items()}


def _drop_cell(art: dict, lab: str, cell: tuple) -> None:
    art["rows"] = [r for r in art["rows"]
                   if not (r["candidate"] == lab and
                           (r["buffer_size"], r["pipeline_depth"],
                            r["request_capacity"]) == cell)]
    art["execution_order"]["rounds"] = [
        [e for e in rnd if e != f"{lab}|B={cell[0]},P={cell[1]},"
                                 f"C={cell[2]}"]
        for rnd in art["execution_order"]["rounds"]]


def _mutate_campaign(base: dict, which: str) -> dict:
    art = copy.deepcopy(base)
    r0 = art["rows"][0]
    if which == "clean":
        pass
    elif which == "drop_row":
        art["rows"].pop(3)
    elif which == "drop_cell":
        _drop_cell(art, "r1", (4096, 1, 2))
    elif which == "drop_cell_resync":
        _drop_cell(art, "r1", (4096, 1, 2))
        _embed_derived(art)
    elif which == "drop_candidate":
        art["rows"] = [r for r in art["rows"] if r["candidate"] != "r1"]
        art["execution_order"]["rounds"] = [
            [e for e in rnd if not e.startswith("r1|")]
            for rnd in art["execution_order"]["rounds"]]
    elif which == "relabel_candidate":
        art["rows"][5]["candidate"] = "r1" if \
            art["rows"][5]["candidate"] == "r0" else "r0"
    elif which == "change_file_size":
        art["params"]["file_bytes"] = FILE_BYTES * 2
    elif which == "change_source_hash":
        art["rows"][7]["source_sha256"] = "f" * 64
    elif which == "change_bytes":
        art["rows"][9]["bytes_copied"] = FILE_BYTES - 1
    elif which == "false_verification":
        art["rows"][11]["content_verified"] = False
    elif which == "real_uring_false":
        art["rows"][13]["real_uring"] = False
    elif which == "tamper_instructions":
        # Falsify one cell's counter across ALL reps WITHOUT re-embedding
        # derived (the realistic artifact-tampering attack: the cell median
        # shifts, so the honest derived section no longer matches the raw
        # rows and the recomputation must catch it).
        for r in art["rows"]:
            if r["candidate"] == "r0" and (r["buffer_size"],
                                           r["pipeline_depth"],
                                           r["request_capacity"]) == \
                    (4096, 8, 128):
                r["instructions_user"] *= 0.5
    elif which == "tamper_normalized_ratio":
        k = next(iter(art["derived"]["ratios"]))
        art["derived"]["ratios"][k]["normalized_instr"] *= 0.9
    elif which == "tamper_gm":
        art["gm"]["gm_normalized_instr"] *= 0.9
    elif which == "tamper_order":
        art["rows"][2], art["rows"][3] = art["rows"][3], art["rows"][2]
        for i, row in enumerate(art["rows"]):
            row["execution_order_index"] = i
    elif which == "tamper_q":
        art["rows"][4]["queue_depth"] = 32
    else:
        raise SystemExit(f"unknown mutation {which}")
    return art


def _mutate_aa(base: dict, which: str) -> dict:
    art = copy.deepcopy(base)
    if which == "clean":
        pass
    elif which == "drop_row":
        art["rows"].pop(2)
    elif which == "relabel_candidate":
        art["rows"][3]["candidate"] = \
            "r0a" if art["rows"][3]["candidate"] == "r0b" else "r0b"
    elif which == "change_source_hash":
        art["rows"][4]["source_sha256"] = "e" * 64
    elif which == "change_bytes":
        art["rows"][6]["bytes_copied"] = FILE_BYTES - 4096
    elif which == "false_verification":
        art["rows"][8]["content_verified"] = False
    elif which == "real_uring_false":
        art["rows"][10]["real_uring"] = False
    elif which == "tamper_instructions":
        # Falsify one label's counter for one whole cell (every paired
        # diff in that cell shifts) WITHOUT re-embedding the effects —
        # the recomputation must catch the stale/inconsistent artifact.
        for r in art["rows"]:
            if r["candidate"] == "r0a" and r["buffer_size"] == 4096:
                r["instructions_user"] *= 1.4
    elif which == "tamper_noise_threshold":
        # Tampering the A/A envelope recorded in the artifact must fail:
        # the validator recomputes it from raw rows.
        art["paired_effects"]["instructions_user"]["p90_abs_log2"] *= 10
    elif which == "tamper_order":
        art["rows"][1], art["rows"][2] = art["rows"][2], art["rows"][1]
        for i, row in enumerate(art["rows"]):
            row["execution_order_index"] = i
    else:
        raise SystemExit(f"unknown aa mutation {which}")
    return art


def self_test() -> int:
    failures = 0

    def expect_reject(label, fn, art):
        nonlocal failures
        errs: list[str] = []
        try:
            fn(art, errs)
        except Invalid as e:
            errs.append(str(e))
        if errs:
            print(f"  mutation {label}: rejected as expected "
                  f"({len(errs)} finding(s))")
        else:
            print(f"  mutation {label}: NOT DETECTED (validator bug)")
            failures += 1

    # --- campaign fixture (clean must pass) ---
    cells = sorted((b, p, c) for b in BUFFERS for p in DEPTHS
                   for c in [C_NEAR[p]] + C_FIXED)
    base = _fixture(cells, LABELS, CAMPAIGN_REPS, "tax0copyab")
    _embed_derived(base)
    errs: list[str] = []
    validate_campaign(base, errs)
    if errs:
        print("campaign clean fixture FAILED (validator/fixture bug):")
        for e in errs:
            print(f"  - {e}")
        failures += 1
    else:
        print("campaign clean fixture: PASS")
    muts = ["drop_row", "drop_cell", "drop_cell_resync", "drop_candidate",
            "relabel_candidate", "change_file_size", "change_source_hash",
            "change_bytes", "false_verification", "real_uring_false",
            "tamper_instructions", "tamper_normalized_ratio", "tamper_gm",
            "tamper_order", "tamper_q"]
    for m in muts:
        art = _mutate_campaign(base, m)
        expect_reject(m, validate_campaign, art)

    # --- aa fixture ---
    base_aa = _fixture(AA_CELLS, AA_LABELS, AA_REPS, "tax0copyab-aa",
                       experiment="TAX-0-COPY-AB-1-AA")
    _aa_embed_effects(base_aa)
    errs = []
    validate_aa(base_aa, errs)
    if errs:
        print("aa clean fixture FAILED (validator/fixture bug):")
        for e in errs:
            print(f"  - {e}")
        failures += 1
    else:
        print("aa clean fixture: PASS")
    for m in ["drop_row", "relabel_candidate", "change_source_hash",
              "change_bytes", "false_verification", "real_uring_false",
              "tamper_instructions", "tamper_noise_threshold",
              "tamper_order"]:
        art = _mutate_aa(base_aa, m)
        expect_reject(m, validate_aa, art)

    # --- control fixture ---
    base_ctl = _fixture([CONTROL_CELL], ["threadpool"], CONTROL_REPS,
                        "tax0copyab-control",
                        experiment="TAX-0-COPY-AB-1-CONTROL")
    errs = []
    validate_control(base_ctl, errs)
    if errs:
        print("control clean fixture FAILED (validator/fixture bug):")
        for e in errs:
            print(f"  - {e}")
        failures += 1
    else:
        print("control clean fixture: PASS")
    art = copy.deepcopy(base_ctl)
    art["rows"][0]["real_uring"] = True
    expect_reject("control_real_uring_true", validate_control, art)

    print(f"\nself-test {'FAIL' if failures else 'PASS'} "
          f"({failures} undetected mutations)")
    return 1 if failures else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--aa", nargs="*", default=[])
    ap.add_argument("--campaign", nargs="*", default=[])
    ap.add_argument("--control", nargs="*", default=[])
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args()
    if args.self_test:
        return self_test()
    if not (args.aa or args.campaign or args.control):
        ap.error("nothing to validate")
    return validate(args.aa, args.campaign, args.control)


if __name__ == "__main__":
    sys.exit(main())
