#!/usr/bin/env python3
"""TAX-0 COPY-AB-1 validator — external frozen manifest + full recomputation.

The validator holds the campaign matrix and the A/A-derived noise envelope
as INDEPENDENT facts and forces every artifact to match them: an artifact
cannot redefine its own matrix, its derived section, or its materiality
classification. Every decision quantity is RECOMPUTED from the raw rows:

  * per-cell medians / normalized ratios / GM — descriptive statistics,
    recomputed and pinned against the artifact's derived section;
  * per-cell PAIRED round effects (log2(R1/R0) per round) — the decision
    statistic. For every (round, cell) exactly one row of each candidate
    must exist; materiality is classified from the recomputed paired
    MEDIAN per cell against the frozen thresholds;
  * the A/A noise envelope — recomputed from the official A/A raw rows
    under the frozen rule (per-cell p90, nearest-rank index
    ceil(0.90*n)-1, session noise = max over cells, campaign envelope =
    max over both filesystem sessions) and fail-closed against the frozen
    manifest constants below.

EVIDENCE-CORRECTIVE (post-freeze review; raw artifacts byte-identical):
the freeze-era implementation classified materiality from the
ratio-of-arm-medians (a DESCRIPTIVE statistic), pooled all A/A pairs
before taking p90, and used the biased-low int(0.9*n)-1 index. The frozen
DESIGN always specified the paired median and the max per-cell p90; this
validator now implements the frozen rule as written. The freeze-era
constants (0.002212/0.146368/0.411414) are superseded; the corrected
envelope is recomputed live from the official A/A raw rows at every
top-level validation.

The top-level invocation is a SIX-ARTIFACT CAMPAIGN SEAL: exactly one
tmpfs + one btrfs artifact for each of A/A, campaign and control, with
cross-artifact consistency checks (source bytes, benchmark binary, seed,
matrix constants, filesystem labels vs recorded filesystem types). Rows
carry an identity binding of their own — the exact campaign experiment
id, the frozen candidate->backend map, the session fs_label and the
frozen file size — and the seal requires ONE benchmark binary sha256
across all three classes: an A/A envelope is a noise threshold only for
the binary that produced the campaign.

REVIEW-CORRECTIVE (second post-freeze review; raw artifacts
byte-identical): the earlier row check accepted any of the three
experiment ids on every row and never bound row backend, filesystem or
file_bytes, and the seal compared binary hashes only WITHIN a class. The
pins above close those tails. The official artifacts already satisfy
every new pin; no measurement was rerun.

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
# research/tax0/TAX0-COPY-AB1-DESIGN.md; do not edit after the freeze; a
# changed matrix means a NEW freeze and a rerun.
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

# A/A noise envelope — EVIDENCE-CORRECTIVE recomputation (2026-08-31) of the
# AS-FROZEN rule from the OFFICIAL A/A raw rows (tax0-copy-ab1-aa-{tmpfs,
# btrfs}.json, 11 reps, seed 0x434F5059; raw files byte-identical):
# per cell p90(|log2 paired ratio|) with nearest-rank index ceil(0.90*n)-1,
# session noise = max over that session's cells, campaign envelope =
# max over both sessions. The freeze-era validator constants
# (instr 0.002212 / cycles 0.146368 / wall 0.411414) are superseded: they
# were computed by an implementation that pooled pairs and biased the p90
# index low; the corrected values below regenerate from the official raw
# rows at every top-level validation (fail-closed).
#   instr:  max cell p90 = tmpfs  (B=4KiB, P=8, C=32)   0.00203005
#   cycles: max cell p90 = btrfs  (B=1MiB, P=8, C=512)  0.14521617
#   wall:   max cell p90 = tmpfs  (B=4KiB, P=8, C=32)   0.39822390
NOISE = {"instr": 0.00203005043359491,
         "cycles": 0.1452161663822206,
         "wall": 0.3982239036457863}
# Frozen materiality rule (unchanged by the corrective): an R1/R0 effect is
# MATERIAL for metric m iff the recomputed per-cell paired median satisfies
#   median_log2 <= -max(2 * NOISE[m], MATERIALITY_FLOOR_LOG2)  (improvement)
#   median_log2 >= +max(2 * NOISE[m], MATERIALITY_FLOOR_LOG2)  (regression)
# i.e. the effect must exceed BOTH twice the A/A noise envelope AND 2%.
# Corrected thresholds (log2): instr 0.029146 (2% floor dominates;
# ratio <= 0.9800), cycles 0.290432 (ratio <= 0.8177), wall 0.796448
# (ratio <= 0.5758).
MATERIALITY_FLOOR_LOG2 = math.log2(1 / 0.98)
METRIC_KEY = {"instr": "instructions_user", "cycles": "cycles_user",
              "wall": "copy_wall_ns"}

# The campaign seal: artifact kind + experiment id per category.
KIND_OF = {"aa": ("tax0copyab-aa", "TAX-0-COPY-AB-1-AA"),
           "campaign": ("tax0copyab", "TAX-0-COPY-AB-1"),
           "control": ("tax0copyab-control", "TAX-0-COPY-AB-1-CONTROL")}

# Row identity binding (REVIEW-CORRECTIVE): the runner stamps EVERY row
# (A/A and control included) with the campaign experiment id — category
# identity lives in params.experiment, sealed per category in
# _load_sealed — and each candidate label is pinned to exactly one
# backend. A row can no longer be re-homed into a foreign experiment, a
# candidate cannot silently change backend, and row filesystem/file_bytes
# must match the frozen session parameters.
ROW_EXPERIMENT = KIND_OF["campaign"][1]
CANDIDATE_BACKEND = {"r0": "uring-r0", "r1": "uring-r1",
                     "r0a": "uring-r0", "r0b": "uring-r0",
                     "threadpool": "threadpool"}


class Invalid(Exception):
    pass


def _med(vals):
    return statistics.median(vals)


def _gm(vals) -> float:
    prod = 1.0
    for v in vals:
        prod *= v
    return prod ** (1.0 / len(vals))


def _mad(samples: list[float]) -> float:
    """Median absolute deviation from the median (runner convention)."""
    m = statistics.median(samples)
    return statistics.median([abs(x - m) for x in samples])


def _p90_nearest_rank(vals: list[float]) -> float:
    """p90 of |values|, nearest-rank order statistic: index ceil(0.90*n)-1
    of the ascending sort. The frozen A/A rule; the freeze-era
    int(0.9*n)-1 index was biased low."""
    ordered = sorted(abs(v) for v in vals)
    return ordered[math.ceil(0.9 * len(ordered)) - 1]


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
    _check(row.get("experiment") == ROW_EXPERIMENT,
           f"row {i}: experiment {row.get('experiment')!r} != "
           f"{ROW_EXPERIMENT!r}", errs)
    backend = CANDIDATE_BACKEND.get(row.get("candidate"))
    _check(backend is not None and row.get("backend") == backend,
           f"row {i}: backend {row.get('backend')!r} violates the frozen "
           f"candidate->backend binding "
           f"{row.get('candidate')!r}->{backend!r}", errs)
    _check(row.get("file_bytes") == FILE_BYTES,
           f"row {i}: file_bytes != frozen file size", errs)
    _check(row.get("filesystem") == (art.get("params") or {}).get("fs_label"),
           f"row {i}: filesystem {row.get('filesystem')!r} != session "
           f"fs_label", errs)
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


def _check_env_fs(art: dict, errs, ctx: str) -> None:
    """The declared fs_label must match the filesystem type actually
    recorded for the workload input AND output paths."""
    fs = (art.get("env") or {}).get("filesystem") or {}
    label = (art.get("params") or {}).get("fs_label")
    for side in ("input", "output"):
        t = (fs.get(side) or {}).get("type")
        _check(t == label,
               f"{ctx}: env filesystem {side} type {t!r} != fs_label "
               f"{label!r}", errs)


# ---------------------------------------------------------------------------
# Paired-round recomputation — the decision-statistic authority.
# ---------------------------------------------------------------------------
def _paired_rounds(rows: list[dict], labels: list[str], errs: list[str],
                   ctx: str) -> dict:
    """cell -> metric -> [log2((B/bytes_B)/(A/bytes_A)) per round], built
    ONLY from raw rows. Proves every (round, cell) holds exactly one row of
    each candidate label; a broken round pair never contributes a value
    (and is reported)."""
    seen: dict[tuple, dict] = {}
    duplicated: set = set()
    for r in rows:
        key = (r["round"], r["buffer_size"], r["pipeline_depth"],
               r["request_capacity"], r["file_bytes"])
        bucket = seen.setdefault(key, {})
        if r["candidate"] in bucket:
            duplicated.add(key + (r["candidate"],))
        bucket[r["candidate"]] = r
    for key in sorted(duplicated):
        errs.append(f"{ctx}: duplicate row for round {key[0]} cell "
                    f"B={key[1]},P={key[2]},C={key[3]} label {key[4]}")
    cells: dict[tuple, dict] = {}
    for key, slot in sorted(seen.items()):
        cell = key[1:]
        if set(slot) != set(labels):
            errs.append(f"{ctx}: round {key[0]} cell B={cell[0]},"
                        f"P={cell[1]},C={cell[2]} does not hold exactly one "
                        f"of {sorted(labels)} (got {sorted(slot)})")
            continue
        a, b = slot[labels[0]], slot[labels[1]]
        for m in METRIC_KEY.values():
            d = math.log2((b[m] / b["file_bytes"]) /
                          (a[m] / a["file_bytes"]))
            cells.setdefault(cell, {}).setdefault(m, []).append(d)
    return cells


def _cell_effects(ds: list[float]) -> dict:
    """Corrected per-cell paired aggregate (mission §6 shape)."""
    return {
        "n_pairs": len(ds),
        "paired_median_log2": statistics.median(ds),
        "paired_mean_log2": sum(ds) / len(ds),
        "paired_gm_ratio": 2 ** (sum(ds) / len(ds)),
        "mad_log2": _mad(ds),
        "p90_abs_log2": _p90_nearest_rank(ds),
    }


def _freeze_era_effects(ds: list[float]) -> dict | None:
    """The paired aggregate EXACTLY as the freeze-era runner recorded it
    (pooled field name `gm_ratio`; p90 index int(0.9*n)-1). Used ONLY to
    pin the recorded fields of the sealed official artifacts for tamper
    evidence — never as a decision statistic. None for an empty pool
    (broken rows are reported by the pair builder itself)."""
    if not ds:
        return None
    return {
        "n_pairs": len(ds),
        "paired_median_log2": statistics.median(ds),
        "paired_mean_log2": sum(ds) / len(ds),
        "gm_ratio": 2 ** (sum(ds) / len(ds)),
        "mad_log2": _mad(ds),
        "p90_abs_log2": sorted(abs(v) for v in ds)[
            max(0, int(0.9 * len(ds)) - 1)],
    }


def _freeze_era_pooled(cells: dict) -> dict:
    """Pooled-across-cells paired ds per metric (the freeze-era recorded
    shape) = flattening of the per-cell paired values."""
    out = {}
    for m in METRIC_KEY.values():
        out[m] = [d for cell in cells.values() for d in cell.get(m, [])]
    return out


def _pin_recorded_pooled(art: dict, labels: list[str], errs: list[str],
                         ctx: str) -> None:
    """The artifact's recorded `paired_effects` section must reproduce from
    raw rows under the freeze-era formula. This pins the RECORDED fields
    for tamper evidence (an attacker editing raw rows without re-embedding
    the recorded paired evidence is caught here). The recorded values are
    NEVER the decision authority: materiality below is classified from the
    validator's own per-cell recomputation."""
    got = art.get("paired_effects") or {}
    pooled = _freeze_era_pooled(_paired_rounds(art["rows"], labels, errs,
                                               ctx))
    for m, ds in pooled.items():
        want = _freeze_era_effects(ds)
        if want is None:
            continue  # broken rows already reported by the pair builder
        have = got.get(m)
        _check(isinstance(have, dict),
               f"{ctx}: paired_effects[{m}] missing", errs)
        if not isinstance(have, dict):
            continue
        for f, w in want.items():
            g = have.get(f)
            _check(isinstance(g, (int, float)) and
                   abs(g - w) <= 1e-9 * max(1.0, abs(w)),
                   f"{ctx}: paired_effects[{m}].{f} tampered "
                   f"(recorded {g} != recomputed {w})", errs)


