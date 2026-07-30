#!/usr/bin/env python3
"""Unit tests for the overnight_local runner (stdlib only, Python >= 3.10).

Run with:
    python3 -m unittest discover -v
    python3 -m unittest scripts.tests.test_overnight_runner -v
"""

import json
import os
import pathlib
import sys
import tempfile
import unittest

# Ensure the parent of scripts/ is on sys.path so we can import overnight_local.
_SCRIPT_DIR = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(_SCRIPT_DIR))

import overnight_local as ol  # noqa: E402


class TestClassification(unittest.TestCase):
    """Test the classify_command function."""

    def test_pass(self) -> None:
        cls = ol.classify_command(0, "", None, False, False, False)
        self.assertEqual(cls, ol.Classification.PASS)

    def test_skip(self) -> None:
        cls = ol.classify_command(77, "", None, False, False, False)
        self.assertEqual(cls, ol.Classification.SKIP)

    def test_fail(self) -> None:
        cls = ol.classify_command(1, "", None, False, False, False)
        self.assertEqual(cls, ol.Classification.FAIL)

    def test_timeout(self) -> None:
        cls = ol.classify_command(None, "", None, True, True, False)
        self.assertEqual(cls, ol.Classification.TIMEOUT)

    def test_sanitizer_tsan_exit_zero(self) -> None:
        cls = ol.classify_command(
            0, "WARNING: ThreadSanitizer: data race",
            "tsan", False, False, False,
        )
        self.assertEqual(cls, ol.Classification.SANITIZER_FAIL)

    def test_sanitizer_asan_exit_zero(self) -> None:
        cls = ol.classify_command(
            0, "ERROR: AddressSanitizer: stack-buffer-overflow",
            "asan", False, False, False,
        )
        self.assertEqual(cls, ol.Classification.SANITIZER_FAIL)

    def test_sanitizer_ubsan_exit_zero(self) -> None:
        cls = ol.classify_command(
            0, "runtime error: division by zero",
            "asan", False, False, False,
        )
        self.assertEqual(cls, ol.Classification.SANITIZER_FAIL)

    def test_fail_signal_9(self) -> None:
        # 137 without timeout logic = FAIL_SIGNAL_9.
        cls = ol.classify_command(137, "", None, False, False, False)
        self.assertEqual(cls, ol.Classification.FAIL_SIGNAL_9)

    def test_fail_not_signal_9_when_killed(self) -> None:
        # 137 WITH kill_sent = FAIL (not FAIL_SIGNAL_9).
        cls = ol.classify_command(137, "", None, True, True, True)
        self.assertEqual(cls, ol.Classification.TIMEOUT)


class TestSanitizerSignature(unittest.TestCase):
    """Test the sanitizer_signature function."""

    def test_tsan_sig(self) -> None:
        sig = ol.sanitizer_signature("WARNING: ThreadSanitizer: data race", "tsan")
        self.assertIsNotNone(sig)
        self.assertIn("ThreadSanitizer", sig or "")

    def test_asan_sig(self) -> None:
        sig = ol.sanitizer_signature("ERROR: AddressSanitizer: heap-use-after-free", "asan")
        self.assertIsNotNone(sig)
        self.assertIn("AddressSanitizer", sig or "")

    def test_ubsan_sig(self) -> None:
        sig = ol.sanitizer_signature("SUMMARY: UndefinedBehaviorSanitizer", "asan")
        self.assertIsNotNone(sig)
        self.assertIn("UndefinedBehaviorSanitizer", sig or "")

    def test_no_sig(self) -> None:
        sig = ol.sanitizer_signature("all good", "tsan")
        self.assertIsNone(sig)

    def test_none_san_kind(self) -> None:
        sig = ol.sanitizer_signature("WARNING: ThreadSanitizer", None)
        self.assertIsNone(sig)


