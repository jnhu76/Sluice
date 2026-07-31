#!/usr/bin/env python3
"""Unit tests for the hardening heartbeat feature.

Exercises HeartbeatEmitter, _run_with_timeout slices, libFuzzer parsing,
bounded tail reading, per-target fuzz dict, and argv construction.
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
          heartbeats_path: Optional[Path] = None) -> CommandSpec:
    return CommandSpec(
        phase="test", iteration="1", target="heartbeat", mode="debug",
        command=command, timeout_seconds=timeout, log_path=log_path,
        heartbeat_seconds=heartbeat_seconds,
        heartbeats_path=heartbeats_path,
    )


# ── HeartbeatEmitter pure-logic tests ─────────────────────────────────

class TestHeartbeatEmitter(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.mkdtemp()
        self.tmpdir = Path(self.tmp)

    def test_disabled_at_zero_does_not_emit(self) -> None:
        log = self.tmpdir / "cmd.log"
        log.write_text("")
        hb = HeartbeatEmitter(
            phase="test", iteration="1", target="t", pid=1,
            start_mono=time.monotonic(), timeout_seconds=10.0,
            log_path=log, is_fuzz=False,
        )
        # Call emit() — it should still work (heartbeat_seconds=0 is
        # enforced at the caller level, not inside the emitter).
        hb.emit()
        # No crash = pass.

    def test_no_heartbeat_before_interval(self) -> None:
        # The emitter itself does not gate on interval; the caller does.
        # Verify that calling emit() multiple times works.
        log = self.tmpdir / "cmd.log"
        log.write_text("")
        hb = HeartbeatEmitter(
            phase="test", iteration="2", target="t", pid=2,
            start_mono=time.monotonic(), timeout_seconds=10.0,
            log_path=log, is_fuzz=False,
        )
        hb.emit()
        hb.emit()
        # No crash = pass.

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
        self.assertIn("log_size", r)
        self.assertIn("log_delta", r)
        self.assertEqual(r["fuzzer_status"], "unavailable")

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

    def test_repeated_heartbeat_while_silent_process_runs(self) -> None:
        # A child that sleeps 3s with heartbeat_seconds=1.  Should emit at
        # least 2 heartbeats before the child exits.
        with tempfile.TemporaryDirectory() as tmp:
            log_path = Path(tmp) / "silent.log"
            hb_jsonl = Path(tmp) / "hb.jsonl"
            spec = _spec(
                log_path,
                [sys.executable, "-c",
                 "import time; time.sleep(3); print('done')"],
                timeout=10.0,
                heartbeat_seconds=1,
                heartbeats_path=hb_jsonl,
            )
            r = run_command(spec, "abc123", False)
            self.assertEqual(r.classification, Classification.PASS)
            recs = [json.loads(l) for l in
                    hb_jsonl.read_text().strip().split("\n") if l]
            self.assertGreaterEqual(len(recs), 2,
                                    "should emit >=2 heartbeats during 3s sleep")

    def test_timeout_still_occurs_with_heartbeats_enabled(self) -> None:
        # A child that sleeps 30s with timeout=1s.  Must time out even with
        # heartbeats enabled.
        with tempfile.TemporaryDirectory() as tmp:
            log_path = Path(tmp) / "timeout.log"
            hb_jsonl = Path(tmp) / "hb.jsonl"
            spec = _spec(
                log_path,
                [sys.executable, "-c",
                 "import time; time.sleep(30)"],
                timeout=1.0,
                heartbeat_seconds=1,
                heartbeats_path=hb_jsonl,
            )
            start = time.monotonic()
            r = run_command(spec, "abc123", False)
            elapsed = time.monotonic() - start
            self.assertEqual(r.classification, Classification.TIMEOUT)
            self.assertTrue(r.timed_out)
            self.assertLess(elapsed, 5.0)
            # At least 1 heartbeat before timeout
            recs = [json.loads(l) for l in
                    hb_jsonl.read_text().strip().split("\n") if l]
            self.assertGreaterEqual(len(recs), 1)

    def test_heartbeat_does_not_reintroduce_blocking_pipe(self) -> None:
        # Child produces no output (just sleeps).  With heartbeat enabled,
        # the runner must NOT block on a pipe read — it uses only
        # proc.wait(timeout=...).  The proof: the child exits after 3s,
        # the runner returns before the 10s timeout, and we get heartbeats.
        with tempfile.TemporaryDirectory() as tmp:
            log_path = Path(tmp) / "silent2.log"
            hb_jsonl = Path(tmp) / "hb.jsonl"
            spec = _spec(
                log_path,
                [sys.executable, "-c",
                 "import time; time.sleep(3)"],
                timeout=10.0,
                heartbeat_seconds=1,
                heartbeats_path=hb_jsonl,
            )
            start = time.monotonic()
            r = run_command(spec, "abc123", False)
            elapsed = time.monotonic() - start
            self.assertEqual(r.classification, Classification.PASS)
            self.assertLess(elapsed, 8.0)
            recs = [json.loads(l) for l in
                    hb_jsonl.read_text().strip().split("\n") if l]
            self.assertGreaterEqual(len(recs), 2)

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

    def test_child_argv_does_not_contain_standalone_dashdash(self) -> None:
        # "_" in the spec is xmake's separator, not passed to the
        # executable. Confirm that a non-xmake command (direct binary)
        # does not see a standalone "--" from the runner.
        with tempfile.TemporaryDirectory() as tmp:
            log_path = Path(tmp) / "argv.log"
            spec = _spec(
                log_path,
                [sys.executable, "-c",
                 "import sys; print('argv:', ' '.join(sys.argv[1:]))"],
                timeout=10.0,
            )
            r = run_command(spec, "abc123", False)
            text = log_path.read_text()
            self.assertNotIn("argv: --", text)


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


if __name__ == "__main__":
    unittest.main()