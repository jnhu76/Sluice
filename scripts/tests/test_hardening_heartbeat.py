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
import queue
import signal
import subprocess
import sys
import tempfile
import threading
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
    HeartbeatSnapshot,
    HeartbeatTick,
    _parse_libfuzzer_status,
    _tail_read_log,
    run_command,
)
from hardening.phases import FUZZ_DICTS, FUZZ_MAXLEN, build_fuzz_argv


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


def _read_jsonl(path: Path) -> List[dict]:
    """Read a JSONL file, returning parsed records (empty list if missing)."""
    if not path.exists():
        return []
    text = path.read_text().strip()
    if not text:
        return []
    return [json.loads(l) for l in text.split("\n") if l]


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
        # Give the writer thread a moment to flush.
        hb.shutdown()
        recs = _read_jsonl(hb_jsonl)
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

    def test_jsonl_includes_command_log_path(self) -> None:
        """JSONL records the command log path for diagnostics."""
        log = self.tmpdir / "cmd.log"
        log.write_text("")
        hb_jsonl = self.tmpdir / "hb.jsonl"
        hb = HeartbeatEmitter(
            phase="test", iteration="1", target="t", pid=1,
            start_mono=time.monotonic(), timeout_seconds=10.0,
            log_path=log, heartbeats_jsonl_path=hb_jsonl, is_fuzz=False,
        )
        hb.emit()
        hb.shutdown()
        recs = _read_jsonl(hb_jsonl)
        self.assertEqual(len(recs), 1)
        self.assertEqual(recs[0]["command_log_path"], str(log))

    def test_emit_alive_at_capture_propagates_to_run_log(self) -> None:
        """emit(alive_at_capture=False) → run.log shows alive=no, not alive=yes.

        The run_log_path sink receives the same ``[heartbeat]`` line the writer
        thread emits to stderr; here we read it back from the file (deterministic,
        no stderr capture needed) and assert the ``alive=`` field reflects the
        observation captured at emit time.

        Regression: the writer used to hardcode ``alive=yes``.  Now the tick
        carries the observation captured at emit time and the writer emits
        exactly that value, so a tick built just after the child exited can
        report ``alive=no`` instead of lying.
        """
        log = self.tmpdir / "cmd.log"
        log.write_text("")
        run_log = self.tmpdir / "run.log"
        hb = HeartbeatEmitter(
            phase="test", iteration="1", target="t", pid=1,
            start_mono=time.monotonic(), timeout_seconds=10.0,
            log_path=log, run_log_path=run_log, is_fuzz=False,
        )
        hb.emit(alive_at_capture=False)
        hb.shutdown()
        content = run_log.read_text()
        self.assertIn("alive=no", content)
        self.assertNotIn("alive=yes", content)

    def test_emit_alive_at_capture_propagates_to_jsonl(self) -> None:
        """emit(alive_at_capture=False) → JSONL alive=false."""
        log = self.tmpdir / "cmd.log"
        log.write_text("")
        hb_jsonl = self.tmpdir / "hb.jsonl"
        hb = HeartbeatEmitter(
            phase="test", iteration="1", target="t", pid=1,
            start_mono=time.monotonic(), timeout_seconds=10.0,
            log_path=log, heartbeats_jsonl_path=hb_jsonl, is_fuzz=False,
        )
        hb.emit(alive_at_capture=False)
        hb.shutdown()
        recs = _read_jsonl(hb_jsonl)
        self.assertEqual(len(recs), 1)
        self.assertIs(recs[0]["alive"], False)

    def test_emit_default_alive_at_capture_is_true(self) -> None:
        """Default emit() (no arg) records alive_at_capture=True."""
        log = self.tmpdir / "cmd.log"
        log.write_text("")
        hb_jsonl = self.tmpdir / "hb.jsonl"
        hb = HeartbeatEmitter(
            phase="test", iteration="1", target="t", pid=1,
            start_mono=time.monotonic(), timeout_seconds=10.0,
            log_path=log, heartbeats_jsonl_path=hb_jsonl, is_fuzz=False,
        )
        hb.emit()  # default
        hb.shutdown()
        recs = _read_jsonl(hb_jsonl)
        self.assertIs(recs[0]["alive"], True)

    def test_log_size_delta(self) -> None:
        log = self.tmpdir / "cmd.log"
        log.write_text("")
        hb = HeartbeatEmitter(
            phase="test", iteration="1", target="t", pid=1,
            start_mono=time.monotonic(), timeout_seconds=10.0,
            log_path=log, is_fuzz=False,
        )
        # First emit: log is empty, delta should be 0.
        hb.emit()
        hb.shutdown()
        self.assertEqual(hb._last_log_size, 0)
        # Second emit: log has 11 bytes, delta should be 11.
        log.write_bytes(b"hello world")
        hb2 = HeartbeatEmitter(
            phase="test", iteration="1", target="t", pid=1,
            start_mono=time.monotonic(), timeout_seconds=10.0,
            log_path=log, is_fuzz=False,
        )
        hb2.emit()
        hb2.shutdown()
        self.assertEqual(hb2._last_log_size, 11)

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

    def test_corpus_size_mb(self) -> None:
        log = self.tmpdir / "corpus_mb.log"
        log.write_text("#1\tINITED corp: 400/3Mb exec/s: 100 rss: 10Mb\n")
        status = _parse_libfuzzer_status(log)
        self.assertIsNotNone(status)
        self.assertEqual(status["corpus_count"], 400)
        self.assertEqual(status["corpus_size"], 3 * 1024 * 1024)

    def test_corpus_size_kb(self) -> None:
        log = self.tmpdir / "corpus_kb.log"
        log.write_text("#1\tINITED corp: 12/900Kb exec/s: 100 rss: 10Mb\n")
        status = _parse_libfuzzer_status(log)
        self.assertIsNotNone(status)
        self.assertEqual(status["corpus_size"], 900 * 1024)

    def test_corpus_size_bytes(self) -> None:
        log = self.tmpdir / "corpus_b.log"
        log.write_text("#1\tINITED corp: 12/42b exec/s: 100 rss: 10Mb\n")
        status = _parse_libfuzzer_status(log)
        self.assertIsNotNone(status)
        self.assertEqual(status["corpus_size"], 42)

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
        hb.shutdown()
        recs = _read_jsonl(hb_jsonl)
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
        hb.shutdown()
        recs = _read_jsonl(hb_jsonl)
        self.assertEqual(len(recs), 1)
        self.assertNotIn("fuzzer_status", recs[0])

    def test_emit_survives_broken_stderr(self) -> None:
        """emit() must not raise even if stderr write fails."""
        log = self.tmpdir / "cmd.log"
        log.write_text("")
        hb = HeartbeatEmitter(
            phase="test", iteration="1", target="t", pid=1,
            start_mono=time.monotonic(), timeout_seconds=10.0,
            log_path=log, is_fuzz=False,
        )
        # The writer thread does the actual stderr write; mock it there.
        with mock.patch("sys.stderr.write", side_effect=BrokenPipeError):
            hb.emit()
            # Give the writer thread a moment.
            hb.shutdown()
        # Must not raise.

    def test_emit_survives_blocking_stderr_queue_full(self) -> None:
        """emit() is non-blocking: a full queue drops the heartbeat.

        Deterministic: we pin ``_build_snapshot`` on a ``threading.Event``
        so the writer thread cannot consume the placeholder tick before we
        check ``queue.full()``.  Without this, on a loaded machine the writer
        could drain the queue between ``put_nowait`` and the ``full()`` check,
        making the assertion racy.
        """
        log = self.tmpdir / "cmd.log"
        log.write_text("")
        hb = HeartbeatEmitter(
            phase="test", iteration="1", target="t", pid=1,
            start_mono=time.monotonic(), timeout_seconds=10.0,
            log_path=log, is_fuzz=False,
        )
        gate = threading.Event()
        original_build = hb._build_snapshot

        def _gated_build(tick):
            gate.wait()  # hold the writer here so it can't drain the queue
            return original_build(tick)

        hb._build_snapshot = _gated_build  # type: ignore[method-assign]
        try:
            # Fill the queue (maxsize=1) with a placeholder tick.  The writer
            # thread cannot drain it because _build_snapshot is gated.
            tick = HeartbeatTick(pid=1, ts=time.time())
            hb._queue.put_nowait(tick)
            self.assertTrue(hb._queue.full())
            # emit() should not block, should not raise; tick is dropped.
            hb.emit()
        finally:
            # Release the writer so shutdown can drain cleanly.
            gate.set()
            hb._build_snapshot = original_build  # type: ignore[method-assign]
            hb.shutdown()

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
        hb.shutdown()
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
        hb.shutdown()
        recs = _read_jsonl(hb_jsonl)
        self.assertEqual(recs[0]["global_remaining"], 3600.0)

    def test_keyboard_interrupt_not_swallowed(self) -> None:
        """_build_snapshot() uses except Exception, not BaseException —
        KeyboardInterrupt must propagate from the writer thread."""
        log = self.tmpdir / "cmd.log"
        log.write_text("")
        hb = HeartbeatEmitter(
            phase="test", iteration="1", target="t", pid=1,
            start_mono=time.monotonic(), timeout_seconds=10.0,
            log_path=log, is_fuzz=False,
        )
        original = hb._build_snapshot

        def _raising(*args, **kwargs):
            raise KeyboardInterrupt()

        hb._build_snapshot = _raising  # type: ignore[method-assign]
        # Emit a tick — the writer thread will call _build_snapshot and
        # get KeyboardInterrupt, which should propagate out of the thread.
        hb.emit()
        # Poll the writer thread until it terminates from the
        # KeyboardInterrupt.  ``KeyboardInterrupt`` is not an ``Exception``
        # subclass, so ``except Exception`` in the writer loop does not
        # swallow it; the thread dies.  We assert that explicitly rather
        # than only relying on "shutdown did not hang".
        deadline = time.monotonic() + 5.0
        while hb._writer_thread.is_alive() and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertFalse(
            hb._writer_thread.is_alive(),
            "writer thread still alive: KeyboardInterrupt was swallowed by "
            "an except Exception clause"
        )
        # Restore _build_snapshot before shutdown so the sentinel doesn't
        # also raise.
        hb._build_snapshot = original  # type: ignore[method-assign]
        # Shutdown should still work (the sentinel finds no live thread).
        hb.shutdown()


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
        """heartbeat=3s, child=2s → 0 heartbeats (child exits before first beat).

        This is a clean boundary: the child exits at ~2s and the first beat is
        not due until 3s, so no scheduling jitter can produce a heartbeat.
        The exact zero is intentional and not made timing-tolerant.
        """
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
            recs = _read_jsonl(hb_jsonl)
            self.assertEqual(len(recs), 0,
                             "0 heartbeats: child exits before first 3s interval")

    def test_heartbeat_2s_interval_5s_child(self) -> None:
        """heartbeat=2s, child=5s → at least one beat, no upper bound.

        Timing-tolerant: under scheduler variance the exact count (2 or 3) is
        not a reliable contract — child startup latency or a beat landing just
        before/after the 5s exit can shift it by one.  What matters is that
        the heartbeat machinery actually fired during a run long enough to
        contain at least one full interval, so we assert ``>= 1`` and leave
        the precise cadence to the deterministic unit-level tests.
        """
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
            self.assertGreaterEqual(
                len(recs), 1,
                ">=1 heartbeat: a 5s child with 2s intervals must emit at "
                "least one beat regardless of scheduler jitter"
            )

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

    def test_timeout_not_delayed_by_blocking_writer(self) -> None:
        """Timeout must fire on schedule even if the writer thread blocks.

        We simulate a blocking writer by patching _write_snapshot to sleep.
        """
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
            # Make _write_snapshot sleep 5s — if the timeout thread waited
            # for it, the timeout would be delayed by 5s+.
            with mock.patch.object(HeartbeatEmitter, "_write_snapshot",
                                   lambda self, snap: time.sleep(5)):
                start = time.monotonic()
                r = run_command(spec, "abc123", False)
                elapsed = time.monotonic() - start
            self.assertEqual(r.classification, Classification.TIMEOUT)
            self.assertTrue(r.timed_out)
            # Timeout should fire within ~1s + TERM/KILL escalation, not
            # be delayed by the 5s writer sleep.
            self.assertLess(elapsed, 15.0)

    def test_child_exiting_at_heartbeat_boundary_no_false_alive(self) -> None:
        """A child that exits in the heartbeat window must not get alive=yes.

        Regression for the race: after ``wait()`` raises TimeoutExpired the
        runner used to unconditionally ``heartbeater.emit()`` without
        re-checking liveness, so a child that exited in that window could be
        reported ``alive=yes``.  The fix adds ``proc.poll()`` before emit:
        if the child already exited, the runner returns the real exit code
        and emits nothing.

        Deterministic test: we drive ``_run_with_timeout`` with a fake proc
        whose ``wait()`` blocks for its full timeout then raises
        TimeoutExpired (so the loop reaches the heartbeat branch) but whose
        ``poll()`` immediately returns a real exit code (child already
        reaped).  We then assert:

          * ``emit()`` is NEVER called (no lying heartbeat), and
          * the runner returns the real exit code with timed_out=False.

        ``wait()`` honors its *timeout* argument by sleeping, which mirrors
        a real ``Popen.wait`` and keeps ``_run_with_timeout``'s loop from
        busy-spinning while it waits for the heartbeat slot to arrive.

        This isolates the post-TimeoutExpired poll() short-circuit without
        relying on OS scheduling of the exact exit moment.
        """
        from hardening.process import _run_with_timeout

        class FakeProc:
            """Mimics the Popen attributes _run_with_timeout uses."""
            def __init__(self, pid: int, exit_code: int):
                self.pid = pid
                self._exit_code = exit_code
                self._wait_calls = 0

            def wait(self, timeout=None):
                self._wait_calls += 1
                # Honor the timeout like a real Popen.wait: block, then report
                # the child as still running.  Without this the runner's loop
                # busy-spins ~10^6 times/sec until the heartbeat slot elapses.
                if timeout is not None and timeout > 0:
                    time.sleep(timeout)
                raise subprocess.TimeoutExpired(cmd=["fake"], timeout=timeout)

            def poll(self):
                # Child already exited — the race window the fix targets.
                return self._exit_code

        class RecordingHeartbeater:
            def __init__(self):
                self.emit_calls = 0

            def emit(self, alive_at_capture: bool = True) -> None:
                self.emit_calls += 1

        hb = RecordingHeartbeater()
        proc = FakeProc(pid=4242, exit_code=0)
        # Small heartbeat_seconds still exercises the post-TimeoutExpired
        # poll() branch but keeps the test fast; the fake wait() sleeps for
        # exactly this long before raising, so there is no busy-spin.
        exit_code, timed_out, _term_sent, _kill_sent = _run_with_timeout(
            proc, timeout_seconds=5.0, heartbeater=hb,  # type: ignore[arg-type]
            heartbeat_seconds=1)
        # The fix: poll() returned 0 → runner returns it, emits no heartbeat.
        self.assertEqual(exit_code, 0)
        self.assertFalse(timed_out)
        self.assertEqual(hb.emit_calls, 0,
                         "emit() must not be called when poll() shows the "
                         "child already exited (would lie alive=yes)")


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
        """Verify fuzz command argv via production build_fuzz_argv()."""
        argv = build_fuzz_argv(
            target="wal_read_record_fuzz",
            corpus="/tmp/corpus",
            artifact_dir="/tmp/artifacts/",
            per_target=30,
            maxlen=1048576,
            dict_path="/tmp/wal_record.dict",
        )
        self.assertEqual(argv[0], "xmake")
        self.assertEqual(argv[1], "run")
        self.assertEqual(argv[2], "wal_read_record_fuzz")
        self.assertEqual(argv[3], "--")
        self.assertIn("/tmp/corpus", argv)
        self.assertIn("-max_total_time=30", argv)
        self.assertIn("-artifact_prefix=/tmp/artifacts/", argv)
        self.assertIn("-max_len=1048576", argv)
        self.assertIn("-dict=/tmp/wal_record.dict", argv)

    def test_fuzz_command_no_dict_when_none(self) -> None:
        argv = build_fuzz_argv(
            target="copy_all_fault_fuzz",
            corpus="/tmp/corpus",
            artifact_dir="/tmp/artifacts/",
            per_target=10,
            maxlen=8192,
            dict_path=None,
        )
        self.assertNotIn("-dict=", " ".join(argv))

    def test_fuzz_argv_separator_position(self) -> None:
        """Verify exactly ONE ``--`` separator sits at the correct position.

        This checks argv *construction* (build_fuzz_argv), not runtime
        delivery.  xmake run's actual semantics: everything after ``--`` is
        passed to the executable, and xmake ALSO forwards the ``--`` element
        itself into the spawned binary's argv (verified at runtime by
        scripts/verify-fuzz-argv-separator.sh via /proc/<pid>/cmdline).  So
        what we assert here is that construction emits the separator exactly
        once and that no second ``--`` leaks into the fuzzer-args portion —
        i.e. there is one separator, at the documented position.

        Runtime note: libFuzzer silently ignores any ``--``-prefixed argument
        (including a bare ``--``); see FuzzerDriver.cpp ParseOneFlag.  It is
        NOT an option terminator, so flags/corpus after the forwarded ``--``
        are still parsed normally.  The forwarded ``--`` is therefore harmless.
        """
        argv = build_fuzz_argv(
            target="copy_all_fault_fuzz",
            corpus="/tmp/corpus",
            artifact_dir="/tmp/artifacts/",
            per_target=30,
            maxlen=8192,
            dict_path=None,
        )
        # Structure: xmake run <target> -- <corpus> <flags...>
        self.assertEqual(argv[0], "xmake")
        self.assertEqual(argv[1], "run")
        self.assertEqual(argv[3], "--")
        # Everything before "--" is xmake-targeted.
        xmake_part = argv[:4]
        fuzzer_part = argv[4:]
        self.assertIn("xmake", xmake_part[0])
        self.assertIn("run", xmake_part[1])
        # The fuzzer part must not contain a standalone "--".
        self.assertNotIn("--", fuzzer_part)
        # All libFuzzer flags must be in the fuzzer part.
        for flag in fuzzer_part:
            if flag.startswith("-"):
                self.assertTrue(
                    flag.startswith(("-max_total_time=", "-artifact_prefix=",
                                     "-rss_limit_mb=", "-max_len=", "-timeout=",
                                     "-dict=")),
                    f"unexpected flag in fuzzer argv: {flag}"
                )

    def test_fuzz_argv_all_flags_after_separator(self) -> None:
        """Every libFuzzer flag must appear after the ``--`` separator."""
        argv = build_fuzz_argv(
            target="wal_roundtrip_fuzz",
            corpus="/tmp/corpus",
            artifact_dir="/tmp/artifacts/",
            per_target=60,
            maxlen=262144,
            dict_path="/tmp/wal_record.dict",
        )
        sep_idx = argv.index("--")
        # Everything xmake-related (xmake, run, target) is before "--".
        self.assertLess(argv.index("xmake"), sep_idx)
        self.assertLess(argv.index("run"), sep_idx)
        self.assertLess(argv.index("wal_roundtrip_fuzz"), sep_idx)
        # The corpus dir and all dash-prefixed flags are after "--".
        for item in argv[sep_idx + 1:]:
            if item.startswith("-"):
                self.assertIn("=", item, f"dangling flag without value: {item}")


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