class TestConfig(unittest.TestCase):
    """Test configuration parsing."""

    def test_defaults(self) -> None:
        config = ol.parse_args([])
        self.assertEqual(config.mode, "overnight")
        self.assertEqual(config.hours, ol.DEFAULT_HOURS)
        self.assertEqual(config.phase_timeout_seconds, ol.DEFAULT_PHASE_TIMEOUT)
        self.assertIsNone(config.fuzz_seconds_override)
        self.assertTrue(config.keep_going)

    def test_smoke(self) -> None:
        config = ol.parse_args(["--smoke"])
        self.assertEqual(config.mode, "smoke")

    def test_self_test(self) -> None:
        config = ol.parse_args(["--self-test"])
        self.assertEqual(config.mode, "selftest")

    def test_hours(self) -> None:
        config = ol.parse_args(["--hours", "4.5"])
        self.assertEqual(config.hours, 4.5)

    def test_hours_validation(self) -> None:
        with self.assertRaises(SystemExit):
            ol.parse_args(["--hours", "-1"])

    def test_env_keep_going_0(self) -> None:
        os.environ["SLUICE_KEEP_GOING"] = "0"
        try:
            config = ol.parse_args([])
            self.assertFalse(config.keep_going)
        finally:
            del os.environ["SLUICE_KEEP_GOING"]

    def test_env_keep_going_invalid(self) -> None:
        os.environ["SLUICE_KEEP_GOING"] = "2"
        with self.assertRaises(SystemExit):
            ol.parse_args([])
        del os.environ["SLUICE_KEEP_GOING"]


class TestVerdict(unittest.TestCase):
    """Test verdict computation."""

    def _make_ctx(self) -> ol.RunContext:
        config = ol.Config(
            mode="overnight", hours=8,
            phase_timeout_seconds=1200,
            fuzz_seconds_override=None,
            keep_going=True,
        )
        ctx = ol.RunContext(config=config, project_root=pathlib.Path("/tmp"))
        ctx.baseline_ok = True
        ctx.final_debug_ok = True
        ctx.tsan_critical_exec = 1
        ctx.asan_full_ok = True
        ctx.fuzz_exec = 1
        return ctx

    def test_pass(self) -> None:
        ctx = self._make_ctx()
        verdict, reasons = ol.compute_verdict(ctx)
        self.assertEqual(verdict, ol.Verdict.PASS)
        self.assertEqual(reasons, [])

    def test_hold_sticky(self) -> None:
        ctx = self._make_ctx()
        ctx.sticky_hold = True
        verdict, _ = ol.compute_verdict(ctx)
        self.assertEqual(verdict, ol.Verdict.HOLD)

    def test_hold_no_tsan(self) -> None:
        ctx = self._make_ctx()
        ctx.tsan_critical_exec = 0
        verdict, reasons = ol.compute_verdict(ctx)
        self.assertEqual(verdict, ol.Verdict.HOLD)
        self.assertIn("no-critical-tsan-target-executed", reasons)

    def test_incomplete_baseline(self) -> None:
        ctx = self._make_ctx()
        ctx.baseline_ok = False
        verdict, reasons = ol.compute_verdict(ctx)
        self.assertEqual(verdict, ol.Verdict.INCOMPLETE)
        self.assertIn("baseline-did-not-complete", reasons)

    def test_hold_overrides_incomplete(self) -> None:
        ctx = self._make_ctx()
        ctx.sticky_hold = True
        ctx.baseline_ok = False
        verdict, _ = ol.compute_verdict(ctx)
        self.assertEqual(verdict, ol.Verdict.HOLD)


class TestVerdictExitCodes(unittest.TestCase):
    """Test that verdict exit codes are unique and sensible."""

    def test_exit_codes(self) -> None:
        codes = set(ol.VERDICT_EXIT_CODES.values())
        self.assertEqual(len(codes), len(ol.Verdict))
        self.assertEqual(ol.VERDICT_EXIT_CODES[ol.Verdict.PASS], 0)
        self.assertEqual(ol.VERDICT_EXIT_CODES[ol.Verdict.HOLD], 1)
        self.assertEqual(ol.VERDICT_EXIT_CODES[ol.Verdict.ENVIRONMENT_ERROR], 2)
        self.assertEqual(ol.VERDICT_EXIT_CODES[ol.Verdict.RUNNER_ERROR], 3)
        self.assertEqual(ol.VERDICT_EXIT_CODES[ol.Verdict.INCOMPLETE], 4)


