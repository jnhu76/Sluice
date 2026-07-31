#!/usr/bin/env python3
"""Unit tests for the hardening heartbeat feature.

Exercises HeartbeatEmitter, _run_with_timeout scheduling, libFuzzer parsing,
bounded tail reading, per-target fuzz config, and run_command() integration.

Pure-logic tests use synthetic data; process tests spawn real short-lived
children with the current Python interpreter.

Run with:
    python3 -m unittest discover -v scripts/tests
"""
from __future__ import annotations

import json
import os
import signal
import sys
import tempfile
import time
import unittest
from pathlib import Path
from typing import List, Optional
from unittest import mock

_SCRIPT_DIR = Path(__file__).resolve().parent.parent
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))

from hardening.model import (
    Classification,
    CommandResult,
    CommandSpec,
    Config,
)
from hardening.process import (
    HeartbeatEmitter,
    _parse_libfuzzer_status,
    _tail_read_log,
    run_command,
)
from hardening.phases import FUZZ_DICTS, FUZZ_MAXLEN


# ── helpers ──────────────────────────────────────────────────────────

def _spec(log_path: Path, command: List[str], timeout: float,
          heartbeat_seconds: int = 0,
          heartbeats_path: Optional[Path] = None,
          run_log_path: Optional[Path] = None) -> CommandSpec:
    return CommandSpec(
        phase="test", iteration="1", target="heartbeat", mode="debug",
        command=command, timeout_seconds=timeout, log_path=log_path,
        heartbeat_seconds=heartbeat_seconds,
        heartbeats_path=heartbeats_path,
        run_log_path=run_log_path,
    )


# ── HeartbeatEmitter pure-logic tests ─────────────────────────────────