def corrected_envelope(aa_arts: list[dict]) -> tuple[dict, dict]:
    """Recompute the A/A noise envelope from raw rows under the frozen
    rule: per cell p90(|d|) nearest-rank; session noise = max over that
    session's cells; campaign envelope = max over sessions. Returns
    (campaign envelope, per-filesystem session noises)."""
    env: dict = {}
    per_fs: dict = {}
    scratch: list[str] = []
    for art in aa_arts:
        cells = _paired_rounds(art.get("rows") or [], AA_LABELS, scratch,
                               "envelope")
        sess = {}
        for key in ("instr", "cycles", "wall"):
            m = METRIC_KEY[key]
            vals = [_p90_nearest_rank(c[m]) for c in cells.values()
                    if c.get(m)]
            if vals:
                sess[key] = max(vals)
        fs = (art.get("params") or {}).get("fs_label")
        if fs:
            per_fs[fs] = sess
        for k, v in sess.items():
            env[k] = max(env.get(k, 0.0), v)
    return env, per_fs


# ---------------------------------------------------------------------------
# Descriptive statistics (recomputed; pinned against the artifact).
# ---------------------------------------------------------------------------
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
    """labels[1]/labels[0] normalized per-byte ratios from cell medians.
    DESCRIPTIVE statistic: informs, never classifies."""
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


