#!/usr/bin/env python3
"""Unit tests for the pure gates of scripts/verify_v2_package.py."""

import importlib.util
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location(
    "verify_v2_package", REPO / "scripts" / "verify_v2_package.py")
verifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verifier)


class ExpectedSurfaceTests(unittest.TestCase):
    def test_contract_is_eleven_headers(self):
        self.assertEqual(len(verifier.EXPECTED_V2_HEADERS), 11)

    def test_contract_matches_install_allowlist_in_libraries_lua(self):
        text = (REPO / "xmake" / "libraries.lua").read_text()
        for header in verifier.EXPECTED_V2_HEADERS:
            self.assertIn(header, text,
                          f"{header} missing from xmake/libraries.lua allowlist")

    def test_no_overlap_between_contracted_and_uninstalled(self):
        overlap = set(verifier.EXPECTED_V2_HEADERS) & set(
            verifier.UNINSTALLED_TREE_HEADERS + verifier.DELETED_HEADERS)
        self.assertEqual(overlap, set())

    def test_deleted_host_headers_absent_from_tree(self):
        for header in verifier.DELETED_HEADERS:
            self.assertFalse(
                (REPO / "include" / header).exists(),
                f"deleted host header {header} is present in the tree")

    def test_uninstalled_list_names_real_tree_headers(self):
        for header in verifier.UNINSTALLED_TREE_HEADERS:
            self.assertTrue((REPO / "include" / header).exists(),
                            f"uninstalled probe names absent header {header}")

    def test_uninstalled_covers_every_header_outside_the_contract(self):
        tree = sorted(str(p.relative_to(REPO / "include"))
                      for p in (REPO / "include").rglob("*.hpp"))
        outside = [h for h in tree if h not in verifier.EXPECTED_V2_HEADERS]
        self.assertEqual(sorted(outside), sorted(verifier.UNINSTALLED_TREE_HEADERS),
                         "the unavailable-header probe set must track the tree")


class IncludeScanTests(unittest.TestCase):
    def test_angle_bracket_includes_pass(self):
        with tempfile.TemporaryDirectory() as tmp:
            consumer_dir = Path(tmp) / "consumers"
            consumer_dir.mkdir()
            (consumer_dir / "ok.cpp").write_text(
                "#include <sluice/async/request.hpp>\n"
                "#include <vector>\n"
                "int main() { return 0; }\n")
            verifier.CONSUMERS = consumer_dir
            self.assertEqual(verifier.scan_consumer_includes(), [])

    def test_quoted_absolute_and_traversal_includes_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            consumer_dir = Path(tmp) / "consumers"
            consumer_dir.mkdir()
            (consumer_dir / "bad.cpp").write_text(
                '#include "local.hpp"\n'
                "#include </abs/path.hpp>\n"
                "#include <a/../b.hpp>\n")
            verifier.CONSUMERS = consumer_dir
            violations = verifier.scan_consumer_includes()
            self.assertEqual(len(violations), 3)


class CleanRoomCommandTests(unittest.TestCase):
    def test_repository_include_flag_is_a_violation(self):
        violations = verifier.check_clean_room(
            [f"g++ -I{REPO}/include -c x.cpp -o x.o"], "/tmp/prefix")
        self.assertEqual(len(violations), 1)

    def test_prefix_include_flag_is_clean(self):
        violations = verifier.check_clean_room(
            ["g++ -I/tmp/prefix/include -c x.cpp -o x.o"], "/tmp/prefix")
        self.assertEqual(violations, [])


def stage_installed_headers(tmp):
    prefix = Path(tmp) / "prefix"
    for rel in verifier.EXPECTED_V2_HEADERS:
        dst = prefix / "include" / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(REPO / "include" / rel, dst)
    return prefix


@unittest.skipUnless(shutil.which("g++"), "g++ is required for probe compiles")
class SeamNegativeProbeTests(unittest.TestCase):
    def test_healthy_headers_pass_the_seam_gate(self):
        with tempfile.TemporaryDirectory() as tmp:
            prefix = stage_installed_headers(tmp)
            build = Path(tmp) / "build"
            build.mkdir()
            failures = verifier.run_seam_negative_probes(build, prefix, [])
            self.assertEqual(failures, [])

    def test_reexposed_backend_definition_is_killed(self):
        with tempfile.TemporaryDirectory() as tmp:
            prefix = stage_installed_headers(tmp)
            shutil.copytree(REPO / "include" / "sluice",
                            prefix / "include" / "sluice",
                            dirs_exist_ok=True)
            installed = prefix / "include" / "sluice" / "async" / "async_io_context.hpp"
            installed.write_text(
                installed.read_text()
                + "\n#include <sluice/async/detail/async_backend.hpp>\n")
            build = Path(tmp) / "build"
            build.mkdir()
            failures = verifier.run_seam_negative_probes(build, prefix, [])
            killed = [f for f in failures if "compiled against the installed headers" in f]
            self.assertTrue(
                killed,
                f"re-exposing the backend definition must fail the seam gate, "
                f"got {failures}")

    def test_unexpected_failure_reason_does_not_pass(self):
        with tempfile.TemporaryDirectory() as tmp:
            prefix = stage_installed_headers(tmp)
            build = Path(tmp) / "build"
            build.mkdir()
            ok = verifier.expect_compile_failure(
                build, prefix, "bogus_diagnostic",
                verifier.seam_probes()["derive_backend"][0], [],
                expect_diagnostics=("a diagnostic fragment that never appears",))
            self.assertFalse(
                ok, "a probe failing for another reason must not count as the "
                    "expected negative result")

    def test_installed_mutant_define_is_flagged(self):
        with tempfile.TemporaryDirectory() as tmp:
            prefix = stage_installed_headers(tmp)
            self.assertEqual(verifier.installed_mutant_define_findings(prefix), [],
                             "the allowlisted request_scope template seams are the only "
                             "installed mutation regions")
            installed = prefix / "include" / "sluice" / "async" / "request.hpp"
            installed.write_text(
                installed.read_text()
                + "\n#if defined(SLUICE_PUBLIC_REQUEST_MUTANT_RELEASE_FORGETS_BINDING)\n"
                  "#endif\n")
            findings = verifier.installed_mutant_define_findings(prefix)
            self.assertEqual(len(findings), 1,
                             f"a mutant-guarded installed header must be flagged, "
                             f"got {findings}")


if __name__ == "__main__":
    unittest.main()