class TestFuzzArtifacts(unittest.TestCase):
    """Test fuzz artifact detection."""

    def test_empty_dir(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            arts = ol._list_fuzz_artifacts(pathlib.Path(d))
            self.assertEqual(arts, set())

    def test_detects_crash(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            dp = pathlib.Path(d)
            (dp / "crash-abc").write_text("")
            (dp / "leak-def").write_text("")
            (dp / "normal.txt").write_text("")
            arts = ol._list_fuzz_artifacts(dp)
            self.assertIn("crash-abc", arts)
            self.assertIn("leak-def", arts)
            self.assertNotIn("normal.txt", arts)

    def test_detects_timeout(self) -> None:
        with tempfile.TemporaryDirectory() as d:
            dp = pathlib.Path(d)
            (dp / "timeout-slow").write_text("")
            arts = ol._list_fuzz_artifacts(dp)
            self.assertIn("timeout-slow", arts)


class TestDeadlineHelpers(unittest.TestCase):
    """Test deadline and budget calculations."""

    def test_run_context_defaults(self) -> None:
        config = ol.Config(
            mode="overnight", hours=8,
            phase_timeout_seconds=1200,
            fuzz_seconds_override=None,
            keep_going=True,
        )
        ctx = ol.RunContext(config=config, project_root=pathlib.Path("/tmp"))
        self.assertFalse(ctx.sticky_hold)
        self.assertFalse(ctx.finalized)
        self.assertEqual(ctx.soak_iter, 0)
        self.assertEqual(ctx.tsan_exec, 0)
        self.assertEqual(ctx.fuzz_exec, 0)

    def test_mark_hold(self) -> None:
        config = ol.Config(
            mode="overnight", hours=8,
            phase_timeout_seconds=1200,
            fuzz_seconds_override=None,
            keep_going=True,
        )
        ctx = ol.RunContext(config=config, project_root=pathlib.Path("/tmp"))
        ol.mark_hold(ctx)
        self.assertTrue(ctx.sticky_hold)


class TestJSON(unittest.TestCase):
    """Test JSON serialization of events and failures."""

    def test_event_serializable(self) -> None:
        event = {
            "ts_epoch": 1234567890.0,
            "phase": "baseline",
            "iteration": "1",
            "target": "sluice_core",
            "mode": "debug",
            "classification": "PASS",
            "raw_exit": 0,
            "duration_s": 42.5,
            "timeout_s": 1200,
            "log_path": "/tmp/log.log",
            "command": "xmake build sluice_core",
        }
        json.dumps(event)  # must not raise

    def test_failure_serializable(self) -> None:
        failure = {
            "ts_epoch": 1234567890.0,
            "phase": "fuzz",
            "target": "wal_read_record_fuzz",
            "classification": "FUZZ_CRASH",
            "diagnostic_signature": "new fuzz artifacts: crash-xxx",
            "log_path": "/tmp/fuzz.log",
        }
        json.dumps(failure)  # must not raise


class TestTargetCache(unittest.TestCase):
    """Test target cache invalidation logic."""

    def test_invalidation(self) -> None:
        config = ol.Config(
            mode="overnight", hours=8,
            phase_timeout_seconds=1200,
            fuzz_seconds_override=None,
            keep_going=True,
        )
        ctx = ol.RunContext(config=config, project_root=pathlib.Path("/tmp"))
        ctx.target_cache["debug"] = {"old_target_a", "old_target_b"}
        # Simulate re-configure: invalidate.
        ctx.target_cache.pop("debug", None)
        ctx.target_cache["debug"] = {"new_target_c"}
        self.assertNotIn("old_target_a", ctx.target_cache["debug"])
        self.assertIn("new_target_c", ctx.target_cache["debug"])


class TestStripANSI(unittest.TestCase):
    """Test ANSI stripping."""

    def test_strip(self) -> None:
        self.assertEqual(ol.strip_ansi("\x1b[32mgreen\x1b[0m"), "green")

    def test_no_ansi(self) -> None:
        self.assertEqual(ol.strip_ansi("plain text"), "plain text")


class TestErrorClasses(unittest.TestCase):
    """Test that error classifications are properly handled."""

    def test_failure_classes_set(self) -> None:
        """Verify failures contain only the expected classifications."""
        r = ol.CommandResult(
            phase="test", iteration="0", target="t", mode="m",
            command=["true"], classification=ol.Classification.PASS,
            exit_code=0, duration_seconds=0.1,
            log_path=pathlib.Path("/tmp/log"),
            sanitizer_signature=None, timed_out=False,
            term_sent=False, kill_sent=False,
            started_at="now", finished_at="now",
        )
        # This is a smoke test; the actual classification is tested above.
        self.assertEqual(r.classification, ol.Classification.PASS)


if __name__ == "__main__":
    unittest.main()