def validate_aa(art: dict, errs: list[str]) -> None:
    params = art.get("params", {})
    _check(params.get("labels") == AA_LABELS, "aa params: labels", errs)
    _check(params.get("seed") == SEED, "aa params: seed", errs)
    _check(params.get("reps") == AA_REPS, "aa params: reps", errs)
    _check(params.get("file_bytes") == FILE_BYTES, "aa params: file size",
           errs)
    _check(params.get("fs_label") in ALLOWED_FS, "aa params: fs", errs)
    _check(params.get("queue_depth") == QUEUE_DEPTH, "aa params: Q", errs)
    _check_env_fs(art, errs, "aa")
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
    # Recorded freeze-era pooled fields must reproduce from raw rows.
    # The recorded `per_cell` subkey in the official A/A artifacts is
    # KNOWN-CORRUPT (freeze-era runner overwrote rounds; every entry has
    # n_pairs=1) — it is ignored, not pinned and not repaired in place;
    # decision statistics are recomputed from raw rows.
    _pin_recorded_pooled(art, AA_LABELS, errs, "aa")


def _thresholds(envelope: dict) -> dict:
    return {k: max(2.0 * envelope[k], MATERIALITY_FLOOR_LOG2)
            for k in ("instr", "cycles", "wall")}


def validate_campaign(art: dict, errs: list[str],
                      envelope: dict | None = None) -> dict:
    """Validate one official campaign artifact. `envelope` overrides the
    frozen NOISE manifest (self-test fixtures only); materiality is ALWAYS
    classified from the validator's recomputed per-cell PAIRED median."""
    thr = _thresholds(envelope if envelope is not None else NOISE)
    params = art.get("params", {})
    _check(params.get("labels") == LABELS, "params: labels", errs)
    _check(params.get("seed") == SEED, "params: seed", errs)
    _check(params.get("reps") == CAMPAIGN_REPS, "params: reps", errs)
    _check(params.get("file_bytes") == FILE_BYTES, "params: file size", errs)
    _check(params.get("fs_label") in ALLOWED_FS, "params: fs", errs)
    _check(params.get("queue_depth") == QUEUE_DEPTH, "params: Q", errs)
    _check(params.get("warmup_rounds") == 1, "params: warmup", errs)
    _check_env_fs(art, errs, "campaign")
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

    # Independent paired-round recomputation (decision authority).
    paired_cells = _paired_rounds(art["rows"], LABELS, errs, "campaign")
    paired = {"B={},P={},C={}".format(*cell):
              {k: _cell_effects(ds[METRIC_KEY[k]]) for k in
               ("instr", "cycles", "wall")}
              for cell, ds in sorted(paired_cells.items())}
    # Pin the recorded (freeze-era) pooled paired fields for tamper evidence.
    _pin_recorded_pooled(art, LABELS, errs, "campaign")

    # Descriptive: recompute per-cell medians + normalized ratios from raw
    # rows and compare with the artifact's derived section.
    my_ratios = _ratios(_recompute(art))
    if not my_ratios:
        errs.append("derived: no comparable r0/r1 cell pairs "
                    "(candidate or cells missing)")
        return {"ratios": {}, "gm": {}, "classification": {}, "paired": paired}
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
    # DECISION: materiality from the recomputed per-cell PAIRED MEDIAN —
    # never from the ratio-of-arm-medians (descriptive) above.
    classification = {}
    for cell, effects in paired.items():
        cls = {}
        for k in ("instr", "cycles", "wall"):
            d_med = effects[k]["paired_median_log2"]
            cls[k] = ("material_improvement" if d_med <= -thr[k]
                      else "material_regression" if d_med >= thr[k]
                      else "within_band")
        classification[cell] = cls
    return {"ratios": my_ratios, "gm": my_gm,
            "classification": classification, "paired": paired}


