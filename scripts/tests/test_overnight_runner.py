#!/usr/bin/env python3
"""Unit tests for the overnight runner - pure logic only.

These tests do NOT require xmake, clang, or any real Sluice build.
They verify data models, classification, deadline calculation, verdict
computation, and other pure-logic functions.

Run with:
    python3 -m unittest discover -v scripts/tests
"""

from __future__ import annotations

import json
import os
import sys
import tempfile
import time
import unittest
from pathlib import Path
from typing import List

# Ensure the package is importable.
_SCRIPT_DIR = Path(__file__).resolve().parent.parent
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))

from overnight.model import (
    Classification,
    CommandResult,
    CommandSpec,
    Config,
    FuzzCorpusSnapshot,
    PhaseStats,
    Verdict,
    VERDICT_EXIT,
)
from overnight.process import scan_sanitizer, classify_with_sanitizer
from overnight.reporting import (
    write_events_jsonl,
    write_failures_jsonl,
    write_summary_txt,
    write_summary_json,
    write_preflight_txt,
    write_preflight_json,
    write_environment_json,
)
from overnight.preflight import PreflightCheck, PreflightResult
from overnight.cli import parse_args
from overnight.phases import (
    PhaseContext,
    _strip_ansi,
    _is_valid_target_token,
    _is_fuzz_artifact,
    _list_fuzz_artifacts,
    _corpus_file_count,
    _corpus_file_bytes,
    TSAN_HOT_SET,
)


# ======================================================================
# Configuration parsing tests
# ======================================================================

class TestConfigParsing(unittest.TestCase):

    def test_default_config(self):
        config = parse_args([])
        self.assertEqual(config.mode, "overnight")
        self.assertEqual(config.hours, 8.0)
        self.assertEqual(config.phase_timeout_seconds, 1200)
        self.assertIsNone(config.fuzz_seconds_override)
        self.assertTrue(config.keep_going)

    def test_smoke_mode(self):
        config = parse_args(["--smoke"])
        self.assertEqual(config.mode, "smoke")
        self.assertEqual(config.hours, 1.0)

    def test_self_test_mode(self):
        config = parse_args(["--self-test"])
        self.assertEqual(config.mode, "selftest")

    def test_hours_cli(self):
        config = parse_args(["--hours", "4.5"])
        self.assertEqual(config.hours, 4.5)
        self.assertEqual(config.hours_source, "cli")

    def test_hours_env(self):
        os.environ["SLUICE_OVERNIGHT_HOURS"] = "3"
        try:
            config = parse_args([])
            self.assertEqual(config.hours, 3.0)
            self.assertEqual(config.hours_source, "env")
        finally:
            del os.environ["SLUICE_OVERNIGHT_HOURS"]

    def test_hours_cli_overrides_env(self):
        os.environ["SLUICE_OVERNIGHT_HOURS"] = "3"
        try:
            config = parse_args(["--hours", "6"])
            self.assertEqual(config.hours, 6.0)
            self.assertEqual(config.hours_source, "cli")
        finally:
            del os.environ["SLUICE_OVERNIGHT_HOURS"]

    def test_phase_timeout_env(self):
        os.environ["SLUICE_PHASE_TIMEOUT"] = "300"
        try:
            config = parse_args([])
            self.assertEqual(config.phase_timeout_seconds, 300)
        finally:
            del os.environ["SLUICE_PHASE_TIMEOUT"]

    def test_fuzz_override_env(self):
        os.environ["SLUICE_FUZZ_SECONDS"] = "120"
        try:
            config = parse_args([])
            self.assertEqual(config.fuzz_seconds_override, 120)
        finally:
            del os.environ["SLUICE_FUZZ_SECONDS"]

    def test_keep_going_env(self):
        os.environ["SLUICE_KEEP_GOING"] = "0"
        try:
            config = parse_args([])
            self.assertFalse(config.keep_going)
        finally:
            del os.environ["SLUICE_KEEP_GOING"]

    def test_keep_going_env_invalid(self):
        os.environ["SLUICE_KEEP_GOING"] = "2"
        try:
            parse_args([])
            self.fail("Expected ValueError")
        except ValueError:
            pass
        finally:
            del os.environ["SLUICE_KEEP_GOING"]

    def test_negative_hours_raises(self):
        with self.assertRaises(ValueError):
            parse_args(["--hours", "-1"])

    def test_zero_hours_raises(self):
        with self.assertRaises(ValueError):
            parse_args(["--hours", "0"])


