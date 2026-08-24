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
  G. seam/production exclusion (C4 / issue #135, extended by #142 review) —
     the internal-testing control plane lives in NON-INSTALLED seam headers
     (src/async/*_test_seams.hpp, apps/sluice-copy/safe_output_test_seams.hpp)
     included by production-compiled sources ONLY under the matching
     *_INTERNAL_TESTING macro. Installed headers must reference seam headers
     only inside the SLUICE_ASYNC_INTERNAL_TESTING guard; app production
     sources must reference app seam headers only inside the app guard; and
     the production sluice_async / sluice-copy targets must never define the
     macros, gain a seam include path, or list a seam source (AGENTS.md §15
     persistence contract — one mechanical authority for every seam family).

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
    139,                                # request-lifecycle walkthrough nav comment
                                       # (2026-08-20, docs-only attribution entry)
    161,                                # idle-dance contribution-identity stall
                                       # (2026-08-21, corrective delta attribution
                                       # in this report; gate doc in docs/architecture/)
    162,                                # C++<->TLA+ adversarial abstraction audit
                                       # (2026-08-21, Phase-7 CPP-001/002 corrective
                                       # delta attribution in this report; closeout
                                       # docs/history/closeout/e12-cross-primitive-
                                       # terminal-audit.md SS11.4-11.7)
    170,                                # inert worker-inbox notification surface
                                       # removal (2026-08-22, LOC-only delta in this
                                       # report; wake authority unchanged)
    196,                                # E9 trace-conformance pilot (2026-08-24,
                                       # test-only recorder call sites — LOC delta
                                       # attribution in this report; see
                                       # docs/verification/formal/
                                       # e9-trace-conformance.md)
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


# ═══════════════════════════════════════════════════════════════════════════════
# G. Seam/production exclusion (C4 / issue #135; app-seam extension from the
#    #142 review). The internal-testing control plane lives in NON-INSTALLED
#    seam headers — src/async/*_test_seams.hpp for async,
#    apps/sluice-copy/safe_output_test_seams.hpp for the copy app — included
#    by production-compiled sources ONLY under the matching *_INTERNAL_TESTING
#    macro. AGENTS.md §15 persistence contract, mechanically enforced for every
#    seam family: production targets never compile test control plane.
# ═══════════════════════════════════════════════════════════════════════════════

SEAM_HEADER_INCLUDE_RE = re.compile(
    r"^\s*#\s*include\s*[<\"]([^>\"]*(?:test_seams|test_access)\.hpp)[>\"]", re.M)
INTERNAL_TESTING_MACRO = "SLUICE_ASYNC_INTERNAL_TESTING"
# Private seam families (app or core): macro -> production xmake target that
# must never define the macro nor list a seam header. A new seam family must
# register here in the same reviewed change that introduces it (#142 review
# merge-blocker: no ungated seam families). Core families (e.g. #143's file
# close seam) pair the macro with sluice_core; app families with the app
# target. The apps/ source scan below accepts any family's guard — an app
# file including a core seam header would still have to guard it.
APP_SEAM_FAMILIES = {
    "SLUICE_COPY_INTERNAL_TESTING": "sluice-copy",
    "SLUICE_FILE_INTERNAL_TESTING": "sluice_core",
}

# xmake lua uses IMPLICIT target scoping: a `target("name")` block ends at the
# next top-level target()/option()/task()/includes() (or EOF), not at an
# explicit `end`. The repo's xmake files rely on this (no `end` after
# target("sluice_async") in libraries.lua), so block extraction must NOT count
# `end` keywords.
TARGET_DECL_RE = re.compile(r'target\(\s*"([^"]+)"\s*\)')
NEXT_BLOCK_RE = re.compile(r"^\s*(?:target|option|task|includes)\s*\(", re.M)


def _seam_includes_guarded(text, macros=(INTERNAL_TESTING_MACRO,)):
    """True iff every seam-header `#include` in `text` sits inside an active
    guard region for at least one of `macros` (an app seam passes the app
    family, an installed header the async macro — never the union). Naive
    `#if`-stack scan: each `#if`/`#ifdef`/`#ifndef` pushes whether that region
    is compiled only when a macro is defined; `#elif`/`#else` update the top
    entry; `#endif` pops. A seam include is allowed only while some enclosing
    region is macro-guarded — a bare `#if SLUICE_HAS_LIBURING` guard is NOT
    sufficient."""
    stack = []
    for line in text.splitlines():
        m = re.match(r"#\s*(ifndef|ifdef|if|elif|else|endif)\b", line.lstrip())
        if m:
            kind = m.group(1)
            if kind in ("if", "ifdef", "ifndef"):
                guarded = any(mac in line for mac in macros) and kind != "ifndef"
                stack.append(guarded)
            elif kind in ("elif", "else"):
                if stack:
                    if kind == "elif":
                        stack[-1] = any(mac in line for mac in macros)
                    else:
                        stack[-1] = not stack[-1]
            else:  # endif
                if stack:
                    stack.pop()
            continue
        if SEAM_HEADER_INCLUDE_RE.match(line) and not any(stack):
            return False
    return True


def _xmake_lua_sources(root=None):
    """All xmake lua sources (xmake.lua + xmake/*.lua + xmake/tests/*.lua)."""
    base = Path(root) if root else REPO
    out = []
    if (base / "xmake.lua").is_file():
        out.append((base / "xmake.lua").read_text(errors="replace"))
    for d in ("xmake", "xmake/tests"):
        p = base / d
        if p.is_dir():
            for f in sorted(p.glob("*.lua")):
                out.append(f.read_text(errors="replace"))
    return out


def _implicit_target_regions(text, names):
    """Configuration region of each `target("<name>")` block under xmake's
    implicit scoping (block ends at the next top-level target()/option()/
    task()/includes() or EOF). Comments are stripped first. The region is a
    superset — it may include following top-level `local` helper definitions
    — which is harmless for contamination detection."""
    clean = _strip_lua_comments(text)
    out = []
    for m in TARGET_DECL_RE.finditer(clean):
        if m.group(1) not in names:
            continue
        start = m.end()
        nxt = NEXT_BLOCK_RE.search(clean, start)
        end = nxt.start() if nxt else len(clean)
        out.append(clean[start:end])
    return out


def check_seam_production_exclusion(root=None):
    """C4 (issue #135) persistence contract (AGENTS.md §15), extended by the
    #142 review to app-private seams: production targets never compile
    testing-only control-plane code. Mechanical facts:
      - installed headers reference seam headers only inside the
        SLUICE_ASYNC_INTERNAL_TESTING guard;
      - the production sluice_async target never defines the macro;
      - the production sluice_async target never gains the src/async seam
        include path;
      - app sources compiled by a production app target reference app seam
        headers only inside that family's *_INTERNAL_TESTING guard;
      - the production app targets never define their seam macro nor list a
        seam header as a source (test targets may define it freely)."""
    base = Path(root) if root else REPO
    errs = []
    inc = base / "include" / "sluice"
    for hdr in sorted(inc.rglob("*.hpp")) if inc.is_dir() else []:
        text = hdr.read_text(errors="replace")
        if SEAM_HEADER_INCLUDE_RE.search(text) and not _seam_includes_guarded(text):
            errs.append(
                f"{hdr.relative_to(base)}: seam header include outside a "
                f"{INTERNAL_TESTING_MACRO} guard region"
            )
    apps = base / "apps"
    if apps.is_dir():
        for f in sorted(apps.rglob("*")):
            if f.suffix not in (".cpp", ".hpp") or not f.is_file():
                continue
            if f.name.endswith(("test_seams.hpp", "test_access.hpp")):
                continue  # the seam header itself, not a consumer
            text = f.read_text(errors="replace")
            if SEAM_HEADER_INCLUDE_RE.search(text) and not _seam_includes_guarded(
                    text, tuple(APP_SEAM_FAMILIES)):
                macros = "/".join(APP_SEAM_FAMILIES)
                errs.append(
                    f"{f.relative_to(base)}: seam header include outside a "
                    f"{macros} guard region"
                )
    for src in _xmake_lua_sources(root):
        for region in _implicit_target_regions(src, {"sluice_async"}):
            if INTERNAL_TESTING_MACRO in region:
                errs.append(
                    f"production target sluice_async defines "
                    f"{INTERNAL_TESTING_MACRO} (xmake implicit-scope region)"
                )
            if re.search(r"add_includedirs\s*\([^)]*src/async", region):
                errs.append(
                    f"production target sluice_async gains the src/async seam "
                    f"include path (xmake implicit-scope region)"
                )
        for macro, target in APP_SEAM_FAMILIES.items():
            for region in _implicit_target_regions(src, {target}):
                if macro in region:
                    errs.append(
                        f"production target {target} defines {macro} "
                        f"(xmake implicit-scope region)"
                    )
                if re.search(r"test_seams|test_access", region):
                    errs.append(
                        f"production target {target} lists a test seam header "
                        f"as source/include (xmake implicit-scope region)"
                    )
    return errs


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
    errs += check_seam_production_exclusion(root=REPO)
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

        # G: seam/production exclusion (C4 / issue #135). An unguarded seam
        # include in an installed header must fire; a macro-guarded include
        # must not; a bare liburing-only guard must NOT count as a seam guard.
        (t / "include" / "sluice" / "async").mkdir(parents=True)
        bad = t / "include" / "sluice" / "async" / "bad.hpp"
        bad.write_text('#include "scheduler_test_access.hpp"\n')
        if not check_seam_production_exclusion(t):
            failures.append("self-test: unguarded seam include failed to fire")
        bad.write_text(
            "#if defined(SLUICE_ASYNC_INTERNAL_TESTING)\n"
            '#include "scheduler_test_access.hpp"\n'
            "#endif\n"
        )
        if check_seam_production_exclusion(t):
            failures.append("self-test: guarded seam include false positive")
        bad.write_text(
            "#if defined(SLUICE_HAS_LIBURING)\n"
            '#include "uring_test_seams.hpp"\n'
            "#endif\n"
        )
        if not check_seam_production_exclusion(t):
            failures.append("self-test: non-macro guard admitted a seam include")

        # The production sluice_async target must not define the macro nor
        # gain the src/async seam include path (implicit-scope region check;
        # add_files of src/async/*.cpp is legitimate and must not fire).
        # Reset the header fixture to a clean macro-guarded include first so
        # the whole-tree scan only reflects the prod.lua under test.
        bad.write_text(
            "#if defined(SLUICE_ASYNC_INTERNAL_TESTING)\n"
            '#include "scheduler_test_access.hpp"\n'
            "#endif\n"
        )
        prod = t / "xmake" / "prod.lua"
        prod.write_text(
            'target("sluice_async")\n'
            '    add_defines("SLUICE_ASYNC_INTERNAL_TESTING", {public = true})\n'
        )
        if not check_seam_production_exclusion(t):
            failures.append("self-test: production macro define failed to fire")
        prod.write_text(
            'target("sluice_async")\n'
            '    add_includedirs(R .. "src/async", {public = true})\n'
        )
        if not check_seam_production_exclusion(t):
            failures.append("self-test: production seam include path failed to fire")
        prod.write_text(
            'target("sluice_async")\n'
            '    add_includedirs(R .. "include", {public = true})\n'
            '    add_files(R .. "src/async/*.cpp")\n'
            'local s = function()\n'
            '    return { R .. "src/async/*.cpp" }\n'
            'end\n'
            'target("sluice_async_internal_testing")\n'
            '    add_includedirs(R .. "include", R .. "src/async", '
            '{public = true})\n'
            '    add_defines("SLUICE_ASYNC_INTERNAL_TESTING", {public = true})\n'
        )
        if check_seam_production_exclusion(t):
            failures.append("self-test: clean production target false positive")

        # G (app seams, #142 review): an app source compiled by a production
        # app target may include an app seam header ONLY inside the family's
        # internal-testing guard; the production sluice-copy target must
        # never define the macro nor list a seam header; a TEST target may
        # define the macro freely.
        (t / "apps" / "sluice-copy").mkdir(parents=True)
        app = t / "apps" / "sluice-copy" / "safe_output.cpp"
        app.write_text('#include "safe_output_test_seams.hpp"\n')
        if not check_seam_production_exclusion(t):
            failures.append("self-test: unguarded app seam include failed to fire")
        app.write_text(
            "#ifdef SLUICE_COPY_INTERNAL_TESTING\n"
            '#include "safe_output_test_seams.hpp"\n'
            "#endif\n"
        )
        if check_seam_production_exclusion(t):
            failures.append("self-test: guarded app seam include false positive")
        app_prod = t / "xmake" / "app_prod.lua"
        app_prod.write_text(
            'target("sluice-copy")\n'
            '    add_defines("SLUICE_COPY_INTERNAL_TESTING")\n'
        )
        if not check_seam_production_exclusion(t):
            failures.append("self-test: production app macro define failed to fire")
        app_prod.write_text(
            'target("sluice-copy")\n'
            '    add_files(R .. "apps/sluice-copy/safe_output.cpp", '
            'R .. "apps/sluice-copy/safe_output_test_seams.hpp")\n'
        )
        if not check_seam_production_exclusion(t):
            failures.append("self-test: production app seam source failed to fire")
        app_prod.write_text(
            'target("sluice-copy")\n'
            '    add_files(R .. "apps/sluice-copy/safe_output.cpp")\n'
            'target("sluice_copy_safe_output_test")\n'
            '    add_defines("SLUICE_COPY_INTERNAL_TESTING")\n'
        )
        if check_seam_production_exclusion(t):
            failures.append("self-test: clean app targets false positive")
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
          "SHA refs, tracker refs, test totals, seam production exclusion)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