def validate_control(art: dict, errs: list[str]) -> None:
    params = art.get("params", {})
    _check(params.get("labels") == ["threadpool"], "control: labels", errs)
    _check(params.get("seed") == SEED, "control: seed", errs)
    _check(params.get("reps") == CONTROL_REPS, "control: reps", errs)
    _check(params.get("file_bytes") == FILE_BYTES, "control: file size", errs)
    _check_env_fs(art, errs, "control")
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


# ---------------------------------------------------------------------------
# Six-artifact campaign seal + cross-artifact consistency.
# ---------------------------------------------------------------------------
def _load_sealed(category: str, items: list, errs: list[str]) -> list[dict]:
    """Load and seal one artifact category: EXACTLY two artifacts, one tmpfs
    + one btrfs, exact kind + experiment id, no duplicate paths or
    filesystems. Dict items (self-test fixtures) bypass file loading."""
    kind, experiment = KIND_OF[category]
    _check(len(items) == 2,
           f"seal[{category}]: the campaign seal requires EXACTLY two "
           f"{category} artifacts — one tmpfs + one btrfs (got "
           f"{len(items)})", errs)
    str_paths = [p for p in items if isinstance(p, str)]
    _check(len(set(str_paths)) == len(str_paths),
           f"seal[{category}]: duplicate artifact path supplied", errs)
    arts: list[dict] = []
    seen_fs: dict = {}
    for item in items:
        if isinstance(item, dict):
            art = item
        else:
            try:
                art = json.loads(Path(item).read_text())
            except (OSError, json.JSONDecodeError) as e:
                errs.append(f"seal[{category}]: cannot load {item}: {e}")
                continue
        arts.append(art)
        _check(art.get("kind") == kind,
               f"seal[{category}]: wrong kind (want {kind!r}, got "
               f"{art.get('kind')!r})", errs)
        params = art.get("params") or {}
        _check(params.get("experiment") == experiment,
               f"seal[{category}]: wrong experiment id (want {experiment!r}, "
               f"got {params.get('experiment')!r})", errs)
        fs = params.get("fs_label")
        _check(fs in ALLOWED_FS,
               f"seal[{category}]: unknown filesystem {fs!r} "
               f"(allowed: {sorted(ALLOWED_FS)})", errs)
        if fs in seen_fs:
            errs.append(f"seal[{category}]: duplicate {fs} session; the "
                        f"seal requires exactly one tmpfs + one btrfs")
        seen_fs[fs] = True
    _check(set(seen_fs) == set(ALLOWED_FS),
           f"seal[{category}]: filesystem coverage "
           f"{sorted(str(f) for f in seen_fs)} != {sorted(ALLOWED_FS)} "
           f"(missing session?)", errs)
    return arts


