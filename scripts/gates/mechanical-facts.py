#!/usr/bin/env python3
"""mechanical-facts.py — Mechanical Correctness Gate (post-freeze R1, PR #114).

Rationale (issue #113): AI-authored changes can be semantically sound while
getting SPELLING, COUNTING, and CROSS-REFERENCES wrong. A human/LLM re-review
of those facts is the wrong tool; this gate checks them mechanically:

  A. identifier near-miss — any all-caps identifier in code that is edit
     distance 1 from a canonical SLUICE_* project identifier but is not
     itself canonical is an ERROR (the class that produced SLUCE_ ...).
  B. doc LOC claims — every ``path`` | NNN | table row in docs/post-freeze/
     must satisfy wc -l(path) == NNN.
  C. split layout completeness — the actual scheduler split file set must
     equal the documented inventory set exactly (no missing/extra/duplicate).
  D. SHA references — hex tokens referenced in docs/post-freeze/ must
     resolve to real git objects.
  E. tracker references — #NNN references in docs/post-freeze/ must be in
     the explicit KNOWN_TRACKER_REFS registry (offline, deterministic).
  F. test totals — ``test:default-gate-targets`` rows in docs/post-freeze/
     must equal the mechanically counted default-`xmake test` gate size
     (the ``running.test`` line count of a Linux Clang Debug run),
     derived from the xmake lua registration constructs, not hand-typed.

Fail-closed: exits non-zero on the first category with findings, printing
the exact reproduction. --self-test plants violations in a temp dir and
requires every category's detector to fire (D excepted: it needs a real git
repo and is exercised by the main run against this repository).

Usage: python3 scripts/gates/mechanical-facts.py [--self-test]
"""

import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent

# Code files scanned for identifier near-misses. docs/ are deliberately NOT
# scanned: they quote historical typos verbatim (e.g. the #113 post-mortem).
CODE_DIRS = ["src", "include", "tests", "bench", "xmake"]
CODE_EXTS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".hxx", ".ipp", ".lua"}
SCAN_SELF_EXEMPT = {"mechanical-facts.py"}

# Docs whose tables/claims are verified.
FACT_DOCS_DIR = REPO / "docs" / "post-freeze"

# Layout authority: the actual split file set must equal the documented set.
SPLIT_GLOB_CPP = "src/async/scheduler_*.cpp"
SPLIT_GLOB_HPP = "src/async/scheduler_internal.hpp"

# Offline registry of tracker references allowed in fact docs. Adding a new
# issue/PR reference requires adding its number here (one line, reviewed).
KNOWN_TRACKER_REFS = {
    94, 95, 96, 97, 98, 99, 100, 101,  # audit #94-#101 closeout
    109, 110, 111, 112,                # Phase G closeout / freeze / known limits
    113, 114, 115,                      # phantom-fault post-mortem / hygiene PR /
                                       # parked-worker stranding observation
    116,                                # runtime lost re-entry liveness (fixed
                                       # 2026-08-17; corrective note in this report)
}

TOKEN_RE = re.compile(r"[A-Z][A-Z0-9_]{7,}")
DEFINE_RE = re.compile(r"^\s*#\s*define\s+([A-Z_][A-Z0-9_]*)", re.M)
ADD_DEFINES_RE = re.compile(r'add_defines\("([A-Z_][A-Z0-9_]*)"')
# Table rows of the form | `path` | NNN | (space after opening pipe allowed).
LOC_ROW_RE = re.compile(r"^\|\s*`([^`|]+)`\s*\|\s*(\d+)\s*\|", re.M)
SHA_RE = re.compile(r"(?<![0-9a-f])[0-9a-f]{7,40}(?![0-9a-f])")
REF_RE = re.compile(r"#(\d+)\b")


def code_files(root=None):
    base = root or REPO
    for d in CODE_DIRS:
        p = base / d
        if not p.is_dir():
            continue
        for f in sorted(p.rglob("*")):
            if f.suffix in CODE_EXTS and f.is_file():
                if root is None and f.name in SCAN_SELF_EXEMPT:
                    continue
                if "build" in f.parts:
                    continue
                yield f


def edit_distance_le1(a, b):
    """True iff levenshtein(a, b) <= 1."""
    if abs(len(a) - len(b)) > 1:
        return False
    if len(a) == len(b):
        return sum(x != y for x, y in zip(a, b)) <= 1
    if len(a) > len(b):
        a, b = b, a
    # a is shorter by 1: b must reduce to a by deleting one char.
    i = j = 0
    skipped = False
    while i < len(a) and j < len(b):
        if a[i] == b[j]:
            i += 1
            j += 1
        elif skipped:
            return False
        else:
            skipped = True
            j += 1
    return True


