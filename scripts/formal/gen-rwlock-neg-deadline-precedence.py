#!/usr/bin/env python3
r"""Regenerate E12RwLockNegDeadlinePrecedence.tla from the positive E12RwLock.tla.

NEG-RW4 derivation law (audit #162 MODEL-003 negative control; PR #168 review
closeout, narrowed after adversarial review):

    1. MODULE name: E12RwLock -> E12RwLockNegDeadlinePrecedence (header dashes
       preserved verbatim; SANY accepts any dash line length).
    2. Insert the NEG-RW4 provenance comment block after the module's first
       comment line.
    3. In BOTH timed-admission actions (ReadUntilAdmit, WriteUntilAdmit),
       restructure the body so the environment-chosen due bit selects the
       disposition:
         - due = FALSE : the successor is EXACTLY the positive model's
           (Woken, ownership committed) — behavior unchanged;
         - due = TRUE  : DEFECT — the already-due deadline wins over the
           admissible resource (the C++ order is the opposite: the resource
           claim at rwlock_{read,write}_lock_until precedence 1 runs before
           the due recheck at precedence 2) and NO ownership is committed
           (no reader grant, no activeReaders increment, no writerOwner
           install), so the mutant cannot manufacture collateral states such
           as an Expired node that still owns the writer lock.
       The evidence latches are untouched (admissionSawResource = TRUE;
       admissionSawDue = the environment's due bit), so the mutant cannot
       make InvResourceFirstDeadline self-proving by erasing evidence.
       The common assignments (mode', resolutionCount', admissionSawResource',
       the action's UNCHANGED tail) are hoisted BEFORE the IF and the IF is
       the action's last conjunct, so every variable is assigned exactly once
       on both successors under any reading of TLA+ quantifier/IF scope.
       Parity with E12Semaphore E12SemNeg7DeadlinePrecedence (NEG-SEM-7).

Specificity law (the negative is EXACT): E12RwLockNegDeadlinePrecedence.cfg
checks that ONLY InvResourceFirstDeadline is violated, and the sibling
E12RwLockNegDeadlinePrecedence.specificity.cfg checks the SAME mutant against
the remaining 12 positive invariants and must PASS — proving the negative
fails for the deadline-precedence defect and nothing else.

The anchor is the exact block from the `\E due \in BOOLEAN :` line through
the action's final UNCHANGED conjunct of each *UntilAdmit action — the ONLY
two places where an environment-chosen due bit and a Woken resolution
co-occur. Exactly one read anchor and one write anchor must match; anything
else aborts. The replacement is a whole-block text substitution (never a
regex), and a round-trip check proves the derived file is the exact inverse
of the source.

Usage:
    python3 scripts/formal/gen-rwlock-neg-deadline-precedence.py
    python3 scripts/formal/gen-rwlock-neg-deadline-precedence.py --check

Exit status: 0 in write mode only if E12RwLockNegDeadlinePrecedence.tla was
regenerated; in --check mode only if the committed negative is byte-identical
to what the current positive model would generate (stale -> non-zero, and the
repository is left untouched).
"""
from __future__ import annotations

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SPEC = HERE.parent.parent / "spec" / "tla" / "e12_rwlock"
SRC = SPEC / "E12RwLock.tla"
DST = SPEC / "E12RwLockNegDeadlinePrecedence.tla"

MODULE_LINE = "------------------------------- MODULE E12RwLock -------------------------------"
NEG_NAME = "E12RwLockNegDeadlinePrecedence"