# ======================================================================
# Model tests
# ======================================================================

class TestModel(unittest.TestCase):

    def test_classification_values(self):
        self.assertEqual(Classification.PASS.value, "PASS")
        self.assertEqual(Classification.FAIL.value, "FAIL")
        self.assertEqual(Classification.TIMEOUT.value, "TIMEOUT")
        self.assertEqual(Classification.SKIP.value, "SKIP")
        self.assertEqual(Classification.SANITIZER_FAIL.value, "SANITIZER_FAIL")
        self.assertEqual(Classification.FUZZ_CRASH.value, "FUZZ_CRASH")

    def test_verdict_values(self):
        self.assertEqual(Verdict.PASS.value, "PASS")
        self.assertEqual(Verdict.HOLD.value, "HOLD")
        self.assertEqual(Verdict.INCOMPLETE.value, "INCOMPLETE")
        self.assertEqual(Verdict.ENVIRONMENT_ERROR.value, "ENVIRONMENT_ERROR")
        self.assertEqual(Verdict.RUNNER_ERROR.value, "RUNNER_ERROR")

    def test_verdict_exit_codes(self):
        self.assertEqual(VERDICT_EXIT[Verdict.PASS], 0)
        self.assertEqual(VERDICT_EXIT[Verdict.HOLD], 1)
        self.assertEqual(VERDICT_EXIT[Verdict.ENVIRONMENT_ERROR], 2)
        self.assertEqual(VERDICT_EXIT[Verdict.RUNNER_ERROR], 3)
        self.assertEqual(VERDICT_EXIT[Verdict.INCOMPLETE], 4)

    def test_command_header_lines(self):
        spec = CommandSpec(
            phase="test", iteration="1", target="tgt", mode="debug",
            command=["echo", "hello"], timeout_seconds=30,
            log_path=Path("/tmp/test.log"),
        )
        lines = spec.header_lines("abc123", False)
        self.assertIn("phase=test", lines)
        self.assertIn("iteration=1", lines)
        self.assertIn("target=tgt", lines)
        self.assertIn("mode=debug", lines)
        self.assertIn("head=abc123", lines)
        self.assertIn("worktree_dirty=0", lines)
        self.assertIn("command=echo hello", lines)
        self.assertIn("timeout_s=30", lines)
        self.assertIn("----- output -----", lines)

    def test_command_footer_lines(self):
        spec = CommandSpec(
            phase="test", iteration="1", target="tgt", mode="debug",
            command=["true"], timeout_seconds=10,
            log_path=Path("/tmp/test.log"),
        )
        lines = spec.footer_lines(
            end_epoch=1000, duration_s=0.5, raw_exit=0,
            classification=Classification.PASS,
            timed_out=False, term_sent=False, kill_sent=False,
            sanitizer_signature=None,
        )
        self.assertIn("duration_s=0.5", lines)
        self.assertIn("raw_exit=0", lines)
        self.assertIn("classification=PASS", lines)
        self.assertIn("timed_out=0", lines)

    def test_command_result_to_json(self):
        r = CommandResult(
            phase="test", iteration="1", target="tgt", mode="debug",
            command=["true"], classification=Classification.PASS,
            exit_code=0, duration_seconds=0.5, log_path=Path("/tmp/test.log"),
            sanitizer_signature=None, timed_out=False,
            term_sent=False, kill_sent=False,
            started_at="2024-01-01T00:00:00",
            finished_at="2024-01-01T00:00:01",
        )
        d = r.to_json_dict()
        self.assertEqual(d["classification"], "PASS")
        self.assertEqual(d["exit_code"], 0)
        self.assertEqual(d["duration_seconds"], 0.5)
        self.assertEqual(d["synthetic"], False)


# ======================================================================
# Sanitizer scanning tests
# ======================================================================

