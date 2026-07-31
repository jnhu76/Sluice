"""Test-failure detail extraction for the hardening runner.

A best-effort diagnostic layer that parses a command log and extracts a
content-based failure fingerprint (failing binary, case name, source file:line,
assertion expression, xmake summary). This is an INDEX over the raw log — the
log remains the authoritative evidence. Parse failures degrade gracefully and
are never reported as runner errors.

Layering (prompt §9):
  - classification          : runner-determined fact (FAIL/TIMEOUT/...)
  - diagnostic extraction   : parser-extracted fact (this module)
  - root-cause note         : human/model analysis (NOT this module)

This module never reclassifies an exit code. exit 255 stays FAIL; it only adds
a human-readable `exit_semantics` note for the narrow, proven case of
`xmake test -v` with a test-failure summary and no timeout/signal.
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional

# ANSI escape stripper (xmake banners + tool_version strings carry color codes).
_ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")

# Sluice harness FAILED block (tests/harness.hpp).
_HARNESS_CASE_RE = re.compile(r"FAILED in case: (?P<case>[^\n]+)")
_HARNESS_BLOCK_RE = re.compile(
    r"FAILED (?P<n>\d+) check\(s\):\n(?P<body>(?:  [^\n]*\n)+)"
)
_HARNESS_CHECK_RE = re.compile(
    r"^\s{2}(?P<file>[^:\s][^:]*?):(?P<line>\d+):\s*(?P<expr>.+?)\s*$"
)
_HARNESS_MSG_RE = re.compile(r"^\s{4,}(?P<msg>.+?)\s*$")

# xmake "Detailed summary: Failed tests:" block.
_XMAKE_SUMMARY_FAILED_RE = re.compile(
    r"Failed tests:\s*\n((?:\s*-\s*[^\n]+\n)+)", re.MULTILINE
)
# xmake "99% tests passed, 1 test(s) failed out of 119" summary line.
_XMAKE_PCT_RE = re.compile(
    r"(\d+)% tests passed, (\d+) test\(s\) failed out of (\d+)"
)
# xmake per-binary failure banner "[ 96%]: <binary>/<binary> ... failed".
_XMAKE_FAILED_BANNER_RE = re.compile(
    r"\[\s*\d+%\]:[^\n]*?(?P<bin>[A-Za-z0-9_]+)/[A-Za-z0-9_]+"
    r"[^\n]*?(?P<verb>failed)[^\n]*"
)
# "running.test <binary>" preceding an abort (fallback binary source).
_XMAKE_BIN_RUN_RE = re.compile(r"running\.test\s+(?P<bin>[A-Za-z0-9_]+)")

# Abnormal process termination signatures (NO harness FAILED block).
_TERMINATE_RE = re.compile(
    r"terminate called (?:without an active exception"
    r"|after throwing an instance of .*)"
)
_ABORT_RE = re.compile(
    r"(?:Segmentation fault|core dumped|Aborted|SIGSEGV|trace/bpt trap)"
)
_PROD_ASSERT_RE = re.compile(
    r"(?P<file>[^:\s][^:]*?):(?P<line>\d+):[^:]*: "
    r"Assertion `(?P<expr>[^\']+)\' (?:failed|not reached)"
)


def strip_ansi(s: str) -> str:
    """Remove ANSI color escapes. Exposed for unit tests."""
    return _ANSI_RE.sub("", s)


@dataclass
class TestFailureDetail:
    """Content-based failure detail extracted from one command log.

    All fields are Optional: a parse that finds nothing leaves them None and
    `parse_status` describes why. The `fingerprint` is always a stable string
    built from the available fields (missing fields become "?").
    """

    framework: Optional[str] = None  # "sluice_harness" | None
    suite_command: Optional[str] = None  # the command that ran the suite
    failing_binary: Optional[str] = None
    case_name: Optional[str] = None
    source_file: Optional[str] = None
    source_line: Optional[int] = None
    expression: Optional[str] = None
    message: Optional[str] = None
    xmake_summary: Optional[str] = None
    exit_semantics: Optional[str] = None
    abnormal_signature: Optional[str] = None
    parse_status: str = "unknown"  # harness_assertion | process_terminate |
    #                                  process_abort | process_assert | unknown

    @property
    def fingerprint(self) -> str:
        parts = [
            self.failing_binary or "?",
            self.case_name or self.abnormal_signature or "?",
            self.source_file or "?",
            str(self.source_line) if self.source_line is not None else "?",
            self.expression or self.abnormal_signature or "?",
        ]
        return " | ".join(parts)

    def to_dict(self) -> Dict[str, Any]:
        return {
            "framework": self.framework,
            "suite_command": self.suite_command,
            "failing_binary": self.failing_binary,
            "case_name": self.case_name,
            "source_file": self.source_file,
            "source_line": self.source_line,
            "expression": self.expression,
            "message": self.message,
            "xmake_summary": self.xmake_summary,
            "exit_semantics": self.exit_semantics,
            "abnormal_signature": self.abnormal_signature,
            "parse_status": self.parse_status,
            "fingerprint": self.fingerprint,
        }


def classify_exit_semantics(
    exit_code: Optional[int],
    timed_out: bool,
    term_sent: bool,
    kill_sent: bool,
    command: str,
    detail: "TestFailureDetail",
) -> str:
    """Human-readable exit semantics.

    ONLY annotates narrow, proven cases. exit 255 / -1 are NEVER reclassified
    to PASS or infrastructure_error. The runner's classification (FAIL) stays
    authoritative; this is only a human note attached to the failure record.
    """
    if timed_out or term_sent or kill_sent:
        return "timeout_or_signal"
    if detail.parse_status == "process_terminate":
        # A test binary called std::terminate (uncaught exception in a noexcept
        # scope / joinable-thread destruction). xmake surfaces this as a failing
        # test. Still FAIL.
        return "test binary aborted via std::terminate"
    if detail.parse_status in ("process_abort", "process_assert"):
        return "test binary aborted (signal/production assertion)"
    if command.strip() == "xmake test -v" and exit_code == 255 and \
            detail.xmake_summary:
        return "xmake test failure exit (-1 surfaced as 255)"
    return "normal"


def parse_failure_detail(log_path: Path, command: str = "") -> TestFailureDetail:
    """Parse one command log; return a TestFailureDetail.

    Never raises on malformed input — returns whatever it could extract.
    Missing/unreadable logs yield parse_status describing the failure mode.
    """
    detail = TestFailureDetail(suite_command=command or None)
    try:
        raw = log_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        detail.parse_status = "missing_log"
        detail.exit_semantics = classify_exit_semantics(
            None, False, False, False, command, detail)
        return detail
    if not raw.strip():
        detail.parse_status = "empty_log"
        detail.exit_semantics = classify_exit_semantics(
            None, False, False, False, command, detail)
        return detail

    txt = strip_ansi(raw)

    # xmake summary line.
    m = _XMAKE_PCT_RE.search(txt)
    if m:
        pct, nfail, ntot = m.group(1), m.group(2), m.group(3)
        detail.xmake_summary = (
            f"{nfail} test(s) failed out of {ntot} ({pct}% passed)"
        )

    # Failing binary via xmake "Detailed summary: Failed tests:".
    bins: List[str] = []
    m = _XMAKE_SUMMARY_FAILED_RE.search(txt)
    if m:
        for ln in m.group(1).splitlines():
            ln = ln.strip()
            if ln.startswith("-"):
                tok = ln.lstrip("- ").strip().split("/")
                if tok and tok[0]:
                    bins.append(tok[0])
    if not bins:
        for bm in _XMAKE_FAILED_BANNER_RE.finditer(txt):
            if bm.group("bin") not in bins:
                bins.append(bm.group("bin"))

    # Harness FAILED block (assertion failures like T32).
    case_match = _HARNESS_CASE_RE.search(txt)
    block = _HARNESS_BLOCK_RE.search(txt)
    if case_match and block:
        detail.framework = "sluice_harness"
        detail.case_name = case_match.group("case").strip()
        body = block.group("body")
        for ln in body.splitlines():
            cm = _HARNESS_CHECK_RE.match(ln)
            if cm:
                detail.source_file = cm.group("file").strip()
                detail.source_line = int(cm.group("line"))
                detail.expression = cm.group("expr").strip()
                break
        for ln in body.splitlines():
            if _HARNESS_CHECK_RE.match(ln):
                continue
            mm = _HARNESS_MSG_RE.match(ln)
            if mm:
                detail.message = mm.group("msg").strip()
                break
        if bins:
            detail.failing_binary = bins[0]
        detail.parse_status = "harness_assertion"
        detail.exit_semantics = classify_exit_semantics(
            None, False, False, False, command, detail)
        return detail

    # No harness block: look for process-level abnormal termination.
    # Order matters: a failed production assertion (`assert`/`__assert_fail`)
    # normally also emits "Aborted (core dumped)" in the same log, so the more
    # specific _PROD_ASSERT_RE must be probed BEFORE the generic _ABORT_RE —
    # otherwise the file/line/expression would be dropped from the fingerprint
    # and distinct assertions would collapse into one "process_abort_or_signal"
    # group. Reuse a single search result instead of searching twice.
    pa = _PROD_ASSERT_RE.search(txt)
    if _TERMINATE_RE.search(txt):
        detail.abnormal_signature = "terminate_called_without_active_exception"
        detail.parse_status = "process_terminate"
    elif pa:
        detail.abnormal_signature = f"production_assert: {pa.group('expr')[:80]}"
        detail.source_file = pa.group("file").strip()
        detail.source_line = int(pa.group("line"))
        detail.expression = pa.group("expr").strip()
        detail.parse_status = "process_assert"
    elif _ABORT_RE.search(txt):
        detail.abnormal_signature = "process_abort_or_signal"
        detail.parse_status = "process_abort"

    # Capture the failing binary even for abnormal terminations.
    if not bins:
        run_bins = [b.group("bin") for b in _XMAKE_BIN_RUN_RE.finditer(txt)]
        if run_bins:
            bins = [run_bins[-1]]
    if bins:
        detail.failing_binary = bins[0]

    detail.exit_semantics = classify_exit_semantics(
        None, False, False, False, command, detail)
    return detail


@dataclass
class FailureGroup:
    """Aggregated occurrences of one failure fingerprint."""

    fingerprint: str
    occurrences: int = 0
    binary: Optional[str] = None
    case: Optional[str] = None
    source_file: Optional[str] = None
    source_line: Optional[int] = None
    expression: Optional[str] = None
    message: Optional[str] = None
    abnormal_signature: Optional[str] = None
    framework: Optional[str] = None
    parse_status: Optional[str] = None
    exit_semantics: Optional[str] = None
    xmake_summary: Optional[str] = None
    phases: List[str] = field(default_factory=list)
    iterations: List[str] = field(default_factory=list)
    sample_logs: List[str] = field(default_factory=list)
    first_seen: Optional[str] = None
    last_seen: Optional[str] = None
    retry_reproduced: int = 0
    retry_total: int = 0

    def to_dict(self) -> Dict[str, Any]:
        return {
            "fingerprint": self.fingerprint,
            "occurrences": self.occurrences,
            "binary": self.binary,
            "case": self.case,
            "source_file": self.source_file,
            "source_line": self.source_line,
            "expression": self.expression,
            "message": self.message,
            "abnormal_signature": self.abnormal_signature,
            "framework": self.framework,
            "parse_status": self.parse_status,
            "exit_semantics": self.exit_semantics,
            "xmake_summary": self.xmake_summary,
            "phases": list(self.phases),
            "iterations": list(self.iterations),
            "sample_logs": list(self.sample_logs),
            "first_seen": self.first_seen,
            "last_seen": self.last_seen,
            "retry_reproduced": self.retry_reproduced,
            "retry_total": self.retry_total,
        }


def aggregate_failures(
    failures: List[Any],
) -> tuple[List[FailureGroup], int]:
    """Cluster CommandResult-like failure records by content fingerprint.

    `failures` is a list of objects with the CommandResult attributes (phase,
    iteration, target, classification, exit_code, timed_out, term_sent,
    kill_sent, command, log_path, started_at). Returns (groups, total_count)
    sorted by occurrence count descending.
    """
    groups: Dict[str, FailureGroup] = {}
    for fr in failures:
        cmd = " ".join(getattr(fr, "command", [])) if isinstance(
            getattr(fr, "command", None), list) else str(
            getattr(fr, "command", ""))
        detail = parse_failure_detail(
            Path(getattr(fr, "log_path", "") or ""), cmd)
        # Override exit_semantics with the real exit code if available.
        detail.exit_semantics = classify_exit_semantics(
            getattr(fr, "exit_code", None),
            getattr(fr, "timed_out", False),
            getattr(fr, "term_sent", False),
            getattr(fr, "kill_sent", False),
            cmd, detail)
        fp = detail.fingerprint
        is_retry = "-retry" in str(getattr(fr, "iteration", ""))

        if fp not in groups:
            groups[fp] = FailureGroup(
                fingerprint=fp,
                binary=detail.failing_binary,
                case=detail.case_name,
                source_file=detail.source_file,
                source_line=detail.source_line,
                expression=detail.expression,
                message=detail.message,
                abnormal_signature=detail.abnormal_signature,
                framework=detail.framework,
                parse_status=detail.parse_status,
                exit_semantics=detail.exit_semantics,
                xmake_summary=detail.xmake_summary,
            )
        g = groups[fp]
        g.occurrences += 1
        phase = getattr(fr, "phase", "?")
        if phase not in g.phases:
            g.phases.append(phase)
        g.iterations.append(str(getattr(fr, "iteration", "?")))
        log_str = str(getattr(fr, "log_path", ""))
        if log_str not in g.sample_logs and len(g.sample_logs) < 3:
            g.sample_logs.append(log_str)
        started = getattr(fr, "started_at", "")
        if started:
            if g.first_seen is None or started < g.first_seen:
                g.first_seen = started
            if g.last_seen is None or started > g.last_seen:
                g.last_seen = started
        if is_retry:
            g.retry_total += 1
            # A retry "reproduced" if it is also a failure kind (it is in this
            # list, since we only aggregate failures).
            g.retry_reproduced += 1
            # Track base-iteration reproduction (kept simple: count reproductions).

    sorted_groups = sorted(groups.values(), key=lambda g: -g.occurrences)
    total = sum(g.occurrences for g in sorted_groups)
    return sorted_groups, total