def _cross_class(category: str, arts: list[dict], errs: list[str]) -> None:
    """Cross-artifact consistency within one class (tmpfs vs btrfs
    session). Environment properties that legitimately differ between
    filesystems are deliberately NOT compared."""
    if len(arts) != 2:
        return  # the seal already reported the missing session

    def one(what: str, getter) -> None:
        vals = {getter(a) for a in arts}
        _check(len(vals) == 1,
               f"cross[{category}]: {what} differs across the tmpfs/btrfs "
               f"sessions ({sorted(repr(v) for v in vals)})", errs)

    def first_row(a: dict, field: str):
        rows = a.get("rows") or []
        return rows[0].get(field) if rows else None

    one("source sha256", lambda a: first_row(a, "source_sha256"))
    one("benchmark binary sha256",
        lambda a: (a.get("binary") or {}).get("sha256"))
    one("seed", lambda a: (a.get("params") or {}).get("seed"))
    one("file_bytes", lambda a: (a.get("params") or {}).get("file_bytes"))
    one("workers", lambda a: (a.get("params") or {}).get("workers"))
    one("candidate labels",
        lambda a: tuple((a.get("params") or {}).get("labels") or ()))
    if category in ("aa", "campaign"):  # uring sessions pin the ring depth
        one("queue_depth",
            lambda a: (a.get("params") or {}).get("queue_depth"))


def validate_all(aa_paths: list, campaign_paths: list, control_paths: list,
                 frozen_envelope: dict | None = None) -> tuple:
    """Seal + validate the full six-artifact campaign. Returns
    (errors, per-campaign recomputed results). The recomputed A/A envelope
    must regenerate the frozen manifest constants exactly."""
    errs: list[str] = []
    aa_arts = _load_sealed("aa", aa_paths, errs)
    campaign_arts = _load_sealed("campaign", campaign_paths, errs)
    control_arts = _load_sealed("control", control_paths, errs)

    for art in aa_arts:
        validate_aa(art, errs)
    results = []
    for art in campaign_arts:
        results.append((art, validate_campaign(art, errs,
                                               envelope=frozen_envelope)))
    for art in control_arts:
        validate_control(art, errs)
    _cross_class("aa", aa_arts, errs)
    _cross_class("campaign", campaign_arts, errs)
    _cross_class("control", control_arts, errs)
    # Cross-CLASS binary seal: the recomputed A/A envelope is a noise
    # threshold ONLY for the benchmark binary that produced the campaign
    # (and control) rows. One binary sha256 across all six artifacts.
    bins = {(a.get("binary") or {}).get("sha256")
            for a in aa_arts + campaign_arts + control_arts}
    _check(len(bins) == 1,
           f"seal: benchmark binary sha256 differs across the A/A, "
           f"campaign and control classes ({sorted(repr(b) for b in bins)}) "
           f"— the A/A envelope does not bound a different binary", errs)

    # The frozen envelope is only valid if the OFFICIAL A/A raw rows
    # regenerate it under the frozen rule.
    frozen = frozen_envelope if frozen_envelope is not None else NOISE
    if len(aa_arts) == 2:
        env, _ = corrected_envelope(aa_arts)
        for k in ("instr", "cycles", "wall"):
            have = env.get(k)
            _check(have is not None,
                   f"envelope: {k} could not be recomputed from the "
                   f"official A/A raw rows", errs)
            if have is not None:
                _check(abs(have - frozen[k]) <=
                       1e-12 * max(1.0, abs(frozen[k])),
                       f"envelope: frozen NOISE[{k}]={frozen[k]!r} does not "
                       f"regenerate from the official A/A raw rows "
                       f"(recomputed {have!r})", errs)
    return errs, results


