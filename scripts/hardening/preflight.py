"""Preflight checks - run before any test phase.

Every check returns a structured ``PreflightResult``.  A fatal failure
(``is_fatal=True``) means the runner must stop immediately.
"""

from __future__ import annotations

import os
import platform
import shutil
import subprocess
import sys
import tempfile
import textwrap
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional

from .model import Verdict

# ═══════════════════════════════════════════════════════════════════════════════
# Constants
# ═══════════════════════════════════════════════════════════════════════════════

MIN_PYTHON = (3, 10)
MIN_DISK_GB = 2
WARN_DISK_GB = 10

# ═══════════════════════════════════════════════════════════════════════════════
# Result types
# ═══════════════════════════════════════════════════════════════════════════════

@dataclass
class PreflightCheck:
    name: str
    passed: bool
    message: str
    is_fatal: bool = False
    detail: str = ""


@dataclass
class PreflightResult:
    """Aggregate preflight outcome."""

    checks: List[PreflightCheck] = field(default_factory=list)
    head_sha: str = ""
    head_short: str = ""
    worktree_dirty: bool = False
    worktree_diff: str = ""
    worktree_cached_diff: str = ""
    tool_versions: Dict[str, str] = field(default_factory=dict)
    compiler_probes: Dict[str, bool] = field(default_factory=dict)
    disk_gib: float = 0.0
    nproc: int = 1

    @property
    def passed(self) -> bool:
        return all(c.passed for c in self.checks if c.is_fatal)

    @property
    def fatal_failures(self) -> List[PreflightCheck]:
        return [c for c in self.checks if not c.passed and c.is_fatal]

    @property
    def warnings(self) -> List[PreflightCheck]:
        return [c for c in self.checks if not c.passed and not c.is_fatal]

    @property
    def verdict(self) -> Verdict:
        if self.fatal_failures:
            return Verdict.ENVIRONMENT_ERROR
        return Verdict.PASS


# ═══════════════════════════════════════════════════════════════════════════════
# Probe source
# ═══════════════════════════════════════════════════════════════════════════════

_CPP20_SOURCE = textwrap.dedent("""\
    #include <concepts>
    #include <version>
    static_assert(__cplusplus >= 202002L, "C++20 required");
    int main() { return 0; }
""")

_ASAN_SOURCE = textwrap.dedent("""\
    int main() {
        volatile int x = 0;
        return x;
    }
""")

_TSAN_SOURCE = textwrap.dedent("""\
    int main() { return 0; }
""")

_FUZZER_SOURCE = textwrap.dedent("""\
    extern "C" int LLVMFuzzerTestOneInput(
        const unsigned char *, unsigned long
    ) { return 0; }
""")


# ═══════════════════════════════════════════════════════════════════════════════
# Helpers
# ═══════════════════════════════════════════════════════════════════════════════

def _check_version(cmd: List[str], name: str, timeout: float = 10) -> str:
    """Run *cmd --version* and return the first line of output, or error."""
    try:
        r = subprocess.run(
            cmd + ["--version"],
            capture_output=True,
            text=True,
            timeout=timeout,
        )
        if r.returncode == 0:
            return r.stdout.splitlines()[0].strip() if r.stdout else "(no output)"
        else:
            return f"(exit {r.returncode})"
    except FileNotFoundError:
        return "(not found)"
    except subprocess.TimeoutExpired:
        return "(timeout)"


def _compile_probe(
    source: str,
    cxx: str,
    flags: List[str],
    timeout: float = 30,
) -> bool:
    """Compile a C++ probe source and return whether it succeeded."""
    with tempfile.TemporaryDirectory(prefix="sluice-preflight-") as tmp:
        src = Path(tmp) / "probe.cpp"
        src.write_text(source)
        out = Path(tmp) / "probe"
        try:
            r = subprocess.run(
                [cxx, "-std=c++20", "-o", str(out), str(src)] + flags,
                capture_output=True,
                text=True,
                timeout=timeout,
            )
            return r.returncode == 0
        except (FileNotFoundError, subprocess.TimeoutExpired):
            return False


# ═══════════════════════════════════════════════════════════════════════════════
# Main preflight
# ═══════════════════════════════════════════════════════════════════════════════