# Inserted after the module's first comment line (the `\* sluice::...` line).
NEG_HEADER = """\\*
\\* NEGATIVE MODEL (audit-added, NEG-RW4): this variant ISOLATES the audit
\\* MODEL-003 precedence defect. In ReadUntilAdmit/WriteUntilAdmit, only when
\\* the environment chose due = TRUE (deadline already due AND the resource
\\* admissible) does the admission wrongly resolve Expired and commit NO
\\* ownership (no reader grant, no writerOwner install) — the C++
\\* rwlock_{read,write}_lock_until order is the opposite (the resource claim
\\* is precedence 1). The due = FALSE successor is exactly the positive
\\* model's behavior. Expected TLC verdicts: VIOLATION of
\\* InvResourceFirstDeadline ONLY (cfg E12RwLockNegDeadlinePrecedence.cfg)
\\* and PASS of the remaining 12 positive invariants on this same mutant
\\* (cfg E12RwLockNegDeadlinePrecedence.specificity.cfg) — the negative is
\\* exact: it fails for the deadline-precedence defect and nothing else."""

# The exact resolution blocks, from the `\E due` input latch through the
# action's final UNCHANGED conjunct (the IF replacement keeps that tail and
# appends the due-selected disposition as the action's last conjunct).
DUE_LATCH = (
    "    /\\ \\E due \\in BOOLEAN :\n"
    "        /\\ deadlineDue' = [deadlineDue EXCEPT ![e] = due]\n"
    "        /\\ admissionSawDue' = [admissionSawDue EXCEPT ![e] = due]\n"
)
READ_OK = DUE_LATCH + (
    "    /\\ mode' = [mode EXCEPT ![e] = \"read\"]\n"
    "    /\\ nodeState' = [nodeState EXCEPT ![e] = \"Woken\"]\n"
    "    /\\ resolutionCount' = [resolutionCount EXCEPT ![e] = 1]\n"
    "    /\\ grantedReaders' = grantedReaders \\cup {e}\n"
    "    /\\ activeReaders' = activeReaders + 1\n"
    "    /\\ admissionSawResource' = [admissionSawResource EXCEPT ![e] = TRUE]\n"
    "    /\\ UNCHANGED <<writerOwner, queue, publicationCount, bargingOccurred,\n"
    "                   writerWasQueued, revocationOccurred>>"
)
READ_MUT = DUE_LATCH + (
    "        /\\ mode' = [mode EXCEPT ![e] = \"read\"]\n"
    "        /\\ resolutionCount' = [resolutionCount EXCEPT ![e] = 1]\n"
    "        /\\ admissionSawResource' = [admissionSawResource EXCEPT ![e] = TRUE]\n"
    "        /\\ UNCHANGED <<writerOwner, queue, publicationCount, bargingOccurred,\n"
    "                       writerWasQueued, revocationOccurred>>\n"
    "        /\\ IF due THEN\n"
    "             \\* DEFECT (NEG-RW4): the due deadline wins over the admissible\n"
    "             \\* resource and NO ownership is committed.\n"
    "             /\\ nodeState' = [nodeState EXCEPT ![e] = \"Expired\"]\n"
    "             /\\ UNCHANGED <<grantedReaders, activeReaders>>\n"
    "         ELSE\n"
    "             \\* correct precedence 1: the resource claim wins.\n"
    "             /\\ nodeState' = [nodeState EXCEPT ![e] = \"Woken\"]\n"
    "             /\\ grantedReaders' = grantedReaders \\cup {e}\n"
    "             /\\ activeReaders' = activeReaders + 1"
)
WRITE_OK = DUE_LATCH + (
    "    /\\ mode' = [mode EXCEPT ![e] = \"write\"]\n"
    "    /\\ nodeState' = [nodeState EXCEPT ![e] = \"Woken\"]\n"
    "    /\\ resolutionCount' = [resolutionCount EXCEPT ![e] = 1]\n"
    "    /\\ writerOwner' = e\n"
    "    /\\ admissionSawResource' = [admissionSawResource EXCEPT ![e] = TRUE]\n"
    "    /\\ UNCHANGED <<activeReaders, queue, publicationCount, grantedReaders,\n"
    "                   bargingOccurred, writerWasQueued, revocationOccurred>>"
)
WRITE_MUT = DUE_LATCH + (
    "        /\\ mode' = [mode EXCEPT ![e] = \"write\"]\n"
    "        /\\ resolutionCount' = [resolutionCount EXCEPT ![e] = 1]\n"
    "        /\\ admissionSawResource' = [admissionSawResource EXCEPT ![e] = TRUE]\n"
    "        /\\ UNCHANGED <<activeReaders, queue, publicationCount, grantedReaders,\n"
    "                       bargingOccurred, writerWasQueued, revocationOccurred>>\n"
    "        /\\ IF due THEN\n"
    "             \\* DEFECT (NEG-RW4): the due deadline wins over the admissible\n"
    "             \\* resource and NO ownership is committed.\n"
    "             /\\ nodeState' = [nodeState EXCEPT ![e] = \"Expired\"]\n"
    "             /\\ UNCHANGED <<writerOwner>>\n"
    "         ELSE\n"
    "             \\* correct precedence 1: the resource claim wins.\n"
    "             /\\ nodeState' = [nodeState EXCEPT ![e] = \"Woken\"]\n"
    "             /\\ writerOwner' = e"
)
FIRST_COMMENT = "\\* sluice::async::AsyncRwLock -- writer-fair phase-batched RwLock SAFETY model"