def canonical_identifiers():
    canon = set()
    for f in code_files():
        text = f.read_text(errors="replace")
        canon.update(DEFINE_RE.findall(text))
        if f.suffix == ".lua":
            canon.update(ADD_DEFINES_RE.findall(text))
    return canon


def check_identifier_near_miss(canon, files=None):
    errs = []
    for f in (files or code_files()):
        for m in TOKEN_RE.finditer(f.read_text(errors="replace")):
            tok = m.group(0)
            if tok in canon:
                continue
            if tok.count("SLUC") == 0:
                # Only police the project namespace: near-misses of the
                # SLUICE_* family (and nothing else) are gate business.
                continue
            near = sorted(c for c in canon if edit_distance_le1(tok, c))
            if near:
                errs.append(
                    f"{f}: unknown project identifier '{tok}' "
                    f"(edit distance 1 from {', '.join(near[:3])})"
                )
    return errs


def check_doc_loc_claims(doc_paths, root=None):
    """Verify ``path`` | NNN | rows. If the row's line pins a commit SHA
    (historical snapshot claim), verify the line count AT that commit via
    `git show`; otherwise verify against the current tree. Rows with the
    virtual ``test:`` path prefix are test-total claims handled by
    check_test_total_claims, not file LOC claims."""
    errs = []
    for doc in doc_paths:
        for line in doc.read_text(errors="replace").splitlines():
            m = LOC_ROW_RE.search(line)
            if not m:
                continue
            path, claimed = m.group(1).strip(), int(m.group(2))
            if path.startswith("test:"):
                continue  # verified by check_test_total_claims
            sha_m = SHA_RE.search(line)
            if sha_m:
                sha = sha_m.group(0)
                r = subprocess.run(
                    ["git", "-C", str(REPO), "show", f"{sha}:{path}"],
                    capture_output=True,
                )
                if r.returncode != 0:
                    errs.append(f"{doc.name}: '{path}' not present at {sha}")
                    continue
                actual = r.stdout.count(b"\n")
                if actual != claimed:
                    errs.append(
                        f"{doc.name}: claims '{path}' = {claimed} lines at "
                        f"{sha}, actual = {actual}"
                    )
                continue
            target = (root or REPO) / path
            if not target.is_file():
                errs.append(f"{doc.name}: LOC row references missing file '{path}'")
                continue
            # wc -l semantics: count newline bytes so a final unterminated
            # line is not counted (docs quote wc -l, not editor line count).
            actual = target.read_bytes().count(b"\n")
            if actual != claimed:
                errs.append(
                    f"{doc.name}: claims '{path}' = {claimed} lines, wc -l = {actual}"
                )
    return errs


def documented_split_set(doc_paths):
    seen = {}
    for doc in doc_paths:
        for m in LOC_ROW_RE.finditer(doc.read_text(errors="replace")):
            path = m.group(1).strip()
            if re.match(r"src/async/scheduler", path):
                if path in seen:
                    seen[path] = "duplicate"
                else:
                    seen[path] = doc.name
    return seen


def check_split_layout(doc_paths, root=None):
    # Layout authority is the as-built inventory table in the final report;
    # other docs (e.g. the audit's historical risk matrix) may legitimately
    # mention the same files without being completeness authorities.
    base = root or REPO
    actual = {str(p.relative_to(base)) for p in base.glob(SPLIT_GLOB_CPP)}
    # The RETAINED scheduler.cpp TU is part of the as-built inventory too
    # (split-layout completeness covers it, not just the new scheduler_*.cpp).
    actual.add("src/async/scheduler.cpp")
    actual |= {SPLIT_GLOB_HPP}
    documented = documented_split_set(
        [p for p in doc_paths if p.name == "post-freeze-final-report.md"]
        or doc_paths)
    errs = []
    for path, where in documented.items():
        if where == "duplicate":
            errs.append(f"split layout: '{path}' listed more than once in doc tables")
    doc_set = set(documentated_keys_no_dup := {
        p for p, w in documented.items() if w != "duplicate"})
    for missing in sorted(actual - doc_set):
        errs.append(f"split layout: file '{missing}' exists but is not in any doc table")
    for extra in sorted(doc_set - actual):
        errs.append(f"split layout: doc table lists '{extra}' but no such file exists")
    return errs