class TestSanitizerScanning(unittest.TestCase):

    def test_tsan_signature(self):
        log = "some output\nWARNING: ThreadSanitizer: data race\nmore output"
        sig = scan_sanitizer(log, "tsan")
        self.assertIsNotNone(sig)
        self.assertIn("ThreadSanitizer", sig or "")

    def test_tsan_data_race(self):
        log = "WARNING: data race detected"
        sig = scan_sanitizer(log, "tsan")
        self.assertIsNotNone(sig)

    def test_tsan_deadlock(self):
        log = "WARNING: deadlock detected"
        sig = scan_sanitizer(log, "tsan")
        self.assertIsNotNone(sig)

    def test_asan_error(self):
        log = "ERROR: AddressSanitizer: heap-use-after-free"
        sig = scan_sanitizer(log, "asan")
        self.assertIsNotNone(sig)

    def test_asan_leak(self):
        log = "LeakSanitizer: detected memory leaks"
        sig = scan_sanitizer(log, "asan")
        self.assertIsNotNone(sig)

    def test_ubsan_runtime_error(self):
        log = "runtime error: shift exponent too large"
        sig = scan_sanitizer(log, "asan")
        self.assertIsNotNone(sig)

    def test_ubsan_summary(self):
        log = "SUMMARY: UndefinedBehaviorSanitizer"
        sig = scan_sanitizer(log, "asan")
        self.assertIsNotNone(sig)

    def test_no_false_positive_in_header(self):
        log = "----- output -----\nnormal output\n"
        sig = scan_sanitizer(log, "tsan")
        self.assertIsNone(sig)

    def test_exit_zero_with_sanitizer(self):
        cls, sig = classify_with_sanitizer(
            0, "WARNING: ThreadSanitizer", "tsan", False
        )
        self.assertEqual(cls, Classification.SANITIZER_FAIL)
        self.assertIsNotNone(sig)

    def test_exit_zero_clean(self):
        cls, sig = classify_with_sanitizer(0, "all good", "tsan", False)
        self.assertEqual(cls, Classification.PASS)
        self.assertIsNone(sig)

    def test_timed_out_overrides_sanitizer(self):
        cls, sig = classify_with_sanitizer(
            0, "WARNING: ThreadSanitizer", "tsan", True
        )
        self.assertEqual(cls, Classification.TIMEOUT)

    def test_exit_77_is_skip(self):
        cls, sig = classify_with_sanitizer(77, "some output", None, False)
        self.assertEqual(cls, Classification.SKIP)


# ======================================================================
# Verdict calculation tests
# ======================================================================

class TestVerdictCalculation(unittest.TestCase):

    def _make_preflight(self, passed: bool = True) -> PreflightResult:
        p = PreflightResult()
        if passed:
            p.checks.append(PreflightCheck(
                name="dummy", passed=True, is_fatal=False, message="ok",
            ))
        else:
            p.checks.append(PreflightCheck(
                name="tool-xmake", passed=False, is_fatal=True,
                message="xmake not found",
            ))
        return p

    def _make_ctx(self, **kwargs) -> PhaseContext:
        defaults = dict(
            config=Config(mode="overnight", hours=8,
                          phase_timeout_seconds=1200,
                          fuzz_seconds_override=None, keep_going=True),
            project_root=Path("/tmp"), run_dir=Path("/tmp"),
            head_sha="x", head_short="x", worktree_dirty=False,
            nproc=1, global_deadline=time.monotonic() + 3600,
            final_debug_reserved=1200, sticky_hold=False,
            baseline_ok=True, final_debug_ok=True,
        )
        defaults.update(kwargs)
        return PhaseContext(**defaults)

    def test_preflight_failure_is_environment_error(self):
        from overnight_local import calculate_verdict
        preflight = self._make_preflight(passed=False)
        ctx = self._make_ctx()
        verdict = calculate_verdict(ctx, preflight)
        self.assertEqual(verdict, Verdict.ENVIRONMENT_ERROR)

    def test_sticky_hold_is_hold(self):
        from overnight_local import calculate_verdict
        preflight = self._make_preflight()
        ctx = self._make_ctx(sticky_hold=True)
        verdict = calculate_verdict(ctx, preflight)
        self.assertEqual(verdict, Verdict.HOLD)

    def test_all_pass_is_pass(self):
        from overnight_local import calculate_verdict
        preflight = self._make_preflight()
        ctx = self._make_ctx()
        ctx.results.append(CommandResult(
            phase="tsan", iteration="1", target=TSAN_HOT_SET[0], mode="tsan",
            command=["xmake", "run", TSAN_HOT_SET[0]],
            classification=Classification.PASS, exit_code=0,
            duration_seconds=1, log_path=Path("/tmp/test.log"),
            sanitizer_signature=None, timed_out=False,
            term_sent=False, kill_sent=False,
            started_at="x", finished_at="x",
        ))
        ctx.stats["asanubsan"] = PhaseStats(executed=1, passed=1)
        ctx.stats["fuzz"] = PhaseStats(executed=1, passed=1)
        verdict = calculate_verdict(ctx, preflight)
        self.assertEqual(verdict, Verdict.PASS)

    def test_no_tsan_is_incomplete(self):
        from overnight_local import calculate_verdict
        preflight = self._make_preflight()
        ctx = self._make_ctx()
        ctx.stats["asanubsan"] = PhaseStats(executed=1, passed=1)
        ctx.stats["fuzz"] = PhaseStats(executed=1, passed=1)
        verdict = calculate_verdict(ctx, preflight)
        self.assertEqual(verdict, Verdict.INCOMPLETE)

    def test_no_asan_is_incomplete(self):
        from overnight_local import calculate_verdict
        preflight = self._make_preflight()
        ctx = self._make_ctx()
        ctx.results.append(CommandResult(
            phase="tsan", iteration="1", target=TSAN_HOT_SET[0], mode="tsan",
            command=["xmake", "run", TSAN_HOT_SET[0]],
            classification=Classification.PASS, exit_code=0,
            duration_seconds=1, log_path=Path("/tmp/test.log"),
            sanitizer_signature=None, timed_out=False,
            term_sent=False, kill_sent=False,
            started_at="x", finished_at="x",
        ))
        ctx.stats["fuzz"] = PhaseStats(executed=1, passed=1)
        verdict = calculate_verdict(ctx, preflight)
        self.assertEqual(verdict, Verdict.INCOMPLETE)


