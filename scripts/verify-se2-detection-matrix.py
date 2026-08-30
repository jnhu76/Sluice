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

LAYERS = [f"L{i}" for i in range(11)]
POSITIVE = {"PREVENTS", "REJECTS", "DETECTS", "REPRODUCES"}
NEEDS_REASON = {"NOT_TESTED", "BLOCKED", "UNKNOWN"}
ALLOWED = POSITIVE | NEEDS_REASON | {"MISSES", "NOT_APPLICABLE", "NOT_MODELED"}
SCORE_KEY_RE = re.compile(r"score|win_rate|percent|rating|net_?safety_value", re.I)
CLAIM_RE = re.compile(r"sluice is safer|safer than|net [-+]?\d|wins \d+ hazards", re.I)


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

    # SB-10 must not be a denominator row
    if "SE1-SB-10" in row_ids:
        problems.append("SE1-SB-10 must stay OUT-OF-SE1 (exploratory section only)")
    if not sb10.get("denominator_impact", "").startswith("NONE"):
        problems.append("sb10_exploratory.denominator_impact must be NONE")

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
    print("  score-like fields:    none")
    print("  forbidden claims:     none")
    return 0


if __name__ == "__main__":
    sys.exit(main())
