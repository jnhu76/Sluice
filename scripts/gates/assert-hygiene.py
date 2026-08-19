#!/usr/bin/env python3
"""assert-hygiene.py — changed-lines assert-family gate (issue #144 / #135).

Policy authority: AGENTS.md §9.2 and docs/architecture/failure-model.md.
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

ALLOWED shapes (AGENTS.md §9.2 / failure-model.md §5):
  1. Completion L9 pattern / pure diagnostics — path allowlisted in
     scripts/gates/assert-hygiene.allowlist with a written reason;
  2. internal-testing preconditions — lines INSIDE an
     SLUICE_ASYNC_INTERNAL_TESTING preprocessor guard are auto-allowed
     (mechanically detected in the new-file content; the #else/#elif branch
     of such a guard is production code and is NOT auto-allowed).

USAGE
    python3 scripts/gates/assert-hygiene.py            # staged + working tree vs HEAD
    python3 scripts/gates/assert-hygiene.py <args>...  # args passed to `git diff`
                                                        # (e.g. <remote>..<local>)
    python3 scripts/gates/assert-hygiene.py --self-test

Fail-closed: exits non-zero listing every violation with file:line and the
reproduction command. --self-test plants each violation shape and requires
every detector to fire with zero false positives.
"""

import fnmatch
import re
import subprocess
import sys
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


def load_allowlist(path: Path = ALLOWLIST):
    """Entries: list of (glob, reason). '#' starts a comment; the token before
    it is the path glob."""
    entries = []
    if not path.is_file():
        return entries
    for raw in path.read_text(encoding="utf-8").splitlines():
        line, sep, comment = raw.partition("#")
        glob = line.strip()
        if glob:
            entries.append((glob, comment.strip() if sep else ""))
    return entries


def allowlisted(path: str, entries) -> bool:
    return any(fnmatch.fnmatchcase(path, g) for g, _ in entries)


# --- preprocessor guard tracking -------------------------------------------

PP_IF_RE = re.compile(r"^\s*#\s*(ifdef|ifndef|if)\b")
PP_ELSE_RE = re.compile(r"^\s*#\s*(else|elif)\b")
PP_ENDIF_RE = re.compile(r"^\s*#\s*endif\b")
TESTING_MACRO = "SLUICE_ASYNC_INTERNAL_TESTING"


def testing_guard_lines(text: str):
    """1-based line numbers that are inside an active
    SLUICE_ASYNC_INTERNAL_TESTING guard region of `text`.

    Conservative by design: `#else`/`#elif` of a testing guard is treated as
    PRODUCTION (not auto-allowed); unknown polarity is never auto-allowed.
    Under-allowing forces an explicit, reviewed allowlist entry — the safe
    direction for a policy gate.
    """
    stack = []  # per open #if level: True = this level selects testing code
    inside = set()
    n = 0
    for line in text.splitlines():
        n += 1
        if PP_IF_RE.match(line):
            if "ifdef " + TESTING_MACRO in line or TESTING_MACRO in line.replace(
                "#if", "", 1
            ):
                if "ifndef " + TESTING_MACRO in line:
                    stack.append(False)  # #ifndef X: the IF branch is production
                else:
                    stack.append(True)
            else:
                stack.append(False)
        elif PP_ELSE_RE.match(line):
            if stack:
                stack[-1] = False  # else/elif branch of any guard: production
        elif PP_ENDIF_RE.match(line):
            if stack:
                stack.pop()
        if True in stack:
            inside.add(n)
    return inside


# --- diff parsing -----------------------------------------------------------

def parse_added_lines(diff_text: str):
    """Yield (path, new_lineno, text) for ADDED lines. Renames use the b/
    side; deletions (new side /dev/null) are skipped."""
    path = None
    new_lineno = 0
    for line in diff_text.splitlines():
        if line.startswith("+++ b/"):
            path = line[len("+++ b/") :].strip()
            continue
        if line.startswith("+++ "):  # /dev/null or odd header
            path = None
            continue
        m = HUNK_RE.match(line)
        if m:
            new_lineno = int(m.group(1))
            continue
        if path is None:
            continue
        if line.startswith("+"):
            yield path, new_lineno, line[1:]
            new_lineno += 1
        elif line.startswith(" ") or line.startswith("\\"):
            new_lineno += 1
        elif line.startswith("-"):
            pass
        # 'diff --git' / 'index' / '---' headers reset to path discovery
        if line.startswith("diff --git"):
            path = None


