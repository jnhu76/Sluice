#!/usr/bin/env python3
"""assert-hygiene.py — changed-lines assert-family gate (issue #144 / #135).

Policy authority: AGENTS.md §3.8 and docs/architecture/failure-model.md.
`NDEBUG` is not semantic authority: a bare assert() vanishes in Release and in
every downstream build that defines NDEBUG, so it must never be the sole
enforcement for correctness, liveness, ownership, lifetime, request lifecycle,
Completion, Scheduler, backend, or resource-accounting invariants.

WHAT IT CHECKS (changed lines only — existing sites are grandfathered):

  ADDED lines of a git diff, restricted to include/ and src/, must not
  introduce the C assert family:
    - assert( calls            (static_assert is exempt: compile-time)
    - #include <cassert>
    - #include <assert.h> / "assert.h"

ALLOWED shapes (AGENTS.md §3.8 / failure-model.md §5):
  1. internal-testing preconditions — lines INSIDE a preprocessor region that
     is provably active only when SLUICE_ASYNC_INTERNAL_TESTING is defined
     are auto-allowed. Guard detection is deliberately under-allowing: only
     #ifdef MACRO, #if defined(MACRO), and &&-conjunctions in which the macro
     appears positively (and never negated) qualify. #ifndef, #if !defined,
     any ||, and any unprovable form are treated as PRODUCTION code — an
     auto-allow must rest on a proof that the branch implies the macro, never
     on a pattern match that happens to mention it. The #else branch of such
     a guard is production and is NOT auto-allowed; #elif is auto-allowed
     only when its own expression proves the macro.
  2. Completion L9 pattern / pure diagnostics — a SITE-LEVEL entry in
     scripts/gates/assert-hygiene.allowlist (path glob + source-line
     substring + category + reason). Whole-file exemption is deliberately
     unsupported: a new, unregistered assert-family line in an allowlisted
     file still fails.

The diff reader is a minimal unified-diff state machine, not a line filter:
a file path is taken ONLY from a `--- old` / `+++ new` header pair, and once
inside a hunk EVERY `+`-prefixed line is added content — so an added source
line `++iterator;` (rendered `+++iterator;`) is content, not a header. New
files, deletions (`+++ /dev/null`), renames (b-side path), and binary pairs
are handled; an unparseable shape raises and fails the gate instead of
misattributing lines to the previous file or silently skipping one.

USAGE
    python3 scripts/gates/assert-hygiene.py            # staged + working tree vs HEAD
    python3 scripts/gates/assert-hygiene.py <args>...  # args passed to `git diff`
                                                        # (e.g. <base>..<head>)
    python3 scripts/gates/assert-hygiene.py --self-test

Fail-closed: exits non-zero listing every violation with file:line and the
reproduction command; a malformed allowlist or diff is itself a failure.
--self-test plants each violation shape (including the adversarial guard
polarities, site-exemption escape, and diff-parser traps) and requires every
detector to fire with zero false positives.
"""

import fnmatch
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
ALLOWLIST = REPO / "scripts" / "gates" / "assert-hygiene.allowlist"

SCANNED_PREFIXES = ("include/", "src/")

# assert( not preceded by a word character: static_assert, SLUICE_ASSERT, etc.
# are NOT matches (their preceding '_'/'word' char blocks the lookbehind).
ASSERT_CALL_RE = re.compile(r"(?<!\w)assert\s*\(")
ASSERT_INCLUDE_RE = re.compile(r'#\s*include\s*[<"](cassert|assert\.h)[>"]')
# Leading C++ comment lines never carry executable asserts.
COMMENT_LINE_RE = re.compile(r"^\s*(//|/\*|\*)")

HUNK_RE = re.compile(r"^@@ -\d+(?:,\d+)? \+(\d+)(?:,\d+)? @@")

LEGAL_CATEGORIES = ("L9", "diagnostic", "testing")


class AllowlistError(Exception):
    """Malformed allowlist — fail the gate rather than weaken it."""


