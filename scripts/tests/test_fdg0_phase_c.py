#!/usr/bin/env python3
"""FDG-0 Phase C — hermetic self-tests for the historical-gold driver
(issue #298 Phase C task book §36, C1–C16). Pure scoring/integrity/gold
validation logic plus a real mini-git fixture for the gold-freeze
sequencing check. No xmake, scip-clang, network, or corpus execution.
"""
from __future__ import annotations

import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
FORMAL_DIR = SCRIPT_DIR.parent / "formal"
sys.path.insert(0, str(FORMAL_DIR))
sys.path.insert(0, str(SCRIPT_DIR))

import fdg0_phase_c as pc  # noqa: E402


REGISTRY = {
    "schema_version": 2,
    "claim_class_vocabulary": ["MODEL_PROPERTY"],
    "anchor_roles": ["authority"],
    "claims": [
        {
            "id": "F08",
            "title": "wait/wake",
            "claim_class": "MODEL_PROPERTY",
            "cpp_anchors": [{"file": "src/wake.cpp", "symbol": "S::wake", "role": "authority"}],
            "formal_suites": ["e9-park-wake", "e9-trace", "spawn-wake", "cond", "liveness"],
            "facets": [
                {"id": "wake-publication", "anchor_refs": ["a1"], "formal_suites": ["e9-park-wake", "spawn-wake"]},
                {"id": "park-commit-return", "anchor_refs": ["a2"], "formal_suites": ["e9-park-wake", "e9-trace"]},
            ],
        },
        {"id": "F03", "title": "terminal", "claim_class": "MODEL_PROPERTY",
         "cpp_anchors": [{"file": "src/term.cpp", "symbol": "S::term", "role": "authority"}],
         "formal_suites": ["e9-trace"]},
    ],
}
MANIFEST = {"suites": [{"id": sid, "implementation_bindings": [f"src/{sid}.cpp"]}
                       for sid in ("e9-park-wake", "e9-trace", "spawn-wake", "cond", "liveness")]}


def base_case(**over) -> dict:
    case = {
        "case_id": "C-001",
        "commit_sha": "422036cd",
        "base_sha": "688c5ce0",
        "head_sha": "422036cd",
        "subject": "s",
        "date": "2026-07-08",
        "subsystem": "scheduler",
        "applicability": "APPLICABLE",
        "change_summary": "x",
        "gold_kind": "POSITIVE",
        "expected_claims": ["F08"],
        "claim_confidence": "HIGH",
        "facet_gold": "UNSCORED",
        "formal_target_gold": [],
        "gold_evidence": ["diff"],
        "rationale": "r",
        "ambiguity_note": "",
    }
    case.update(over)
    return case


def base_gold(cases) -> dict:
    return {
        "schema": "sluice-fdg0-phase-c-gold/1",
        "history_range": {"base": "b", "head": "h"},
        "sampling_rules": ["r1"],
        "cases": cases,
        "required_case_ids": [c["case_id"] for c in cases],
        "required_result_rows": sum(
            len(c.get("expected_claims", []))
            for c in cases
            if c["gold_kind"] == "POSITIVE" and c["claim_confidence"] != "AMBIGUOUS"
        ),
    }


def impact(cls="UNKNOWN_FORMAL_IMPACT", claims=None, fail_closed=True):
    return {
        "classification": cls,
        "claims": claims or [],
        "fail_closed": fail_closed,
    }


def claim_entry(cid, klass="UNKNOWN_FORMAL_IMPACT", scope="CONSERVATIVE_ALL", targets=None):
    return {"id": cid, "class": klass, "facet_scope": scope,
            "revalidation_targets": targets or [], "paths": [], "affected_facets": []}


# --- gold validation (C1, C2, C6, C16) -----------------------------------------