def check_sha_references(doc_paths):
    errs = []
    for doc in doc_paths:
        for line in doc.read_text(errors="replace").splitlines():
            if "upstream" in line:
                # Upstream foreign-repo pins (e.g. ziglang/zig commits) are
                # deliberately not Sluice git objects; docs must mark such
                # lines with the word "upstream".
                continue
            for m in SHA_RE.finditer(line):
                sha = m.group(0)
                r = subprocess.run(
                    ["git", "-C", str(REPO), "cat-file", "-e", f"{sha}^{{commit}}"],
                    capture_output=True,
                )
                if r.returncode != 0:
                    errs.append(
                        f"{doc.name}: references SHA '{sha}' not in repository")
    return errs


def check_tracker_refs(doc_paths):
    errs = []
    for doc in doc_paths:
        for m in REF_RE.finditer(doc.read_text(errors="replace")):
            n = int(m.group(1))
            if n not in KNOWN_TRACKER_REFS:
                errs.append(
                    f"{doc.name}: references #{n} which is not in "
                    f"KNOWN_TRACKER_REFS (scripts/gates/mechanical-facts.py)"
                )
    return errs


# ═══════════════════════════════════════════════════════════════════════════════
# F. Test-total claims — the "NNN/NNN" test totals quoted in gate/evidence
#    docs are derived mechanically, never hand-typed. The virtual-path row
#      | `test:default-gate-targets` | NNN |
#    must equal the number of tests registered into the default `xmake test`
#    gate (Linux semantics: every current platform_gate includes linux).
# ═══════════════════════════════════════════════════════════════════════════════

# Virtual row path reserved for the default-gate test-target count.
TEST_TOTAL_ROW = "test:default-gate-targets"

# Registration constructs used by xmake.lua / xmake/tests/*.lua. Mirrored
# here so a registration change (add/remove/rename) that the docs do not
# follow fails the gate instead of drifting silently.
ONE_FILE_TARGET_RE = re.compile(
    r'sluice_one_file_target\(\s*"(\w+)",\s*"(\w+)",\s*(.*?),\s*"([^"]+)"')
WRAPPER_TEST_RE = re.compile(
    r'sluice_(?:internal_async|production_async|one_file)_test\(\s*"([A-Za-z0-9_]+)"')
ADD_TESTS_RE = re.compile(r'add_tests\(\s*"([A-Za-z0-9_]+)"')


def _strip_lua_comments(text):
    """Remove -- line comments while preserving "--" inside string literals
    (only occurrence in the tree: xmake.lua's "--hardened" flag string).
    The test lua files contain no --[[ ]] block comments today."""
    out = []
    in_str = False
    i = 0
    n = len(text)
    while i < n:
        ch = text[i]
        if in_str:
            out.append(ch)
            if ch == '"':
                in_str = False
            i += 1
            continue
        if text.startswith("--", i):
            j = text.find("\n", i)
            i = n if j < 0 else j + 1
            continue
        if ch == '"':
            in_str = True
        out.append(ch)
        i += 1
    return "".join(out)


def _strip_liburing_guards(text):
    """Drop `if has_liburing ...` / `if has_config("with-liburing") ...` blocks
    (and their contents): those registrations exist only in real-liburing
    builds, while the mechanically counted default gate is the stub build
    (e.g. uring_submit_failure_test). Nested if/do/end are depth-counted so an
    inner guard inside a target block does not swallow the target's own
    add_tests."""
    lines = text.splitlines(keepends=True)
    out = []
    depth = 0
    in_guard = False
    for ln in lines:
        if not in_guard:
            m = re.match(
                r"\s*if\s+(?:has_liburing\b|has_config\s*\(\s*\"with-liburing\")",
                ln)
            if m:
                in_guard = True
                depth = 1
                continue
            out.append(ln)
            continue
        depth += len(re.findall(r"\b(?:then|do)\b", ln))
        depth -= len(re.findall(r"\bend\b", ln))
        if depth <= 0:
            in_guard = False
    return "".join(out)