# ======================================================================
# Fuzz artifact detection tests
# ======================================================================

class TestFuzzArtifacts(unittest.TestCase):

    def test_crash_artifact(self):
        self.assertTrue(_is_fuzz_artifact("crash-1234"))
        self.assertTrue(_is_fuzz_artifact("crash-selftest"))

    def test_leak_artifact(self):
        self.assertTrue(_is_fuzz_artifact("leak-5678"))

    def test_timeout_artifact(self):
        self.assertTrue(_is_fuzz_artifact("timeout-9012"))

    def test_oom_artifact(self):
        self.assertTrue(_is_fuzz_artifact("oom-3456"))

    def test_panic_artifact(self):
        self.assertTrue(_is_fuzz_artifact("panic-7890"))

    def test_regular_file_not_artifact(self):
        self.assertFalse(_is_fuzz_artifact("normal_file.txt"))
        self.assertFalse(_is_fuzz_artifact("README.md"))

    def test_list_artifacts(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d / "crash-1").write_text("x")
            (d / "normal.txt").write_text("x")
            (d / "leak-2").write_text("x")
            arts = _list_fuzz_artifacts(d)
            self.assertIn("crash-1", arts)
            self.assertIn("leak-2", arts)
            self.assertNotIn("normal.txt", arts)

    def test_corpus_file_count(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d / "a").write_text("x")
            (d / "b").write_text("x")
            self.assertEqual(_corpus_file_count(d), 2)
            self.assertEqual(_corpus_file_count(Path("/nonexistent")), 0)

    def test_corpus_file_bytes(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d / "a").write_text("hello")
            (d / "b").write_text("world")
            self.assertGreater(_corpus_file_bytes(d), 0)
            self.assertEqual(_corpus_file_bytes(Path("/nonexistent")), 0)


# ======================================================================
# Reporting tests
# ======================================================================