def load_allowlist(path: Path = ALLOWLIST):
    """SITE-LEVEL entries: list of (path-glob, line-substring, category, reason).

    Entry line format — four '|' separated fields; full-line '#' comments only
    (a trailing '#' cannot be a comment marker because a registered substring
    may legitimately contain '#', e.g. `#include <cassert>`):

        <path-glob> | <source-line-substring> | <category> | <reason>

    Raises AllowlistError on a malformed line, an unknown category, or an
    empty reason: a broken allowlist must never degrade into a silent pass.
    """
    entries = []
    if not path.is_file():
        return entries
    for n, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = [p.strip() for p in line.split("|")]
        if len(parts) != 4:
            raise AllowlistError(
                f"{path.name}:{n}: expected 4 fields "
                f"'<glob> | <substring> | <category> | <reason>', got {len(parts)}"
            )
        glob, substr, category, reason = parts
        if not glob or not substr:
            raise AllowlistError(
                f"{path.name}:{n}: path glob and source-line substring must be non-empty"
            )
        if category not in LEGAL_CATEGORIES:
            raise AllowlistError(
                f"{path.name}:{n}: category {category!r} not in {LEGAL_CATEGORIES}"
            )
        if not reason:
            raise AllowlistError(f"{path.name}:{n}: reason must be non-empty")
        entries.append((glob, substr, category, reason))
    return entries


def site_exempt(path: str, text: str, entries) -> bool:
    """True iff (path, added-line text) matches a registered site entry —
    the exemption is per site (path glob AND line substring), never per file."""
    stripped = text.strip()
    return any(
        fnmatch.fnmatchcase(path, g) and s in stripped for g, s, _cat, _reason in entries
    )


# --- preprocessor guard tracking -------------------------------------------

PP_LINE_RE = re.compile(r"^\s*#\s*(ifdef|ifndef|if|elif|else|endif)\b(.*)$")
TESTING_MACRO = "SLUICE_ASYNC_INTERNAL_TESTING"

DEFINED_TESTING_RE = re.compile(rf"^defined\s*\(\s*{TESTING_MACRO}\s*\)$")
NEG_DEFINED_TESTING_RE = re.compile(
    rf"^(?:!\s*defined|not\s+defined)\s*\(\s*{TESTING_MACRO}\s*\)$"
)


def _strip_parens(token: str) -> str:
    while token.startswith("(") and token.endswith(")"):
        token = token[1:-1].strip()
    return token


def _active_branch_is_testing(directive: str, expr: str) -> bool:
    """True only when the ACTIVE branch of `#<directive> <expr>` provably
    implies TESTING_MACRO is defined.

    Recognized provable forms:
      #ifdef SLUICE_ASYNC_INTERNAL_TESTING
      #if defined(SLUICE_ASYNC_INTERNAL_TESTING)
      #if defined(X) && defined(SLUICE_ASYNC_INTERNAL_TESTING)   (any &&-count)
      #if defined(SLUICE_ASYNC_INTERNAL_TESTING) && !defined(X)
      #elif <any of the above expressions>

    Everything else — #ifndef, #if !defined(...), any expression containing
    `||`, plain macros, arithmetic, unknown syntax — yields False. That is the
    safe direction: an unrecognized form is treated as production code and
    needs an explicit reviewed allowlist entry (fail closed / under-allow).
    """
    expr = expr.split("//")[0].strip()
    if directive == "ifdef":
        return expr == TESTING_MACRO
    if directive == "ifndef":
        return False  # active branch implies the macro is NOT defined
    if directive == "else":
        return False  # #else of any guard: production
    # `if` / `elif`: flat && expression analysis.
    if not expr or "||" in expr:
        # A disjunction can never imply the macro is defined; an empty or
        # arithmetic expression says nothing about it.
        return False
    conjuncts = [_strip_parens(c.strip()) for c in expr.split("&&")]
    if any(NEG_DEFINED_TESTING_RE.match(c) for c in conjuncts):
        return False  # active implies the macro is NOT defined
    return any(DEFINED_TESTING_RE.match(c) for c in conjuncts)