def validate(aa_paths: list, campaign_paths: list,
             control_paths: list) -> int:
    errs, results = validate_all(aa_paths, campaign_paths, control_paths)
    if errs:
        print("VALIDATION FAILED — fail closed:")
        for e in errs:
            print(f"  - {e}")
        return 1

    env, per_fs = corrected_envelope(
        [json.loads(Path(p).read_text()) for p in aa_paths])
    thr = _thresholds(NOISE)
    print("VALIDATION PASS (six-artifact campaign seal)")
    print(f"  A/A envelope recomputed from official raw rows: "
          f"instr={env.get('instr'):.6f} cycles={env.get('cycles'):.6f} "
          f"wall={env.get('wall'):.6f} — matches the frozen manifest")
    for fs, sess in sorted(per_fs.items()):
        print(f"    [{fs}] max-cell p90: " +
              " ".join(f"{k}={sess.get(k, float('nan')):.6f}"
                       for k in ("instr", "cycles", "wall")))
    print(f"  materiality thresholds (log2): " +
          " ".join(f"{k}={thr[k]:.6f}" for k in ("instr", "cycles", "wall")))
    for art, r in results:
        fs = art["params"]["fs_label"]
        gm = r["gm"]
        mat = r["classification"]
        print(f"\n[{fs}] campaign: {len(mat)} cells, "
              f"GM ratio-of-medians (DESCRIPTIVE): "
              f"instr={gm['gm_normalized_instr']:.4f} "
              f"cycles={gm['gm_normalized_cycles']:.4f} "
              f"wall={gm['gm_normalized_wall']:.4f}")
        for k in ("instr", "cycles", "wall"):
            n_imp = sum(1 for v in mat.values()
                        if v[k] == "material_improvement")
            n_reg = sum(1 for v in mat.values()
                        if v[k] == "material_regression")
            print(f"  DECISION {k}: {n_imp} material improvements, "
                  f"{n_reg} material regressions, "
                  f"{len(mat) - n_imp - n_reg} within band "
                  f"(paired-median rule, threshold ratio "
                  f"<={2 ** -thr[k]:.4f})")
    return 0


# ---------------------------------------------------------------------------
# Self test — clean fixtures must PASS, every mutation must FAIL CLOSED.
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
        "env": {"filesystem": {"input": {"type": fs},
                               "output": {"type": fs}}},
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
        _embed_paired_recorded(art, LABELS)
    return art


def _embed_derived(art: dict) -> None:
    """Embed the RECOMPUTED descriptive derived/gm in the runner's exact
    shape — the fixture must look like an honest artifact; tampering is
    detected by the validator's independent recomputation."""
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


def _embed_paired_recorded(art: dict, labels: list[str]) -> None:
    """Embed the recomputed pooled paired fields under the freeze-era names
    (what an honest freeze-era runner would have recorded)."""
    scratch: list[str] = []
    cells = _paired_rounds(art["rows"], labels, scratch, "fixture")
    art["paired_effects"] = {
        m: _freeze_era_effects(ds)
        for m, ds in _freeze_era_pooled(cells).items()}