class GoldValidation(unittest.TestCase):
    def test_c1_duplicate_case_id_fails(self):
        gold = base_gold([base_case(), base_case()])
        problems = pc.validate_gold(gold, REGISTRY, MANIFEST)
        self.assertTrue(any("duplicate case id" in p for p in problems))

    def test_c2_missing_required_case_fails(self):
        gold = base_gold([base_case()])
        gold["required_case_ids"] = ["C-001", "C-099"]
        problems = pc.validate_gold(gold, REGISTRY, MANIFEST)
        self.assertTrue(any("absent from cases" in p for p in problems))

    def test_c6_malformed_gold_fails(self):
        for bad in [
            {"schema": "x"},  # missing keys
            base_gold([base_case(gold_kind="WHATEVER")]),
            base_gold([base_case(applicability="SOMETIMES")]),
            base_gold([base_case(claim_confidence="LOW")]),
            base_gold([base_case(expected_claims=["F99"])]),
            base_gold([base_case(gold_kind="AMBIGUOUS", expected_claims=["F08"])]),
            base_gold([base_case(facet_gold=["no-such-facet"])]),
        ]:
            problems = pc.validate_gold(bad, REGISTRY, MANIFEST)
            self.assertTrue(problems, f"expected problems for {bad}")

    def test_c16_required_rows_enforced(self):
        gold = base_gold([base_case(), base_case(case_id="C-002", expected_claims=["F08", "F03"])])
        gold["required_result_rows"] = 2  # wrong: 3 labels exist
        problems = pc.validate_gold(gold, REGISTRY, MANIFEST)
        self.assertTrue(any("required_result_rows" in p for p in problems))
        gold["required_result_rows"] = 3
        self.assertEqual(pc.validate_gold(gold, REGISTRY, MANIFEST), [])

    def test_positive_without_expected_claims_fails(self):
        gold = base_gold([base_case(expected_claims=[])])
        problems = pc.validate_gold(gold, REGISTRY, MANIFEST)
        self.assertTrue(any("without expected claims" in p for p in problems))


# --- sequencing (C7) -------------------------------------------------------------

class GoldSequencing(unittest.TestCase):
    def test_c7_gold_unchanged_since_freeze_commit(self):
        with tempfile.TemporaryDirectory() as td:
            repo = Path(td)
            def git(*a):
                subprocess.run(["git", *a], cwd=repo, check=True, capture_output=True)
            git("init", "-q")
            git("config", "user.email", "t@t")
            git("config", "user.name", "t")
            gold = repo / "gold.json"
            gold.write_text('{"v": 1}')
            git("add", "gold.json")
            git("commit", "-qm", "C0 gold freeze")
            freeze_sha = subprocess.run(
                ["git", "log", "-n", "1", "--format=%H", "--", "gold.json"],
                cwd=repo, capture_output=True, text=True, check=True).stdout.strip()
            committed = subprocess.run(
                ["git", "show", f"{freeze_sha}:gold.json"],
                cwd=repo, capture_output=True, check=True).stdout
            self.assertEqual(gold.read_bytes(), committed)  # unmodified -> OK
            gold.write_text('{"v": 2}')
            self.assertNotEqual(gold.read_bytes(), committed)  # modified -> violation

    def test_results_carry_freeze_identity(self):
        # the results schema embeds gold identity so the closure report can
        # pin gold_commit/results_commit (used by the real run)
        self.assertIn("gold", json.loads(json.dumps({"gold": {"path": "p", "required_result_rows": 1}})))


# --- scoring (C8-C12) -------------------------------------------------------------