def testing_guard_lines(text: str):
    """1-based line numbers that are inside a region provably active only
    under SLUICE_ASYNC_INTERNAL_TESTING (see _active_branch_is_testing).

    Nested guards compose: a testing guard inside a non-testing guard still
    marks its region. `#else` flips any level to production; `#elif` is
    re-classified by its own expression (auto-allowed only when provable).
    """
    stack = []  # per open #if level: True = this level selects testing code
    inside = set()
    n = 0
    for line in text.splitlines():
        n += 1
        m = PP_LINE_RE.match(line)
        if m:
            directive, rest = m.group(1), m.group(2)
            if directive == "endif":
                if stack:
                    stack.pop()
            elif directive == "else":
                if stack:
                    stack[-1] = False
            elif directive == "elif":
                if stack:
                    stack[-1] = _active_branch_is_testing("if", rest)
            else:  # ifdef / ifndef / if
                stack.append(_active_branch_is_testing(directive, rest))
        if True in stack:
            inside.add(n)
    return inside


# --- diff parsing -----------------------------------------------------------

class DiffShapeError(ValueError):
    """Unparseable unified-diff shape — fail closed instead of guessing."""


def parse_added_lines(diff_text: str):
    """Minimal unified-diff state machine. Yields (path, new_lineno, text)
    for ADDED lines of the NEW side.

    The current path is taken ONLY from a `--- <old>` / `+++ <new>` header
    pair (so an added source line `+++iterator;` inside a hunk is content,
    not a header). Handles new files (`--- /dev/null`), deletions
    (`+++ /dev/null` — no new-side lines), renames (b-side path), quoted
    paths, and binary pairs (`Binary files ... differ`, no hunks). A
    backslash-prefix "No newline at end of file" marker annotates the
    previous line and consumes no line number. Any other shape raises
    DiffShapeError: misattributing lines to the previous file, or silently
    skipping one, is not acceptable.
    """
    HEADER, EXPECT_NEW, BETWEEN, IN_HUNK = range(4)
    state = HEADER
    path = None
    new_lineno = 0
    for line in diff_text.splitlines():
        if line.startswith("diff --git "):
            state, path = HEADER, None
            continue
        if state == HEADER:
            if line.startswith("--- "):
                state = EXPECT_NEW
            elif line.startswith("Binary files "):
                state = BETWEEN  # binary pair: no text hunks follow
            # index / mode / similarity / rename / copy metadata: stay.
            continue
        if state == EXPECT_NEW:
            if not line.startswith("+++"):
                raise DiffShapeError(
                    f"'---' file header not followed by a '+++' header: {line!r}"
                )
            new = line[3:].strip()
            if new.startswith('"') and new.endswith('"') and len(new) >= 2:
                new = new[1:-1]
            new = new.split("\t")[0].strip()  # strip optional timestamp field
            if new == "/dev/null":
                path = None  # deleted file: no new-side lines
            elif new.startswith("b/"):
                path = new[2:]
            else:
                raise DiffShapeError(f"unparseable '+++' file header: {line!r}")
            state = BETWEEN
            continue
        if state == BETWEEN:
            m = HUNK_RE.match(line)
            if m:
                new_lineno = int(m.group(1))
                state = IN_HUNK
            continue
        # IN_HUNK: every '+'-prefixed line here is added CONTENT (a rendered
        # '+++ b/x' inside a hunk is the added line '++ b/x', not a header).
        m = HUNK_RE.match(line)
        if m:
            new_lineno = int(m.group(1))
        elif line.startswith("+"):
            if path is not None:
                yield path, new_lineno, line[1:]
            new_lineno += 1
        elif line.startswith(" "):
            new_lineno += 1
        elif line.startswith("\\"):
            pass  # '\ No newline...' annotates the previous line
        # '-' removed lines consume no new-side line number.


def resolve_new_text(args, path: str):
    """New-side file content for guard detection. Range/commit mode asks git;
    default (no-arg) mode reads the working tree. None = no guard info
    (conservative: only site-level allowlisting can then allow a line)."""
    if not args:
        f = REPO / path
        return f.read_text(encoding="utf-8", errors="replace") if f.is_file() else None
    newrev = None
    if len(args) == 1:
        tok = args[0]
        m = re.fullmatch(r"[0-9a-f]{7,40}\.\.([0-9a-f]{7,40})", tok)
        if m:
            newrev = m.group(1)
        elif re.fullmatch(r"[0-9a-f]{7,40}", tok):
            newrev = tok
    if newrev is None:
        f = REPO / path
        return f.read_text(encoding="utf-8", errors="replace") if f.is_file() else None
    out = subprocess.run(
        ["git", "show", f"{newrev}:{path}"],
        cwd=REPO, capture_output=True, text=True, check=False,
    )
    return out.stdout if out.returncode == 0 else None


