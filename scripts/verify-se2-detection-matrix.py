#!/usr/bin/env python3
"""verify-se2-detection-matrix.py — SE-2 matrix structural gate (task §24).

Narrow validator for docs/results/safety/se2-detection-matrix.json:
  - row population is exactly the frozen SE-1 population (no missing / extra IDs)
  - SB-10 and other OUT-OF-SE1 records never enter the denominator
  - every row carries L0..L10 on the sluice side; conventional rows also on the
    conventional side
  - only the frozen status vocabulary is used
  - positive statuses (PREVENTS/REJECTS/DETECTS/REPRODUCES) require evidence
  - MISSES requires an evidence/observation note
  - NOT_TESTED / BLOCKED / UNKNOWN require a reason
  - conventional vs Sluice probe sections are present and distinct
  - every conventional-origin row carries a `comparison` block whose
    comparison_basis / migration_class / direct_ts1b_support are consistent
    (review 5060477073 Blockers 1+3):
      * direct_ts1b_support == true  requires comparison_basis ==
        current-executed AND an executed conventional probe source
      * current-documented / historical-fixed / blocked rows can never be
        direct support (a historical broken kernel is not a baseline)
    with per-subshape consistency when subshapes are present
  - ts1b_adjudication partitions all 13 conventional rows exactly, and its
    support_rows equal the set of rows with direct_ts1b_support == true
  - docs/verification/se2-detection-matrix.md is DERIVED from this JSON:
    migration-class counts, direct-support row list, T-S1b status string, and
    per-family pair-table classes must match (markdown counts have no
    authority of their own)
  - no score-like fields, no net-safety arithmetic, no "Sluice is safer" claim
  - bucket counts match the SE-1 corpus

Exits 0 with a PASS summary line, or 1 with the list of violations.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MATRIX = ROOT / "docs/results/safety/se2-detection-matrix.json"
SE1 = ROOT / "docs/results/safety/se1-hazard-corpus.json"
MD = ROOT / "docs/verification/se2-detection-matrix.md"

LAYERS = [f"L{i}" for i in range(11)]
POSITIVE = {"PREVENTS", "REJECTS", "DETECTS", "REPRODUCES"}
NEEDS_REASON = {"NOT_TESTED", "BLOCKED", "UNKNOWN"}
ALLOWED = POSITIVE | NEEDS_REASON | {"MISSES", "NOT_APPLICABLE", "NOT_MODELED"}
SCORE_KEY_RE = re.compile(r"score|win_rate|percent|rating|net_?safety_value", re.I)
CLAIM_RE = re.compile(r"sluice is safer|safer than|net [-+]?\d|wins \d+ hazards", re.I)
COMPARISON_BASES = {"current-executed", "current-documented", "historical-fixed", "blocked"}
MIGRATION_CLASSES = {"M0", "M1", "M2", "M3", "M4", "M5", "MX"}
DIRECT_REQUIRES = "current-executed"


def check_comparison(rid: str, where: str, c: dict, has_executed_conv_probe: bool, problems: list[str]) -> None:
    basis = c.get("comparison_basis")
    if basis not in COMPARISON_BASES:
        problems.append(f"{rid}/{where}: illegal comparison_basis {basis!r}")
    cls = c.get("migration_class")
    if cls not in MIGRATION_CLASSES:
        problems.append(f"{rid}/{where}: illegal migration_class {cls!r}")
    direct = c.get("direct_ts1b_support")
    if not isinstance(direct, bool):
        problems.append(f"{rid}/{where}: direct_ts1b_support must be a boolean")
        return
    if direct:
        if basis != DIRECT_REQUIRES:
            problems.append(
                f"{rid}/{where}: direct_ts1b_support=true requires "
                f"comparison_basis={DIRECT_REQUIRES} (got {basis!r})"
            )
        if not has_executed_conv_probe:
            problems.append(
                f"{rid}/{where}: direct_ts1b_support=true requires an executed "
                f"conventional probe (conventional_probe.source must be set)"
            )
    for s in c.get("subshapes", []) or []:
        check_comparison(rid, f"{where}.subshape[{s.get('name', '?')!r}]", s,
                         has_executed_conv_probe, problems)
    if direct and c.get("subshapes"):
        if not any(s.get("direct_ts1b_support") for s in c["subshapes"]):
            problems.append(f"{rid}/{where}: row direct=true needs a direct subshape")
    if (not direct) and any(s.get("direct_ts1b_support") for s in c.get("subshapes", []) or []):
        problems.append(f"{rid}/{where}: row direct=false contradicts a direct subshape")


def se1_population() -> tuple[list[str], dict[str, str]]:
    data = json.loads(SE1.read_text())
    rows: list[str] = []
    buckets: dict[str, str] = {}
    for e in data["entries"]:
        if e.get("corpus_eligibility") == "IN-SE1" and e.get("entry_role") == "population-case":
            rows.append(e["id"])
            buckets[e["id"]] = e.get("primary_bucket", "")
    return rows, buckets


def main() -> int:
    problems: list[str] = []
    m = json.loads(MATRIX.read_text())
    expected_ids, expected_buckets = se1_population()

    if m.get("schema") != "se2-detection-matrix-schema":
        problems.append(f"schema field is {m.get('schema')!r}")
    if m.get("schema_version") != 1:
        problems.append("schema_version must be 1")

    rows = m.get("rows", [])
    row_ids = [r.get("corpus_id") for r in rows]

    missing = sorted(set(expected_ids) - set(row_ids))
    extra = sorted(set(row_ids) - set(expected_ids))
    if missing:
        problems.append(f"missing denominator rows: {missing}")
    if extra:
        problems.append(f"extra denominator rows (SE-1 denominator must not change): {extra}")
    if len(row_ids) != len(set(row_ids)):
        problems.append("duplicate corpus_id rows")

    # bucket counts must match SE-1 exactly
    got_buckets = {r.get("corpus_id"): r.get("bucket", "") for r in rows}
    for rid in expected_ids:
        if got_buckets.get(rid) != expected_buckets.get(rid):
            problems.append(
                f"bucket mismatch for {rid}: matrix={got_buckets.get(rid)!r} se1={expected_buckets.get(rid)!r}"
            )

    def check_cell(rid: str, side: str, layer: str, cell: dict) -> None:
        status = cell.get("status")
        if status not in ALLOWED:
            problems.append(f"{rid}/{side}/{layer}: illegal status {status!r}")
            return
        if status in POSITIVE and not (cell.get("evidence") or "").strip():
            problems.append(f"{rid}/{side}/{layer}: {status} requires evidence")
        if status == "MISSES" and not (
            (cell.get("evidence") or "").strip() or (cell.get("notes") or "").strip()
        ):
            problems.append(f"{rid}/{side}/{layer}: MISSES requires an evidence/observation note")
        if status in NEEDS_REASON and not (cell.get("notes") or "").strip():
            problems.append(f"{rid}/{side}/{layer}: {status} requires a reason in notes")

    sb10 = m.get("sb10_exploratory", {})
    for r in rows:
        rid = r["corpus_id"]
        layers = r.get("layers", {})
        if "sluice" not in layers:
            problems.append(f"{rid}: missing sluice layer side")
            continue
        for layer in LAYERS:
            cell = layers["sluice"].get(layer)
            if not isinstance(cell, dict):
                problems.append(f"{rid}/sluice: missing {layer} cell")
            else:
                check_cell(rid, "sluice", layer, cell)
        is_conventional_row = r.get("origin", "").startswith("conventional")
        if is_conventional_row:
            if "conventional" not in layers:
                problems.append(f"{rid}: conventional-origin row missing conventional layer side")
            else:
                for layer in LAYERS:
                    cell = layers["conventional"].get(layer)
                    if not isinstance(cell, dict):
                        problems.append(f"{rid}/conventional: missing {layer} cell")
                    else:
                        check_cell(rid, "conventional", layer, cell)
        else:
            if "conventional" in layers:
                problems.append(f"{rid}: induced row must not carry a conventional side")
        # probe sections distinct and present
        if not isinstance(r.get("sluice_probe"), dict):
            problems.append(f"{rid}: missing sluice_probe section")
        if is_conventional_row and not isinstance(r.get("conventional_probe"), dict):
            problems.append(f"{rid}: conventional-origin row missing conventional_probe section")
        if not is_conventional_row and r.get("conventional_probe") is not None:
            problems.append(f"{rid}: induced row must not carry a conventional_probe")
        # comparison block (review 5060477073 Blockers 1+3)
        if is_conventional_row:
            comp = r.get("comparison")
            if not isinstance(comp, dict):
                problems.append(f"{rid}: missing comparison block")
            else:
                executed = bool((r.get("conventional_probe") or {}).get("source"))
                check_comparison(rid, "comparison", comp, executed, problems)

    # SB-10 must not be a denominator row
    if "SE1-SB-10" in row_ids:
        problems.append("SE1-SB-10 must stay OUT-OF-SE1 (exploratory section only)")
    if not sb10.get("denominator_impact", "").startswith("NONE"):
        problems.append("sb10_exploratory.denominator_impact must be NONE")

    # T-S1b adjudication must partition the conventional rows exactly and must
    # agree with the per-row direct flags (Blocker 3: the claim lives HERE,
    # in the JSON authority).
    conv_rows = [r for r in rows if r.get("origin", "").startswith("conventional")]
    conv_ids = {r["corpus_id"] for r in conv_rows}
    adj = m.get("ts1b_adjudication")
    direct_ids: set[str] = set()
    for r in conv_rows:
        c = r.get("comparison") or {}
        if c.get("direct_ts1b_support") is True:
            direct_ids.add(r["corpus_id"])
    md_text = ""
    if isinstance(adj, dict):
        sup = set(adj.get("support_rows", []))
        cnd = set(adj.get("claimed_not_direct_rows", []))
        cev = set(adj.get("counterevidence_rows", []))
        unknown = (sup | cnd | cev) - conv_ids
        if unknown:
            problems.append(f"ts1b_adjudication lists non-conventional rows: {sorted(unknown)}")
        overlap = (sup & cnd) | (sup & cev) | (cnd & cev)
        if overlap:
            problems.append(f"ts1b_adjudication partition overlaps: {sorted(overlap)}")
        if sup | cnd | cev != conv_ids:
            problems.append(
                "ts1b_adjudication must partition all 13 conventional rows "
                f"(missing: {sorted(conv_ids - (sup | cnd | cev))})"
            )
        if sup != direct_ids:
            problems.append(
                f"ts1b_adjudication.support_rows {sorted(sup)} != rows with "
                f"direct_ts1b_support=true {sorted(direct_ids)}"
            )
        if not (adj.get("status") or "").strip():
            problems.append("ts1b_adjudication.status is required")
        md_text = MD.read_text() if MD.exists() else ""
        status = (adj.get("status") or "").strip()
        if status and status not in md_text:
            problems.append(
                f"markdown T-S1b section does not state the JSON status {status!r} "
                "(markdown counts have no authority of their own)"
            )
    else:
        problems.append("missing ts1b_adjudication block")

    # Markdown derivation (Blocker 3): migration counts, direct-support list,
    # and per-family pair-table classes must be exported from this JSON.
    if md_text:
        from collections import Counter
        classes = Counter(
            (r.get("comparison") or {}).get("migration_class")
            for r in conv_rows
            if isinstance(r.get("comparison"), dict)
        )
        for cls in sorted(MIGRATION_CLASSES):
            line = f"{cls}: {classes.get(cls, 0)}"
            if line not in md_text:
                problems.append(f"markdown derived block missing migration count line {line!r}")
        sup_list = ", ".join(sorted(direct_ids))
        if direct_ids and f"direct support rows ({len(direct_ids)}): {sup_list}" not in md_text:
            problems.append(
                "markdown derived block missing direct-support row line: "
                f"direct support rows ({len(direct_ids)}): {sup_list}"
            )
        for r in conv_rows:
            fam = r.get("family", "")
            cls = (r.get("comparison") or {}).get("migration_class")
            if not fam or not cls:
                continue
            row_re = re.compile(rf"^\|\s*{re.escape(fam)}\s*\|.*\|\s*{re.escape(cls)}\s*\|\s*$", re.M)
            if not row_re.search(md_text):
                problems.append(
                    f"markdown pair-table row for {fam} does not end with "
                    f"migration class {cls} as exported from JSON"
                )

    # no score-like fields anywhere; no forbidden claims anywhere
    def scan(obj, path: str) -> None:
        if isinstance(obj, dict):
            for k, v in obj.items():
                if SCORE_KEY_RE.search(str(k)):
                    problems.append(f"score-like field at {path}.{k}")
                scan(v, f"{path}.{k}")
        elif isinstance(obj, list):
            for i, v in enumerate(obj):
                scan(v, f"{path}[{i}]")
        elif isinstance(obj, str):
            if CLAIM_RE.search(obj):
                problems.append(f"forbidden claim text at {path}: {obj[:80]!r}")

    scan(m, "matrix")

    if problems:
        print("FAIL: SE-2 detection matrix")
        for p in problems:
            print(f"  - {p}")
        return 1

    conv = sum(1 for r in rows if r.get("origin", "").startswith("conventional"))
    induced = len(rows) - conv
    print("PASS: SE-2 detection matrix integrity")
    print(f"  rows:                 {len(rows)} (conventional {conv} + induced {induced})")
    print(f"  cells:                sluice {len(rows) * 11}, conventional {conv * 11}")
    print("  SB-10:                OUT-OF-SE1 exploratory, not in denominator")
    if isinstance(adj, dict):
        print(f"  T-S1b status:         {adj.get('status')}")
        print(f"  direct support rows:  {sorted(direct_ids)}")
    print("  comparison blocks:    13 conventional rows, basis/class/direct consistent")
    print("  markdown derivation:  counts, direct list, pair-table classes match JSON")
    print("  score-like fields:    none")
    print("  forbidden claims:     none")
    return 0


if __name__ == "__main__":
    sys.exit(main())