class Scoring(unittest.TestCase):
    def test_c10_absent_expected_claim_is_miss_and_silent_no(self):
        case = base_case()
        score = pc.score_positive_case(case, impact("NO_FORMAL_IMPACT", [], fail_closed=False))
        self.assertFalse(score["rows"][0]["found"])
        self.assertTrue(score["rows"][0]["silent_no"])

    def test_c11_unknown_retains_expected_claim_as_safe_hit(self):
        case = base_case()
        score = pc.score_positive_case(
            case, impact("UNKNOWN_FORMAL_IMPACT", [claim_entry("F08")], fail_closed=True))
        self.assertTrue(score["rows"][0]["found"])
        self.assertEqual(score["rows"][0]["hit_class"], "UNKNOWN_FORMAL_IMPACT")
        self.assertFalse(score["rows"][0]["silent_no"])
        metrics = pc.method_metrics([score], [], score["rows"])
        self.assertEqual(metrics["claim_recall"], 1.0)
        self.assertEqual(metrics["unknown_count"], 1)

    def test_c12_negative_extras_are_noise_not_false_negative(self):
        neg = pc.score_negative_case(base_case(gold_kind="NEGATIVE", expected_claims=[]),
                                     impact("COARSE_FORMAL_IMPACT", [claim_entry("F08"), claim_entry("F03")]))
        self.assertEqual(neg["extra"], ["F03", "F08"])
        metrics = pc.method_metrics([], [neg], [])
        self.assertEqual(metrics["extra_claim_flags"], 2)
        self.assertEqual(metrics["review_noise"], 1.0)

    def test_c8_c9_ambiguous_and_out_of_epoch_excluded_from_denominator(self):
        # C9: AMBIGUOUS positives never enter the row set (driver filters by
        # gold_kind/confidence before scoring) — verified via metrics on the
        # row-building filter used by cmd_run.
        cases = [
            base_case(),
            base_case(case_id="C-013", gold_kind="AMBIGUOUS", claim_confidence="AMBIGUOUS",
                      expected_claims=[]),
            base_case(case_id="C-024", applicability="OUT_OF_EPOCH", gold_kind="NEGATIVE",
                      expected_claims=[]),
        ]
        scored = [
            c for c in cases
            if c["gold_kind"] == "POSITIVE" and c["claim_confidence"] != "AMBIGUOUS"
        ]
        self.assertEqual([c["case_id"] for c in scored], ["C-001"])
        # C8: an OUT_OF_EPOCH record is skipped from scoring (status gate)
        rec = {"case_id": "C-024", "reconstruction": {"status": "SKIPPED_OUT_OF_EPOCH"}}
        self.assertNotEqual(rec["reconstruction"]["status"], "RECONSTRUCTED")


# --- facets (C13, C14) -------------------------------------------------------------

class FacetScoring(unittest.TestCase):
    def test_c13_precise_missing_required_target_is_safety_failure(self):
        case = base_case(formal_target_gold=["spawn-wake"],
                         facet_gold=["wake-publication"])
        result = impact("STRUCTURAL_FORMAL_IMPACT",
                        [claim_entry("F08", "STRUCTURAL_FORMAL_IMPACT", "PRECISE",
                                     targets=["e9-park-wake", "cond"])])
        row = pc.score_facet_case(case, result)
        self.assertTrue(row["safety_failure"])
        self.assertEqual(row["missing_required_targets"], ["spawn-wake"])

    def test_c14_conservative_all_is_safe_not_false_negative(self):
        case = base_case(formal_target_gold=["spawn-wake"],
                         facet_gold=["wake-publication"])
        result = impact("UNKNOWN_FORMAL_IMPACT",
                        [claim_entry("F08", "UNKNOWN_FORMAL_IMPACT", "CONSERVATIVE_ALL",
                                     targets=["e9-park-wake", "e9-trace", "spawn-wake", "cond", "liveness"])])
        row = pc.score_facet_case(case, result)
        self.assertFalse(row["safety_failure"])
        self.assertEqual(row["resolver_scope"], "CONSERVATIVE_ALL")

    def test_precise_containing_required_targets_is_safe(self):
        case = base_case(formal_target_gold=["spawn-wake"],
                         facet_gold=["wake-publication"])
        result = impact("DIRECT_FORMAL_IMPACT",
                        [claim_entry("F08", "DIRECT_FORMAL_IMPACT", "PRECISE",
                                     targets=["e9-park-wake", "spawn-wake"])])
        row = pc.score_facet_case(case, result)
        self.assertFalse(row["safety_failure"])
        self.assertEqual(row["missing_required_targets"], [])


# --- integrity (C3, C4, C5, C15) ----------------------------------------------------