def fail(msg: str) -> None:
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def derive(text: str) -> str:
    """Apply the NEG-RW4 derivation law; fail closed on any anchor drift."""
    lines = text.splitlines()
    if not lines or lines[0] != MODULE_LINE:
        fail("source header mismatch; expected " + MODULE_LINE)
    if text.count(READ_OK) != 1:
        fail(f"expected exactly 1 ReadUntilAdmit resolution block, found {text.count(READ_OK)}")
    if text.count(WRITE_OK) != 1:
        fail(f"expected exactly 1 WriteUntilAdmit resolution block, found {text.count(WRITE_OK)}")
    if not any(line == FIRST_COMMENT for line in lines):
        fail("first comment line not found; insertion point is ambiguous")

    out = text.replace(READ_OK, READ_MUT, 1)
    out = out.replace(WRITE_OK, WRITE_MUT, 1)
    out = out.replace(MODULE_LINE, MODULE_LINE.replace("MODULE E12RwLock", f"MODULE {NEG_NAME}"), 1)
    out = out.replace(FIRST_COMMENT, FIRST_COMMENT + "\n" + NEG_HEADER, 1)

    # Round-trip the derived text back through the inverse substitutions and
    # require a byte-identical copy of the source.
    back = out.replace(READ_MUT, READ_OK, 1)
    back = back.replace(WRITE_MUT, WRITE_OK, 1)
    back = back.replace(
        MODULE_LINE.replace("MODULE E12RwLock", f"MODULE {NEG_NAME}"),
        MODULE_LINE,
        1,
    )
    back = back.replace(FIRST_COMMENT + "\n" + NEG_HEADER, FIRST_COMMENT, 1)
    if back != text:
        fail("round-trip check failed: derived file is not the exact inverse of the source")
    if out.count(READ_MUT) != 1 or out.count(WRITE_MUT) != 1:
        fail("disposition split did not apply exactly once per timed-admission action")
    return out


def main() -> int:
    check_only = "--check" in sys.argv[1:]
    if not SRC.is_file():
        fail(f"source model not found: {SRC}")
    out = derive(SRC.read_text(encoding="utf-8"))

    if check_only:
        if not DST.is_file():
            fail(f"generated negative is missing: {DST.relative_to(HERE.parent.parent)}")
        committed = DST.read_text(encoding="utf-8")
        if committed != out:
            print(
                "error: generated negative is stale; regenerate it:\n"
                f"    python3 {Path(__file__).name}",
                file=sys.stderr,
            )
            return 1
        print(f"fresh: {DST.relative_to(HERE.parent.parent)} matches the current positive model")
        return 0

    DST.write_text(out, encoding="utf-8")
    print(
        f"regenerated {DST.relative_to(HERE.parent.parent)} "
        f"({MODULE_LINE.count('-')}-dash header, 2 due-split dispositions, "
        f"evidence latches intact, no ownership in the defect branch)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
