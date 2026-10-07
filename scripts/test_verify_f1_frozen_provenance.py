#!/usr/bin/env python3
"""Self-tests for the F1 frozen-baseline provenance pinning (run with python3).

Discriminators: a --check-frozen reproduction must be invariant to docs-only
advancement of master/origin, must keep the frozen PRODUCTION_BASELINE_SHA
identity while origin moves, and must still surface real src/include drift
from the pinned baseline. The pinned identity itself must stay immutable —
never a volatile comparison key.
"""

import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import verify_f1_package as verifier


class FrozenBaselineProvenanceTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.repo = Path(self._tmp.name) / "repo"
        self.git("init", "-q", "-b", "master", "repo", cwd=self._tmp.name)

    def git(self, *args, cwd=None):
        r = subprocess.run(["git", *args], cwd=str(cwd or self.repo),
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, f"git {' '.join(args)}: {r.stderr}")
        return r.stdout.strip()

    def commit(self, rel, content, msg):
        path = self.repo / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        self.git("add", "-A")
        self.git("-c", "user.email=t@example.com", "-c", "user.name=t",
                 "commit", "-q", "-m", msg)
        return self.git("rev-parse", "HEAD")

    def baseline_tree(self):
        self.commit("src/core.cpp", "int core_one();\n", "src")
        self.commit("include/sluice/a.hpp", "#pragma once\n", "headers")
        return self.commit("docs/readme.md", "v1\n", "docs")

    def test_docs_only_advancement_keeps_pinned_baseline(self):
        baseline = self.baseline_tree()
        head = self.commit("docs/readme.md", "v2\n", "docs only")
        self.assertNotEqual(head, baseline)
        resolved = verifier.resolve_production_baseline(
            self.repo, {"PRODUCTION_BASELINE_SHA": baseline})
        self.assertEqual(resolved, baseline)
        self.assertEqual(verifier.production_diff_paths(self.repo, resolved), [])

    def test_origin_advancement_does_not_move_pinned_identity(self):
        baseline = self.baseline_tree()
        local_head = self.commit("docs/readme.md", "v2\n", "docs ahead of baseline")
        origin = Path(self._tmp.name) / "origin.git"
        self.git("clone", "-q", "--bare", str(self.repo), str(origin))
        self.commit("docs/notes.md", "only on origin\n", "origin moves on")
        self.git("push", "-q", str(origin), "master")
        self.git("reset", "-q", "--hard", local_head)
        self.git("remote", "add", "origin", str(origin))
        self.git("fetch", "-q", "origin")
        # The pre-fix derivation (merge-base HEAD origin/master) differs from
        # the pinned baseline in exactly this scenario: the fixture must
        # encode it, or the test cannot discriminate the repair.
        self.assertNotEqual(self.git("merge-base", "HEAD", "origin/master"), baseline)
        resolved = verifier.resolve_production_baseline(
            self.repo, {"PRODUCTION_BASELINE_SHA": baseline})
        self.assertEqual(resolved, baseline)
        self.assertEqual(verifier.production_diff_paths(self.repo, resolved), [])

    def test_production_drift_from_pinned_baseline_is_detected(self):
        baseline = self.baseline_tree()
        self.commit("include/sluice/a.hpp", "#pragma once\nint drifted;\n",
                    "drift include")
        resolved = verifier.resolve_production_baseline(
            self.repo, {"PRODUCTION_BASELINE_SHA": baseline})
        self.assertEqual(resolved, baseline)
        self.assertEqual(verifier.production_diff_paths(self.repo, resolved),
                         ["include/sluice/a.hpp"])

    def test_scripts_drift_does_not_invalidate_baseline(self):
        baseline = self.baseline_tree()
        self.commit("scripts/tool.py", "print('tool')\n", "verifier-side change")
        resolved = verifier.resolve_production_baseline(
            self.repo, {"PRODUCTION_BASELINE_SHA": baseline})
        self.assertEqual(resolved, baseline)
        self.assertEqual(verifier.production_diff_paths(self.repo, resolved), [])

    def test_pinned_baseline_must_exist_as_commit(self):
        self.baseline_tree()
        with self.assertRaises(SystemExit):
            verifier.resolve_production_baseline(
                self.repo, {"PRODUCTION_BASELINE_SHA": "0" * 40})

    def test_pinned_baseline_must_be_ancestor_of_head(self):
        baseline = self.baseline_tree()
        self.git("checkout", "-q", "--orphan", "detached-world")
        self.commit("unrelated.txt", "x\n", "orphan root")
        with self.assertRaises(SystemExit):
            verifier.resolve_production_baseline(
                self.repo, {"PRODUCTION_BASELINE_SHA": baseline})

    def test_missing_pinned_key_is_rejected(self):
        self.baseline_tree()
        with self.assertRaises(SystemExit):
            verifier.resolve_production_baseline(self.repo, {})


    def test_pinned_sha_is_not_a_volatile_key(self):
        # The forbidden cheat-fix — hiding the identity key behind the volatile
        # set — would pass every functional test above, so pin the invariant.
        self.assertNotIn("PRODUCTION_BASELINE_SHA", verifier.VOLATILE_MANIFEST_KEYS)

    def test_freeze_mode_discovers_merge_base_baseline(self):
        baseline = self.baseline_tree()
        local_head = self.commit("docs/readme.md", "v2\n", "docs ahead of baseline")
        origin = Path(self._tmp.name) / "origin.git"
        self.git("clone", "-q", "--bare", str(self.repo), str(origin))
        self.commit("docs/notes.md", "only on origin\n", "origin moves on")
        self.git("push", "-q", str(origin), "master")
        self.git("reset", "-q", "--hard", local_head)
        self.git("remote", "add", "origin", str(origin))
        self.git("fetch", "-q", "origin")
        self.assertEqual(verifier.resolve_production_baseline(self.repo, None),
                         self.git("merge-base", "HEAD", "origin/master"))

    def test_non_dict_frozen_manifest_is_rejected_cleanly(self):
        self.baseline_tree()
        with self.assertRaises(SystemExit):
            verifier.resolve_production_baseline(self.repo, ["not", "a", "dict"])


if __name__ == "__main__":
    unittest.main()
