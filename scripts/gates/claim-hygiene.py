#!/usr/bin/env python3
"""claim-hygiene.py — changed-lines overclaim guard for verification claims (#200).

Machine enforcement for the #163 §12 claim vocabulary rule:

    None of the claim classes alone means "the C++ implementation is
    formally verified."

AGENTS.md §21 and docs/verification/formal-models.md state the prohibition in
prose; this gate fails when NEW text (added lines in the scanned diff) makes
an affirmative claim of the overclaim class about the implementation. Scope:
Markdown files under docs/ and spec/, plus the root README.md and AGENTS.md.

Fail-closed under-allowing exceptions (an escape must be provable, never a
pattern-match coincidence):

  1. NEGATION — a negation token on the same line (`never`, `not`, `no`,
     `cannot`, `must not`, `does not`, `doesn't`, `neither`, `nor`, `rather
     than`, `instead of`). A negated mention is policy/prohibition text.
  2. QUOTED — the matched phrase occurrence is wrapped in quotes or
     backticks (mentioned-as-a-phrase, e.g. vocabulary definitions).
  3. ALLOWLIST — a site-level entry `path-glob | substring | reason` in
     scripts/gates/claim-hygiene.allowlist; BOTH the path must match the
     glob AND the substring must occur in the added line. Legitimate
     affirmative statements are reviewed entries, never silent passes.

Grandfathering: changed-lines only — existing text is not scanned (the
2026-08-23 audit found zero violations, so the gate starts from zero).

Usage:
  claim-hygiene.py --self-test
  claim-hygiene.py [-- <git-diff-args>...]   # e.g. -- range1 range2

Exit code 0 iff every added line in scope is clean (or an exception applies).
"""

from __future__ import annotations

import fnmatch
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
ALLOWLIST = Path(__file__).resolve().parent / "claim-hygiene.allowlist"

SCOPE_SUFFIX = ".md"
SCOPE_ROOTS = ("docs/", "spec/")
SCOPE_EXACT = {"README.md", "AGENTS.md"}

# Affirmative overclaim class (case-insensitive). Each pattern names the
# implementation/C++/code as the object of formal verification/proof.
OVERCLAIM_PATTERNS = [
    re.compile(r"formally\s+verif\w*\s+(the\s+)?(implementation|c\+\+|code)",
               re.IGNORECASE),
    re.compile(r"(implementation|c\+\+|code)\s+is\s+formally\s+verif\w*",
               re.IGNORECASE),
    re.compile(r"formal(ly)?\s+proves?\s+(the\s+)?(implementation|c\+\+|code)",
               re.IGNORECASE),
    re.compile(r"proves?\s+the\s+(implementation|c\+\+)\s+"
               r"(is\s+)?(correct|verified|safe|bug[- ]free)",
               re.IGNORECASE),
]

# Negation tokens (line-level, fail-closed under-allowing: ANY of these
# anywhere on the line makes the line policy text, not an affirmative claim).
NEGATION_RE = re.compile(
    r"\b(never|not|no|cannot|can't|must\s+not|does\s+not|doesn't|isn't|"
    r"neither|nor|rather\s+than|instead\s+of|without\s+claiming)\b",
    re.IGNORECASE)

# Quote/backtick pair immediately around the matched occurrence.
WRAP_CHARS = "\"'`"


def load_allowlist(path: Path) -> list[tuple[str, str, str]]:
    entries: list[tuple[str, str, str]] = []
    if not path.is_file():
        return entries
    for lineno, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = [p.strip() for p in line.split("|")]
        if len(parts) != 3 or not parts[0] or not parts[1] or not parts[2]:
            print(f"claim-hygiene: malformed allowlist entry at "
                  f"{path.name}:{lineno}: {raw!r} "
                  f"(expected 'path-glob | substring | reason')",
                  file=sys.stderr)
            sys.exit(2)
        entries.append((parts[0], parts[1], parts[2]))
    return entries


def in_scope(path: str) -> bool:
    if path in SCOPE_EXACT:
        return True
    if not path.endswith(SCOPE_SUFFIX):
        return False
    return any(path.startswith(root) for root in SCOPE_ROOTS)


def overclaim_match(line: str):
    """Return (pattern, match) for the first affirmative overclaim on the
    line, or None."""
    for pat in OVERCLAIM_PATTERNS:
        m = pat.search(line)
        if m:
            return pat, m
    return None


