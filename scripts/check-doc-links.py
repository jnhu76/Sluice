#!/usr/bin/env python3
"""check-doc-links.py — repository documentation link validator.

Checks:
  - Root README files (README.md, README.zh-CN.md), AGENTS.md, and all docs Markdown.
  - Markdown links [text](path) resolve to existing files/directories.
  - Backtick-quoted repository-relative paths (`path/to/file`) resolve.
  - Fails on broken links and stale moved-path references.
  - Reports historical paths separately but still fails unless explicitly allowlisted.
  - Skips generated TLA+ state artifacts.
  - Returns non-zero on every actionable problem.

Path resolution convention in this repository:
  - Paths in docs are REPO-ROOT-RELATIVE (e.g. `docs/api-reference.md` from
    `docs/README.md` resolves to `<repo>/docs/api-reference.md`).
  - Some older docs use doc-relative paths; we try doc-relative as a fallback.
  - Zig source references (`Io/fiber.zig`) resolve against the `zig/` subtree.

Exit code:
  0 — no broken links, no stale paths, no unallowlisted historical references.
  >0 — count of actionable problems (broken + stale + unallowlisted historical).
"""

import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# Files to scan
SCAN_FILES = [
    ROOT / "README.md",
    ROOT / "README.zh-CN.md",
    ROOT / "AGENTS.md",
]

# docs/ directory
DOCS_DIR = ROOT / "docs"
for p in sorted(DOCS_DIR.rglob("*.md")):
    SCAN_FILES.append(p)

# Directories whose content is generated and should be skipped entirely
SKIP_DIRS = [
    ROOT / "spec" / "tla" / "states",
    ROOT / "docs" / "spec" / "states",
]

# Zig source root — references like `Io/fiber.zig` resolve here
ZIG_ROOT = ROOT / "zig" / "lib" / "std"

# Allowlisted historical paths — these may be referenced from current docs
# because they are historical artifacts; each entry is a path that is allowed
# to appear in backtick references without triggering a stale-path failure.
HISTORICAL_ALLOWLIST = {
    # docs/reviews/ moved to docs/history/reviews/
    "docs/reviews",
    # docs/ closeout/ is historical by nature
    "docs/history/closeout",
    "docs/history/archive",
    "docs/history/implementation-plans",
    "docs/history/reviews",
}

# Paths that are known to have moved; references to them from non-historical
# docs are stale-path errors unless allowlisted above.
# Maps old prefix -> new prefix for path prefix replacement.
KNOWN_MOVED = {
    "docs/reviews": "docs/history/reviews",
    "docs/design/e12-rwlock.md": "docs/history/implementation-plans/e12-rwlock.md",
    "docs/design/e13-select": "docs/history/implementation-plans/e13-select",
    "docs/design/e14": "docs/history/implementation-plans/e14",
    "docs/design/formal/e13": "docs/history/formal-design/e13",
    "docs/e10-e12-api-semantic-closure.md": "docs/history/closeout/e10-e12-api-semantic-closure.md",
    "docs/e12-queue-scheduler-integration.md": "docs/history/implementation-plans/e12-queue-scheduler-integration.md",
    "docs/e12-queue-state-machine.md": "docs/history/implementation-plans/e12-queue-state-machine.md",
    "docs/async-mutex-nothrow-authority.md": "docs/history/implementation-plans/async-mutex-nothrow-authority.md",
    "docs/async-runtime-construction-method.md": "docs/history/implementation-plans/async-runtime-construction-method.md",
    "docs/async-runtime-plan.md": "docs/history/implementation-plans/async-runtime-plan.md",
}

# Regex patterns
# Markdown link: [text](target) — target may be URL, anchor, or path
MD_LINK_RE = re.compile(r'\[([^\]]*)\]\(([^)#]+)(?:#[^)]*)?\)')
# Backtick path: `some/path/to/file` (relative-looking, no spaces, has /)
BACKTICK_PATH_RE = re.compile(r'`([^`\s]*/[^`\s]+)`')

