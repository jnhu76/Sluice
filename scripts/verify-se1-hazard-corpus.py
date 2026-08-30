#!/usr/bin/env python3
"""SE-1 hazard corpus integrity validator (fail-closed).

Enforces MECHANICAL corpus integrity for
docs/results/safety/se1-hazard-corpus.json:
  - schema/meta fields and top-level vocabulary agreement (closed sets in the
    JSON must equal this validator's closed sets)
  - unique IDs
  - required H01-H13 family coverage from POPULATION CASES ONLY
  - enum validity: origin, primary bucket, corpus eligibility, provenance
    quality, entry role, Sluice status, comparison validity, pairing
  - population law: POPULATION = IN-SE1 AND entry_role == population-case;
    probe companions are excluded from every count and from family coverage
  - probe-companion shape: same_case_as present, target exists, target is a
    population-case, target family matches (unless explicitly justified)
  - population-case shape: same_case_as forbidden
  - related_entries: targets exist (no count effect)
  - the legacy multi-meaning `aliases` field is forbidden
  - provenance shape: sources are structured {url, role, authority} records;
    roles/authorities are closed sets; role<->authority consistency
  - origin<->quality closed relation: conventional-real->{C0,C2},
    conventional-documentation->{C1}, conventional-minimal->{C3},
    sluice-induced->{S0}
  - provenance-quality role contract:
      C0 needs >=1 PRIMARY-AUTHORITY incident record (bug_record or
        official_bug_corpus carrying upstream-primary/official-primary
        authority) AND >=1 upstream_fix source; a supporting-authority
        bug_record annotates but never establishes C0
      C1 needs >=1 official_contract source
      C2 needs >=1 official_bug_corpus source
      S0 needs >=1 repo_evidence source; an IN-SE1 population case requires
        >=1 repo-primary source; repo evidence that is exclusively
        repo-untracked (E4: recorded only in untracked human artifacts)
        makes the entry an UNCONFIRMED CANDIDATE which must be OUT-OF-SE1
      C3 must not claim bug_record/upstream_fix roles
      supporting sources never satisfy the above
  - Sluice-induced population cases: root_cause_key + root_cause_class
    required; duplicate root_cause_key among distinct induced population
    cases fails
  - OUT-OF-SE1 entries carry an exclusion reason
  - every NO VALID ENTRY YET family is explicit, and coverage_gate never
    overstates what entries establish
  - no forbidden net-safety conclusion and no score-like field

This validator does NOT and cannot prove: that a URL's content is truthful or
matches its claimed role; that a source actually describes the claimed bug;
that a normalized trace is semantically supported by its source. Those remain
HUMAN SEMANTIC PROVENANCE REVIEW (docs/verification/se1-hazard-corpus.md).

It is NOT a mutation engine, benchmark harness, scraper, scoring framework,
or generic research database. Wired into the repository mechanical gate
(scripts/gates/pre-push.sh, also run by CI) per SE-1-CORRECTIVE-1 human
authorization: any future change that puts the corpus into an invalid state
fails the gate.
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
ENTRY_ROLES = {"population-case", "probe-companion"}
SOURCE_ROLES = {
    "bug_record",
    "upstream_fix",
    "official_contract",
    "official_bug_corpus",
    "repo_evidence",
    "supporting",
}
SOURCE_AUTHORITIES = {
    "upstream-primary",
    "official-primary",
    "repo-primary",
    "repo-untracked",
    "supporting",
}

# role -> authority values this role may carry (closed consistency contract)
ROLE_AUTHORITY = {
    "bug_record": {"upstream-primary", "official-primary", "supporting"},
    "upstream_fix": {"upstream-primary"},
    "official_contract": {"upstream-primary", "official-primary"},
    "official_bug_corpus": {"upstream-primary", "official-primary"},
    "repo_evidence": {"repo-primary", "repo-untracked"},
    "supporting": SOURCE_AUTHORITIES,
}
# roles that must not appear on C3 (conventional-minimal, no incident claim)
INCIDENT_ROLES = {"bug_record", "upstream_fix"}
# C0 PRIMARY-INCIDENT contract (#245 review 5060124249 FIX 1): the incident
# record itself must be primary-authority; a supporting-authority bug_record
# does not establish a primary incident. upstream_fix is pinned to
# upstream-primary by ROLE_AUTHORITY, so role presence suffices on the fix side.
PRIMARY_INCIDENT_ROLES = {"bug_record", "official_bug_corpus"}
PRIMARY_AUTHORITIES = {"upstream-primary", "official-primary"}

# closed origin<->quality relation: an origin may only claim the quality
# classes its evidence kind can establish (enforced per entry below)
ORIGIN_QUALITY = {
    "conventional-real": {"C0", "C2"},
    "conventional-documentation": {"C1"},
    "conventional-minimal": {"C3"},
    "sluice-induced": {"S0"},
}

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

OUTCOME_RANK = {"REAL SOURCE FOUND": 3, "DOCUMENTED CONTRACT HAZARD FOUND": 2, "CONVENTIONAL-MINIMAL ONLY": 1}


def fail(msg: str) -> None:
    print(f"FAIL: {msg}")
    sys.exit(1)


def outcome_for(quality: str, origin: str) -> str:
    if quality in {"C0", "C2"}:
        return "REAL SOURCE FOUND"
    if origin == "conventional-minimal":
        return "CONVENTIONAL-MINIMAL ONLY"
    return "DOCUMENTED CONTRACT HAZARD FOUND"


def main() -> None:
    if not CORPUS.is_file():
        fail(f"corpus file missing: {CORPUS}")
    raw = CORPUS.read_text(encoding="utf-8")
    try:
        data = json.loads(raw)
    except json.JSONDecodeError as e:
        fail(f"JSON parse error: {e}")

    if data.get("schema") != "se1-corpus-schema":
        fail("schema field must be 'se1-corpus-schema'")
    if data.get("schema_version") != 1:
        fail("schema_version must be 1")
    if data.get("base_sha") is None:
        fail("base_sha missing")
    for key in ("population_law", "status_semantics"):
        if not str(data.get(key, "")).strip():
            fail(f"top-level {key} declaration missing/empty")
    if not isinstance(data.get("source_role_contract"), dict) or not data["source_role_contract"]:
        fail("top-level source_role_contract missing/empty")

    # JSON vocabulary must equal the validator's closed sets (no silent drift).
    vocab = data.get("source_role_vocabulary")
    if not isinstance(vocab, dict):
        fail("source_role_vocabulary missing")
    if set(vocab.get("entry_role", [])) != ENTRY_ROLES:
        fail("source_role_vocabulary.entry_role != validator closed set")
    if set(vocab.get("source_roles", [])) != SOURCE_ROLES:
        fail("source_role_vocabulary.source_roles != validator closed set")
    if set(vocab.get("source_authorities", [])) != SOURCE_AUTHORITIES:
        fail("source_role_vocabulary.source_authorities != validator closed set")

    entries = data.get("entries")
    if not isinstance(entries, list) or not entries:
        fail("entries must be a non-empty list")

    # pre-pass: id presence/uniqueness, so relationship checks below are
    # independent of entry order
    ids: set[str] = set()
    for e in entries:
        eid = e.get("id", "")
        if not eid:
            fail("entry missing id")
        if eid in ids:
            fail(f"duplicate id: {eid}")
        ids.add(eid)

    coverage: dict[str, str] = {}
    induced_rc_keys: dict[str, str] = {}
    counts = {
        "population": 0,
        "companions": 0,
        "conventional_pop": {"total": 0, "C0": 0, "C1": 0, "C2": 0, "C3": 0},
        "induced_pop": 0,
        "out_of_se1": 0,
    }

    for e in entries:
        eid = e.get("id", "")
        family = e.get("family")
        origin = e.get("origin")
        bucket = e.get("primary_bucket")
        elig = e.get("corpus_eligibility")
        quality = e.get("provenance_quality")
        status = e.get("sluice_current_status")
        validity = e.get("comparison_validity")
        pairing = e.get("pairing")
        role = e.get("entry_role")

        if "aliases" in e:
            fail(f"{eid}: legacy multi-meaning 'aliases' field forbidden (use same_case_as/related_entries)")

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
        if role not in ENTRY_ROLES:
            fail(f"{eid}: entry_role missing/invalid {role!r}")

        # exactly one primary bucket is enforced structurally (scalar field);
        # assert no secondary bucket sneaks in via lists
        for listfield in ("buckets", "primary_buckets"):
            if listfield in e:
                fail(f"{eid}: multiple bucket fields forbidden ({listfield})")

        # ---- population law relationships
        if role == "probe-companion":
            counts["companions"] += 1
            parent_id = e.get("same_case_as")
            if not parent_id:
                fail(f"{eid}: probe-companion requires same_case_as")
            elif parent_id not in ids:
                fail(f"{eid}: same_case_as target {parent_id!r} does not exist")
            else:
                parent = next(p for p in entries if p.get("id") == parent_id)
                if parent.get("entry_role") != "population-case":
                    fail(f"{eid}: same_case_as target {parent_id!r} is not a population-case")
                if parent.get("family") != family and not str(e.get("same_case_family_justification", "")).strip():
                    fail(f"{eid}: probe-companion family {family!r} != parent family {parent.get('family')!r} without same_case_family_justification")
        else:
            if elig == "IN-SE1":
                counts["population"] += 1
            if e.get("same_case_as"):
                fail(f"{eid}: population-case must not carry same_case_as")

        if e.get("related_entries") is not None:
            if not isinstance(e["related_entries"], list):
                fail(f"{eid}: related_entries must be a list")
            for t in e["related_entries"]:
                if t == eid:
                    fail(f"{eid}: related_entries self-reference")
                if t not in ids:
                    fail(f"{eid}: related_entries target {t!r} does not exist")

        # ---- provenance shape (structured sources)
        prov = e.get("provenance")
        if not isinstance(prov, dict) or not str(prov.get("publication", "")).strip():
            fail(f"{eid}: provenance must include publication")
        sources = prov.get("sources")
        if not isinstance(sources, list) or not sources:
            fail(f"{eid}: provenance.sources must be a non-empty list of structured records")
        roles_here: set[str] = set()
        for src in sources:
            if not isinstance(src, dict):
                fail(f"{eid}: provenance.sources entries must be objects with url/role/authority")
            if not str(src.get("url", "")).strip():
                fail(f"{eid}: source missing url")
            srole = src.get("role")
            sauth = src.get("authority")
            if srole not in SOURCE_ROLES:
                fail(f"{eid}: invalid source role {srole!r}")
            if sauth not in SOURCE_AUTHORITIES:
                fail(f"{eid}: invalid source authority {sauth!r}")
            if sauth not in ROLE_AUTHORITY[srole]:
                fail(f"{eid}: role {srole!r} may not carry authority {sauth!r}")
            roles_here.add(srole)
        if quality in {"C0", "C1", "C2"}:
            if not any(str(src.get("url", "")).startswith("http") for src in sources):
                fail(f"{eid}: {quality} provenance lacks an http(s) source url")
        if quality == "C3" and roles_here & INCIDENT_ROLES:
            fail(f"{eid}: C3 (conventional-minimal) must not claim incident roles {sorted(roles_here & INCIDENT_ROLES)}")
        # origin<->quality closed relation
        if quality not in ORIGIN_QUALITY[origin]:
            fail(f"{eid}: origin {origin!r} cannot carry provenance_quality {quality!r} (allowed {sorted(ORIGIN_QUALITY[origin])})")
        # quality role contract
        if quality == "C0":
            has_primary_incident = any(
                src.get("role") in PRIMARY_INCIDENT_ROLES and src.get("authority") in PRIMARY_AUTHORITIES
                for src in sources
            )
            if not (has_primary_incident and "upstream_fix" in roles_here):
                fail(
                    f"{eid}: C0 requires >=1 primary-authority incident record "
                    f"(bug_record/official_bug_corpus with upstream-primary/official-primary authority) "
                    f"AND >=1 upstream_fix source (found {sorted((s.get('role'), s.get('authority')) for s in sources)})"
                )
        if quality == "C1" and "official_contract" not in roles_here:
            fail(f"{eid}: C1 requires >=1 official_contract source")
        if quality == "C2" and "official_bug_corpus" not in roles_here:
            fail(f"{eid}: C2 requires >=1 official_bug_corpus source")
        if quality == "S0":
            repo_srcs = [s for s in sources if s.get("role") == "repo_evidence"]
            if not repo_srcs:
                fail(f"{eid}: S0 requires >=1 repo_evidence source")
            if all(s.get("authority") == "repo-untracked" for s in repo_srcs):
                # E4-only evidence is an unconfirmed candidate, never a
                # population case (#245 review 5060124249 FIX 3)
                if elig != "OUT-OF-SE1":
                    fail(f"{eid}: S0 evidence is repo-untracked (E4) only; entry must be OUT-OF-SE1 (unconfirmed candidate)")
            elif elig == "IN-SE1" and role == "population-case":
                if not any(s.get("authority") == "repo-primary" for s in repo_srcs):
                    fail(f"{eid}: IN-SE1 population S0 entry requires >=1 repo-primary repo_evidence source")

        for field in REQUIRED_TEXT_FIELDS:
            v = e.get(field)
            ok = (isinstance(v, list) and v) or (isinstance(v, str) and v.strip())
            if not ok:
                fail(f"{eid}: required field empty: {field}")

        # OUT-OF-SE1 needs a reason
        if elig == "OUT-OF-SE1" and not str(e.get("exclusion_reason", "")).strip():
            fail(f"{eid}: OUT-OF-SE1 entry missing exclusion_reason")
        if elig == "IN-SE1" and e.get("exclusion_reason"):
            fail(f"{eid}: IN-SE1 entry must not carry exclusion_reason")

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
        elif bucket in {"production-runtime", "test-only", "structural-authority", "experiment-process"}:
            # non-induced entries must not claim Sluice-internal buckets
            fail(f"{eid}: conventional origin cannot use Sluice-internal bucket {bucket!r}")

        # ---- population membership: eligibility AND role
        is_population = elig == "IN-SE1" and role == "population-case"

        if origin == "sluice-induced" and is_population:
            rkey = str(e.get("root_cause_key", "")).strip()
            rclass = str(e.get("root_cause_class", "")).strip()
            if not rkey:
                fail(f"{eid}: induced population-case requires non-empty root_cause_key")
            if not rclass:
                fail(f"{eid}: induced population-case requires non-empty root_cause_class")
            if rkey in induced_rc_keys:
                fail(f"{eid}: duplicate root_cause_key {rkey!r} already used by {induced_rc_keys[rkey]}")
            induced_rc_keys[rkey] = eid

        if is_population:
            if origin in {"conventional-real", "conventional-documentation", "conventional-minimal"}:
                counts["conventional_pop"]["total"] += 1
                counts["conventional_pop"][quality] += 1
                prev = coverage.get(family)
                occ = outcome_for(quality, origin)
                if prev is None or OUTCOME_RANK[occ] > OUTCOME_RANK[prev]:
                    coverage[family] = occ
            else:
                counts["induced_pop"] += 1
        elif elig == "OUT-OF-SE1":
            counts["out_of_se1"] += 1
        # probe companions (IN-SE1) are artifacts: counted, never in coverage/quality denominators

    # coverage: every family must have an explicit outcome derived from
    # POPULATION CASES ONLY, and the JSON coverage_gate must agree.
    gate = data.get("coverage_gate", {})
    for fam in FAMILIES:
        if fam not in coverage:
            # allowed only if explicitly declared NO VALID ENTRY YET
            if gate.get(fam) != "NO VALID ENTRY YET":
                fail(f"family {fam} has no IN-SE1 conventional population case and no explicit NO VALID ENTRY YET")
        else:
            declared = gate.get(fam)
            if declared == "NO VALID ENTRY YET":
                fail(f"family {fam} has entries but coverage_gate says NO VALID ENTRY YET")
            if declared == "REAL SOURCE FOUND" and coverage[fam] != "REAL SOURCE FOUND":
                fail(f"family {fam}: coverage_gate overstates provenance ({coverage[fam]})")

    # forbidden claims must not appear anywhere in the file
    blob = raw.lower()
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
    print(f"  records total:         {len(entries)}")
    print(f"  population cases:      {counts['population']} (conventional {counts['conventional_pop']['total']} + induced {counts['induced_pop']})")
    cp = counts["conventional_pop"]
    print(f"  conventional quality:  C0={cp['C0']} C1={cp['C1']} C2={cp['C2']} C3={cp['C3']}")
    print(f"  probe companions:      {counts['companions']} (excluded from all denominators)")
    print(f"  OUT-OF-SE1:            {counts['out_of_se1']}")
    real = sum(1 for v in coverage.values() if v == "REAL SOURCE FOUND")
    doc = sum(1 for v in coverage.values() if v == "DOCUMENTED CONTRACT HAZARD FOUND")
    minimal_only = sum(1 for v in coverage.values() if v == "CONVENTIONAL-MINIMAL ONLY")
    no_valid = sum(1 for f in FAMILIES if coverage.get(f) is None)
    print(f"  family outcomes:       REAL={real} DOCUMENTED={doc} MINIMAL_ONLY={minimal_only} NO_VALID_ENTRY={no_valid}")
    print(f"  induced root causes:   {len(induced_rc_keys)} unique keys (dedup enforced)")


if __name__ == "__main__":
    main()