def scan(diff_text: str, new_text_of, entries):
    """Return violation strings for one diff. `new_text_of(path)` supplies the
    new-side file content (or None)."""
    per_file_lines = {}
    for path, lineno, text in parse_added_lines(diff_text):
        if not path.startswith(SCANNED_PREFIXES):
            continue
        if ASSERT_CALL_RE.search(text) or ASSERT_INCLUDE_RE.search(text):
            if COMMENT_LINE_RE.match(text):
                continue
            per_file_lines.setdefault(path, []).append((lineno, text))
    violations = []
    for path, hits in sorted(per_file_lines.items()):
        new_text = new_text_of(path)
        guarded = testing_guard_lines(new_text) if new_text else set()
        for lineno, text in hits:
            if lineno in guarded:
                continue
            if site_exempt(path, text, entries):
                continue
            violations.append(
                f"  {path}:{lineno}: {text.strip()}\n"
                f"    new assert-family line in include/src; classify per\n"
                f"    docs/architecture/failure-model.md §5 and register the\n"
                f"    site in scripts/gates/assert-hygiene.allowlist, or use a\n"
                f"    typed result / named fail-fast instead"
            )
    return violations


# --- self-test --------------------------------------------------------------

SELF_DIFF = """\
diff --git a/include/a.hpp b/include/a.hpp
new file mode 100644
--- /dev/null
+++ b/include/a.hpp
@@ -0,0 +1,7 @@
+#pragma once
+assert(bare);
+#include <cassert>
+#include <assert.h>
+#include <cstdint>
+static_assert(sizeof(int) == 4);
+// assert(commented);
diff --git a/src/guard_ok1.cpp b/src/guard_ok1.cpp
new file mode 100644
--- /dev/null
+++ b/src/guard_ok1.cpp
@@ -0,0 +1,3 @@
+#ifdef SLUICE_ASYNC_INTERNAL_TESTING
+assert(ok1_ifdef);
+#endif
diff --git a/src/guard_ok2.cpp b/src/guard_ok2.cpp
new file mode 100644
--- /dev/null
+++ b/src/guard_ok2.cpp
@@ -0,0 +1,3 @@
+#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
+assert(ok2_defined);
+#endif
diff --git a/src/guard_ok3.cpp b/src/guard_ok3.cpp
new file mode 100644
--- /dev/null
+++ b/src/guard_ok3.cpp
@@ -0,0 +1,3 @@
+#if defined(SLUICE_HAS_LIBURING) && defined(SLUICE_ASYNC_INTERNAL_TESTING)
+assert(ok3_and);
+#endif
diff --git a/src/guard_ok4.cpp b/src/guard_ok4.cpp
new file mode 100644
--- /dev/null
+++ b/src/guard_ok4.cpp
@@ -0,0 +1,3 @@
+#if defined(SLUICE_ASYNC_INTERNAL_TESTING) && !defined(SLUICE_OTHER)
+assert(ok4_and_not_other);
+#endif
diff --git a/src/guard_ok5.cpp b/src/guard_ok5.cpp
new file mode 100644
--- /dev/null
+++ b/src/guard_ok5.cpp
@@ -0,0 +1,4 @@
+#if defined(SLUICE_OTHER)
+#elif defined(SLUICE_ASYNC_INTERNAL_TESTING)
+assert(ok5_elif_provable);
+#endif
diff --git a/src/guard_bad1.cpp b/src/guard_bad1.cpp
new file mode 100644
--- /dev/null
+++ b/src/guard_bad1.cpp
@@ -0,0 +1,3 @@
+#ifndef SLUICE_ASYNC_INTERNAL_TESTING
+assert(bad1_ifndef);
+#endif
diff --git a/src/guard_bad2.cpp b/src/guard_bad2.cpp
new file mode 100644
--- /dev/null
+++ b/src/guard_bad2.cpp
@@ -0,0 +1,3 @@
+#if !defined(SLUICE_ASYNC_INTERNAL_TESTING)
+assert(bad2_not_defined);
+#endif
diff --git a/src/guard_bad3.cpp b/src/guard_bad3.cpp
new file mode 100644
--- /dev/null
+++ b/src/guard_bad3.cpp
@@ -0,0 +1,3 @@
+#if defined(SLUICE_ASYNC_INTERNAL_TESTING) || defined(SLUICE_OTHER)
+assert(bad3_or_testing_first);
+#endif
diff --git a/src/guard_bad4.cpp b/src/guard_bad4.cpp
new file mode 100644
--- /dev/null
+++ b/src/guard_bad4.cpp
@@ -0,0 +1,3 @@
+#if defined(SLUICE_OTHER) || defined(SLUICE_ASYNC_INTERNAL_TESTING)
+assert(bad4_or_testing_second);
+#endif
diff --git a/src/guard_bad5.cpp b/src/guard_bad5.cpp
new file mode 100644
--- /dev/null
+++ b/src/guard_bad5.cpp
@@ -0,0 +1,4 @@
+#if defined(SLUICE_OTHER)
+#elif defined(SLUICE_ASYNC_INTERNAL_TESTING) || defined(SLUICE_X)
+assert(bad5_elif_or);
+#endif
diff --git a/include/fake/compl.hpp b/include/fake/compl.hpp
new file mode 100644
--- /dev/null
+++ b/include/fake/compl.hpp
@@ -0,0 +1,2 @@
+assert(false && "registered L9 site");
+assert(unregistered_in_same_file);
diff --git a/src/plusplus.cpp b/src/plusplus.cpp
new file mode 100644
--- /dev/null
+++ b/src/plusplus.cpp
@@ -0,0 +1,2 @@
+++iterator;
+assert(plusplus_found_after_plusplus_line);
diff --git a/src/deleted.cpp b/src/deleted.cpp
deleted file mode 100644
--- a/src/deleted.cpp
+++ /dev/null
@@ -1,2 +0,0 @@
-assert(removed_never_scanned);
-assert(removed2_never_scanned);
diff --git a/src/old_name.cpp b/src/renamed.cpp
similarity index 90%
rename from src/old_name.cpp
rename to src/renamed.cpp
index 1111111..2222222 100644
--- a/src/old_name.cpp
+++ b/src/renamed.cpp
@@ -1,2 +1,3 @@
 int ctx;
+assert(renamed_found_on_new_path);
 int more;
diff --git a/src/bin.dat b/src/bin.dat
index 3333333..4444444 100644
Binary files a/src/bin.dat and b/src/bin.dat differ
diff --git a/include/after_binary.hpp b/include/after_binary.hpp
new file mode 100644
--- /dev/null
+++ b/include/after_binary.hpp
@@ -0,0 +1,1 @@
+assert(after_binary_no_path_bleed);
"""

