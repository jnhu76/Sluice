"""TLA+ tool-chain resolution and TLC process execution.

The jar search order is:
    1. TLA2TOOLS_JAR environment variable
    2. Sluice user cache (~/.cache/sluice/formal/tla2tools.jar)
    3. The pre-existing untracked repo-root jar (legacy compatibility)
    4. Fail with an actionable message pointing at bootstrap.py

Every TLC invocation MUST happen inside an isolated workspace produced by
workspace.prepare_workspace(); never run TLC directly inside spec/tla/.
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Iterable

SCRIPT_DIR = Path(__file__).resolve().parent.parent
LOCK_FILE = SCRIPT_DIR / "tla2tools.lock.json"
REPO_ROOT = SCRIPT_DIR.parent.parent
SPEC_ROOT = REPO_ROOT / "spec" / "tla"
CACHE_ROOT = Path.home() / ".cache" / "sluice" / "formal"
ARTIFACT_ROOT = REPO_ROOT / "build" / "formal"

# Prefix every temp dir with this so cleanup can never confuse our workspace
# with unrelated /tmp content.
WORKSPACE_PREFIX = "sluice-formal."


def _read_lock() -> dict:
    try:
        with LOCK_FILE.open("r", encoding="utf-8") as f:
            return json.load(f)
    except FileNotFoundError:
        return {}
    except json.JSONDecodeError:
        return {}


def _sha256(path: Path) -> str:
    import hashlib

    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def resolve_jar(strict: bool = True) -> Path:
    """Resolve the tla2tools.jar path, optionally verifying its checksum.

    Parameters
    ----------
    strict:
        When True (default), verify the jar's SHA-256 against the lock file.
        When False, skip the checksum check and emit a warning instead.
    """
    lock = _read_lock()
    expected = lock.get("sha256", "")

    candidates: list[tuple[Path, str]] = []
    env_jar = os.environ.get("TLA2TOOLS_JAR")
    if env_jar:
        candidates.append((Path(env_jar), "TLA2TOOLS_JAR"))
    cached = CACHE_ROOT / lock.get("cache_filename", "tla2tools.jar")
    if cached.is_file():
        candidates.append((cached, "user cache"))
    legacy = REPO_ROOT / "tla2tools.jar"
    if legacy.is_file():
        candidates.append((legacy, "repo-root legacy"))

    if not candidates:
        print(
            "error: tla2tools.jar not found.\n"
            "  searched: TLA2TOOLS_JAR, ~/.cache/sluice/formal/, <repo>/tla2tools.jar\n"
            "  fix: python3 scripts/formal/bootstrap.py",
            file=sys.stderr,
        )
        raise FileNotFoundError("tla2tools.jar not found")

    # Prefer the first existing candidate; if multiple exist, prefer env > cache > legacy.
    for path, source in candidates:
        if not path.is_file():
            continue
        if strict and expected:
            actual = _sha256(path)
            if actual != expected:
                print(
                    f"warning: {source} jar checksum mismatch\n"
                    f"  path:     {path}\n"
                    f"  expected: {expected}\n"
                    f"  actual:   {actual}",
                    file=sys.stderr,
                )
                continue
        return path

    raise FileNotFoundError("tla2tools.jar not found (all candidates failed checksum)")


def tlc_version(jar: Path) -> str:
    """Return the TLC version string reported by the jar."""
    try:
        result = subprocess.run(
            ["java", "-cp", str(jar), "tlc2.TLC"],
            capture_output=True,
            text=True,
            timeout=30,
        )
    except (subprocess.TimeoutExpired, FileNotFoundError):
        return "(unavailable)"
    for line in (result.stdout + result.stderr).splitlines():
        if line.startswith("TLC2 Version"):
            return line.strip()
    return "(unknown)"


def run_tlc(
    jar: Path,
    workdir: Path,
    model: str,
    cfg: str,
    workers: str = "1",
    metadir: Path | None = None,
    *,
    extra_args: Iterable[str] = (),
) -> subprocess.CompletedProcess[str]:
    """Run TLC inside an already-prepared workspace directory.

    The caller is responsible for creating the workspace (via
    workspace.prepare_workspace) and copying the needed .tla/.cfg files into
    it before calling this function.
    """
    if metadir is None:
        metadir = workdir.parent / f"{workdir.name}.meta"
    metadir.mkdir(parents=True, exist_ok=True)

    cmd = [
        "java",
        "-XX:+UseParallelGC",
        "-cp",
        str(jar),
        "tlc2.TLC",
        "-nowarning",
        "-workers",
        str(workers),
        "-metadir",
        str(metadir),
        *extra_args,
        "-config",
        cfg,
        model,
    ]
    return subprocess.run(
        cmd,
        cwd=str(workdir),
        capture_output=True,
        text=True,
        timeout=600,
    )


# --- TLC output predicates ------------------------------------------------


def tlc_launched(output: str) -> bool:
    return "Starting..." in output


def tlc_passed(output: str) -> bool:
    return "Model checking completed. No error has been found" in output


def tlc_deadlocked(output: str) -> bool:
    import re

    return bool(re.search(r"Deadlock reached|is deadlocked", output, re.IGNORECASE))


def named_violation(output: str, name: str) -> bool:
    """Return True when TLC reports a violation of the named property.

    TLC's emitted phrasing depends on the property kind and toolchain version
    (lock pins tla2tools v1.8.0 / 2026.07.18):
      - safety invariant : "Invariant <Name> is violated"
      - temporal property: "Temporal property <Name> was violated"
    """
    import re

    patterns = [
        rf"Invariant {re.escape(name)} is violated",
        rf"Temporal property {re.escape(name)} was violated",
    ]
    return any(re.search(p, output) for p in patterns)


def no_typeok_violation(output: str) -> bool:
    """Return True when the output contains NO EventTimerTypeOK violation."""
    import re

    return not re.search(
        r"Invariant EventTimerTypeOK is violated|EventTimerTypeOK is.*[Vv]iolated",
        output,
    )


def metrics(output: str) -> str:
    """Extract a compact 'states / depth / runtime' summary from TLC output."""
    import re

    states = ""
    depth = ""
    runtime = ""
    m = re.search(r"states generated.*", output)
    if m:
        states = m.group(0).strip()
    m = re.search(r"The depth of the complete state graph search is.*", output)
    if m:
        depth = m.group(0).strip()
    m = re.search(r"^Finished in.*", output, re.MULTILINE)
    if m:
        runtime = m.group(0).strip()
    return "; ".join(filter(None, [states, depth, runtime]))
