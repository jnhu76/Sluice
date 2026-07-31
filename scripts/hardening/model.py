"""Sluice hardening test runner – data models.

Pure-data types used throughout the runner.  No side effects, no I/O.
"""

from __future__ import annotations

import datetime
import enum
import shlex
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional


# ═══════════════════════════════════════════════════════════════════════════════
# Classification  – per-command outcome
# ═══════════════════════════════════════════════════════════════════════════════

class Classification(enum.Enum):
    PASS = "PASS"
    FAIL = "FAIL"
    TIMEOUT = "TIMEOUT"
    SKIP = "SKIP"
    SANITIZER_FAIL = "SANITIZER_FAIL"
    FUZZ_CRASH = "FUZZ_CRASH"
    BUILD_FAIL = "BUILD_FAIL"
    RUNNER_ERROR = "RUNNER_ERROR"


# ═══════════════════════════════════════════════════════════════════════════════
# Verdict      – whole-run outcome
# ═══════════════════════════════════════════════════════════════════════════════

class Verdict(enum.Enum):
    PASS = "PASS"
    HOLD = "HOLD"
    INCOMPLETE = "INCOMPLETE"
    ENVIRONMENT_ERROR = "ENVIRONMENT_ERROR"
    RUNNER_ERROR = "RUNNER_ERROR"


# ═══════════════════════════════════════════════════════════════════════════════
# Exit codes
# ═══════════════════════════════════════════════════════════════════════════════

EXIT_PASS = 0
EXIT_HOLD = 1
EXIT_ENVIRONMENT_ERROR = 2
EXIT_RUNNER_ERROR = 3
EXIT_INCOMPLETE = 4

VERDICT_EXIT: dict[Verdict, int] = {
    Verdict.PASS: EXIT_PASS,
    Verdict.HOLD: EXIT_HOLD,
    Verdict.ENVIRONMENT_ERROR: EXIT_ENVIRONMENT_ERROR,
    Verdict.RUNNER_ERROR: EXIT_RUNNER_ERROR,
    Verdict.INCOMPLETE: EXIT_INCOMPLETE,
}


# ═══════════════════════════════════════════════════════════════════════════════
# Configuration
# ═══════════════════════════════════════════════════════════════════════════════

@dataclass(frozen=True)
class Config:
    """Immutable runner configuration, populated from CLI + env vars."""

    mode: str  # "hardening" | "smoke" | "version-b" | "selftest"
    hours: float
    phase_timeout_seconds: int
    fuzz_seconds_override: Optional[int]
    keep_going: bool
    # Heartbeat interval for long-running commands (seconds). 0 disables.
    # See process.run_command: while a child runs, a heartbeat line is emitted
    # every `heartbeat_seconds` to stderr/run.log/heartbeats.jsonl so a silent
    # fuzz campaign or soak iteration cannot look hung. Default 60.
    heartbeat_seconds: int = 60

    # Source tracking for diagnostics
    hours_source: str = "default"
    timeout_source: str = "default"
    fuzz_source: str = "default"
    keep_going_source: str = "default"
    heartbeat_source: str = "default"


# ═══════════════════════════════════════════════════════════════════════════════
# Command specification / result  (one OS process)
# ═══════════════════════════════════════════════════════════════════════════════

