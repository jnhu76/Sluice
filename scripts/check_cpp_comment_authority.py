#!/usr/bin/env python3
"""Narrow CI guard against pseudo-authority patterns in C/C++ comments.

The scanner detects obvious pseudo-authority patterns. It does not decide
whether a C++ comment is semantically appropriate: human review remains
responsible for subtle semantic narration, platform prose, misleading
compression, and locally false but regex-clean comments.

There are no comment-count thresholds. Adding comments is never flagged by
itself; only the high-confidence patterns below are.

Scanned extensions: .h .hpp .hh .c .cpp .cc. Excluded paths are build or
generated outputs only (.git, build, .xmake, .lake, node_modules); no
maintained source directory is excluded.
"""

import argparse
import re
import sys
from pathlib import Path

RULES = [
    ("requirement-id",
     r"\b(?:SEM|ERR|REQ|LIFE|HANDLE|VERIFY|BOUND|SHUT|INV)-\d+\b(?![\w-])"),
    ("issue-pr-reference",
     r"(?i)\b(?:issue|pr|pull request|follow-up|followup|owned by)\s*#\s*\d+\b"),
    ("authority-claim",
     r"(?i)\b(?:canonical authority|source of truth|this proves|this closes|"
     r"this satisfies|this pins|this guarantees|ledger-recorded|"
     r"spec-mandated|the authoritative path|this path owns)\b"),
    ("stage-narration",
     r"\b(?:Corrective-?\d+|V1-[A-Z]\d+)(?![\w-])|\bPhase\s+[A-D]\b|"
     r"(?i:\b(?:stage|slice|phase)\s+(?:A1|A2|B1))\b"),
]

COMPILED_RULES = [(name, re.compile(pattern)) for name, pattern in RULES]

CPP_SUFFIXES = {".h", ".hpp", ".hh", ".c", ".cpp", ".cc"}
EXCLUDED_DIRS = {".git", "build", ".xmake", ".lake", "node_modules"}

FOOTER = (
    "C++ comments may document local hazards, invariants, or mechanism\n"
    "constraints, but must not restate repository semantic authority.\n"
    "See AGENTS.md."
)


def comments(text):
    """Yield (start_line, comment_text) for every // and /* */ comment,
    ignoring comment-like content inside string and character literals."""
    i, n, line = 0, len(text), 1
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
        elif c == "\\" and i + 1 < n and text[i + 1] == "\n":
            line += 1
            i += 2
        elif c == "/" and i + 1 < n and text[i + 1] == "/":
            start, j = line, i + 2
            while j < n and text[j] != "\n":
                if text[j] == "\\" and j + 1 < n and text[j + 1] == "\n":
                    line += 1
                    j += 2
                else:
                    j += 1
            yield start, text[i:j]
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            start, j = line, i + 2
            while j + 1 < n and not (text[j] == "*" and text[j + 1] == "/"):
                if text[j] == "\n":
                    line += 1
                j += 1
            end = min(j + 2, n)
            yield start, text[i:end]
            i = end
        elif c == "'" and i > 0 and text[i - 1].isdigit():
            # C++14 digit separator (1'000), not a character literal
            i += 1
        elif c == '"' or c == "'":
            line0, closed = line, False
            j = i + 1
            while j < n:
                if text[j] == "\\" and j + 1 < n:
                    if text[j + 1] == "\n":
                        line += 1
                    j += 2
                elif text[j] == "\n":
                    break
                elif text[j] == c:
                    j += 1
                    closed = True
                    break
                else:
                    j += 1
            if closed:
                i = j
            else:
                # Not a literal (e.g. an apostrophe in preprocessor prose):
                # treat the quote as plain text instead of skipping to EOF.
                line = line0
                i += 1
        elif c == "R" and i + 1 < n and text[i + 1] == '"':
            delim_end = text.find("(", i + 2)
            delim = text[i + 2:delim_end] if delim_end != -1 else ""
            closer = ")" + delim + '"'
            j = delim_end + 1 if delim_end != -1 else i + 2
            while j < n and not text.startswith(closer, j):
                if text[j] == "\n":
                    line += 1
                j += 1
            i = j + len(closer) if j < n else n
        else:
            i += 1


def scan_source(text):
    """Return [(line, rule, excerpt)] for one file's text, in line order."""
    findings = []
    for start, body in comments(text):
        for rule, pattern in COMPILED_RULES:
            for m in pattern.finditer(body):
                phys = start + body[: m.start()].count("\n")
                line_start = body.rfind("\n", 0, m.start()) + 1
                line_end = body.find("\n", m.start())
                if line_end == -1:
                    line_end = len(body)
                excerpt = body[line_start:line_end].strip()
                findings.append((phys, rule, excerpt))
    findings.sort(key=lambda f: (f[0], f[1]))
    return findings


def scan_path(path):
    return scan_source(Path(path).read_text(encoding="utf-8", errors="replace"))


def cpp_files(root):
    root = Path(root)
    for p in sorted(root.rglob("*")):
        if p.suffix not in CPP_SUFFIXES or not p.is_file():
            continue
        if any(part in EXCLUDED_DIRS for part in p.relative_to(root).parts):
            continue
        yield p


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        epilog=("Detects obvious pseudo-authority patterns only; it is not a "
                "semantic-completeness checker."),
    )
    parser.add_argument("root", nargs="?", default=".",
                        help="repository root to scan (default: .)")
    args = parser.parse_args(argv)

    failed = False
    for path in cpp_files(args.root):
        for line, rule, excerpt in scan_path(path):
            failed = True
            print(f"error: pseudo-authority pattern in C++ comment")
            print(f"{path}:{line}")
            print(f"rule: {rule}")
            print(f'comment: "{excerpt}"')
            print()
    if failed:
        print(FOOTER)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
