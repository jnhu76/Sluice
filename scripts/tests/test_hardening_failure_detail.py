#!/usr/bin/env python3
"""Unit tests for the hardening failure-detail diagnostic layer.

Verifies the best-effort log parser + failure fingerprinting + aggregation
(prompt §8/§10). Pure-logic: no xmake, clang, or real Sluice build required.
Log samples are written to temp files so the parser reads real file content.

Run with:
    python3 -m unittest discover -v scripts/tests
"""
from __future__ import annotations

import sys
import tempfile
import unittest
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional

_SCRIPT_DIR = Path(__file__).resolve().parent.parent
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))

from hardening.failure_detail import (
    FailureGroup,
    TestFailureDetail,
    aggregate_failures,
    classify_exit_semantics,
    parse_failure_detail,
    strip_ansi,
)
from hardening.model import Classification, CommandResult, Verdict


def _write_log(tmpdir: Path, name: str, content: str) -> Path:
    p = tmpdir / name
    p.write_text(content)
    return p


# A minimal CommandResult-like record for aggregate_failures tests. We use the
# real CommandResult so the integration path is exercised end-to-end.
def _result(tmpdir: Path, name: str, content: str, *,
            iteration: str = "1", phase: str = "debug-soak",
            classification: Classification = Classification.FAIL,
            exit_code: int = 255, started_at: str = "2026-07-30T16:00:00",
            command: Optional[List[str]] = None) -> CommandResult:
    log = _write_log(tmpdir, name, content)
    return CommandResult(
        phase=phase, iteration=iteration, target="full-suite", mode="debug",
        command=command or ["xmake", "test", "-v"],
        classification=classification, exit_code=exit_code,
        duration_seconds=6.0, log_path=log, sanitizer_signature=None,
        timed_out=False, term_sent=False, kill_sent=False,
        started_at=started_at, finished_at=started_at,
    )


# Canonical Sluice harness FAILED block (tests/harness.hpp output) + xmake
# "Detailed summary: Failed tests:" block (the real soak log format).
_HARNESS_FAIL_LOG = """[run] event_parked_live_worker_awakened_by_external_set
FAILED in case: event_parked_live_worker_awakened_by_external_set
FAILED 1 check(s):
  tests/event_primitive_test.cpp:2438: !watchdog_fired.load()
      T32: watchdog fired (external setter did not wake the parked worker within the bound)

errors: run event_primitive_test/event_primitive_test failed, exit code: 1

Detailed summary:
Failed tests:
 - event_primitive_test/event_primitive_test

99% tests passed, 1 test(s) failed out of 119, spent 5.297s
"""

# xmake abnormal-termination log (NO harness FAILED block): std::terminate.
_TERMINATE_LOG = """[ 96%]: wait_queue_unlink_topology_test/wait_queue_unlink_topology_test ............................ failed 0.144s
stdout: [run] wqtopo_c5_scheduler_integrated_topology

stderr: terminate called without an active exception

errors: run wait_queue_unlink_topology_test/wait_queue_unlink_topology_test failed, exit code: -1
99% tests passed, 1 test(s) failed out of 119, spent 4.796s
"""


class StripAnsiTest(unittest.TestCase):
    def test_strips_color_escapes(self) -> None:
        s = "\x1b[38;5;2;1m99%\x1b[0m tests passed"
        self.assertEqual(strip_ansi(s), "99% tests passed")

    def test_no_escapes_unchanged(self) -> None:
        self.assertEqual(strip_ansi("plain text"), "plain text")


class ParseHarnessFailureTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp()
        self.tmpdir = Path(self.tmp)

    def test_parses_single_harness_assertion(self) -> None:
        log = _write_log(self.tmpdir, "a.log", _HARNESS_FAIL_LOG)
        d = parse_failure_detail(log, "xmake test -v")
        self.assertEqual(d.parse_status, "harness_assertion")
        self.assertEqual(d.framework, "sluice_harness")
        self.assertEqual(d.failing_binary, "event_primitive_test")
        self.assertEqual(d.case_name,
                         "event_parked_live_worker_awakened_by_external_set")
        self.assertEqual(d.source_file, "tests/event_primitive_test.cpp")
        self.assertEqual(d.source_line, 2438)
        self.assertEqual(d.expression, "!watchdog_fired.load()")
        self.assertIn("watchdog fired", d.message or "")
        self.assertIn("1 test(s) failed out of 119", d.xmake_summary or "")

    def test_file_line_parsed_as_int(self) -> None:
        log = _write_log(self.tmpdir, "a.log", _HARNESS_FAIL_LOG)
        d = parse_failure_detail(log)
        self.assertIsInstance(d.source_line, int)
        self.assertGreater(d.source_line or 0, 0)


class ParseCaseNameTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp()
        self.tmpdir = Path(self.tmp)

    def test_case_name_extracted(self) -> None:
        log = _write_log(self.tmpdir, "a.log", _HARNESS_FAIL_LOG)
        d = parse_failure_detail(log)
        self.assertEqual(d.case_name,
                         "event_parked_live_worker_awakened_by_external_set")


class ParseMultipleChecksTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp()
        self.tmpdir = Path(self.tmp)

    def test_first_check_block_extracted(self) -> None:
        # A log with multiple FAILED check lines: the first file:line:expr wins.
        log = _write_log(self.tmpdir, "a.log", """FAILED in case: multi_case
FAILED 3 check(s):
  tests/foo.cpp:10: cond_a
      msg a
  tests/foo.cpp:20: cond_b
      msg b
  tests/foo.cpp:30: cond_c
      msg c
99% tests passed, 1 test(s) failed out of 119
""")
        d = parse_failure_detail(log)
        self.assertEqual(d.source_line, 10)
        self.assertEqual(d.expression, "cond_a")
        self.assertEqual(d.message, "msg a")


class ParseMessageTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp()
        self.tmpdir = Path(self.tmp)

    def test_message_extracted(self) -> None:
        log = _write_log(self.tmpdir, "a.log", _HARNESS_FAIL_LOG)
        d = parse_failure_detail(log)
        self.assertIsNotNone(d.message)
        self.assertTrue(d.message.startswith("T32:"))


class ParseXmakeSummaryTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp()
        self.tmpdir = Path(self.tmp)

    def test_summary_line_parsed(self) -> None:
        log = _write_log(self.tmpdir, "a.log", _HARNESS_FAIL_LOG)
        d = parse_failure_detail(log)
        self.assertEqual(d.xmake_summary,
                         "1 test(s) failed out of 119 (99% passed)")

    def test_summary_with_color_codes(self) -> None:
        log = _write_log(self.tmpdir, "a.log",
                         "\x1b[38;5;2;1m99%\x1b[0m tests passed, "
                         "\x1b[38;5;1;1m1\x1b[0m test(s) failed out of 119")
        d = parse_failure_detail(log)
        self.assertEqual(d.xmake_summary,
                         "1 test(s) failed out of 119 (99% passed)")


class Exit255SemanticsTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp()
        self.tmpdir = Path(self.tmp)

    def test_xmake_test_255_annotated(self) -> None:
        log = _write_log(self.tmpdir, "a.log", _HARNESS_FAIL_LOG)
        d = parse_failure_detail(log, "xmake test -v")
        # Override exit semantics with the real exit code path.
        d.exit_semantics = classify_exit_semantics(
            255, False, False, False, "xmake test -v", d)
        self.assertEqual(d.exit_semantics,
                         "xmake test failure exit (-1 surfaced as 255)")

    def test_arbitrary_255_not_annotated_as_xmake(self) -> None:
        # A non-xmake-test command with exit 255 must NOT get the xmake note.
        d = TestFailureDetail(parse_status="unknown")
        d.exit_semantics = classify_exit_semantics(
            255, False, False, False, "./some_binary", d)
        self.assertNotIn("xmake", d.exit_semantics)

    def test_process_terminate_semantics(self) -> None:
        d = TestFailureDetail(parse_status="process_terminate")
        d.exit_semantics = classify_exit_semantics(
            -1, False, False, False, "xmake test -v", d)
        self.assertEqual(d.exit_semantics,
                         "test binary aborted via std::terminate")

    def test_timeout_not_misclassified(self) -> None:
        d = TestFailureDetail(parse_status="unknown")
        d.exit_semantics = classify_exit_semantics(
            124, True, False, False, "xmake test -v", d)
        self.assertEqual(d.exit_semantics, "timeout_or_signal")


class UnknownLogFallbackTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp()
        self.tmpdir = Path(self.tmp)

    def test_garbage_log_does_not_crash(self) -> None:
        log = _write_log(self.tmpdir, "a.log", "random garbage\nno structure\n")
        d = parse_failure_detail(log, "xmake test -v")
        self.assertEqual(d.parse_status, "unknown")
        self.assertIsNone(d.failing_binary)
        self.assertIsNone(d.case_name)

    def test_missing_log_handled(self) -> None:
        d = parse_failure_detail(self.tmpdir / "does_not_exist.log")
        self.assertEqual(d.parse_status, "missing_log")

    def test_empty_log_handled(self) -> None:
        log = _write_log(self.tmpdir, "a.log", "")
        d = parse_failure_detail(log)
        self.assertEqual(d.parse_status, "empty_log")

    def test_process_terminate_no_harness_block(self) -> None:
        log = _write_log(self.tmpdir, "a.log", _TERMINATE_LOG)
        d = parse_failure_detail(log, "xmake test -v")
        self.assertEqual(d.parse_status, "process_terminate")
        self.assertEqual(d.abnormal_signature,
                         "terminate_called_without_active_exception")
        self.assertEqual(d.failing_binary, "wait_queue_unlink_topology_test")


class FingerprintStabilityTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp()
        self.tmpdir = Path(self.tmp)

    def test_same_log_same_fingerprint(self) -> None:
        log1 = _write_log(self.tmpdir, "a.log", _HARNESS_FAIL_LOG)
        log2 = _write_log(self.tmpdir, "b.log", _HARNESS_FAIL_LOG)
        d1 = parse_failure_detail(log1)
        d2 = parse_failure_detail(log2)
        self.assertEqual(d1.fingerprint, d2.fingerprint)

    def test_different_case_different_fingerprint(self) -> None:
        log_a = _write_log(self.tmpdir, "a.log", _HARNESS_FAIL_LOG)
        log_b = _write_log(self.tmpdir, "b.log", _HARNESS_FAIL_LOG.replace(
            "event_parked_live_worker_awakened_by_external_set",
            "some_other_case"))
        d_a = parse_failure_detail(log_a)
        d_b = parse_failure_detail(log_b)
        self.assertNotEqual(d_a.fingerprint, d_b.fingerprint)


class AggregateFailuresTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp()
        self.tmpdir = Path(self.tmp)

    def test_groups_by_fingerprint(self) -> None:
        r1 = _result(self.tmpdir, "a1.log", _HARNESS_FAIL_LOG, iteration="22")
        r2 = _result(self.tmpdir, "a2.log", _HARNESS_FAIL_LOG, iteration="40")
        r3 = _result(self.tmpdir, "a3.log", _HARNESS_FAIL_LOG, iteration="56")
        groups, total = aggregate_failures([r1, r2, r3])
        self.assertEqual(len(groups), 1)
        self.assertEqual(total, 3)
        self.assertEqual(groups[0].occurrences, 3)
        self.assertEqual(groups[0].binary, "event_primitive_test")

    def test_distinct_groups_separate(self) -> None:
        r1 = _result(self.tmpdir, "a.log", _HARNESS_FAIL_LOG, iteration="22")
        r2 = _result(self.tmpdir, "b.log", _TERMINATE_LOG, iteration="23",
                     exit_code=-1)
        groups, total = aggregate_failures([r1, r2])
        self.assertEqual(len(groups), 2)
        self.assertEqual(total, 2)
        binaries = {g.binary for g in groups}
        self.assertIn("event_primitive_test", binaries)
        self.assertIn("wait_queue_unlink_topology_test", binaries)

    def test_sorted_by_occurrence_desc(self) -> None:
        # 3 of group A, 1 of group B -> A first.
        recs = [
            _result(self.tmpdir, f"a{i}.log", _HARNESS_FAIL_LOG,
                    iteration=str(i)) for i in range(3)
        ]
        recs.append(_result(self.tmpdir, "b.log", _TERMINATE_LOG,
                            iteration="99", exit_code=-1))
        groups, _ = aggregate_failures(recs)
        self.assertEqual(groups[0].occurrences, 3)
        self.assertEqual(groups[1].occurrences, 1)


class RetryAggregationTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp()
        self.tmpdir = Path(self.tmp)

    def test_retry_iteration_counted(self) -> None:
        r1 = _result(self.tmpdir, "a.log", _HARNESS_FAIL_LOG, iteration="40")
        r2 = _result(self.tmpdir, "a2.log", _HARNESS_FAIL_LOG,
                     iteration="40-retry2")
        groups, total = aggregate_failures([r1, r2])
        self.assertEqual(total, 2)
        self.assertEqual(groups[0].occurrences, 2)
        # The retry is a failure-kind too, so it counts as reproduced.
        self.assertEqual(groups[0].retry_total, 1)
        self.assertEqual(groups[0].retry_reproduced, 1)

    def test_base_and_retry_in_same_group(self) -> None:
        r1 = _result(self.tmpdir, "a.log", _HARNESS_FAIL_LOG, iteration="405")
        r2 = _result(self.tmpdir, "b.log", _HARNESS_FAIL_LOG,
                     iteration="405-retry2")
        groups, _ = aggregate_failures([r1, r2])
        self.assertEqual(len(groups), 1)
        self.assertIn("405", groups[0].iterations)
        self.assertIn("405-retry2", groups[0].iterations)


class SummaryTextSnapshotTest(unittest.TestCase):
    """Verify the standalone failure-summary.txt writer produces key fields."""

    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp()
        self.tmpdir = Path(self.tmp)

    def test_summary_txt_contains_distinct_groups(self) -> None:
        from hardening.reporting import write_failure_summary_txt
        r1 = _result(self.tmpdir, "a.log", _HARNESS_FAIL_LOG, iteration="22")
        r2 = _result(self.tmpdir, "b.log", _TERMINATE_LOG, iteration="23",
                     exit_code=-1)
        out_dir = Path(tempfile.mkdtemp())
        write_failure_summary_txt(out_dir, [r1, r2])
        txt = (out_dir / "failure-summary.txt").read_text()
        self.assertIn("Distinct failures: 2", txt)
        self.assertIn("Total failure occurrences: 2", txt)
        self.assertIn("event_primitive_test", txt)
        self.assertIn("wait_queue_unlink_topology_test", txt)
        self.assertIn("FINGERPRINT:", txt)

    def test_summary_txt_empty_when_no_failures(self) -> None:
        from hardening.reporting import write_failure_summary_txt
        out_dir = Path(tempfile.mkdtemp())
        write_failure_summary_txt(out_dir, [])
        txt = (out_dir / "failure-summary.txt").read_text()
        self.assertIn("Distinct failures: 0", txt)
        self.assertIn("(no failures)", txt)


class SummaryJsonSchemaTest(unittest.TestCase):
    """Verify summary.json failure-group schema fields (schema_version 3)."""

    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp()
        self.tmpdir = Path(self.tmp)

    def _write_summary_json(self, failures: list) -> dict:
        import json
        from hardening.model import Config, FuzzCorpusSnapshot
        from hardening.preflight import PreflightResult
        from hardening.reporting import write_summary_json
        out_dir = Path(tempfile.mkdtemp())
        cfg = Config(mode="hardening", hours=1.0, phase_timeout_seconds=1200,
                     fuzz_seconds_override=None, keep_going=True)
        pf = PreflightResult(head_sha="x" * 40, head_short="abcdef",
                             worktree_dirty=False, nproc=8, disk_gib=100.0,
                             checks=[], tool_versions={}, compiler_probes={})
        write_summary_json(out_dir, Verdict.HOLD, cfg, pf,
                           0.0, 1.0, [], failures, {}, [], False)
        return json.loads((out_dir / "summary.json").read_text())

    def test_schema_version_is_3(self) -> None:
        d = self._write_summary_json([])
        self.assertEqual(d["schema_version"], 3)

    def test_failure_group_fields_present(self) -> None:
        r = _result(self.tmpdir, "a.log", _HARNESS_FAIL_LOG, iteration="22")
        d = self._write_summary_json([r])
        self.assertIn("distinct_failures", d)
        self.assertIn("total_failure_occurrences", d)
        self.assertIn("failure_groups", d)
        self.assertEqual(d["distinct_failures"], 1)
        self.assertEqual(d["total_failure_occurrences"], 1)
        g = d["failure_groups"][0]
        for field in ("fingerprint", "occurrences", "binary", "case",
                      "source_file", "source_line", "expression", "message",
                      "phases", "iterations", "sample_logs", "first_seen",
                      "last_seen", "retry_reproduced", "retry_total"):
            self.assertIn(field, g, f"missing field {field}")
        self.assertEqual(g["binary"], "event_primitive_test")
        self.assertEqual(g["source_line"], 2438)


if __name__ == "__main__":
    unittest.main()