def resolve_new_text(args, path: str):
    """New-side file content for guard detection. Range/commit mode asks git;
    default (no-arg) mode reads the working tree. None = no guard info
    (conservative: only path allowlisting can then allow a line)."""
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
        if allowlisted(path, entries):
            continue
        new_text = new_text_of(path)
        guarded = testing_guard_lines(new_text) if new_text else set()
        for lineno, text in hits:
            if lineno in guarded:
                continue
            violations.append(
                f"  {path}:{lineno}: {text.strip()}\n"
                f"    new assert-family line in include/src; classify per\n"
                f"    docs/architecture/failure-model.md §5 and allowlist it in\n"
                f"    scripts/gates/assert-hygiene.allowlist, or use a typed\n"
                f"    result / named fail-fast instead"
            )
    return violations


# --- self-test --------------------------------------------------------------

SELF_DIFF = """\
diff --git a/include/a.hpp b/include/a.hpp
--- a/include/a.hpp
+++ b/include/a.hpp
@@ -1,4 +1,9 @@
 #pragma once
+assert(p != nullptr);
+#include <cassert>
+#include <assert.h>
+#include <cstdint>
+static_assert(sizeof(int) == 4);
+// assert(old) was documented here
 assert(grandfathered);
-assert(removed);
diff --git a/src/guarded.cpp b/src/guarded.cpp
--- a/src/guarded.cpp
+++ b/src/guarded.cpp
@@ -1,3 +1,7 @@
+#ifdef SLUICE_ASYNC_INTERNAL_TESTING
+assert(seam_precondition);
+#else
+assert(production_side);
+#endif
 int x;
diff --git a/src/ok.cpp b/src/ok.cpp
--- a/src/ok.cpp
+++ b/src/ok.cpp
@@ -1,2 +1,3 @@
 int y;
+assert(allowlisted_path);
+std::size_t z = 0;
diff --git a/tests/t.cpp b/tests/t.cpp
--- a/tests/t.cpp
+++ b/tests/t.cpp
@@ -1 +1,2 @@
+assert(outside_scanned_prefixes);
"""

SELF_GUARDED_TEXT = """\
#ifdef SLUICE_ASYNC_INTERNAL_TESTING
assert(seam_precondition);
#else
assert(production_side);
#endif
int x;
"""

SELF_OK_TEXT = "int y;\nassert(allowlisted_path);\nstd::size_t z = 0;\n"


def self_test():
    entries = [("src/ok.cpp", "pure diagnostics (planted for self-test)")]
    texts = {
        "include/a.hpp": "#pragma once\n",
        "src/guarded.cpp": SELF_GUARDED_TEXT,
        "src/ok.cpp": SELF_OK_TEXT,
        "tests/t.cpp": "",
    }
    violations = scan(SELF_DIFF, lambda p: texts.get(p), entries)

    def has(substr):
        return any(substr in v for v in violations)

    planted = [
        ("bare assert added in include/", "include/a.hpp:2"),
        ("cassert include added", "include/a.hpp:3"),
        ("assert.h include added", "include/a.hpp:4"),
        ("testing-guard #else production assert", "src/guarded.cpp:4"),
    ]
    missed = [name for name, marker in planted if not has(marker)]
    # expected hits: exactly the 4 planted; the auto-allowed seam line, the
    # allowlisted path, static_assert, cstdint, comments, context/removed
    # lines, and tests/ must NOT appear.
    false_positives = [
        marker
        for marker in ("a.hpp:5", "a.hpp:6", "a.hpp:7", "a.hpp:8",
                       "guarded.cpp:2", "ok.cpp:3", "t.cpp:2")
        if has(marker)
    ]
    if missed or false_positives or len(violations) != len(planted):
        print("assert-hygiene --self-test FAILED:")
        for name in missed:
            print(f"  detector did not fire: {name}")
        for marker in false_positives:
            print(f"  false positive: {marker}")
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
    entries = load_allowlist()
    cache = {}

    def new_text_of(path):
        if path not in cache:
            cache[path] = resolve_new_text(args, path)
        return cache[path]

    violations = scan(proc.stdout, new_text_of, entries)
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
