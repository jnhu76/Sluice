# test_verify_coverage_gaps.py
#
# Pure-data self-tests for the coverage_gaps referenced-file validation in
# scripts/formal/verify.py (issue #100 review follow-up).
#
# The validator must prove that every referenced file exists INSIDE the
# repository, not merely somewhere on the machine: `(REPO_ROOT / p)` silently
# ignores REPO_ROOT for absolute paths, and `..` components walk outside the
# checkout. These probes assert the containment boundary:
#
#   valid relative repo path          -> GREEN (no errors)
#   absolute path (/etc/passwd)       -> RED   (escapes REPO_ROOT)
#   dotdot escape (../../..)          -> RED   (escapes REPO_ROOT)
#   dotdot resolving back inside repo -> GREEN (containment on the RESOLVED
#                                       path, not the raw spelling)
#   missing in-repo file              -> RED   (file not found, unchanged msg)
#
# Run with:
#   python3 -m unittest discover -v scripts/tests        # CI / hardening runner
#   python3 scripts/tests/test_verify_coverage_gaps.py
# Both invocations exit 0 on success, non-zero on any failure.
"""Repo-containment probes for the coverage_gaps referenced-file validator."""
import importlib.util
import os
import sys
import unittest

_VERIFY_PATH = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "..", "formal", "verify.py"
)
_spec = importlib.util.spec_from_file_location("sluice_verify", _VERIFY_PATH)
V = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(V)


def _manifest_with_bindings(bindings):
    return {
        "coverage_gaps": [
            {
                "id": "probe",
                "status": "open",
                "protocol": "arena",
                "why_no_model_yet": "x",
                "revisit_triggers": ["t"],
                "implementation_bindings": bindings,
                "regression_test_cross_links": [],
                "owner_docs": [],
            }
        ]
    }


class CoverageGapsContainmentTests(unittest.TestCase):
    def test_valid_relative_repo_path_stays_green(self):
        errors = V._check_coverage_gaps(
            _manifest_with_bindings(["docs/verification/formal-models.md"])
        )
        self.assertEqual(errors, [])

    def test_absolute_path_escapes_repo_root(self):
        errors = V._check_coverage_gaps(_manifest_with_bindings(["/etc/passwd"]))
        self.assertTrue(
            any("escapes REPO_ROOT" in e for e in errors),
            f"absolute path must be rejected, got: {errors}",
        )

    def test_dotdot_escape_rejected(self):
        # Walks above the checkout regardless of its depth; the resolved path
        # is outside REPO_ROOT even if the raw spelling looks relative.
        escape = os.path.join("..", "..", "..", "etc", "passwd")
        errors = V._check_coverage_gaps(_manifest_with_bindings([escape]))
        self.assertTrue(
            any("escapes REPO_ROOT" in e for e in errors),
            f"dotdot escape must be rejected, got: {errors}",
        )

    def test_dotdot_resolving_inside_repo_is_green(self):
        # ../<checkout-name>/README.md resolves back INTO the repo: containment
        # is decided on the resolved path, so this is not an escape.
        inside = os.path.join(
            "..", os.path.basename(V.REPO_ROOT), "README.md"
        )
        errors = V._check_coverage_gaps(_manifest_with_bindings([inside]))
        self.assertEqual(errors, [])

    def test_missing_in_repo_file_reported_not_found(self):
        errors = V._check_coverage_gaps(
            _manifest_with_bindings(["docs/verification/does-not-exist.md"])
        )
        self.assertTrue(
            any("file not found" in e for e in errors),
            f"missing file must keep the file-not-found error, got: {errors}",
        )


if __name__ == "__main__":
    unittest.main()