SELF_TEXTS = {
    "include/a.hpp": "#pragma once\nassert(bare);\n#include <cassert>\n"
    "#include <assert.h>\n#include <cstdint>\n"
    "static_assert(sizeof(int) == 4);\n// assert(commented);\n",
    "src/guard_ok1.cpp": "#ifdef SLUICE_ASYNC_INTERNAL_TESTING\n"
    "assert(ok1_ifdef);\n#endif\n",
    "src/guard_ok2.cpp": "#if defined(SLUICE_ASYNC_INTERNAL_TESTING)\n"
    "assert(ok2_defined);\n#endif\n",
    "src/guard_ok3.cpp": "#if defined(SLUICE_HAS_LIBURING) && "
    "defined(SLUICE_ASYNC_INTERNAL_TESTING)\nassert(ok3_and);\n#endif\n",
    "src/guard_ok4.cpp": "#if defined(SLUICE_ASYNC_INTERNAL_TESTING) && "
    "!defined(SLUICE_OTHER)\nassert(ok4_and_not_other);\n#endif\n",
    "src/guard_ok5.cpp": "#if defined(SLUICE_OTHER)\n"
    "#elif defined(SLUICE_ASYNC_INTERNAL_TESTING)\n"
    "assert(ok5_elif_provable);\n#endif\n",
    "src/guard_bad1.cpp": "#ifndef SLUICE_ASYNC_INTERNAL_TESTING\n"
    "assert(bad1_ifndef);\n#endif\n",
    "src/guard_bad2.cpp": "#if !defined(SLUICE_ASYNC_INTERNAL_TESTING)\n"
    "assert(bad2_not_defined);\n#endif\n",
    "src/guard_bad3.cpp": "#if defined(SLUICE_ASYNC_INTERNAL_TESTING) || "
    "defined(SLUICE_OTHER)\nassert(bad3_or_testing_first);\n#endif\n",
    "src/guard_bad4.cpp": "#if defined(SLUICE_OTHER) || "
    "defined(SLUICE_ASYNC_INTERNAL_TESTING)\n"
    "assert(bad4_or_testing_second);\n#endif\n",
    "src/guard_bad5.cpp": "#if defined(SLUICE_OTHER)\n"
    "#elif defined(SLUICE_ASYNC_INTERNAL_TESTING) || defined(SLUICE_X)\n"
    "assert(bad5_elif_or);\n#endif\n",
    "include/fake/compl.hpp": 'assert(false && "registered L9 site");\n'
    "assert(unregistered_in_same_file);\n",
    "src/plusplus.cpp": "++iterator;\n"
    "assert(plusplus_found_after_plusplus_line);\n",
    "include/after_binary.hpp": "assert(after_binary_no_path_bleed);\n",
    "src/renamed.cpp": "int ctx;\nassert(renamed_found_on_new_path);\nint more;\n",
}