@dataclass
class CommandSpec:
    """Everything needed to run one external command."""

    phase: str
    iteration: str
    target: str
    mode: str
    command: List[str]
    timeout_seconds: float
    log_path: Path
    environment: Dict[str, str] = field(default_factory=dict)
    sanitizer_kind: Optional[str] = None  # "tsan" | "asan" | None
    synthetic: bool = False
    # Heartbeat interval (seconds) while this command runs. 0 disables. The
    # runner emits a heartbeat line every interval to stderr/run.log plus
    # heartbeats.jsonl (when heartbeats_path is set) so a silent long command
    # cannot appear hung. See process.run_command.
    heartbeat_seconds: int = 0
    # Optional path to the per-run heartbeats.jsonl (None => no JSONL output).
    heartbeats_path: Optional[Path] = None
    # Optional path to the run log for heartbeat appending (None => no run.log write).
    run_log_path: Optional[Path] = None

    def header_lines(self, head_sha: str, dirty: bool) -> List[str]:
        """Return the log header lines for this command."""
        lines: List[str] = []
        lines.append("## sluice hardening command log")
        if self.synthetic:
            lines.append("synthetic=yes")
        lines.append(f"phase={self.phase}")
        lines.append(f"iteration={self.iteration}")
        lines.append(f"target={self.target}")
        lines.append(f"mode={self.mode}")
        lines.append(f"head={head_sha}")
        lines.append(f"worktree_dirty={'1' if dirty else '0'}")
        lines.append(f"command={shlex.join(self.command)}")
        lines.append(f"sanitizer={self.sanitizer_kind or 'none'}")
        for k, v in sorted(self.environment.items()):
            lines.append(f"sanitizer_env={k}={v}")
        lines.append(f"timeout_s={self.timeout_seconds}")
        now = datetime.datetime.now(datetime.timezone.utc)
        lines.append(f"start_iso={now.isoformat()}")
        lines.append(f"start_epoch={int(now.timestamp())}")
        lines.append("----- output -----")
        return lines

    def footer_lines(
        self,
        end_epoch: int,
        duration_s: float,
        raw_exit: Optional[int],
        classification: Classification,
        timed_out: bool,
        term_sent: bool,
        kill_sent: bool,
        sanitizer_signature: Optional[str],
    ) -> List[str]:
        """Return the log footer lines for this command."""
        lines: List[str] = []
        lines.append("----- end output -----")
        end_dt = datetime.datetime.fromtimestamp(end_epoch, tz=datetime.timezone.utc)
        lines.append(f"end_iso={end_dt.isoformat()}")
        lines.append(f"end_epoch={end_epoch}")
        lines.append(f"duration_s={duration_s}")
        lines.append(f"raw_exit={raw_exit}")
        lines.append(f"classification={classification.value}")
        lines.append(f"timed_out={'1' if timed_out else '0'}")
        lines.append(f"term_sent={'1' if term_sent else '0'}")
        lines.append(f"kill_sent={'1' if kill_sent else '0'}")
        if sanitizer_signature:
            lines.append(f"santizer_signature={sanitizer_signature}")
        if self.synthetic:
            lines.append("synthetic=yes (no hardening HOLD)")
        return lines


@dataclass
class CommandResult:
    """Outcome of one executed command."""

    phase: str
    iteration: str
    target: str
    mode: str
    command: List[str]
    classification: Classification
    exit_code: Optional[int]
    duration_seconds: float
    log_path: Path
    sanitizer_signature: Optional[str]
    timed_out: bool
    term_sent: bool
    kill_sent: bool
    started_at: str
    finished_at: str
    synthetic: bool = False

    def to_json_dict(self) -> dict:
        return {
            "phase": self.phase,
            "iteration": self.iteration,
            "target": self.target,
            "mode": self.mode,
            "command": shlex.join(self.command),
            "classification": self.classification.value,
            "exit_code": self.exit_code,
            "duration_seconds": self.duration_seconds,
            "log_path": str(self.log_path),
            "sanitizer_signature": self.sanitizer_signature,
            "timed_out": self.timed_out,
            "term_sent": self.term_sent,
            "kill_sent": self.kill_sent,
            "started_at": self.started_at,
            "finished_at": self.finished_at,
            "synthetic": self.synthetic,
        }


# ═══════════════════════════════════════════════════════════════════════════════
# Phase-level statistics (accumulated per phase)
# ═══════════════════════════════════════════════════════════════════════════════

@dataclass
class PhaseStats:
    """Mutable counters for one phase."""

    iteration: int = 0
    executed: int = 0
    passed: int = 0
    failed: int = 0
    skipped: int = 0
    timed_out: int = 0
    sanitizer_fail: int = 0
    fuzz_crash: int = 0
    build_fail: int = 0


# ═══════════════════════════════════════════════════════════════════════════════
# Fuzz corpus snapshot
# ═══════════════════════════════════════════════════════════════════════════════

@dataclass
class FuzzCorpusSnapshot:
    target: str
    before_files: int = 0
    before_bytes: int = 0
    after_files: int = 0
    after_bytes: int = 0
    new_artifacts: List[str] = field(default_factory=list)
    classification: Optional[Classification] = None