def _registered_test_names(text):
    """All test names one lua source registers into the default gate."""
    clean = _strip_liburing_guards(_strip_lua_comments(text))
    names = set()
    # Name tables: `local <tbl> = { "a", "b", ... }` (core.lua `tests`).
    tables = {}
    for m in re.finditer(r"local\s+(\w+)\s*=\s*\{(.*?)\}", clean, re.S):
        tables[m.group(1)] = [
            it.strip().strip('"') for it in m.group(2).split(",") if it.strip()
        ]
    # ipairs loops expanding sluice_one_file_target (core.lua: `t .. "_test"`).
    for m in re.finditer(
            r"for\s*_,\s*(\w+)\s+in\s+ipairs\(\s*(\w+)\s*\)\s*do", clean):
        var, tbl = m.group(1), m.group(2)
        items = tables.get(tbl)
        if items is None:
            continue
        body = clean[m.end():]
        cut = body.find("\nend")
        if cut >= 0:
            body = body[:cut]
        for tm in ONE_FILE_TARGET_RE.finditer(body):
            kind, group, name_arg, subdir = tm.groups()
            if group != "test":
                continue
            if name_arg.strip().startswith(var + " .. "):
                suffix = name_arg.split('"', 2)[1]
                for item in items:
                    names.add(item + suffix)
            else:
                names.add(name_arg.strip().strip('"'))
    # Plain sluice_one_file_target / wrapper calls / explicit add_tests.
    for m in ONE_FILE_TARGET_RE.finditer(clean):
        kind, group, name_arg, subdir = m.groups()
        if group == "test" and ".." not in name_arg:
            names.add(name_arg.strip().strip('"'))
    for m in WRAPPER_TEST_RE.finditer(clean):
        names.add(m.group(1))
    for m in ADD_TESTS_RE.finditer(clean):
        names.add(m.group(1))
    return names


def default_gate_test_count(root=None):
    """Mechanical count of tests registered into the default `xmake test`
    gate (the number of `running.test` lines a Linux Clang Debug stub
    `xmake test -v` executes). Scans xmake.lua plus every lua file under
    xmake/ (helpers.lua contributes nothing: its add_tests calls take a
    variable name, which the literal regexes ignore)."""
    base = Path(root) if root else REPO
    sources = []
    if (base / "xmake.lua").is_file():
        sources.append((base / "xmake.lua").read_text(errors="replace"))
    xmake_dir = base / "xmake"
    if xmake_dir.is_dir():
        for f in sorted(xmake_dir.glob("*.lua")):
            sources.append(f.read_text(errors="replace"))
    test_dir = xmake_dir / "tests"
    if test_dir.is_dir():
        for f in sorted(test_dir.glob("*.lua")):
            sources.append(f.read_text(errors="replace"))
    names = set()
    for s in sources:
        names |= _registered_test_names(s)
    return len(names)


def check_test_total_claims(doc_paths, root=None):
    errs = []
    expected = default_gate_test_count(root)
    for doc in doc_paths:
        for line in doc.read_text(errors="replace").splitlines():
            m = LOC_ROW_RE.search(line)
            if not m:
                continue
            path, claimed = m.group(1).strip(), int(m.group(2))
            if path != TEST_TOTAL_ROW:
                continue
            if claimed != expected:
                errs.append(
                    f"{doc.name}: claims default-gate test targets = {claimed}, "
                    f"mechanical count = {expected} (Linux Clang Debug "
                    f"`xmake test -v` `running.test` lines)"
                )
    return errs


def fact_docs(root_dir=None):
    d = root_dir or FACT_DOCS_DIR
    return sorted(p for p in d.glob("*.md")) if d.is_dir() else []


# Issue-116 fix docs carry test-total rows too; only check_test_total_claims
# sees them (the other detectors keep their documented docs/post-freeze
# scope — these docs quote e.g. `run_live#1` tokens that are not tracker
# references and would trip the tracker-ref detector).
TEST_TOTAL_EXTRA_DOCS = [
    "docs/investigations/issue-116-runtime-reentry-liveness.md",
    "docs/architecture/issue-116-reentry-liveness-gate.md",
]


def run_all():
    errs = []
    canon = canonical_identifiers()
    errs += check_identifier_near_miss(canon)
    docs = fact_docs()
    if not docs:
        return ["mechanical-facts: no fact docs found under docs/post-freeze/"]
    errs += check_doc_loc_claims(docs, root=REPO)
    errs += check_split_layout(docs, root=REPO)
    errs += check_sha_references(docs)
    errs += check_tracker_refs(docs)
    total_docs = list(docs)
    for rel in TEST_TOTAL_EXTRA_DOCS:
        p = REPO / rel
        if p.is_file():
            total_docs.append(p)
    errs += check_test_total_claims(total_docs, root=REPO)
    return errs