def is_quoted(line: str, start: int, end: int) -> bool:
    """True when the occurrence is wrapped in matching quote characters."""
    if start == 0 or end >= len(line):
        return False
    return (line[start - 1] in WRAP_CHARS and line[end] in WRAP_CHARS
            and line[start - 1] == line[end])


def allowlisted(entries, path: str, line: str) -> bool:
    for glob, substr, _reason in entries:
        if fnmatch.fnmatch(path, glob) and substr in line:
            return True
    return False


def check_added_line(entries, path: str, line: str):
    """Return an error string for one added line, or None."""
    hit = overclaim_match(line)
    if hit is None:
        return None
    _pat, m = hit
    if NEGATION_RE.search(line):
        return None  # policy/negated mention
    if is_quoted(line, m.start(), m.end()):
        return None  # mentioned-as-a-phrase
    if allowlisted(entries, path, line):
        return None
    return (f"{path}: added line makes an affirmative overclaim of the "
            f"#163 §12 class: {line.strip()!r} — name the evidence class "
            f"(MODEL / TRACE-CONFORMANT / MEMORY-MODEL-CHECKED / ...) "
            f"instead; see scripts/gates/claim-hygiene.py --help")


def parse_diff(diff_text: str):
    """Minimal unified-diff state machine: yields (path, '+', content) for
    added lines. Paths come ONLY from ---/+++ header pairs, so a content line
    beginning with '+' (or '+++') inside a hunk is content, not a header."""
    old_path = new_path = None
    for raw in diff_text.splitlines():
        if raw.startswith("--- "):
            old_path = raw[4:].split("\t")[0].strip()
            continue
        if raw.startswith("+++ "):
            new_path = raw[4:].split("\t")[0].strip()
            continue
        if raw.startswith("+"):
            path = (new_path or old_path or "")
            path = path[2:] if path.startswith("b/") else path
            yield path, raw[1:]
        # '-'/'@@'/context lines carry no additions


def scan_diff(diff_text: str, entries) -> list[str]:
    errors = []
    for path, content in parse_diff(diff_text):
        if not in_scope(path):
            continue
        err = check_added_line(entries, path, content)
        if err:
            errors.append(err)
    return errors


def run_diff(diff_args: list[str]) -> str:
    cmd = ["git", "-C", str(REPO), "diff", "--no-color", *diff_args]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        print(f"claim-hygiene: git diff failed: {proc.stderr.strip()}",
              file=sys.stderr)
        sys.exit(2)
    return proc.stdout


# ---------------------------------------------------------------------------
# self-test: plant each violation shape and each escape shape
# ---------------------------------------------------------------------------

SAMPLE_DIFF = """\
diff --git a/docs/x.md b/docs/x.md
--- a/docs/x.md
+++ b/docs/x.md
@@ -1,2 +1,4 @@
 context line
+clean documentation line with no claim
diff --git a/src/a.cpp b/src/a.cpp
--- a/src/a.cpp
+++ b/src/a.cpp
@@ -1,1 +1,2 @@
+the model formally verifies the implementation
"""