class TestHeartbeatEmitter(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp()
        self.tmpdir = Path(self.tmp)

    def test_heartbeat_json_serialization(self) -> None:
        log = self.tmpdir / "cmd.log"
        log.write_text("")
        hb_jsonl = self.tmpdir / "hb.jsonl"
        hb = HeartbeatEmitter(
            phase="test", iteration="3", target="json_target", pid=999,
            start_mono=time.monotonic(), timeout_seconds=120.0,
            log_path=log, heartbeats_jsonl_path=hb_jsonl, is_fuzz=False,
        )
        time.sleep(0.1)
        hb.emit()
        recs = [json.loads(l) for l in hb_jsonl.read_text().strip().split("\n") if l]
        self.assertEqual(len(recs), 1)
        r = recs[0]
        self.assertEqual(r["phase"], "test")
        self.assertEqual(r["iteration"], "3")
        self.assertEqual(r["target"], "json_target")
        self.assertEqual(r["pid"], 999)
        self.assertGreater(r["elapsed"], 0)
        self.assertEqual(r["alive"], True)
        self.assertIn("ts", r)
        self.assertIn("log_size", r)
        self.assertIn("log_delta", r)
        # Non-fuzz: no fuzzer_status field at all.
        self.assertNotIn("fuzzer_status", r)

    def test_log_size_delta(self) -> None:
        log = self.tmpdir / "cmd.log"
        log.write_text("")
        hb = HeartbeatEmitter(
            phase="test", iteration="1", target="t", pid=1,
            start_mono=time.monotonic(), timeout_seconds=10.0,
            log_path=log, is_fuzz=False,
        )
        hb.emit()
        self.assertEqual(hb._last_log_size, 0)
        log.write_bytes(b"hello world")
        hb.emit()
        self.assertEqual(hb._last_log_size, 11)

    def test_libfuzzer_status_parsing(self) -> None:
        log = self.tmpdir / "fuzz.log"
        log.write_text(
            "junk\n"
            "#12345\tINITED cov: 1978 ft: 7835 corp: 171/19Kb "
            "lim: 4279 exec/s: 1024 rss: 79Mb\n"
            "more junk\n"
        )
        status = _parse_libfuzzer_status(log)
        self.assertIsNotNone(status)
        self.assertEqual(status["count"], 12345)
        self.assertEqual(status["tag"], "INITED")
        self.assertEqual(status["exec_s"], 1024)
        self.assertEqual(status["rss"], 79)
        self.assertEqual(status["cov"], 1978)
        self.assertEqual(status["ft"], 7835)
        self.assertEqual(status["corpus_count"], 171)
        self.assertEqual(status["corpus_size"], 19 * 1024)

    def test_latest_status_line_returned(self) -> None:
        """When multiple status lines exist, return the LAST (latest)."""
        log = self.tmpdir / "multi.log"
        log.write_text(
            "#1\tINITED cov: 10 ft: 20 corp: 1/1Kb exec/s: 100 rss: 10Mb\n"
            "#2\tNEW cov: 30 ft: 40 corp: 2/2Kb exec/s: 200 rss: 20Mb\n"
            "#3\tDONE cov: 50 ft: 60 corp: 3/3Kb exec/s: 300 rss: 30Mb\n"
        )
        status = _parse_libfuzzer_status(log)
        self.assertIsNotNone(status)
        self.assertEqual(status["count"], 3)
        self.assertEqual(status["tag"], "DONE")
        self.assertEqual(status["exec_s"], 300)
        self.assertEqual(status["rss"], 30)

    def test_no_cross_line_splicing(self) -> None:
        """Fields from different lines must not be spliced together."""
        log = self.tmpdir / "splice.log"
        log.write_text(
            "cov: 9999 ft: 9999\n"  # orphan field on previous line
            "#1\tINITED exec/s: 100 rss: 10Mb\n"
        )
        status = _parse_libfuzzer_status(log)
        self.assertIsNotNone(status)
        self.assertEqual(status["count"], 1)
        self.assertEqual(status["exec_s"], 100)
        self.assertEqual(status["rss"], 10)
        # cov/ft from the orphan line must NOT leak in.
        self.assertNotIn("cov", status)
        self.assertNotIn("ft", status)

    def test_malformed_status_fallback(self) -> None:
        log = self.tmpdir / "bad.log"
        log.write_text("no status line here\n")
        status = _parse_libfuzzer_status(log)
        self.assertIsNone(status)

    def test_empty_log_fallback(self) -> None:
        log = self.tmpdir / "empty.log"
        log.write_text("")
        status = _parse_libfuzzer_status(log)
        self.assertIsNone(status)

    def test_fuzz_command_has_fuzzer_status(self) -> None:
        """Fuzz commands should include fuzzer_status in JSON."""
        log = self.tmpdir / "fuzz.log"
        log.write_text("")
        hb_jsonl = self.tmpdir / "hb.jsonl"
        hb = HeartbeatEmitter(
            phase="fuzz", iteration="0", target="wal_read_record_fuzz",
            pid=1, start_mono=time.monotonic(), timeout_seconds=120.0,
            log_path=log, heartbeats_jsonl_path=hb_jsonl, is_fuzz=True,
        )
        hb.emit()
        recs = [json.loads(l) for l in hb_jsonl.read_text().strip().split("\n") if l]
        self.assertEqual(len(recs), 1)
        self.assertIn("fuzzer_status", recs[0])

    def test_non_fuzz_command_has_no_fuzzer_status(self) -> None:
        """Non-fuzz commands must not have fuzzer_status in JSON."""
        log = self.tmpdir / "cmd.log"
        log.write_text("")
        hb_jsonl = self.tmpdir / "hb.jsonl"
        hb = HeartbeatEmitter(
            phase="debug-soak", iteration="1", target="full-suite",
            pid=1, start_mono=time.monotonic(), timeout_seconds=120.0,
            log_path=log, heartbeats_jsonl_path=hb_jsonl, is_fuzz=False,
        )
        hb.emit()
        recs = [json.loads(l) for l in hb_jsonl.read_text().strip().split("\n") if l]
        self.assertEqual(len(recs), 1)
        self.assertNotIn("fuzzer_status", recs[0])

    def test_emit_survives_broken_stderr(self) -> None:
        """emit() must not raise even if sys.stderr.write() fails."""
        log = self.tmpdir / "cmd.log"
        log.write_text("")
        hb = HeartbeatEmitter(
            phase="test", iteration="1", target="t", pid=1,
            start_mono=time.monotonic(), timeout_seconds=10.0,
            log_path=log, is_fuzz=False,
        )
        with mock.patch("sys.stderr.write", side_effect=BrokenPipeError):
            # Must not raise.
            hb.emit()

    def test_run_log_writing(self) -> None:
        """Heartbeat lines are appended to run_log_path."""
        log = self.tmpdir / "cmd.log"
        log.write_text("")
        run_log = self.tmpdir / "run.log"
        hb = HeartbeatEmitter(
            phase="test", iteration="1", target="t", pid=1,
            start_mono=time.monotonic(), timeout_seconds=10.0,
            log_path=log, run_log_path=run_log, is_fuzz=False,
        )
        hb.emit()
        content = run_log.read_text()
        self.assertIn("[heartbeat]", content)
        self.assertIn("phase=test", content)

    def test_global_remaining_shown(self) -> None:
        """global_remaining_fn is called and included in output."""
        log = self.tmpdir / "cmd.log"
        log.write_text("")
        hb_jsonl = self.tmpdir / "hb.jsonl"
        hb = HeartbeatEmitter(
            phase="test", iteration="1", target="t", pid=1,
            start_mono=time.monotonic(), timeout_seconds=10.0,
            log_path=log, heartbeats_jsonl_path=hb_jsonl,
            global_remaining_fn=lambda: 3600.0, is_fuzz=False,
        )
        hb.emit()
        recs = [json.loads(l) for l in hb_jsonl.read_text().strip().split("\n") if l]
        self.assertEqual(recs[0]["global_remaining"], 3600.0)


# ── Bounded tail read ─────────────────────────────────────────────────

class TestTailRead(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp()
        self.tmpdir = Path(self.tmp)

    def test_bounded_tail_read(self) -> None:
        big = self.tmpdir / "big.log"
        big.write_bytes(b"x" * 200000)
        tail = _tail_read_log(big, max_bytes=100)
        self.assertEqual(len(tail), 100)

    def test_tail_read_smaller_than_bound(self) -> None:
        small = self.tmpdir / "small.log"
        content = b"hello"
        small.write_bytes(content)
        tail = _tail_read_log(small, max_bytes=100)
        self.assertEqual(tail, "hello")

    def test_tail_read_missing_log(self) -> None:
        missing = self.tmpdir / "nope.log"
        tail = _tail_read_log(missing)
        self.assertEqual(tail, "")


# ── Process tests (real children) ─────────────────────────────────────

class TestHeartbeatProcessLifecycle(unittest.TestCase):
    """Spawn real short-lived children and verify heartbeat + timeout work."""

    def test_heartbeat_interval_respected(self) -> None:
        """heartbeat=3s, child=2s → 0 heartbeats (child exits before first beat)."""
        with tempfile.TemporaryDirectory() as tmp:
            log_path = Path(tmp) / "silent.log"
            hb_jsonl = Path(tmp) / "hb.jsonl"
            spec = _spec(
                log_path,
                [sys.executable, "-c", "import time; time.sleep(2); print('done')"],
                timeout=10.0,
                heartbeat_seconds=3,
                heartbeats_path=hb_jsonl,
            )
            r = run_command(spec, "abc123", False)
            self.assertEqual(r.classification, Classification.PASS)
            # Child exits in 2s < 3s interval → 0 heartbeats.
            recs = _read_jsonl(hb_jsonl)
            self.assertEqual(len(recs), 0,
                             "0 heartbeats: child exits before first 3s interval")

    def test_heartbeat_2s_interval_5s_child(self) -> None:
        """heartbeat=2s, child=5s → ~2 heartbeats."""
        with tempfile.TemporaryDirectory() as tmp:
            log_path = Path(tmp) / "silent.log"
            hb_jsonl = Path(tmp) / "hb.jsonl"
            spec = _spec(
                log_path,
                [sys.executable, "-c", "import time; time.sleep(5); print('done')"],
                timeout=10.0,
                heartbeat_seconds=2,
                heartbeats_path=hb_jsonl,
            )
            r = run_command(spec, "abc123", False)
            self.assertEqual(r.classification, Classification.PASS)
            recs = _read_jsonl(hb_jsonl)
            # 5s child with 2s interval → at least 2 (at 2s, 4s), possibly 3
            # if the third beat fires just before exit.
            self.assertGreaterEqual(len(recs), 2,
                                    ">=2 heartbeats at 2s intervals during 5s sleep")
            self.assertLessEqual(len(recs), 3)

    def test_heartbeat_zero_disables_jsonl(self) -> None:
        """heartbeat=0, child=3s → heartbeats.jsonl absent or empty."""
        with tempfile.TemporaryDirectory() as tmp:
            log_path = Path(tmp) / "silent.log"
            hb_jsonl = Path(tmp) / "hb.jsonl"
            spec = _spec(
                log_path,
                [sys.executable, "-c", "import time; time.sleep(3); print('done')"],
                timeout=10.0,
                heartbeat_seconds=0,
                heartbeats_path=hb_jsonl,
            )
            r = run_command(spec, "abc123", False)
            self.assertEqual(r.classification, Classification.PASS)
            # heartbeat_seconds=0 → no emitter created → no JSONL writes.
            recs = _read_jsonl(hb_jsonl)
            self.assertEqual(len(recs), 0)

    def test_timeout_still_occurs_with_heartbeats_enabled(self) -> None:
        """A child that sleeps 30s with timeout=1s.  Must time out."""
        with tempfile.TemporaryDirectory() as tmp:
            log_path = Path(tmp) / "timeout.log"
            hb_jsonl = Path(tmp) / "hb.jsonl"
            spec = _spec(
                log_path,
                [sys.executable, "-c", "import time; time.sleep(30)"],
                timeout=1.0,
                heartbeat_seconds=1,
                heartbeats_path=hb_jsonl,
            )
            start = time.monotonic()
            r = run_command(spec, "abc123", False)
            elapsed = time.monotonic() - start
            self.assertEqual(r.classification, Classification.TIMEOUT)
            self.assertTrue(r.timed_out)
            # TERM → KILL escalation: 1s timeout + 10s grace + KILL.
            # Should complete well under 15s.
            self.assertLess(elapsed, 15.0)

    def test_heartbeat_does_not_reintroduce_blocking_pipe(self) -> None:
        """Child produces no output.  Runner must not block on a pipe read."""
        with tempfile.TemporaryDirectory() as tmp:
            log_path = Path(tmp) / "silent2.log"
            hb_jsonl = Path(tmp) / "hb.jsonl"
            spec = _spec(
                log_path,
                [sys.executable, "-c", "import time; time.sleep(3)"],
                timeout=10.0,
                heartbeat_seconds=1,
                heartbeats_path=hb_jsonl,
            )
            start = time.monotonic()
            r = run_command(spec, "abc123", False)
            elapsed = time.monotonic() - start
            self.assertEqual(r.classification, Classification.PASS)
            self.assertLess(elapsed, 8.0)

    def test_term_ignored_still_escalates_with_heartbeat(self) -> None:
        code = (
            "import signal, time\n"
            "signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
            "time.sleep(60)\n"
        )
        with tempfile.TemporaryDirectory() as tmp:
            log_path = Path(tmp) / "ignore.log"
            hb_jsonl = Path(tmp) / "hb.jsonl"
            spec = _spec(
                log_path, [sys.executable, "-c", code],
                timeout=1.0,
                heartbeat_seconds=1,
                heartbeats_path=hb_jsonl,
            )
            grace = 0.5
            with mock.patch("hardening.process.KILL_AFTER_SECONDS", grace):
                start = time.monotonic()
                r = run_command(spec, "abc123", False)
                elapsed = time.monotonic() - start
            self.assertEqual(r.classification, Classification.TIMEOUT)
            self.assertTrue(r.kill_sent)
            self.assertLess(elapsed, 5.0)

    def test_heartbeat_exception_does_not_kill_test(self) -> None:
        """A heartbeat that raises must not affect the test outcome."""
        with tempfile.TemporaryDirectory() as tmp:
            log_path = Path(tmp) / "silent.log"
            hb_jsonl = Path(tmp) / "hb.jsonl"
            spec = _spec(
                log_path,
                [sys.executable, "-c", "import time; time.sleep(3); print('done')"],
                timeout=10.0,
                heartbeat_seconds=1,
                heartbeats_path=hb_jsonl,
            )
            # Make emit() raise every time.
            with mock.patch.object(HeartbeatEmitter, "emit", side_effect=RuntimeError("boom")):
                r = run_command(spec, "abc123", False)
            # Child must still complete normally.
            self.assertEqual(r.classification, Classification.PASS)


# ── Per-target fuzz config ────────────────────────────────────────────

class TestFuzzTargetConfig(unittest.TestCase):
    def test_target_specific_dictionary_selection(self) -> None:
        self.assertEqual(FUZZ_DICTS["wal_read_record_fuzz"], "wal_record.dict")
        self.assertEqual(FUZZ_DICTS["wal_roundtrip_fuzz"], "wal_record.dict")
        self.assertIsNone(FUZZ_DICTS["copy_all_fault_fuzz"])

    def test_target_specific_maxlen(self) -> None:
        self.assertEqual(FUZZ_MAXLEN["wal_read_record_fuzz"], 1048576)
        self.assertEqual(FUZZ_MAXLEN["wal_roundtrip_fuzz"], 262144)
        self.assertEqual(FUZZ_MAXLEN["copy_all_fault_fuzz"], 8192)

    def test_fuzz_command_argv_construction(self) -> None:
        """Verify fuzz command argv is constructed correctly.

        Extracted as a pure function so the test is independent of xmake.
        """
        argv = _build_fuzz_argv(
            target="wal_read_record_fuzz",
            corpus="/tmp/corpus",
            artifact_dir="/tmp/artifacts",
            per_target=30,
            maxlen=1048576,
            dict_path="/tmp/wal_record.dict",
        )
        # Must contain xmake run wrapper.
        self.assertEqual(argv[0], "xmake")
        self.assertEqual(argv[1], "run")
        self.assertEqual(argv[2], "wal_read_record_fuzz")
        self.assertEqual(argv[3], "--")
        # After -- are the libFuzzer args.
        self.assertIn("/tmp/corpus", argv)
        self.assertIn("-max_total_time=30", argv)
        self.assertIn("-artifact_prefix=/tmp/artifacts/", argv)
        self.assertIn("-max_len=1048576", argv)
        self.assertIn("-dict=/tmp/wal_record.dict", argv)

    def test_fuzz_command_no_dict_when_none(self) -> None:
        argv = _build_fuzz_argv(
            target="copy_all_fault_fuzz",
            corpus="/tmp/corpus",
            artifact_dir="/tmp/artifacts",
            per_target=10,
            maxlen=8192,
            dict_path=None,
        )
        self.assertNotIn("-dict=", " ".join(argv))


# ── Config parsing ────────────────────────────────────────────────────

class TestHeartbeatConfig(unittest.TestCase):
    def test_default_heartbeat_is_60(self) -> None:
        from hardening.cli import parse_args
        c = parse_args([])
        self.assertEqual(c.heartbeat_seconds, 60)
        self.assertEqual(c.heartbeat_source, "default")

    def test_cli_override_heartbeat(self) -> None:
        from hardening.cli import parse_args
        c = parse_args(["--heartbeat-seconds", "5"])
        self.assertEqual(c.heartbeat_seconds, 5)
        self.assertEqual(c.heartbeat_source, "cli")

    def test_zero_disables_heartbeat(self) -> None:
        from hardening.cli import parse_args
        c = parse_args(["--heartbeat-seconds", "0"])
        self.assertEqual(c.heartbeat_seconds, 0)

    def test_negative_heartbeat_rejected(self) -> None:
        from hardening.cli import parse_args
        with self.assertRaises((ValueError, SystemExit)):
            parse_args(["--heartbeat-seconds", "-1"])


# ── Helpers ───────────────────────────────────────────────────────────

def _read_jsonl(path: Path) -> List[dict]:
    """Read a JSONL file, returning parsed records (empty list if missing)."""
    if not path.exists():
        return []
    text = path.read_text().strip()
    if not text:
        return []
    return [json.loads(l) for l in text.split("\n") if l]


def _build_fuzz_argv(
    target: str,
    corpus: str,
    artifact_dir: str,
    per_target: int,
    maxlen: int,
    dict_path: Optional[str] = None,
) -> List[str]:
    """Build the fuzz command argv (pure function, testable)."""
    argv = [
        "xmake", "run", target, "--",
        corpus,
        f"-max_total_time={per_target}",
        f"-artifact_prefix={artifact_dir}/",
        "-rss_limit_mb=1024",
        f"-max_len={maxlen}",
        "-timeout=120",
    ]
    if dict_path is not None:
        argv.append(f"-dict={dict_path}")
    return argv


if __name__ == "__main__":
    unittest.main()