def self_test():
    """Plant one violation per detector in a temp workspace; each must fire."""
    failures = []
    with tempfile.TemporaryDirectory() as td:
        t = Path(td)
        (t / "src").mkdir()
        (t / "include").mkdir()
        (t / "docs").mkdir()
        # Canonical identifier defined in a header.
        (t / "include" / "a.hpp").write_text("#define SLUICE_CANON_MACRO 1\n")
        # A: near-miss token in code.
        (t / "src" / "a.cpp").write_text(
            "// guard: SLUCE_CANON_MACRO typo planted\n#define SLUICE_CANON_MACRO 1\n"
        )
        canon = {m.group(1) for m in DEFINE_RE.finditer(
            (t / "include" / "a.hpp").read_text())}
        if not check_identifier_near_miss(canon, [t / "src" / "a.cpp"]):
            failures.append("self-test: near-miss detector failed to fire")

        # B: LOC claim mismatch.
        doc = t / "docs" / "r.md"
        doc.write_text("|`src/a.cpp`|99|\n")
        if not check_doc_loc_claims([doc], root=t):
            failures.append("self-test: LOC-claim detector failed to fire")

        # C: layout mismatch (file missing) — validated against THIS temp
        # root, plus a clean control proving no false positives when the
        # documented set equals the actual set.
        doc.write_text("|`src/async/scheduler_ghost.cpp`|10|\n")
        if not check_split_layout([doc], root=t):
            failures.append("self-test: layout detector failed to fire")
        (t / "src" / "async").mkdir(parents=True)
        (t / "src" / "async" / "scheduler_one.cpp").write_text("int a;\n")
        (t / "src" / "async" / "scheduler.cpp").write_text("int b;\n")
        open(t / "src" / "async" / SPLIT_GLOB_HPP.split("/")[-1], "w").write("int c;\n")
        doc.write_text("|`src/async/scheduler_one.cpp`|1|\n|`src/async/scheduler.cpp`|1|\n|`src/async/scheduler_internal.hpp`|1|\n")
        if check_split_layout([doc], root=t):
            failures.append("self-test: layout false positive on complete inventory")

        # E: unknown tracker ref.
        doc.write_text("see #99999\n")
        if not check_tracker_refs([doc]):
            failures.append("self-test: tracker-ref detector failed to fire")

        # F: test-total claim mismatch. Minimal lua tree exercising every
        # registration construct (wrapper, explicit target, ipairs loop,
        # out-of-gate target without add_tests).
        (t / "xmake" / "tests").mkdir(parents=True)
        (t / "xmake.lua").write_text("includes('xmake/tests/t1.lua')\n")
        (t / "xmake" / "tests" / "t1.lua").write_text(
            'sluice_production_async_test("alpha_test")\n'
            'target("beta_test")\n'
            '    add_tests("beta_test")\n'
            'end\n'
            'target("off_gate_test")\n'
            '    -- no add_tests: out of the default gate\n'
            'end\n'
            'local names = { "gamma", "delta" }\n'
            'for _, n in ipairs(names) do\n'
            '    sluice_one_file_target("binary", "test", n .. "_test", '
            '"tests", "sluice_core")\n'
            'end\n'
        )
        if default_gate_test_count(t) != 4:
            failures.append("self-test: test-count parser mismatch "
                            "(expected 4 registered targets)")
        doc.write_text("|`test:default-gate-targets`|3|\n")
        if not check_test_total_claims([doc], root=t):
            failures.append("self-test: test-total detector failed to fire")
        doc.write_text("|`test:default-gate-targets`|4|\n")
        if check_test_total_claims([doc], root=t):
            failures.append("self-test: test-total false positive on correct count")

        # Clean control: no false positives on canonical text.
        doc.write_text("Refs #109 and #113.\n|`docs/r.md`|2|\n")
        (t / "src" / "a.cpp").write_text("#define SLUICE_CANON_MACRO 1\n")
        if check_doc_loc_claims([doc], root=t):
            failures.append("self-test: LOC-claim false positive on clean input")
        if check_tracker_refs([doc]):
            failures.append("self-test: tracker-ref false positive on clean input")
    return failures


def main():
    if "--self-test" in sys.argv[1:]:
        errs = self_test()
        if errs:
            print("mechanical-facts --self-test FAILED:")
            for e in errs:
                print(f"  {e}")
            return 1
        print("mechanical-facts --self-test: all detectors fired, no false positives")
        return 0
    errs = run_all()
    if errs:
        print("mechanical-facts: FAILED")
        for e in errs:
            print(f"  ERROR: {e}")
        print("reproduce: python3 scripts/gates/mechanical-facts.py")
        return 1
    print("mechanical-facts: OK (near-miss scan, LOC claims, split layout, "
          "SHA refs, tracker refs, test totals)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