def run_preflight(project_root: Path) -> PreflightResult:
    """Run all preflight checks and return the aggregate result.

    This function does NOT write to the artifact directory; the caller is
    responsible for serialising the result.
    """
    result = PreflightResult()
    result.nproc = os.cpu_count() or 1

    # -- 1. Platform and Python ----------------------------------------------
    if sys.platform.startswith("linux"):
        result.checks.append(PreflightCheck(
            name="platform",
            passed=True,
            message=f"Linux ({platform.machine()})",
        ))
    else:
        result.checks.append(PreflightCheck(
            name="platform",
            passed=False,
            is_fatal=True,
            message=f"Unsupported platform: {sys.platform} (Linux required)",
        ))

    py_ver = sys.version_info[:2]
    if py_ver >= MIN_PYTHON:
        result.checks.append(PreflightCheck(
            name="python-version",
            passed=True,
            message=f"Python {py_ver[0]}.{py_ver[1]} (>= {MIN_PYTHON[0]}.{MIN_PYTHON[1]})",
        ))
    else:
        result.checks.append(PreflightCheck(
            name="python-version",
            passed=False,
            is_fatal=True,
            message=f"Python {py_ver[0]}.{py_ver[1]} < required {MIN_PYTHON[0]}.{MIN_PYTHON[1]}",
        ))

    # -- 2. Required tools ----------------------------------------------------
    tools = ["git", "xmake", "clang", "clang++"]
    for tool in tools:
        path = shutil.which(tool)
        if path:
            ver = _check_version([tool], tool)
            result.tool_versions[tool] = ver
            result.checks.append(PreflightCheck(
                name=f"tool-{tool}",
                passed=True,
                message=f"{tool}: {ver}",
                detail=f"path={path}",
            ))
        else:
            result.checks.append(PreflightCheck(
                name=f"tool-{tool}",
                passed=False,
                is_fatal=True,
                message=f"{tool}: not found on PATH",
            ))

    # -- 3. Compiler probes ---------------------------------------------------
    cxx = shutil.which("clang++")
    if cxx:
        # C++20
        cpp20_ok = _compile_probe(_CPP20_SOURCE, cxx, [])
        result.compiler_probes["c++20"] = cpp20_ok
        result.checks.append(PreflightCheck(
            name="probe-c++20",
            passed=cpp20_ok,
            is_fatal=True,
            message="C++20 compilation" if cpp20_ok else "C++20 compilation FAILED",
        ))

        # ASan + UBSan
        asan_ok = _compile_probe(_ASAN_SOURCE, cxx, ["-fsanitize=address,undefined"])
        result.compiler_probes["asan+ubsan"] = asan_ok
        result.checks.append(PreflightCheck(
            name="probe-asan+ubsan",
            passed=asan_ok,
            is_fatal=True,
            message="ASan+UBSan compilation" if asan_ok else "ASan+UBSan compilation FAILED",
        ))

        # TSan
        tsan_ok = _compile_probe(_TSAN_SOURCE, cxx, ["-fsanitize=thread"])
        result.compiler_probes["tsan"] = tsan_ok
        result.checks.append(PreflightCheck(
            name="probe-tsan",
            passed=tsan_ok,
            is_fatal=True,
            message="TSan compilation" if tsan_ok else "TSan compilation FAILED",
        ))

        # libFuzzer + ASan + UBSan
        fuzz_ok = _compile_probe(
            _FUZZER_SOURCE, cxx,
            ["-fsanitize=fuzzer,address,undefined"],
        )
        result.compiler_probes["libfuzzer"] = fuzz_ok
        result.checks.append(PreflightCheck(
            name="probe-libfuzzer",
            passed=fuzz_ok,
            is_fatal=False,  # Fuzz is optional
            message="libFuzzer compilation" if fuzz_ok else "libFuzzer compilation FAILED (fuzz phase will SKIP)",
        ))
    else:
        for probe in ["c++20", "asan+ubsan", "tsan", "libfuzzer"]:
            result.compiler_probes[probe] = False
            result.checks.append(PreflightCheck(
                name=f"probe-{probe}",
                passed=False,
                is_fatal=True,
                message=f"clang++ not found - cannot compile {probe} probe",
            ))

    # -- 4. Repository check -------------------------------------------------
    git = shutil.which("git")
    if git:
        # HEAD SHA
        try:
            r = subprocess.run(
                [git, "-C", str(project_root), "rev-parse", "HEAD"],
                capture_output=True, text=True, timeout=10,
            )
            result.head_sha = r.stdout.strip() if r.returncode == 0 else "unknown"
        except BaseException:
            result.head_sha = "unknown"

        try:
            r = subprocess.run(
                [git, "-C", str(project_root), "rev-parse", "--short=12", "HEAD"],
                capture_output=True, text=True, timeout=10,
            )
            result.head_short = r.stdout.strip() if r.returncode == 0 else "nogit"
        except BaseException:
            result.head_short = "nogit"

        # Dirty check
        try:
            r = subprocess.run(
                [git, "-C", str(project_root), "status", "--porcelain"],
                capture_output=True, text=True, timeout=10,
            )
            result.worktree_dirty = bool(r.stdout.strip())
        except BaseException:
            result.worktree_dirty = False

        # Diff
        if result.worktree_dirty:
            try:
                r = subprocess.run(
                    [git, "-C", str(project_root), "diff"],
                    capture_output=True, text=True, timeout=10,
                )
                result.worktree_diff = r.stdout
            except BaseException:
                result.worktree_diff = ""
            try:
                r = subprocess.run(
                    [git, "-C", str(project_root), "diff", "--cached"],
                    capture_output=True, text=True, timeout=10,
                )
                result.worktree_cached_diff = r.stdout
            except BaseException:
                result.worktree_cached_diff = ""

    # xmake.lua exists
    xmake_lua = project_root / "xmake.lua"
    if xmake_lua.is_file():
        result.checks.append(PreflightCheck(
            name="repo-xmake.lua",
            passed=True,
            message="xmake.lua found",
        ))
    else:
        result.checks.append(PreflightCheck(
            name="repo-xmake.lua",
            passed=False,
            is_fatal=True,
            message="xmake.lua not found - not a valid Sluice repository?",
        ))

    # Scripts and docs directories
    for d in ["scripts", "docs/testing"]:
        p = project_root / d
        result.checks.append(PreflightCheck(
            name=f"repo-{d}",
            passed=p.is_dir(),
            is_fatal=True,
            message=f"{d}/ {'exists' if p.is_dir() else 'MISSING'}",
        ))

    # -- 5. Filesystem --------------------------------------------------------
    for subdir in ["hardening-artifacts", ".hardening-corpus"]:
        d = project_root / subdir
        try:
            d.mkdir(parents=True, exist_ok=True)
            # Write a temp file to verify writeability.
            test_file = d / ".write-test"
            test_file.write_text("ok")
            test_file.unlink()
            result.checks.append(PreflightCheck(
                name=f"fs-{subdir}",
                passed=True,
                message=f"{subdir}/ - writable",
            ))
        except OSError as e:
            result.checks.append(PreflightCheck(
                name=f"fs-{subdir}",
                passed=False,
                is_fatal=True,
                message=f"{subdir}/ - cannot write: {e}",
            ))

    # -- 6. Disk space --------------------------------------------------------
    try:
        usage = shutil.disk_usage(project_root)
        free_gib = usage.free / (1024 ** 3)
        result.disk_gib = free_gib
        if free_gib < MIN_DISK_GB:
            result.checks.append(PreflightCheck(
                name="disk-space",
                passed=False,
                is_fatal=True,
                message=f"Disk free: {free_gib:.1f} GiB < {MIN_DISK_GB} GiB (minimum)",
            ))
        elif free_gib < WARN_DISK_GB:
            result.checks.append(PreflightCheck(
                name="disk-space",
                passed=False,
                is_fatal=False,
                message=f"Disk free: {free_gib:.1f} GiB (below {WARN_DISK_GB} GiB warning threshold)",
            ))
        else:
            result.checks.append(PreflightCheck(
                name="disk-space",
                passed=True,
                message=f"Disk free: {free_gib:.1f} GiB",
            ))
    except OSError as e:
        result.checks.append(PreflightCheck(
            name="disk-space",
            passed=False,
            is_fatal=True,
            message=f"Cannot check disk space: {e}",
        ))

    return result