# The violation set the fixture above must produce, exactly: (path, lineno).
SELF_EXPECTED_HITS = [
    ("include/a.hpp", 2),
    ("include/a.hpp", 3),
    ("include/a.hpp", 4),
    ("src/guard_bad1.cpp", 2),
    ("src/guard_bad2.cpp", 2),
    ("src/guard_bad3.cpp", 2),
    ("src/guard_bad4.cpp", 2),
    ("src/guard_bad5.cpp", 3),
    ("include/fake/compl.hpp", 2),
    ("src/plusplus.cpp", 2),
    ("src/renamed.cpp", 2),
    ("include/after_binary.hpp", 1),
]

# Every auto-allow, exemption, and exemption-escape that must NOT appear.
SELF_EXPECTED_CLEAN = [
    "a.hpp:5", "a.hpp:6", "a.hpp:7",
    "guard_ok1.cpp", "guard_ok2.cpp", "guard_ok3.cpp",
    "guard_ok4.cpp", "guard_ok5.cpp",
    "compl.hpp:1",
    "deleted.cpp",
    "bin.dat",
]


def self_test():
    failures = []

    def check(cond, name):
        if not cond:
            failures.append(name)

    entries = [
        (
            "include/fake/compl.hpp",
            'assert(false && "registered L9 site");',
            "L9",
            "planted for self-test",
        )
    ]
    violations = scan(SELF_DIFF, lambda p: SELF_TEXTS.get(p), entries)

    got = sorted(
        (path, int(m.group(1)))
        for v in violations
        for path in [v.strip().splitlines()[0].split(":")[0]]
        for m in [re.match(r"\s*\S+:(\d+):", v.strip().splitlines()[0])]
        if m
    )
    # Marker form "path:lineno" must appear exactly for the expected hits and
    # for nothing else (exact-set match — no missing detector, no extra hit).
    for path, lineno in SELF_EXPECTED_HITS:
        check(any(f"{path}:{lineno}:" in v for v in violations),
              f"detector did not fire: {path}:{lineno}")
    for marker in SELF_EXPECTED_CLEAN:
        check(not any(marker in v for v in violations),
              f"false positive: {marker}")
    check(sorted(SELF_EXPECTED_HITS) == got, "violation set is exactly the expected set")
    check(len(violations) == len(SELF_EXPECTED_HITS),
          f"violation count {len(violations)} != expected {len(SELF_EXPECTED_HITS)}")

    # --- diff state machine: '\ No newline' consumes no line number --------
    mini = (
        "diff --git a/x b/x\n"
        "--- a/x\n"
        "+++ b/x\n"
        "@@ -1,2 +1,3 @@\n"
        " ctx\n"
        "+assert(one);\n"
        "\\ No newline at end of file\n"
        "+assert(two);\n"
    )
    yielded = list(parse_added_lines(mini))
    check(
        yielded == [("x", 2, "assert(one);"), ("x", 3, "assert(two);")],
        f"'\\ No newline' marker consumed a line number: {yielded}",
    )

    # --- diff state machine: malformed shape fails closed ------------------
    try:
        list(parse_added_lines("diff --git a/x b/x\n--- a/x\nWEIRD LINE\n"))
        failures.append("malformed diff (--- not followed by +++) did not raise")
    except DiffShapeError:
        pass
    try:
        list(parse_added_lines("diff --git a/x b/x\n--- a/x\n+++ noprefix/x\n@@ -0,0 +1,1 @@\n+assert(h);\n"))
        failures.append("unparseable +++ prefix did not raise")
    except DiffShapeError:
        pass

    # --- allowlist loading: malformed entries fail closed ------------------
    with tempfile.TemporaryDirectory() as tmp:
        def load(text):
            p = Path(tmp) / "allowlist"
            p.write_text(text, encoding="utf-8")
            return load_allowlist(p)

        check(load("# comment only\n\n") == [], "comment/blank allowlist lines ignored")
        ok = load("src/ok.cpp | assert(x); | L9 | reason\n")
        check(ok == [("src/ok.cpp", "assert(x);", "L9", "reason")],
              "well-formed site entry parses")
        for text, name in [
            ("src/a.cpp | assert(x); | BOGUS | reason\n", "unknown category rejected"),
            ("src/a.cpp | assert(x); | L9 | \n", "empty reason rejected"),
            ("src/a.cpp | assert(x); | L9\n", "3-field entry rejected"),
            ("src/a.cpp | | L9 | reason\n", "empty substring rejected"),
        ]:
            try:
                load(text)
                failures.append(f"malformed allowlist accepted: {name}")
            except AllowlistError:
                pass

    if failures:
        print("assert-hygiene --self-test FAILED:")
        for f in failures:
            print(f"  {f}")
        for v in violations:
            print(v)
        return False
    print("assert-hygiene --self-test: all detectors fired, no false positives")
    return True


