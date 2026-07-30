"""Sluice overnight test runner - Python standard library implementation.

This package replaces the original Bash overnight-local.sh with a
structured Python implementation.  It has zero third-party dependencies.
"""

from .model import (
    Classification,
    CommandResult,
    CommandSpec,
    Config,
    FuzzCorpusSnapshot,
    PhaseStats,
    Verdict,
    VERDICT_EXIT,
    EXIT_PASS,
    EXIT_HOLD,
    EXIT_ENVIRONMENT_ERROR,
    EXIT_RUNNER_ERROR,
    EXIT_INCOMPLETE,
)
from .preflight import PreflightResult, run_preflight
from .process import run_command
from .phases import (
    PhaseContext,
    PhaseOutcome,
    refresh_target_cache,
    target_exists,
    phase_baseline,
    phase_debug_soak,
    phase_tsan,
    phase_asanubsan,
    phase_fuzz,
    phase_final_debug,
)
from .reporting import (
    write_all_outputs,
    write_preflight_txt,
    write_preflight_json,
    write_environment_json,
)

__all__ = [
    "Classification",
    "CommandResult",
    "CommandSpec",
    "Config",
    "EXIT_HOLD",
    "EXIT_PASS",
    "EXIT_ENVIRONMENT_ERROR",
    "EXIT_RUNNER_ERROR",
    "EXIT_INCOMPLETE",
    "FuzzCorpusSnapshot",
    "PhaseContext",
    "PhaseOutcome",
    "PhaseStats",
    "PreflightResult",
    "Verdict",
    "VERDICT_EXIT",
    "phase_asanubsan",
    "phase_baseline",
    "phase_debug_soak",
    "phase_final_debug",
    "phase_fuzz",
    "phase_tsan",
    "refresh_target_cache",
    "run_command",
    "run_preflight",
    "target_exists",
    "write_all_outputs",
    "write_environment_json",
    "write_preflight_json",
    "write_preflight_txt",
]