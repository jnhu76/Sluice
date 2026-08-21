#!/usr/bin/env python3
r"""Regenerate E12RwLockNegDeadlinePrecedence.tla from the positive E12RwLock.tla.

NEG-RW4 derivation law (audit #162 MODEL-003 negative control; PR #168 review
closeout):

    1. MODULE name: E12RwLock -> E12RwLockNegDeadlinePrecedence (header dashes
       preserved verbatim; SANY accepts any dash line length).
    2. Insert the NEG-RW4 provenance comment block after the module's first
       comment line.
    3. In BOTH timed-admission actions (ReadUntilAdmit, WriteUntilAdmit),
       flip the inline resolution from "Woken" to "Expired" — the precedence
       inversion: an already-due deadline wins over an admissible resource
       (the C++ order is the opposite: the resource claim at
       rwlock_{read,write}_lock_until precedence 1 runs BEFORE the due
       recheck at precedence 2). The mutation changes ONLY the outcome: the
       evidence latches stay exactly as the positive model sets them
       (admissionSawResource = TRUE; admissionSawDue = the environment's
       due bit), so the mutant cannot make InvResourceFirstDeadline
       self-proving by erasing the evidence. Parity with E12Semaphore
       E12SemNeg7DeadlinePrecedence (NEG-SEM-7) and the E12AsyncMutex M7
       control.

The anchor is the exact five-line block (the \E due input latch followed by
the mode/nodeState assignments) of each *UntilAdmit action — the ONLY two
places where an environment-chosen due bit and a Woken resolution co-occur.
Exactly one read anchor and one write anchor must match; anything else
aborts. The replacement is a whole-block text substitution (never a regex),
and a round-trip check proves the derived file is the exact inverse of the
source.

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
\\* NEGATIVE MODEL (audit-added, NEG-RW4): this variant inverts the audit
\\* MODEL-003 precedence — the timed admission actions resolve "Expired" when
\\* the deadline is already due EVEN THOUGH the resource is admissible
\\* (deadline beats resource; the C++ rwlock_{read,write}_lock_until order is
\\* the opposite: the resource claim is precedence 1). The mutation changes
\\* ONLY the outcome: admissionSawResource stays TRUE and admissionSawDue
\\* keeps the environment's due bit, so a due=TRUE admission leaves
\\* TRUE/TRUE evidence with an Expired resolution. Expected TLC verdict:
\\* VIOLATION of InvResourceFirstDeadline (cfg:
\\* E12RwLockNegDeadlinePrecedence.cfg). All other laws remain intact so the
\\* named check is exact."""

# The exact five-line resolution blocks. The `\E due` input latch exists only
# in the two *UntilAdmit actions, so each anchor is unique in the source.
DUE_LATCH = (
    "    /\\ \\E due \\in BOOLEAN :\n"
    "        /\\ deadlineDue' = [deadlineDue EXCEPT ![e] = due]\n"
    "        /\\ admissionSawDue' = [admissionSawDue EXCEPT ![e] = due]\n"
)
READ_OK = DUE_LATCH + (
    "    /\\ mode' = [mode EXCEPT ![e] = \"read\"]\n"
    "    /\\ nodeState' = [nodeState EXCEPT ![e] = \"Woken\"]"
)
READ_MUT = DUE_LATCH + (
    "    /\\ mode' = [mode EXCEPT ![e] = \"read\"]\n"
    "    /\\ nodeState' = [nodeState EXCEPT ![e] = \"Expired\"]"
)
WRITE_OK = DUE_LATCH + (
    "    /\\ mode' = [mode EXCEPT ![e] = \"write\"]\n"
    "    /\\ nodeState' = [nodeState EXCEPT ![e] = \"Woken\"]"
)
WRITE_MUT = DUE_LATCH + (
    "    /\\ mode' = [mode EXCEPT ![e] = \"write\"]\n"
    "    /\\ nodeState' = [nodeState EXCEPT ![e] = \"Expired\"]"
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
        fail("outcome flip did not apply exactly once per timed-admission action")
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
        f"({MODULE_LINE.count('-')}-dash header, 2 outcome flips, evidence latches intact)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
