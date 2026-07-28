"""Isolated TLC workspace management.

Every TLC invocation MUST happen inside a workspace created by
prepare_workspace() so that TLC never writes MC.out, states/, metadir, or
*_TTrace*.tla files into the source tree.
"""
from __future__ import annotations

import os
import shutil
import sys
import tempfile
from contextlib import contextmanager
from pathlib import Path
from typing import Iterator, Sequence

from .tlc import REPO_ROOT, SPEC_ROOT, WORKSPACE_PREFIX

# Files/dirs that TLC may emit. Used by assert_source_clean() to detect
# verifier-produced pollution without touching the user's unrelated files.
GENERATED_ARTIFACTS = [
    "MC.out",
    "states",
    "*.st",
    "TTrace*",
    "*_TTrace*.tla",
    "*_TTrace*.cfg",
]


def prepare_workspace(suite_id: str) -> Path:
    """Create an isolated, Sluice-owned temp directory for one TLC run."""
    tmp = Path(tempfile.mkdtemp(prefix=f"{WORKSPACE_PREFIX}{suite_id}."))
    return tmp


def copy_models(dest: Path, spec_dir: Path, files: Sequence[str]) -> None:
    """Copy the listed .tla/.cfg files from spec_dir into the workspace."""
    dest.mkdir(parents=True, exist_ok=True)
    for name in files:
        src = spec_dir / name
        if src.is_file():
            shutil.copy2(str(src), str(dest / name))


def cleanup_workspace(workspace: Path) -> None:
    """Defensively remove a workspace created by prepare_workspace().

    Refuses to remove anything that does not match the expected prefix, is
    empty, is the filesystem root, or is the repository root.
    """
    if not workspace.exists():
        return
    resolved = workspace.resolve()
    name = resolved.name
    if not name.startswith(WORKSPACE_PREFIX):
        print(
            f"refusing to clean {resolved}: name does not match prefix {WORKSPACE_PREFIX}",
            file=sys.stderr,
        )
        return
    if resolved == Path("/"):
        print("refusing to clean /", file=sys.stderr)
        return
    if resolved == REPO_ROOT.resolve():
        print(f"refusing to clean the repository root: {resolved}", file=sys.stderr)
        return
    # Belt-and-suspenders: only remove if it is actually a directory we own.
    if resolved.is_dir() and str(resolved).startswith(
        str(Path(tempfile.gettempdir()).resolve())
    ):
        shutil.rmtree(str(resolved), ignore_errors=False)


@contextmanager
def isolated_workspace(suite_id: str) -> Iterator[Path]:
    """Context manager that yields an isolated workspace and cleans it up.

    Honors FORMAL_KEEP_ARTIFACTS=1: when set, the workspace is preserved and
    its path is printed to stderr so CI can upload it.
    """
    ws = prepare_workspace(suite_id)
    keep = os.environ.get("FORMAL_KEEP_ARTIFACTS") == "1"
    try:
        yield ws
    finally:
        if keep:
            print(f"FORMAL_KEEP_ARTIFACTS=1 — preserved workspace: {ws}", file=sys.stderr)
        else:
            cleanup_workspace(ws)


def snapshot_tree() -> list[str]:
    """Return a sorted list of git porcelain entries (including untracked)."""
    import subprocess

    result = subprocess.run(
        ["git", "status", "--porcelain"],
        cwd=str(REPO_ROOT),
        capture_output=True,
        text=True,
    )
    return sorted(line for line in result.stdout.splitlines() if line.strip())


def new_entries(before: list[str], after: list[str]) -> list[str]:
    """Return entries present in `after` but not in `before`."""
    return sorted(set(after) - set(before))


def assert_no_source_pollution(before: list[str], after: list[str]) -> list[str]:
    """Return a list of verifier-produced artifacts under spec/tla/ or docs/.

    Only entries that appeared during the run are flagged; pre-existing files
    are left alone.
    """
    import fnmatch

    fresh = new_entries(before, after)
    offenders: list[str] = []
    for entry in fresh:
        # porcelain format: "XY path" or "XY orig -> renamed"
        path = entry[3:].split(" -> ")[-1]
        if not (path.startswith("spec/tla/") or path.startswith("docs/")):
            continue
        basename = Path(path).name
        for pattern in GENERATED_ARTIFACTS:
            if fnmatch.fnmatch(basename, pattern):
                offenders.append(path)
                break
    return offenders
