#!/usr/bin/env python3
"""Check Sluice documentation links.

Verifies that Markdown links pointing to local files resolve to existing files.
Identifies references to moved paths that should be updated.

Usage:
    python3 .docs-refactor/check_links.py
"""

import os
import re
import sys
from pathlib import Path
from collections import defaultdict

REPO_ROOT = Path(__file__).resolve().parent.parent
DOCS_ROOT = REPO_ROOT / "docs"

# Regex to match Markdown links to local files: [text](path)
LINK_RE = re.compile(r'\[([^\]]+)\]\(([^)]+)\)')

# Regex to match backtick paths: `docs/foo.md`
BACKTICK_RE = re.compile(r'`((?:docs|spec|tests|src|include|bench|examples|scripts)/[A-Za-z0-9_./-]+\.md)`')

# Regex to match bare paths (no backticks) in references like "See docs/foo.md."
BARE_RE = re.compile(r'(?:^|\s)(docs/[A-Za-z0-9_./-]+\.md)(?=[\s.])', re.MULTILINE)

broken_links = []      # (file, link, target)
stale_refs = []        # (file, referenced_path) where the file no longer exists at that path
all_md_links = defaultdict(set)
all_refs = defaultdict(set)


def normalize_path(path_str, source_file):
    """Return absolute path if path_str is a local file reference, else None."""
    # Skip URLs and anchors
    if path_str.startswith(('http://', 'https://', 'mailto:', '#')):
        return None
    # Strip anchor
    target = path_str.split('#')[0]
    if not target:
        return None
    # Resolve relative to the source file's directory
    src_dir = source_file.parent
    if target.startswith('/'):
        # Repo-relative
        candidate = REPO_ROOT / target.lstrip('/')
    else:
        candidate = (src_dir / target).resolve()
    return candidate


def check_markdown_links(md_file):
    """Find Markdown links in a file and check they resolve."""
    try:
        content = md_file.read_text(encoding='utf-8', errors='replace')
    except Exception:
        return
    for m in LINK_RE.finditer(content):
        text = m.group(1)
        target = m.group(2)
        candidate = normalize_path(target, md_file)
        if candidate is None:
            continue
        # Only check docs/ references
        if 'docs/' not in str(candidate):
            continue
        if not candidate.exists():
            broken_links.append((str(md_file.relative_to(REPO_ROOT)), f"[{text}]({target})", str(candidate.relative_to(REPO_ROOT) if candidate.is_relative_to(REPO_ROOT) else candidate)))
        else:
            all_md_links[str(candidate.relative_to(REPO_ROOT))].add(str(md_file.relative_to(REPO_ROOT)))


def check_bare_refs(md_file):
    """Find bare docs/...md references in backticks."""
    try:
        content = md_file.read_text(encoding='utf-8', errors='replace')
    except Exception:
        return
    # Find all docs/X.md references in backticks
    for m in BACKTICK_RE.finditer(content):
        ref = m.group(1)
        if not ref.startswith('docs/'):
            continue
        candidate = REPO_ROOT / ref
        if not candidate.exists():
            stale_refs.append((str(md_file.relative_to(REPO_ROOT)), ref))
        else:
            all_refs[ref].add(str(md_file.relative_to(REPO_ROOT)))

    # Find bare references (no backticks) - only check for moved paths
    for m in BARE_RE.finditer(content):
        ref = m.group(1)
        candidate = REPO_ROOT / ref
        if not candidate.exists():
            stale_refs.append((str(md_file.relative_to(REPO_ROOT)), ref))


def main():
    # Find all markdown files under docs/ (excluding history which is preserved as-is)
    md_files = []
    for root, dirs, files in os.walk(DOCS_ROOT):
        # Skip states directories (TLA+ model state traces)
        if 'states' in dirs:
            dirs.remove('states')
        for f in files:
            if f.endswith('.md'):
                md_files.append(Path(root) / f)

    # Also check root docs
    for f in ['README.md', 'README.zh-CN.md', 'AGENTS.md']:
        p = REPO_ROOT / f
        if p.exists():
            md_files.append(p)

    for md_file in md_files:
        check_markdown_links(md_file)
        check_bare_refs(md_file)

    # Deduplicate
    broken_links_unique = sorted(set(broken_links))
    stale_refs_unique = sorted(set(stale_refs))

    print("=" * 70)
    print("SLUICE DOCUMENTATION LINK CHECKER")
    print("=" * 70)
    print(f"Checked {len(md_files)} Markdown files")
    print(f"Broken Markdown links: {len(broken_links_unique)}")
    print(f"Stale moved-path references: {len(stale_refs_unique)}")
    print()

    if broken_links_unique:
        print("--- BROKEN MARKDOWN LINKS ---")
        for f, link, target in broken_links_unique[:50]:
            print(f"  {f}: {link} -> {target}")
        if len(broken_links_unique) > 50:
            print(f"  ... and {len(broken_links_unique) - 50} more")
        print()

    if stale_refs_unique:
        print("--- STALE MOVED-PATH REFERENCES ---")
        for f, ref in stale_refs_unique[:50]:
            print(f"  {f}: {ref}")
        if len(stale_refs_unique) > 50:
            print(f"  ... and {len(stale_refs_unique) - 50} more")
        print()

    return 0 if not broken_links_unique else 1


if __name__ == '__main__':
    sys.exit(main())
