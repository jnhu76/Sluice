#!/usr/bin/env python3
"""Draft a Keep a Changelog release section from commit history.

Drafting (not dumping): the output is a curated-for-humans starting point.
Keep a Changelog entries are curated, not auto-categorized commit dumps, so
the generated text is a reviewable draft: merge duplicates, reword, and
commit it into CHANGELOG.md.

Usage:
  python3 scripts/changelog/generate.py \
      --base <prev-tag-or-ref> [--head <ref>] \
      [--version X.Y.Z] [--date YYYY-MM-DD] [--write [CHANGELOG_PATH]]

Behavior:
  * Reads commit subjects/bodies in the range base..head from the current
    git work tree (head defaults to HEAD).
  * Maps Conventional-Commits prefixes to Keep a Changelog categories:
        feat                -> Added
        fix                 -> Fixed
        perf                -> Changed (performance)
        docs                -> Changed (documentation)
        refactor|chore|build|ci|style|revert -> Changed (maintenance)
        test                -> Tests (repository convention)
  * Breaking changes ("!" after the prefix, or "BREAKING CHANGE:" in the
    message body) are folded into Changed with a "**Breaking:** " lead-in.
  * Prints "## [Unreleased]" (no --version) or "## [X.Y.Z] - date".
  * --write inserts the section into CHANGELOG.md between the Unreleased
    block and the first version heading; it refuses to overwrite an
    existing heading with the same version. Without --write the draft goes
    to stdout.

Conventions:
  Conventional Commits: https://www.conventionalcommits.org/en/v1.0.0/
  Keep a Changelog:     https://keepachangelog.com/en/2.0.0/

--self-test runs the parsing unit tests (no git required).
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent

CATEGORY_ORDER = [
    "Added",
    "Changed",
    "Deprecated",
    "Removed",
    "Fixed",
    "Security",
    "Tests",
]

# Conventional-Commits prefix -> Keep a Changelog category.
PREFIX_TO_CATEGORY = {
    "feat": "Added",
    "fix": "Fixed",
    "perf": "Changed",
    "docs": "Changed",
    "refactor": "Changed",
    "chore": "Changed",
    "build": "Changed",
    "ci": "Changed",
    "style": "Changed",
    "revert": "Changed",
    "test": "Tests",
}

# ^type(scope)?(!)?: subject     (scope is optional and dropped in the draft)
CONVENTIONAL_RE = re.compile(
    r"^(feat|fix|docs|chore|perf|refactor|test|build|ci|style|revert)"
    r"(\([\w./-]+\))?(!)?:\s+(.+)$"
)
BREAKING_BODY_RE = re.compile(r"BREAKING\s+CHANGE:")
_TRAILING_PUNCT_RE = re.compile(r"[.!?\s]+$")


class Commit:
    def __init__(self, sha: str, subject: str, body: str) -> None:
        self.sha = sha
        self.subject = subject
        self.body = body

    def parse(self) -> tuple[str, str, bool]:
        """Return (category, entry_text, breaking)."""
        m = CONVENTIONAL_RE.match(self.subject)
        if not m:
            kind, scope, bang, text = None, None, None, self.subject
            category, breaking = "Changed", False
        else:
            kind, scope, bang, text = m.groups()
            category = PREFIX_TO_CATEGORY.get(kind, "Changed")
            breaking = bool(bang) or bool(BREAKING_BODY_RE.search(self.body))
        text = _TRAILING_PUNCT_RE.sub("", text)
        if breaking:
            category = "Changed"
            text = f"**Breaking:** {text}"
        return category, text, breaking


def parse_commit_message(subject: str, body: str = "") -> tuple[str, str, bool]:
    return Commit("", subject, body).parse()


def _git_log(base: str, head: str) -> list[Commit]:
    """Read commits in base..head.

    git joins commit records with a newline, and commit bodies may contain
    newlines, so each record is terminated with the ASCII record-separator
    byte (\\x1e) and fields within a record use the unit-separator byte
    (\\x1f). Commit messages must not contain either byte.
    """
    sep = "\x1f"
    rec_sep = "\x1e"
    fmt = "%h" + sep + "%s" + sep + "%b" + rec_sep
    out = subprocess.run(
        ["git", "log", f"--pretty=format:{fmt}", f"{base}..{head}"],
        cwd=REPO_ROOT,
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    commits: list[Commit] = []
    for record in out.split(rec_sep):
        fields = record.split(sep)
        if len(fields) < 3 or not fields[0]:
            continue
        sha, subject, body = fields[0], fields[1], fields[2]
        commits.append(Commit(sha, subject, body))
    return commits


def render_section(
    commits: list[Commit], version: str | None, date: str | None
) -> str:
    """Render the markdown draft; entries are grouped by category."""
    grouped: dict[str, list[str]] = {c: [] for c in CATEGORY_ORDER}
    seen: set[str] = set()
    for c in commits:
        category, text, _ = c.parse()
        if text in seen:
            continue
        seen.add(text)
        grouped[category].append(text)

    heading = "## [Unreleased]"
    if version:
        heading = f"## [{version}]"
        if date:
            heading += f" - {date}"
    lines = [heading, ""]
    for category in CATEGORY_ORDER:
        if not grouped[category]:
            continue
        lines.append(f"### {category}")
        lines.append("")
        for text in grouped[category]:
            lines.append(f"- {text}")
        lines.append("")
    if len(lines) == 2:  # nothing but the heading
        return heading + "\n\n(nothing parsed in range)\n"
    return "\n".join(lines)


def _find_unreleased_insert_point(content: str) -> int | None:
    """Line index just after the [Unreleased] block, before the next heading.

    Returns the line index after the last line of the Unreleased block
    (heading line included). None if [Unreleased] is absent.
    """
    headings = [
        (i, line) for i, line in enumerate(content.splitlines())
        if line.startswith("## ")
    ]
    for i, line in headings:
        if line.startswith("## [Unreleased]"):
            for j, other in headings:
                if j > i:
                    return j
            return len(content.splitlines())
    return None


def write_section(path: Path, section: str, version: str | None) -> bool:
    """Insert the draft into CHANGELOG.md; fail-closed on collision."""
    content = path.read_text()
    version_re = re.compile(rf"^## \[{re.escape(version or 'Unreleased')}\]")
    if version and re.search(
        rf"^## \[{re.escape(version)}\]", content, re.M
    ):
        print(f"error: CHANGELOG.md already has a [{version}] section")
        return False
    insert = _find_unreleased_insert_point(content)
    if insert is None:
        print("error: CHANGELOG.md has no '## [Unreleased]' section")
        return False
    lines = content.splitlines()
    lines.insert(insert, section)
    path.write_text("\n".join(lines) + "\n")
    return True  # (unused version_re binding is intentional for clarity)


def _self_test() -> int:
    cases = [
        # (subject, body, expected_category, expected_text, expected_breaking)
        ("feat: add sluice-tail", "", "Added", "add sluice-tail", False),
        ("fix(core): retry EINTR", "", "Fixed", "retry EINTR", False),
        ("fix!: drop legacy API", "", "Changed", "**Breaking:** drop legacy API", True),
        (
            "feat: new runtime",
            "BREAKING CHANGE: config format",
            "Changed",
            "**Breaking:** new runtime",
            True,
        ),
        ("perf: reduce wake cost", "", "Changed", "reduce wake cost", False),
        ("docs: clarify ownership", "", "Changed", "clarify ownership", False),
        ("test: add race case", "", "Tests", "add race case", False),
        ("whatever free text", "", "Changed", "whatever free text", False),
        (
            "fix(select)!: change winner",
            "",
            "Changed",
            "**Breaking:** change winner",
            True,
        ),
        ("feat(scope): keep scope text", "", "Added", "keep scope text", False),
    ]
    failures = 0
    for subject, body, cat, text, breaking in cases:
        got_cat, got_text, got_breaking = parse_commit_message(subject, body)
        if (got_cat, got_text, got_breaking) != (cat, text, breaking):
            failures += 1
            print(f"SELF-TEST FAIL: {subject!r}\n"
                  f"  expected ({cat!r}, {text!r}, {breaking})\n"
                  f"  got      ({got_cat!r}, {got_text!r}, {got_breaking})")
    if failures:
        print(f"self-test: {failures} failure(s)")
        return 1
    print("self-test: all parsing cases passed")
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--base", default=None, help="range start (e.g. v0.0.1)")
    parser.add_argument("--head", default="HEAD", help="range end (default HEAD)")
    parser.add_argument("--version", default=None, help="X.Y.Z for the heading")
    parser.add_argument("--date", default=None, help="YYYY-MM-DD for the heading")
    parser.add_argument(
        "--write",
        nargs="?",
        const="CHANGELOG.md",
        default=None,
        metavar="PATH",
        help="insert the draft into CHANGELOG.md instead of stdout",
    )
    parser.add_argument(
        "--self-test", action="store_true", help="run parsing unit tests"
    )
    args = parser.parse_args(argv)

    if args.self_test:
        return _self_test()

    if not args.base:
        parser.error("--base is required for drafting (e.g. --base v0.0.1)")
    commits = _git_log(args.base, args.head)
    section = render_section(commits, args.version, args.date)

    if args.write:
        path = Path(args.write)
        if not path.is_absolute():
            path = REPO_ROOT / path
        if write_section(path, section, args.version):
            print(f"wrote draft section to {path}")
            return 0
        return 1
    print(section)
    return 0


if __name__ == "__main__":
    sys.exit(main())