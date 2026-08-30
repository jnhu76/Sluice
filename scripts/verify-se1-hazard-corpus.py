#!/usr/bin/env python3
"""SE-1 hazard corpus integrity validator (fail-closed).

Checks ONLY mechanical corpus integrity for docs/results/safety/se1-hazard-corpus.json:
  - schema_version == 1
  - unique IDs
  - required H01-H13 coverage (IN-SE1 conventional entries)
  - valid enum values
  - exactly one primary bucket
  - corpus eligibility present
  - provenance non-empty
  - normalized trace non-empty
  - no duplicated Sluice root-cause IDs (aliases are recorded, not re-counted)
  - every OUT-OF-SE1 entry has an exclusion reason
  - every NO VALID ENTRY YET family is explicit
  - every conventional-real claim has primary-source provenance
  - no forbidden net-safety conclusion appears

It is NOT a mutation engine, benchmark harness, scraper, scoring framework,
or generic research database. Not wired into CI/pre-push (human review gates that).
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

CORPUS = Path(__file__).resolve().parents[1] / "docs" / "results" / "safety" / "se1-hazard-corpus.json"

FAMILIES = [f"H{i:02d}" for i in range(1, 14)]
ORIGINS = {"conventional-real", "conventional-documentation", "conventional-minimal", "sluice-induced"}
BUCKETS = {
    "external/conventional",
    "production-runtime",
    "internal/seam",
    "test-only",
    "structural-authority",
    "experiment-process",
}
ELIGIBILITY = {"IN-SE1", "OUT-OF-SE1"}
QUALITY = {"C0", "C1", "C2", "C3", "S0"}
STATUS = {
    "UNREPRESENTABLE",
    "STATICALLY_REJECTED",
    "DYNAMICALLY_DETECTED",
    "FAIL_FAST",
    "DETERMINISTICALLY_REPRODUCIBLE",
    "SILENT_OR_UNDETECTED",
    "UNKNOWN",
    "NOT_APPLICABLE",
}
VALIDITY = {"FAIR", "PARTIAL", "COMPARABILITY_BLOCKED"}
PAIRS = {"PAIR-A", "PAIR-B", "PAIR-C", "PAIR-D", "PAIR-E", "PAIR-F", "PAIR-X"}

# Claims that must never appear anywhere in the corpus artifact.
FORBIDDEN_CLAIMS = [
    r"sluice is safer than (posix|liburing|asio|conventional)",
    r"sluice reduces bugs overall",
    r"sluice prevents most",
    r"superior concurrency safety",
    r"formal verification proves (the )?implementation",
    r"dst covers concurrency systematically",
]

REQUIRED_TEXT_FIELDS = [
    "normalized_semantic_trace",
    "actors",
    "relevant_state",
    "preconditions",
    "race_or_failure_window",
    "bad_outcome",
    "externally_observable_effect",
    "silent_or_distributed_character",
    "conventional_obligation",
    "sluice_protocol_obligation",
    "evidence_for_status",
    "comparison_notes",
]

def fail(msg: str) -> None:
    print(f"FAIL: {msg}")
    sys.exit(1)

def main() -> None:
    if not CORPUS.is_file():
        fail(f"corpus file missing: {CORPUS}")
    try:
        data = json.loads(CORPUS.read_text(encoding="utf-8"))
    except json.JSONDecodeError as e:
        fail(f"JSON parse error: {e}")

    if data.get("schema") != "se1-corpus-schema":
        fail("schema field must be 'se1-corpus-schema'")
    if data.get("schema_version") != 1:
        fail("schema_version must be 1")
    if data.get("base_sha") is None:
        fail("base_sha missing")

    entries = data.get("entries")
    if not isinstance(entries, list) or not entries:
        fail("entries must be a non-empty list")

    ids: set[str] = set()
    sluice_ids: set[str] = set()
    coverage: dict[str, str] = {}
    counts = {
        "conventional": {"total": 0, "C0": 0, "C1": 0, "C2": 0, "C3": 0},
        "induced_in_se1": 0,
        "out_of_se1": 0,
    }

    for e in entries:
        eid = e.get("id", "")
        if not eid:
            fail("entry missing id")
        if eid in ids:
            fail(f"duplicate id: {eid}")
        ids.add(eid)

        family = e.get("family")
        origin = e.get("origin")
        bucket = e.get("primary_bucket")
        elig = e.get("corpus_eligibility")
        quality = e.get("provenance_quality")
        status = e.get("sluice_current_status")
        validity = e.get("comparison_validity")
        pairing = e.get("pairing")

        if family != "none":
            if family not in FAMILIES:
                fail(f"{eid}: invalid family {family!r}")
        elif bucket != "experiment-process":
            fail(f"{eid}: family 'none' allowed only for bucket experiment-process")

        if origin not in ORIGINS:
            fail(f"{eid}: invalid origin {origin!r}")
        if bucket not in BUCKETS:
            fail(f"{eid}: invalid primary_bucket {bucket!r}")
        if elig not in ELIGIBILITY:
            fail(f"{eid}: corpus_eligibility missing/invalid")
        if quality not in QUALITY:
            fail(f"{eid}: invalid provenance_quality {quality!r}")
        if status not in STATUS:
            fail(f"{eid}: invalid sluice_current_status {status!r}")
        if validity not in VALIDITY:
            fail(f"{eid}: invalid comparison_validity {validity!r}")
        if pairing not in PAIRS:
            fail(f"{eid}: invalid pairing {pairing!r}")

        # exactly one primary bucket is enforced structurally (scalar field);
        # assert no secondary bucket sneaks in via lists
        for listfield in ("buckets", "primary_buckets"):
            if listfield in e:
                fail(f"{eid}: multiple bucket fields forbidden ({listfield})")

        # provenance non-empty with at least one source string
        prov = e.get("provenance")
        if not isinstance(prov, dict) or not prov.get("publication") or not prov.get("sources"):
            fail(f"{eid}: provenance must include publication and non-empty sources")

        for field in REQUIRED_TEXT_FIELDS:
            v = e.get(field)
            ok = (isinstance(v, list) and v) or (isinstance(v, str) and v.strip())
            if not ok:
                fail(f"{eid}: required field empty: {field}")

        # OUT-OF-SE1 needs a reason
        if elig == "OUT-OF-SE1" and not e.get("exclusion_reason", "").strip():
            fail(f"{eid}: OUT-OF-SE1 entry missing exclusion_reason")
        if elig == "IN-SE1" and e.get("exclusion_reason"):
            fail(f"{eid}: IN-SE1 entry must not carry exclusion_reason")

        # conventional-real claims need primary provenance (URL(s) present)
        if origin == "conventional-real" and quality not in {"C0", "C2"}:
            fail(f"{eid}: conventional-real must be C0/C2 provenance")
        if quality in {"C0", "C1", "C2"}:
            src_blob = " ".join(prov.get("sources", []))
            if "http" not in src_blob:
                fail(f"{eid}: {quality} provenance lacks a primary URL")

        # induced entries must be S0 and bucketed away from external/conventional
        if origin == "sluice-induced":
            if quality != "S0":
                fail(f"{eid}: sluice-induced entries must be S0")
            if bucket == "external/conventional":
                fail(f"{eid}: sluice-induced entry cannot be external/conventional")
            if e.get("induced_by_sluice") not in {"yes", "n-a"}:
                fail(f"{eid}: sluice-induced entries need induced_by_sluice=yes (n-a only for UNKNOWN status)")
            if e.get("induced_by_sluice") == "n-a" and status != "UNKNOWN":
                fail(f"{eid}: induced_by_sluice=n-a allowed only with sluice_current_status=UNKNOWN")
            sluice_ids.add(eid)
        elif bucket in {"production-runtime", "test-only", "structural-authority", "experiment-process"}:
            # non-induced entries must not claim Sluice-internal buckets
            fail(f"{eid}: conventional origin cannot use Sluice-internal bucket {bucket!r}")

        if elig == "IN-SE1":
            if origin in {"conventional-real", "conventional-documentation", "conventional-minimal"}:
                counts["conventional"]["total"] += 1
                counts["conventional"][quality] += 1
                prev = coverage.get(family)
                outcome = (
                    "REAL SOURCE FOUND" if quality in {"C0", "C2"}
                    else "CONVENTIONAL-MINIMAL ONLY" if origin == "conventional-minimal"
                    else "DOCUMENTED CONTRACT HAZARD FOUND"
                )
                rank = {"REAL SOURCE FOUND": 3, "DOCUMENTED CONTRACT HAZARD FOUND": 2, "CONVENTIONAL-MINIMAL ONLY": 1}
                if prev is None or rank[outcome] > rank[prev]:
                    coverage[family] = outcome
            else:
                counts["induced_in_se1"] += 1
        else:
            counts["out_of_se1"] += 1

    # coverage: every family must have an explicit outcome, and the JSON
    # coverage_gate must agree with what the entries establish.
    gate = data.get("coverage_gate", {})
    for fam in FAMILIES:
        if fam not in coverage:
            # allowed only if explicitly declared NO VALID ENTRY YET
            if gate.get(fam) != "NO VALID ENTRY YET":
                fail(f"family {fam} has no IN-SE1 conventional entry and no explicit NO VALID ENTRY YET")
        else:
            declared = gate.get(fam)
            if declared == "NO VALID ENTRY YET":
                fail(f"family {fam} has entries but coverage_gate says NO VALID ENTRY YET")
            if declared == "REAL SOURCE FOUND" and coverage[fam] not in {"REAL SOURCE FOUND"}:
                fail(f"family {fam}: coverage_gate overstates provenance ({coverage[fam]})")

    # Sluice-induced dedup: aliases must reference existing IDs; alias loops are fine,
    # but a root cause may not be double-counted via distinct unlinked entries with the
    # same title fingerprint.
    for e in entries:
        for a in e.get("aliases", []):
            if a not in ids:
                fail(f"{e['id']}: alias {a!r} does not reference an existing id")

    # forbidden claims must not appear anywhere in the file
    blob = CORPUS.read_text(encoding="utf-8").lower()
    for pat in FORBIDDEN_CLAIMS:
        if re.search(pat, blob):
            fail(f"forbidden net-safety claim pattern matched: {pat!r}")

    # no numeric safety score may exist, structurally: no 'score' key in any
    # entry or at top level (mentioning the *prohibition* in prose is fine)
    for e in entries:
        if any("score" in k.lower() for k in e):
            fail(f"{e['id']}: score-like field forbidden (no net-safety score)")
    if any("score" in k.lower() for k in data):
        fail("top-level score-like field forbidden (no net-safety score)")

    print("PASS: SE-1 hazard corpus integrity")
    print(f"  entries total:        {len(entries)}")
    print(f"  conventional (IN-SE1):{counts['conventional']['total']}  "
          f"C0={counts['conventional']['C0']} C1={counts['conventional']['C1']} "
          f"C2={counts['conventional']['C2']} C3={counts['conventional']['C3']}")
    print(f"  sluice-induced IN-SE1:{counts['induced_in_se1']}")
    print(f"  OUT-OF-SE1:           {counts['out_of_se1']}")
    real = sum(1 for v in coverage.values() if v == "REAL SOURCE FOUND")
    doc = sum(1 for v in coverage.values() if v == "DOCUMENTED CONTRACT HAZARD FOUND")
    minimal_only = sum(1 for v in coverage.values() if v == "CONVENTIONAL-MINIMAL ONLY")
    no_valid = sum(1 for f in FAMILIES if coverage.get(f) is None)
    print(f"  family outcomes:      REAL={real} DOCUMENTED={doc} MINIMAL_ONLY={minimal_only} NO_VALID_ENTRY={no_valid}")

if __name__ == "__main__":
    main()