def _aa_embed_effects(art: dict) -> None:
    _embed_paired_recorded(art, AA_LABELS)


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
    if which == "clean":
        pass
    elif which == "drop_row":
        art["rows"].pop(3)
    elif which == "drop_cell":
        _drop_cell(art, "r1", (4096, 1, 2))
    elif which == "drop_cell_resync":
        _drop_cell(art, "r1", (4096, 1, 2))
        _embed_derived(art)
        _embed_paired_recorded(art, LABELS)
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
    elif which == "tamper_r1_descriptive_only":
        # Tamper ONE round's R1 counter and re-embed every DESCRIPTIVE
        # median-derived value, but leave the recorded paired evidence
        # stale — the recomputed paired fields no longer match, proving
        # the paired recomputation is the tamper anchor for round-level
        # edits that descriptive medians would absorb.
        for r in art["rows"]:
            if (r["candidate"] == "r1" and r["round"] == 4 and
                    (r["buffer_size"], r["pipeline_depth"],
                     r["request_capacity"]) == (4096, 1, 128)):
                r["instructions_user"] *= 0.5
                break
        _embed_derived(art)
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
    elif which == "swap_backend":
        # An r1 row carrying r0's backend: the candidate/backend binding
        # is broken while membership, order and every statistic stay
        # intact.
        for r in art["rows"]:
            if r["candidate"] == "r1":
                r["backend"] = "uring-r0"
                break
    elif which == "rehome_row_experiment":
        # This PASSED under the old three-id row whitelist; rows must
        # carry exactly the campaign experiment id.
        art["rows"][8]["experiment"] = "TAX-0-COPY-AB-1-CONTROL"
    elif which == "tamper_row_file_bytes":
        # Paired normalization divides by row file_bytes.
        art["rows"][10]["file_bytes"] = FILE_BYTES // 2
    elif which == "tamper_row_filesystem":
        art["rows"][12]["filesystem"] = "ext4"
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
    elif which == "swap_backend":
        # r0b carrying r1's backend: both A/A arms are r0-backend runs by
        # frozen design; a backend swap fakes candidate identity, and the
        # rows must fail even though every statistic still reproduces.
        for r in art["rows"]:
            if r["candidate"] == "r0b":
                r["backend"] = "uring-r1"
                break
    else:
        raise SystemExit(f"unknown aa mutation {which}")
    return art