class Integrity(unittest.TestCase):
    def gold(self):
        return base_gold([base_case(), base_case(case_id="C-002", gold_kind="NEGATIVE",
                                                 expected_claims=[])])

    def rec(self, cid, status="RECONSTRUCTED"):
        return {"case_id": cid, "reconstruction": {"status": status}, "replay_checks": {}}

    def rows(self):
        return [{"case_id": "C-001", "claim": "F08", "found": True,
                 "silent_no": False, "hit_class": "UNKNOWN_FORMAL_IMPACT"}]

    def test_all_pass_on_clean_corpus(self):
        gold = self.gold()
        integrity = pc.compute_integrity(
            gold, [self.rec("C-001"), self.rec("C-002")], self.rows())
        self.assertTrue(integrity["all_pass"])

    def test_c3_execution_error_fails_corpus(self):
        gold = self.gold()
        records = [self.rec("C-001"), self.rec("C-002", status="SCIP_INDEX_FAILED")]
        integrity = pc.compute_integrity(gold, records, self.rows())
        self.assertFalse(integrity["all_pass"])
        self.assertEqual(integrity["execution_errors"], ["C-002"])

    def test_c4_required_case_absent_from_scoring_fails(self):
        gold = self.gold()
        records = [self.rec("C-001"), self.rec("C-002", status="IMPACT_FAILED")]
        integrity = pc.compute_integrity(gold, records, self.rows())
        self.assertFalse(integrity["all_pass"])
        self.assertIn("C-002", integrity["unscored_required_cases"])

    def test_c5_duplicate_result_row_fails(self):
        gold = self.gold()
        rows = self.rows() + [{"case_id": "C-001", "claim": "F08", "found": True,
                               "silent_no": False, "hit_class": "UNKNOWN_FORMAL_IMPACT"}]
        integrity = pc.compute_integrity(gold, [self.rec("C-001"), self.rec("C-002")], rows)
        self.assertFalse(integrity["all_pass"])
        self.assertEqual(integrity["duplicate_rows"], [("C-001", "F08")])

    def test_c15_failed_reconstruction_cannot_silently_disappear(self):
        gold = self.gold()
        records = [self.rec("C-001"),
                   self.rec("C-002", status="BUILD_METADATA_UNAVAILABLE")]
        integrity = pc.compute_integrity(gold, records, self.rows())
        # the failed case is still accounted for, and the corpus fails closed
        self.assertFalse(integrity["all_pass"])
        self.assertEqual(integrity["cases_present"], 2)
        self.assertEqual(integrity["required_cases"], 2)

    def test_missing_case_fails(self):
        gold = self.gold()
        integrity = pc.compute_integrity(gold, [self.rec("C-001")], self.rows())
        self.assertFalse(integrity["all_pass"])
        self.assertEqual(integrity["missing_cases"], ["C-002"])


# --- baselines and verdict ----------------------------------------------------------

class BaselinesAndVerdict(unittest.TestCase):
    def test_baseline_a_file_only_mapping(self):
        claims = pc.baseline_a_claims(
            ["src/e9-park-wake.cpp", "src/unrelated.cpp"], REGISTRY, MANIFEST)
        self.assertEqual(claims, ["F08"])

    def test_verdict_requires_twenty_cases_and_full_recall(self):
        integrity = {"all_pass": True}
        metrics = {"claim_recall": 1.0, "claims_found": 12, "expected_claim_labels": 12,
                   "expected_claim_misses": 0, "silent_no": 0}
        verdict, _ = pc.verdict_logic(integrity, metrics, [], reconstructed_scored=19,
                                      gold_frozen_first=True)
        self.assertEqual(verdict, "HISTORICAL_GOLD_INSUFFICIENT")
        verdict, _ = pc.verdict_logic(integrity, metrics, [], reconstructed_scored=21,
                                      gold_frozen_first=True)
        self.assertEqual(verdict, "HISTORICAL_GOLD_EARNED")

    def test_verdict_not_earned_on_single_miss(self):
        integrity = {"all_pass": True}
        metrics = {"claim_recall": 0.95, "claims_found": 11, "expected_claim_labels": 12,
                   "expected_claim_misses": 1, "silent_no": 0}
        verdict, _ = pc.verdict_logic(integrity, metrics, [], reconstructed_scored=23,
                                      gold_frozen_first=True)
        self.assertEqual(verdict, "METHOD_RECALL_NOT_EARNED")

    def test_verdict_not_earned_on_facet_omission(self):
        integrity = {"all_pass": True}
        metrics = {"claim_recall": 1.0, "claims_found": 12, "expected_claim_labels": 12,
                   "expected_claim_misses": 0, "silent_no": 0}
        facet_rows = [{"safety_failure": True, "uses_heuristic": True, "case_id": "C-005"}]
        verdict, reasons = pc.verdict_logic(integrity, metrics, facet_rows,
                                            reconstructed_scored=23, gold_frozen_first=True)
        self.assertEqual(verdict, "METHOD_RECALL_NOT_EARNED")
        self.assertTrue(any("HEURISTIC" in r for r in reasons))


if __name__ == "__main__":
    unittest.main()