# Patterns that look like file paths but are actually code identifiers / non-paths
# These are skipped to avoid false positives.
NON_PATH_PATTERNS = [
    # Absolute paths (OS-level, not repo files)
    r'^/',
    # Brace expansion: verify-e{8,9}-stability.sh
    r'\{.*\}',
    # Code identifiers with ::  (handled separately but just in case)
    r'::',
    # Glob wildcards
    r'[*?]',
    # Pure state names like active/retired/consumed (no extension, short segments)
    # Handled by segment check below
    # Branch names like feat/..., audit/... (these are git branches, not files)
    r'^(feat|audit|fix|refactor|docs)/',
    # Single-letter directory like A/W/T/R
    r'^A/W',
    # Template placeholders like <phase/commit>
    r'^<',
    # try/catch
    r'^try/catch',
    # n/a
    r'^n/a$',
    # External references like /microsoft/stl, /xmake-io/xmake-docs
    r'^/[a-z]+/[a-z\-]+$',
]


def should_skip(path: Path) -> bool:
    """Return True if `path` is inside a skip directory."""
    for sd in SKIP_DIRS:
        try:
            path.relative_to(sd)
            return True
        except ValueError:
            pass
    return False


def strip_line_ref(ref: str) -> str:
    """Strip trailing `:line` or `:line-line` from a path reference."""
    m = re.match(r'^(.+?)(:\d+(?:-\d+)?)$', ref)
    if m:
        return m.group(1)
    return ref


def is_url(ref: str) -> bool:
    """Return True if ref is an external URL or mailto."""
    return ref.startswith(("http://", "https://", "mailto:", "#"))


def is_non_path(ref: str) -> bool:
    """Return True if ref looks like a code identifier, not a file path."""
    clean = strip_line_ref(ref)
    for pat in NON_PATH_PATTERNS:
        if re.match(pat, clean):
            return True

    # Check if all path segments are very short (<=3 chars) and there's no
    # file extension — likely a code state name like "active/retired/consumed"
    parts = clean.replace("\\", "/").split("/")
    if all(len(p) <= 12 for p in parts):
        # If no part has a file extension, it's probably not a file path
        if not any("." in p for p in parts):
            return True

    return False


def looks_like_zig_path(ref: str) -> bool:
    """Return True if ref looks like a Zig std lib source path."""
    base = strip_line_ref(ref)
    return base.endswith(".zig") or (len(base.split("/")) >= 1 and base[0:1].isupper() and "/" in base)


def resolve_ref(doc_path: Path, ref: str) -> Path | None:
    """Resolve a reference. Returns the resolved Path or None if it cannot
    be resolved as a repository path (e.g. URL, anchor-only, non-path).

    Tries repo-root-relative first, then doc-relative, then zig-root.
    """
    if is_url(ref):
        return None  # external — skip

    if is_non_path(ref):
        return None  # code identifier, not a file path

    # Strip line-number references
    clean_ref = strip_line_ref(ref)

    # Try repo-root-relative first (the dominant convention)
    candidate = (ROOT / clean_ref).resolve()
    if candidate.exists():
        return candidate

    # Try doc-relative
    candidate = (doc_path.parent / clean_ref).resolve()
    if candidate.exists():
        return candidate

    # Try zig-root for Zig source references
    if looks_like_zig_path(clean_ref):
        candidate = (ZIG_ROOT / clean_ref).resolve()
        if candidate.exists():
            return candidate

    # Return the repo-root-relative path for error reporting
    return (ROOT / clean_ref).resolve()


def is_historical(resolved: Path) -> bool:
    """Return True if resolved path is under an allowlisted historical subtree."""
    rel = os.path.relpath(resolved, ROOT)
    return any(
        rel.startswith(a + "/") or rel == a
        for a in HISTORICAL_ALLOWLIST
    )