def self_test() -> int:
    entries = load_allowlist(ALLOWLIST)
    failures = []

    def expect_fire(name, diff, path_hint="docs/x.md"):
        errs = scan_diff(diff, entries)
        if not any(path_hint in e for e in errs):
            failures.append(f"[{name}] detector did not fire; errors={errs}")

    def expect_clean(name, diff):
        errs = scan_diff(diff, entries)
        if errs:
            failures.append(f"[{name}] false positive; errors={errs}")

    def md(added):
        return (f"diff --git a/docs/x.md b/docs/x.md\n"
                f"--- a/docs/x.md\n+++ b/docs/x.md\n"
                f"@@ -1 +1,2 @@\n context\n+{added}\n")

    # --- detectors fire ---
    expect_fire("affirmative claim", md("The model formally verifies the implementation."))
    expect_fire("C++ variant", md("TLC now formally verifies C++."))
    expect_fire("is-verified inversion", md("the implementation is formally verified by the suite"))
    expect_fire("formally proves", md("The spec formally proves the code."))
    expect_fire("proves-correct", md("the model proves the implementation correct"))
    # .cpp paths are out of scope (the SAMPLE_DIFF's second file must be ignored)
    expect_clean("out-of-scope path", SAMPLE_DIFF)

    # --- escapes (fail-closed under-allowing) ---
    expect_clean("negation", md("Never claim the model formally verifies the implementation."))
    expect_clean("negation-does-not", md("This does not mean the C++ is formally verified."))
    expect_clean("negation-cannot", md("A model pass cannot formally verify the implementation."))
    expect_clean("quoted phrase", md("""The phrase "formally verified implementation" is prohibited."""))
    expect_clean("backtick phrase", md("never say `formally verifies the implementation`"))
    expect_clean("plain clean line", md("The suite passes its liveness property under weak fairness."))
    expect_clean("removed line grandfathered",
                 "diff --git a/docs/x.md b/docs/x.md\n--- a/docs/x.md\n"
                 "+++ b/docs/x.md\n@@ -1,2 +1 @@\n-the model formally verifies the implementation\n context\n")
    expect_clean("+++ header trap",
                 "diff --git a/docs/x.md b/docs/x.md\n--- a/docs/x.md\n"
                 "+++ b/docs/x.md\n@@ -1 +1,2 @@\n context\n"
                 "+++not-a-header content line\n")
    # spec/ markdown is in scope; README.md/AGENTS.md root files are in scope
    expect_fire("spec md scope",
                md("the model formally verifies the implementation")
                .replace("docs/x.md", "spec/tla/x/README.md"),
                path_hint="spec/tla/x/README.md")
    # JSON is out of scope
    expect_clean("json out of scope",
                 "diff --git a/docs/x.json b/docs/x.json\n--- a/docs/x.json\n"
                 "+++ b/docs/x.json\n@@ -1 +1,2 @@\n context\n"
                 "+the model formally verifies the implementation\n")

    # --- allowlist: a reviewed site entry allows an affirmative statement ---
    entries_with_site = entries + [("docs/x.md",
                                    "AFFIRMATIVE-SITE-MARKER",
                                    "self-test site entry")]
    errs = scan_diff(md("AFFIRMATIVE-SITE-MARKER: the model formally verifies "
                        "the implementation."), entries_with_site)
    if errs:
        failures.append(f"[allowlist site] false positive; errors={errs}")
    # ...but the SAME file WITHOUT the marker substring still fires (site
    # entries are per-line-substring, never per-file).
    errs = scan_diff(md("the model formally verifies the implementation."),
                     entries_with_site)
    if not errs:
        failures.append("[allowlist site] whole-file escape must not happen")

    # --- CLI arg shape: a bare range (how pre-push.sh/CI invoke this gate)
    #     and a `--`-prefixed range must resolve to the same diff args. ---
    for argv, want in ([["a..b"], ["a..b"]],
                       [["--", "a..b"], ["a..b"]],
                       [["ref1", "ref2"], ["ref1", "ref2"]],
                       [["--", "ref1", "ref2"], ["ref1", "ref2"]]):
        got = _parse_diff_args(argv)
        if got != want:
            failures.append(f"[cli shape] _parse_diff_args({argv!r}) = {got!r}, want {want!r}")

    if failures:
        for f in failures:
            print("self-test FAIL", f)
        return 1
    print("claim-hygiene self-test: all detectors fire, all escapes hold, "
          "site entries are per-line-substring")
    return 0


def _parse_diff_args(argv: list) -> list:
    # Optional `--` separator before git-diff args — same CLI convention as
    # assert-hygiene.py, so pre-push.sh can invoke both with a bare range
    # (CI's `claim-hygiene.py <base>..<head>` has no separator).
    if argv and argv[0] == "--":
        return argv[1:]
    return list(argv)


def main() -> int:
    args = sys.argv[1:]
    if args and args[0] == "--self-test":
        return self_test()
    diff_args = _parse_diff_args(args)
    if not diff_args:
        diff_args = ["HEAD"]  # manual mode: staged + working tree vs HEAD
        print("    (no ranges given; checking staged + working tree vs HEAD)")
    entries = load_allowlist(ALLOWLIST)
    errors = scan_diff(run_diff(diff_args), entries)
    if errors:
        print(f"claim-hygiene: FAIL ({len(errors)} overclaim addition(s))")
        for e in errors:
            print("  ", e)
        print("reproduce: python3 scripts/gates/claim-hygiene.py -- "
              + " ".join(diff_args))
        return 1
    print("claim-hygiene: OK (no overclaim additions in changed lines)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