class TestReporting(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.run_dir = Path(self.tmp.name)
        self.config = Config(mode="overnight", hours=8,
                             phase_timeout_seconds=1200,
                             fuzz_seconds_override=None, keep_going=True)
        self.preflight = PreflightResult()
        self.preflight.checks.append(PreflightCheck(name="dummy", passed=True, is_fatal=False, message="ok"))
        self.preflight.head_sha = "abc123"
        self.preflight.head_short = "abc123"
        self.preflight.nproc = 4
        self.preflight.disk_gib = 50.0

    def tearDown(self):
        self.tmp.cleanup()

    def test_events_jsonl(self):
        results = [
            CommandResult(
                phase="test", iteration="1", target="tgt", mode="debug",
                command=["true"], classification=Classification.PASS,
                exit_code=0, duration_seconds=0.5, log_path=Path("/tmp/l"),
                sanitizer_signature=None, timed_out=False,
                term_sent=False, kill_sent=False,
                started_at="x", finished_at="y",
            ),
        ]
        write_events_jsonl(self.run_dir, results)
        path = self.run_dir / "events.jsonl"
        self.assertTrue(path.is_file())
        data = json.loads(path.read_text().strip())
        self.assertEqual(data["classification"], "PASS")

    def test_failures_jsonl_only_failures(self):
        results = [
            CommandResult(
                phase="test", iteration="1", target="pass", mode="debug",
                command=["true"], classification=Classification.PASS,
                exit_code=0, duration_seconds=0.5, log_path=Path("/tmp/l1"),
                sanitizer_signature=None, timed_out=False,
                term_sent=False, kill_sent=False,
                started_at="x", finished_at="y",
            ),
            CommandResult(
                phase="test", iteration="2", target="fail", mode="debug",
                command=["false"], classification=Classification.FAIL,
                exit_code=1, duration_seconds=0.5, log_path=Path("/tmp/l2"),
                sanitizer_signature=None, timed_out=False,
                term_sent=False, kill_sent=False,
                started_at="x", finished_at="y",
            ),
        ]
        write_failures_jsonl(self.run_dir, results)
        path = self.run_dir / "failures.jsonl"
        lines = [json.loads(l) for l in path.read_text().strip().splitlines()]
        self.assertEqual(len(lines), 1)
        self.assertEqual(lines[0]["target"], "fail")

    def test_summary_txt_shows_none(self):
        write_summary_txt(
            run_dir=self.run_dir, verdict=Verdict.PASS, config=self.config,
            preflight=self.preflight, started_at=time.time(),
            finished_at=time.time(), results=[], failures=[],
            phase_stats={}, fuzz_results=[], interrupted=False,
        )
        text = (self.run_dir / "summary.txt").read_text()
        self.assertIn("Failures:", text)
        self.assertIn("none", text)

    def test_summary_json_has_verdict(self):
        write_summary_json(
            run_dir=self.run_dir, verdict=Verdict.PASS, config=self.config,
            preflight=self.preflight, started_at=time.time(),
            finished_at=time.time(), results=[], failures=[],
            phase_stats={}, fuzz_results=[], interrupted=False,
        )
        data = json.loads((self.run_dir / "summary.json").read_text())
        self.assertEqual(data["verdict"], "PASS")

    def test_preflight_txt(self):
        write_preflight_txt(self.run_dir, self.preflight)
        text = (self.run_dir / "preflight.txt").read_text()
        self.assertIn("HEAD:", text)

    def test_preflight_json(self):
        write_preflight_json(self.run_dir, self.preflight)
        data = json.loads((self.run_dir / "preflight.json").read_text())
        self.assertEqual(data["head_short"], "abc123")

    def test_environment_json(self):
        write_environment_json(self.run_dir, self.config, self.preflight)
        data = json.loads((self.run_dir / "environment.json").read_text())
        self.assertEqual(data["config"]["mode"], "overnight")


# ======================================================================
# Preflight helper tests
# ======================================================================

class TestPreflightHelpers(unittest.TestCase):

    def test_strip_ansi(self):
        clean = _strip_ansi("\x1b[31mhello\x1b[0m")
        self.assertEqual(clean, "hello")

    def test_is_valid_target_token(self):
        self.assertTrue(_is_valid_target_token("my_test"))
        self.assertTrue(_is_valid_target_token("multi_worker_test"))
        self.assertTrue(_is_valid_target_token("sluice_core"))
        self.assertFalse(_is_valid_target_token(""))
        self.assertFalse(_is_valid_target_token("invalid target"))
        self.assertFalse(_is_valid_target_token("target\nwith_newline"))

    def test_preflight_check_model(self):
        c = PreflightCheck(name="test", passed=True, message="ok")
        self.assertEqual(c.name, "test")
        self.assertTrue(c.passed)
        self.assertFalse(c.is_fatal)

        c2 = PreflightCheck(name="fail", passed=False, is_fatal=True,
                            message="bad")
        self.assertTrue(c2.is_fatal)

    def test_preflight_result_passed(self):
        r = PreflightResult()
        r.checks.append(PreflightCheck(name="a", passed=True, is_fatal=True,
                                       message="ok"))
        self.assertTrue(r.passed)
        self.assertEqual(len(r.fatal_failures), 0)

    def test_preflight_result_failed_fatal(self):
        r = PreflightResult()
        r.checks.append(PreflightCheck(name="a", passed=False, is_fatal=True,
                                       message="fail"))
        self.assertFalse(r.passed)
        self.assertEqual(len(r.fatal_failures), 1)

    def test_preflight_result_warning(self):
        r = PreflightResult()
        r.checks.append(PreflightCheck(name="w", passed=False, is_fatal=False,
                                       message="warn"))
        self.assertTrue(r.passed)
        self.assertEqual(len(r.warnings), 1)


# ======================================================================
# Keep-going state tests
# ======================================================================

class TestKeepGoing(unittest.TestCase):

    def test_keep_going_true_default(self):
        config = parse_args([])
        self.assertTrue(config.keep_going)

    def test_keep_going_env_0(self):
        os.environ["SLUICE_KEEP_GOING"] = "0"
        try:
            config = parse_args([])
            self.assertFalse(config.keep_going)
        finally:
            del os.environ["SLUICE_KEEP_GOING"]


# ======================================================================
# JSON serialization round-trip tests
# ======================================================================

class TestJsonSerialization(unittest.TestCase):

    def test_command_result_roundtrip(self):
        r = CommandResult(
            phase="test", iteration="1", target="tgt", mode="debug",
            command=["xmake", "run", "test"], classification=Classification.FAIL,
            exit_code=1, duration_seconds=10.5, log_path=Path("/tmp/test.log"),
            sanitizer_signature="WARNING: ThreadSanitizer",
            timed_out=False, term_sent=False, kill_sent=False,
            started_at="2024-01-01T00:00:00",
            finished_at="2024-01-01T00:00:10",
        )
        d = r.to_json_dict()
        self.assertEqual(d["classification"], "FAIL")
        self.assertEqual(d["exit_code"], 1)
        self.assertEqual(d["duration_seconds"], 10.5)
        self.assertEqual(d["timed_out"], False)
        self.assertEqual(d["sanitizer_signature"], "WARNING: ThreadSanitizer")

        s = json.dumps(d)
        d2 = json.loads(s)
        self.assertEqual(d2["classification"], "FAIL")
        self.assertEqual(d2["exit_code"], 1)


# ======================================================================
# PhaseStats tests
# ======================================================================

class TestPhaseStats(unittest.TestCase):

    def test_defaults(self):
        s = PhaseStats()
        self.assertEqual(s.iteration, 0)
        self.assertEqual(s.executed, 0)
        self.assertEqual(s.passed, 0)

    def test_increment(self):
        s = PhaseStats()
        s.iteration += 1
        s.executed += 1
        s.passed += 1
        self.assertEqual(s.iteration, 1)
        self.assertEqual(s.executed, 1)
        self.assertEqual(s.passed, 1)


# ======================================================================
# FuzzCorpusSnapshot tests
# ======================================================================

class TestFuzzCorpusSnapshot(unittest.TestCase):

    def test_defaults(self):
        s = FuzzCorpusSnapshot(target="my_fuzz")
        self.assertEqual(s.target, "my_fuzz")
        self.assertEqual(s.before_files, 0)
        self.assertEqual(s.new_artifacts, [])


# ======================================================================
# Verdict exit code consistency tests
# ======================================================================

class TestExitCodeConsistency(unittest.TestCase):

    def test_verdict_exit_mapping(self):
        self.assertEqual(VERDICT_EXIT[Verdict.PASS], 0)
        self.assertEqual(VERDICT_EXIT[Verdict.HOLD], 1)
        self.assertEqual(VERDICT_EXIT[Verdict.ENVIRONMENT_ERROR], 2)
        self.assertEqual(VERDICT_EXIT[Verdict.RUNNER_ERROR], 3)
        self.assertEqual(VERDICT_EXIT[Verdict.INCOMPLETE], 4)


if __name__ == "__main__":
    unittest.main()