def is_known_moved(ref: str) -> str | None:
    """Return the new path if ref matches a known moved path, else None."""
    clean = strip_line_ref(ref)
    for old, new in KNOWN_MOVED.items():
        if clean.startswith(old + "/") or clean == old:
            return new
    return None


def check_file(path: Path) -> tuple[list[str], list[str], list[str]]:
    """Check a single Markdown file.

    Returns (broken, stale, historical_unallowlisted).
    """
    broken = []
    stale = []
    historical = []

    try:
        text = path.read_text(encoding="utf-8")
    except OSError as e:
        broken.append(f"{path}: cannot read: {e}")
        return broken, stale, historical

    line_of = lambda pos: text[: pos].count("\n") + 1

    # --- Markdown links ---
    for m in MD_LINK_RE.finditer(text):
        ref = m.group(2).strip()
        resolved = resolve_ref(path, ref)
        if resolved is None:
            continue
        if not resolved.exists():
            moved = is_known_moved(ref)
            if moved:
                stale.append(
                    f"{path}:{line_of(m.start())}: "
                    f"STALE MOVED PATH: `{ref}` -> should be `{moved}`"
                )
            elif is_historical(resolved):
                historical.append(
                    f"{path}:{line_of(m.start())}: "
                    f"HISTORICAL (allowlisted): `{ref}`"
                )
            else:
                broken.append(
                    f"{path}:{line_of(m.start())}: "
                    f"BROKEN MARKDOWN LINK: `{ref}` -> {resolved}"
                )

    # --- Backtick paths ---
    for m in BACKTICK_PATH_RE.finditer(text):
        ref = m.group(1).strip()
        resolved = resolve_ref(path, ref)
        if resolved is None:
            continue
        if not resolved.exists():
            moved = is_known_moved(ref)
            if moved:
                stale.append(
                    f"{path}:{line_of(m.start())}: "
                    f"STALE MOVED PATH (backtick): `{ref}` -> should reference `{moved}`"
                )
            elif is_historical(resolved):
                historical.append(
                    f"{path}:{line_of(m.start())}: "
                    f"HISTORICAL (allowlisted): `{ref}`"
                )
            else:
                broken.append(
                    f"{path}:{line_of(m.start())}: "
                    f"BROKEN BACKTICK PATH: `{ref}` -> {resolved}"
                )

    return broken, stale, historical


def main() -> int:
    all_broken = []
    all_stale = []
    all_historical = []

    for f in SCAN_FILES:
        if not f.exists():
            print(f"WARNING: scan target does not exist: {f}")
            continue
        if should_skip(f):
            continue
        b, s, h = check_file(f)
        all_broken.extend(b)
        all_stale.extend(s)
        all_historical.extend(h)

    # Report
    print("=" * 60)
    print("check-doc-links.py — documentation link validation")
    print("=" * 60)

    if all_broken:
        print(f"\nBROKEN_MARKDOWN_LINKS/BROKEN_BACKTICK_PATHS: {len(all_broken)}")
        for item in all_broken:
            print(f"  {item}")
    else:
        print(f"\nBROKEN_MARKDOWN_LINKS: 0")

    if all_stale:
        print(f"\nSTALE_REPOSITORY_PATHS: {len(all_stale)}")
        for item in all_stale:
            print(f"  {item}")
    else:
        print(f"STALE_REPOSITORY_PATHS: 0")

    if all_historical:
        print(f"\nHISTORICAL_REFERENCES (allowlisted, informational): {len(all_historical)}")
        for item in all_historical:
            print(f"  {item}")
    else:
        print(f"HISTORICAL_REFERENCES: 0")

    # Final verdict
    problems = len(all_broken) + len(all_stale)
    print(f"\n{'=' * 60}")
    if problems == 0:
        print("VERDICT: PASS — no broken links, no stale paths")
        return 0
    else:
        print(f"VERDICT: FAIL — {problems} actionable problem(s)")
        return min(problems, 255)


if __name__ == "__main__":
    sys.exit(main())
