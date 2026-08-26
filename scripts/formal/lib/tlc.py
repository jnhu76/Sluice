"""TLA+ tool-chain resolution and TLC version reporting.

The jar search order is:
    1. TLA2TOOLS_JAR environment variable
    2. Sluice user cache (~/.cache/sluice/formal/tla2tools.jar)
    3. The pre-existing untracked repo-root jar (legacy compatibility)
    4. Fail with an actionable message pointing at bootstrap.py

Governance: every TLC invocation MUST happen inside an isolated temporary
workspace (as the manifest-driven scripts/formal/verify-*.sh verifiers
build them); never run TLC directly inside spec/tla/.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent.parent
LOCK_FILE = SCRIPT_DIR / "tla2tools.lock.json"
REPO_ROOT = SCRIPT_DIR.parent.parent
CACHE_ROOT = Path.home() / ".cache" / "sluice" / "formal"


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