def _seal_six() -> dict:
    """Six-fixture campaign: one tmpfs + one btrfs per category, all
    internally honest (descriptive AND paired recorded fields embedded)."""
    cells = sorted((b, p, c) for b in BUFFERS for p in DEPTHS
                   for c in [C_NEAR[p]] + C_FIXED)
    camp = {fs: _fixture(cells, LABELS, CAMPAIGN_REPS, "tax0copyab", fs=fs)
            for fs in ALLOWED_FS}
    aa = {fs: _fixture(AA_CELLS, AA_LABELS, AA_REPS, "tax0copyab-aa", fs=fs,
                       experiment="TAX-0-COPY-AB-1-AA")
          for fs in ALLOWED_FS}
    _aa_embed_effects(aa["tmpfs"])
    _aa_embed_effects(aa["btrfs"])
    ctl = {fs: _fixture([CONTROL_CELL], ["threadpool"], CONTROL_REPS,
                        "tax0copyab-control", fs=fs,
                        experiment="TAX-0-COPY-AB-1-CONTROL")
           for fs in ALLOWED_FS}
    return {"aa": [aa["tmpfs"], aa["btrfs"]],
            "campaign": [camp["tmpfs"], camp["btrfs"]],
            "control": [ctl["tmpfs"], ctl["btrfs"]]}


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

    def expect_seal_reject(label, six, env):
        nonlocal failures
        errs, _ = validate_all(six["aa"], six["campaign"], six["control"],
                               frozen_envelope=env)
        if errs:
            print(f"  seal mutation {label}: rejected as expected "
                  f"({len(errs)} finding(s))")
        else:
            print(f"  seal mutation {label}: NOT DETECTED (validator bug)")
            failures += 1

    # --- campaign fixture (clean must pass) ---
    cells = sorted((b, p, c) for b in BUFFERS for p in DEPTHS
                   for c in [C_NEAR[p]] + C_FIXED)
    base = _fixture(cells, LABELS, CAMPAIGN_REPS, "tax0copyab")
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
            "tamper_instructions", "tamper_r1_descriptive_only",
            "tamper_normalized_ratio", "tamper_gm",
            "tamper_order", "tamper_q",
            "swap_backend", "rehome_row_experiment",
            "tamper_row_file_bytes", "tamper_row_filesystem"]
    for m in muts:
        art = _mutate_campaign(base, m)
        expect_reject(m, validate_campaign, art)

    # --- paired rule vs descriptive rule MUST disagree here ---
    # 5 band rounds (R1 = 0.99 x R0) + 4 deeply skewed rounds (R0 slow,
    # R1 = 0.48 x R0). ratio-of-arm-medians lands on a skewed arm pair
    # (log2 = -0.0496 <= -thr -> would classify MATERIAL IMPROVEMENT);
    # the paired median (5 of 9 rounds at -0.0145) stays WITHIN BAND.
    # The validator must report the paired result.
    dis = _mutate_campaign(base, "clean")
    band_r0 = [1.2e9, 1.22e9, 1.24e9, 1.26e9, 1.28e9]
    skew_r0, skew_r1 = 1.25e9, 0.60e9
    for r in dis["rows"]:
        if (r["buffer_size"], r["pipeline_depth"], r["request_capacity"]) \
                != (4096, 1, 128):
            continue
        if r["round"] < 5:
            r["instructions_user"] = band_r0[r["round"]] * \
                (0.99 if r["candidate"] == "r1" else 1.0)
        else:
            r["instructions_user"] = skew_r1 if r["candidate"] == "r1" \
                else skew_r0
    _embed_derived(dis)
    _embed_paired_recorded(dis, LABELS)
    errs = []
    res = validate_campaign(dis, errs)
    cell = "B=4096,P=1,C=128"
    desc_log2 = math.log2(1.2078e9 / 1.25e9)
    thr_instr = max(2.0 * NOISE["instr"], MATERIALITY_FLOOR_LOG2)
    if errs:
        print(f"disagreement fixture FAILED (validator/fixture bug): {errs[:3]}")
        failures += 1
    elif res["classification"][cell]["instr"] != "within_band":
        print(f"disagreement fixture: validator classified "
              f"{res['classification'][cell]['instr']} — it did NOT use the "
              f"paired-median rule (validator bug)")
        failures += 1
    elif not desc_log2 <= -thr_instr:
        print("disagreement fixture: descriptive statistic no longer "
              "disagrees — fixture needs re-tuning")
        failures += 1
    else:
        print(f"disagreement fixture: ratio-of-medians says "
              f"material_improvement (log2 {desc_log2:.4f}), paired median "
              f"says within_band ({res['paired'][cell]['instr']['paired_median_log2']:.4f}) "
              f"— validator used the PAIRED rule: PASS")

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
              "tamper_order", "swap_backend"]:
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

    # --- six-artifact campaign seal ---
    six = _seal_six()
    fx_env, fx_per_fs = corrected_envelope(six["aa"])
    errs, _ = validate_all(six["aa"], six["campaign"], six["control"],
                           frozen_envelope=fx_env)
    if errs:
        print("seal clean six-fixture FAILED (validator/fixture bug):")
        for e in errs:
            print(f"  - {e}")
        failures += 1
    else:
        print("seal clean six-fixture: PASS")
    seal_muts = {}

    def sealed(label):
        art6 = copy.deepcopy(six)
        seal_muts[label] = art6
        return art6

    a = sealed("drop_aa_tmpfs"); a["aa"] = a["aa"][:1]
    a = sealed("drop_aa_btrfs"); a["aa"] = a["aa"][1:]
    a = sealed("drop_campaign_tmpfs"); a["campaign"] = a["campaign"][:1]
    a = sealed("drop_campaign_btrfs"); a["campaign"] = a["campaign"][1:]
    a = sealed("drop_control_tmpfs"); a["control"] = a["control"][:1]
    a = sealed("drop_control_btrfs"); a["control"] = a["control"][1:]
    a = sealed("duplicate_tmpfs"); a["aa"] = [a["aa"][0], a["aa"][0]]
    a = sealed("unknown_filesystem")
    a["campaign"][1]["params"]["fs_label"] = "ext4"
    a["campaign"][1]["env"]["filesystem"]["input"]["type"] = "ext4"
    a["campaign"][1]["env"]["filesystem"]["output"]["type"] = "ext4"
    a = sealed("wrong_kind"); a["campaign"][1] = copy.deepcopy(six["aa"][1])
    a = sealed("wrong_experiment")
    a["campaign"][1]["params"]["experiment"] = "TAX-0-COPY-AB-9"
    a = sealed("mismatched_source_hash_across_fs")
    for r in a["campaign"][1]["rows"]:
        r["source_sha256"] = "e" * 64
    a = sealed("mismatched_binary_sha")
    a["campaign"][1]["binary"]["sha256"] = "c" * 64
    a = sealed("mismatched_binary_sha_aa_vs_campaign")
    # BOTH A/A sessions re-stamped consistently: the within-class binary
    # checks still pass, only the cross-class seal can catch this.
    a["aa"][0]["binary"]["sha256"] = "c" * 64
    a["aa"][1]["binary"]["sha256"] = "c" * 64
    a = sealed("paired_row_swap_breaks_round_pair")
    a["campaign"][1]["rows"][5]["candidate"] = "r1" if \
        a["campaign"][1]["rows"][5]["candidate"] == "r0" else "r0"
    a = sealed("tamper_r1_descriptive_only")
    a["campaign"][1] = _mutate_campaign(six["campaign"][1],
                                        "tamper_r1_descriptive_only")
    a = sealed("tamper_aa_envelope_rows")
    for r in a["aa"][0]["rows"]:
        if r["candidate"] == "r0b" and r["buffer_size"] == 1048576:
            r["instructions_user"] *= 1.4
    for label, art6 in seal_muts.items():
        expect_seal_reject(label, art6, fx_env)

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
    if not (args.aa and args.campaign and args.control):
        ap.error("the campaign seal requires --aa, --campaign AND "
                 "--control, each with EXACTLY one tmpfs + one btrfs "
                 "artifact")
    return validate(args.aa, args.campaign, args.control)


if __name__ == "__main__":
    sys.exit(main())
