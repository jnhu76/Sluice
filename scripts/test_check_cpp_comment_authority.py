#!/usr/bin/env python3
"""Self-tests for the C++ comment authority guard (run with python3)."""

import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import check_cpp_comment_authority as checker


class RejectTests(unittest.TestCase):
    def assert_rejected(self, source, rule):
        findings = checker.scan_source(source)
        self.assertTrue(findings, f"expected a finding in:\n{source}")
        self.assertIn(rule, (f[1] for f in findings))

    def test_requirement_id_in_line_comment(self):
        self.assert_rejected("// SEM-03 requires this path.\n", "requirement-id")

    def test_requirement_id_in_block_comment(self):
        self.assert_rejected("/* BOUND-01 rollback applies here. */\n",
                             "requirement-id")

    def test_issue_narration(self):
        self.assert_rejected("// This closes Issue #410.\n", "issue-pr-reference")

    def test_pr_history_in_multiline_block(self):
        self.assert_rejected("/*\n * PR #409 established this behavior.\n */\n",
                             "issue-pr-reference")

    def test_authority_claim(self):
        self.assert_rejected("// Canonical authority for async semantics.\n",
                             "authority-claim")

    def test_proof_claim(self):
        self.assert_rejected("// The invariant holds; this proves REQ-01.\n",
                             "authority-claim")

    def test_stage_tokens(self):
        self.assert_rejected("// Owned by V1-B2.\n", "stage-narration")
        self.assert_rejected("// Corrective-2 tightened this check.\n",
                             "stage-narration")
        self.assert_rejected("// Removed in Phase B.\n", "stage-narration")
        self.assert_rejected("// The slice B1 moves the arena.\n",
                             "stage-narration")

    def test_requirement_id_inside_multiline_block(self):
        source = "/*\n * plain line\n * per VERIFY-02 S1\n * plain line\n */\n"
        findings = checker.scan_source(source)
        self.assertEqual([f[0] for f in findings], [3])
        self.assertEqual(findings[0][1], "requirement-id")

    def test_comment_after_digit_separator_is_still_scanned(self):
        source = "constexpr int n = 1'000'000; // SEM-01 marker\n"
        findings = checker.scan_source(source)
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0][1], "requirement-id")


class AcceptTests(unittest.TestCase):
    def assert_clean(self, source):
        findings = checker.scan_source(source)
        self.assertEqual(findings, [], f"unexpected findings:\n{findings}")

    def test_eintr_hazard(self):
        self.assert_clean(
            "// Retrying close after EINTR can close a reused descriptor.\n")

    def test_underflow_invariant(self):
        self.assert_clean(
            "// confirmed <= requested, so the subtraction cannot underflow.\n")

    def test_fail_closed_seam(self):
        self.assert_clean(
            "// Exhausted scripts fail closed instead of reaching the real syscall.\n")

    def test_strings_are_not_comments(self):
        self.assert_clean('const char* text = "SEM-03";\n'
                          'const char* note = "see issue #410";\n'
                          "char c = '-';\n")

    def test_hazard_comment_with_hyphenated_word(self):
        self.assert_clean("// The SEM-formula path is not involved here.\n")

    def test_url_not_flagged(self):
        self.assert_clean("// See https://example.com/SEM-03-docs#overview\n")

    def test_plain_symbols_not_flagged(self):
        self.assert_clean("// matrix A1 holds the first sample\n"
                          "// register B1 is untouched by this loop\n")

    def test_proof_in_algorithmic_sense(self):
        self.assert_clean(
            "// No underflow: the proof is confirmed <= requested.\n")

    def test_lowercase_unrelated_phase_word(self):
        self.assert_clean("// the second phase of this loop drains the queue\n")

    def test_multiline_hazard_block(self):
        self.assert_clean("/*\n * An EINTR retry of close is unsafe on Linux,\n"
                          " * so the first attempt is terminal.\n */\n")

    def test_digit_separator_does_not_hide_later_comments(self):
        self.assert_clean("constexpr int n = 1'000'000; // at most the requested size\n")
        self.assert_clean("constexpr int h = 0x1'0000; // last addressed byte stays in range\n")


class LineNumberTests(unittest.TestCase):
    def test_line_comment_reports_its_line(self):
        source = "int a;\nint b;\n// SEM-01 marker\nint c;\n"
        findings = checker.scan_source(source)
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0][0], 3)

    def test_block_comment_reports_inner_line(self):
        source = "int a;\n/* line one\n   line two ERR-02\n   line three */\nint b;\n"
        findings = checker.scan_source(source)
        self.assertEqual(len(findings), 1)
        self.assertEqual(findings[0][0], 3)

    def test_excerpt_is_the_physical_comment_line(self):
        source = "// SEM-01 marker\n"
        findings = checker.scan_source(source)
        self.assertEqual(findings[0][2], "// SEM-01 marker")


class RepoScanTests(unittest.TestCase):
    def test_scanner_runs_over_a_directory_without_crashing(self):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "a.cpp").write_text("// fine\n")
            (Path(tmp) / ".lake").mkdir()
            (Path(tmp) / ".lake" / "gen.c").write_text("// REQ-01 in generated code\n")
            files = list(checker.cpp_files(tmp))
            self.assertEqual([p.name for p in files], ["a.cpp"])
            self.assertEqual(checker.scan_path(files[0]), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
