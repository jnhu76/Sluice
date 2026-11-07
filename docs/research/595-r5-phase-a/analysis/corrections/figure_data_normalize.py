#!/usr/bin/env python3
"""Normalize a copy of the frozen R4 figures/figure-data.csv (READ-ONLY input).

Problems fixed in the emitted copy (none of them numeric; every value in the
frozen CSV already matches analysis/MAIN_SUMMARY.json — verified 2026-11-06,
see C7-2-figure-data-columns.md):

 P1 `cell` column: F1/F4 rows carry cell names (MAIN-W03-D16, ...); F3 rows
    carry the *case* string "W03-uring-4k-d16" for BOTH the MAIN cell (6
    contrasts) and the B-SUPP cell (R-B) — two different source cells share
    one string; all 96 F2/F5 rows leave `cell` empty.
 P2 `arm` column: F3 and F5 panel=segment_cpu rows write contrast labels
    ("R-B", "S-R", "R-A0", ...) into `arm`; F2 present_size rows mix
    action-owner ("S") with data-source run ("SHADOW") semantics.
 P3 `segment` column: empty for F1/F3/F4, holds the awareness dataset /
    TRAJECTORY phase name for F2/F5 — overloaded with the "cell" role.
 P4 `n` column silently changes meaning: matched blocks (24) in
    F1/F3/F4/F5b vs decision-line counts (94..60000) in F2/F5a.

Output schema adds: source_cell, contrast, dataset, n_unit; keeps every
original column and value byte-for-byte. Input is never modified.
"""
import csv
import os
import sys

SRC = "/home/jnhu/sluice-595-e5/r4/figures/figure-data.csv"
DST = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "figure-data.normalized.csv")

F3_CELL = {"R-B": "B-SUPP-W03-D16"}  # every other F3 series -> MAIN-W03-D16
REAL_ARMS = {"A0", "R", "S", "SNI", "SNP", "Q2", "Q8", "SHADOW", "B"}


def main() -> int:
    with open(SRC, newline="") as f:
        rows = list(csv.DictReader(f))
    out_fields = ["figure", "panel", "series", "cell", "arm", "segment",
                  "metric", "stat", "value", "n", "unit", "note",
                  "source_cell", "contrast", "dataset", "n_unit"]
    fixed_p1 = fixed_p2 = 0
    with open(DST, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=out_fields)
        w.writeheader()
        for r in rows:
            n = dict(r)
            n["source_cell"] = r["cell"]
            n["contrast"] = ""
            n["dataset"] = r["segment"]
            n["n_unit"] = ""
            if r["figure"] == "F3":
                cell = F3_CELL.get(r["series"], "MAIN-W03-D16")
                if r["cell"] != cell:
                    n["source_cell"] = cell
                    fixed_p1 += 1
            if r["figure"] == "F5" and r["panel"] == "segment_cpu":
                n["source_cell"] = "TRAJECTORY"
            if r["arm"] not in REAL_ARMS:
                n["contrast"] = r["arm"]
                n["arm"] = ""
                fixed_p2 += 1
            if r["figure"] in ("F2", "F5") and r["panel"] in (
                    "branch_share", "quantum_share", "state",
                    "changed_action", "present_size"):
                n["n_unit"] = "decision_lines"
            else:
                n["n_unit"] = "matched_blocks"
            w.writerow(n)
    print(f"rows={len(rows)} cell_corrections={fixed_p1} "
          f"arm_column_disambiguations={fixed_p2} -> {DST}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