# --- entry point ------------------------------------------------------------

def main():
    if "--self-test" in sys.argv[1:]:
        return 0 if self_test() else 1
    args = sys.argv[1:]
    if args and args[0] == "--":
        args = args[1:]
    # No args = manual probe: staged + working tree vs HEAD (`git diff HEAD`).
    # Plain `git diff` would silently ignore fully-staged additions.
    cmd = ["git", "diff"] + (["HEAD"] if not args else args)
    proc = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        print(f"assert-hygiene: FAILED to run `git diff {' '.join(args)}`:")
        print(proc.stderr.strip())
        return 1
    try:
        entries = load_allowlist()
    except AllowlistError as e:
        print(f"assert-hygiene: FAILED (malformed allowlist): {e}")
        print(f"reproduce: edit scripts/gates/assert-hygiene.allowlist")
        return 1
    cache = {}

    def new_text_of(path):
        if path not in cache:
            cache[path] = resolve_new_text(args, path)
        return cache[path]

    try:
        violations = scan(proc.stdout, new_text_of, entries)
    except DiffShapeError as e:
        print(f"assert-hygiene: FAILED (unparseable diff shape): {e}")
        print(f"reproduce: git diff {' '.join(args)}")
        return 1
    if violations:
        print("assert-hygiene: FAILED (changed-lines assert-family scan)")
        print("\n".join(violations))
        print(
            "reproduce: python3 scripts/gates/assert-hygiene.py "
            + " ".join(args)
        )
        return 1
    label = " ".join(args) if args else "HEAD (staged + working tree)"
    print(f"assert-hygiene: OK (changed-lines scan of {label})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
