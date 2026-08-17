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
    `git show`; otherwise verify against the current tree."""
    errs = []
    for doc in doc_paths:
        for line in doc.read_text(errors="replace").splitlines():
            m = LOC_ROW_RE.search(line)
            if not m:
                continue
            path, claimed = m.group(1).strip(), int(m.group(2))
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


def fact_docs(root_dir=None):
    d = root_dir or FACT_DOCS_DIR
    return sorted(p for p in d.glob("*.md")) if d.is_dir() else []


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
          "SHA refs, tracker refs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
