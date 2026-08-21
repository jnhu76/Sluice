#!/usr/bin/env python3
r"""Regenerate E12RwLockNegWriterRevoke.tla from the positive E12RwLock.tla.

NEG-RW3 derivation law (audit #162 MODEL-002 negative control):

    1. MODULE name: E12RwLock -> E12RwLockNegWriterRevoke (header dashes
       preserved verbatim; SANY accepts any dash line length).
    2. Insert the NEG-RW3 provenance comment block after the module's first
       comment line.
    3. In BOTH reconcile actions (CancelQueued, ExpireQueued), drop the
       `activeReaders = 0` conjunct from the writer-grant ELSE-IF guard (the
       C++ `if (active_readers > 0 || writer_active) return;` guard at
       scheduler_rwlock.cpp:119), so a head writer can be granted while a
       live reader set still holds — the reader-revocation hazard.

TLC 2.19 constraint (empirically verified, tla2tools 1.7.4): an assignment
whose right-hand side is a TOP-LEVEL disjunction, e.g.

    x' = y \/ (S # {})

makes TLC report "Successor state is not completely specified ... variable
is not assigned" exactly when the branch becomes enabled with the
non-trivial operand — the assignment conjunct is split like a disjunct and
the second half contributes no assignment. The equivalent IF-form

    x' = IF S = {} THEN y ELSE TRUE

is semantically identical and TLC-safe (IF-THEN-ELSE assignments are the
idiom used everywhere in this model, e.g. nodeState'). The positive model
therefore uses the IF-form, and this script FAILS CLOSED if the source ever
regresses to the \/-form: a stale generation input is an error, not a
silent regeneration.

The guard drop is an exact full-line text replacement (never a regex over
`*` or `-`): the `\*` comment lines and the arithmetic `-` operators in the
source must pass through untouched. Exactly two guard lines must match;
anything else aborts.

Usage:
    python3 scripts/formal/gen-rwlock-neg-writer-revoke.py

Exit status: 0 and only then is E12RwLockNegWriterRevoke.tla written.
"""
from __future__ import annotations

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SPEC = HERE.parent.parent / "spec" / "tla" / "e12_rwlock"
SRC = SPEC / "E12RwLock.tla"
DST = SPEC / "E12RwLockNegWriterRevoke.tla"

MODULE_LINE = "------------------------------- MODULE E12RwLock -------------------------------"
NEG_NAME = "E12RwLockNegWriterRevoke"

# Inserted after the module's first comment line (the `\* sluice::...` line).
NEG_HEADER = """\\*
\\* NEGATIVE MODEL (audit-added, NEG-RW3): this variant restores the audit
\\* MODEL-002 hazard — the cancel/expire writer-reconcile branch drops the C++
\\* `activeReaders = 0` guard (scheduler_rwlock.cpp:119
\\* `if (active_readers > 0 || writer_active) return;`), so a head writer can
\\* be granted while a live reader still holds, silently revoking it. Expected
\\* TLC verdict: VIOLATION of ReaderRevocationFree (cfg:
\\* E12RwLockNegWriterRevoke.cfg). All other laws remain intact so the named
\\* check is exact."""

# The exact full guard line (16-space indent), dropped in both actions.
GUARD_LINE = '                    mode[Head(newQ)] = "write" /\\ activeReaders = 0'
GUARD_DROP = '                    mode[Head(newQ)] = "write"'

# The TLC-unsafe ghost form must NEVER be present in the source.
BROKEN_GHOST = r"revocationOccurred' = revocationOccurred \/ (grantedReaders # {})"
# The canonical IF-form ghost, one per writer-grant branch.
GOOD_GHOST = "revocationOccurred' =\n                    IF grantedReaders = {} THEN revocationOccurred ELSE TRUE"
FIRST_COMMENT = "\\* sluice::async::AsyncRwLock -- writer-fair phase-batched RwLock SAFETY model"


def fail(msg: str) -> None:
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def main() -> int:
    if not SRC.is_file():
        fail(f"source model not found: {SRC}")
    text = SRC.read_text(encoding="utf-8")

    # --- Preconditions: the source must be the post-repair positive model ---
    lines = text.splitlines()
    if not lines or lines[0] != MODULE_LINE:
        fail("source header mismatch; expected " + MODULE_LINE)
    if BROKEN_GHOST in text:
        fail(
            "source still uses the TLC-2.19-unsafe \\/-form ghost "
            "`x' = y \\/ (S # {})`; convert it to the IF-form first"
        )
    if text.count(GOOD_GHOST) != 2:
        fail(f"expected exactly 2 IF-form ghost assignments, found {text.count(GOOD_GHOST)}")
    if text.count(GUARD_LINE) != 2:
        fail(f"expected exactly 2 writer-grant guard lines, found {text.count(GUARD_LINE)}")
    if not any(line == FIRST_COMMENT for line in lines):
        fail("first comment line not found; insertion point is ambiguous")

    # --- Derivation ---
    out = text.replace(GUARD_LINE, GUARD_DROP)
    out = out.replace(MODULE_LINE, MODULE_LINE.replace("MODULE E12RwLock", f"MODULE {NEG_NAME}"), 1)
    out = out.replace(FIRST_COMMENT, FIRST_COMMENT + "\n" + NEG_HEADER, 1)

    # --- Postconditions: the derivation law is EXACT -----------------------
    # Round-trip the derived text back through the inverse substitutions and
    # require a byte-identical copy of the source.
    back = out.replace(GUARD_DROP, GUARD_LINE)
    back = back.replace(
        MODULE_LINE.replace("MODULE E12RwLock", f"MODULE {NEG_NAME}"),
        MODULE_LINE,
        1,
    )
    back = back.replace(FIRST_COMMENT + "\n" + NEG_HEADER, FIRST_COMMENT, 1)
    if back != text:
        fail("round-trip check failed: derived file is not the exact inverse of the source")
    if out.count(GUARD_DROP) != 2:
        fail(f"guard drop did not apply twice; found {out.count(GUARD_DROP)}")

    DST.write_text(out, encoding="utf-8")
    print(f"regenerated {DST.relative_to(HERE.parent.parent)} "
          f"({MODULE_LINE.count('-')}-dash header, 2 guard drops, IF-form ghost)")
    return 0


if __name__ == "__main__":
    sys.exit(main())