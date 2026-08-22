#!/usr/bin/env python3
"""Generate NEG-M1..NEG-M11 from the correct E12AsyncMutex model.

Each NEG is a self-contained TLA+ module identical to E12AsyncMutex EXCEPT for
one single-rule defect (a modified action precondition or effect). The rest is
copied verbatim with the module renamed. A header comment records the defect +
the expected violated invariant.

The committed generated negatives are freshness-gated: `--check` is part of
formal verification. `scripts/formal/verify-async-mutex.sh` runs
`_gen_neg.py --check` BEFORE any TLC invocation, so a committed negative that
went stale after a positive-model or generator edit fails the formal gate
instead of silently checking an outdated mutation while CI stays green.

Usage:
    python3 _gen_neg.py            # regenerate the 22 committed artifacts
    python3 _gen_neg.py --check    # read-only freshness verification

`--check` renders the expected bytes in memory and compares them against the
committed files; it never writes to the working tree. It FAILS (non-zero exit)
on: a stale generated .tla/.cfg, a missing generated file, an unexpected
E12AsyncMutexNeg* artifact not produced by the current CASES, any positive-model
anchor drift (an action signature matching zero or multiple definition lines),
a malformed CASE entry, or a defect body whose signature line does not name the
action it replaces (which would silently leave the source action intact).

Expected violated invariant per NEG (matches docs/history/closeout/e12-async-mutex.md §16):
  NEG-M1  NonOwnerUnlock              -> InvUnlockAuthority
  NEG-M2  RecursiveAcquire            -> InvRecursiveForbidden
  NEG-M3  NonFIFOGrant                -> InvFIFOGrant
  NEG-M4  HandoffFreeWindow           -> InvNoOwnerlessQueuedDemand
  NEG-M5  GrantWithoutOwnerCommit     -> InvGrantOwnerCommit
  NEG-M6  PublicationWithoutGrantCoupling -> InvGrantPublicationCoupling
  NEG-M7  AdmissionClosureFailure     -> InvAdmissionClosure
  NEG-M8  CancelRevokesHandoff        -> InvGrantFinality
  NEG-M9  DeadlineRevokesHandoff      -> InvGrantFinality
  NEG-M10 ImmediatePublication        -> InvPublicationRequiresSuspensionOrHandoff
  NEG-M11 DestructionWhileOwnedOrQueued -> InvDestructionPrecondition
"""
from __future__ import annotations

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
POSITIVE = HERE / "E12AsyncMutex.tla"
POSITIVE_MODULE = "E12AsyncMutex"
ARTIFACT_GLOB = "E12AsyncMutexNeg*"

CFG = """\
SPECIFICATION Spec
INVARIANT {inv}

CONSTANTS
F1 = F1
F2 = F2
F3 = F3
Fibers = {{F1, F2, F3}}
E1 = E1
E2 = E2
E3 = E3
Epochs = {{E1, E2, E3}}
"""


def fail(msg: str) -> None:
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


def render_action(text: str, name: str, new_body: str) -> str:
    """Replace the action definition `name == ...` with new_body (which must
    include the `name ==` line). The action block runs until the next top-level
    definition, a `----` separator, or a line comment introducing the next
    action. We anchor on the action signature line; the block ends at the first
    subsequent line that is a top-level definition (`X ==` or `X(...`) or a
    `----` rule.

    Fail-closed: the signature line must match EXACTLY ONE line of the source;
    zero matches or multiple matches is positive-model anchor drift and aborts
    the generation instead of silently mutating the wrong block."""
    lines = text.split("\n")
    starts = [i for i, ln in enumerate(lines)
              if ln.startswith(name + "(") or ln.startswith(name + " ==")]
    if len(starts) != 1:
        fail(f"positive-model anchor drift: action {name!r} matched "
             f"{len(starts)} signature lines (expected exactly 1) in {POSITIVE.name}")
    start = starts[0]
    end = len(lines)
    for j in range(start + 1, len(lines)):
        ln = lines[j]
        if ln.startswith("----"):
            end = j
            break
        if ln and not ln[0].isspace() and (" ==" in ln or "(" in ln) \
                and not ln.startswith("\\*"):
            end = j
            break
    return "\n".join(lines[:start] + new_body.split("\n") + lines[end:])


def render_case(modname: str, header_desc: str, edits: list, expected_inv: str,
                positive_text: str) -> dict:
    """Render one NEG case into its expected artifact bytes (in memory).

    edits: list of (action_name, defect_body) applied in order."""
    n = positive_text.count(f"MODULE {POSITIVE_MODULE}")
    if n != 1:
        fail(f"positive-model anchor drift: 'MODULE {POSITIVE_MODULE}' found "
             f"{n} times (expected exactly 1) in {POSITIVE.name}")
    text = positive_text.replace(f"MODULE {POSITIVE_MODULE}", f"MODULE {modname}", 1)
    for action_name, defect_body in edits:
        first = defect_body.split("\n", 1)[0]
        if not (first.startswith(action_name + "(")
                or first.startswith(action_name + " ==")):
            fail(f"malformed CASE metadata: defect body for {modname} targets "
                 f"action {action_name!r} but its first line defines {first.split('(')[0]!r}")
        text = render_action(text, action_name, defect_body)
    if "EXTENDS" not in text:
        fail(f"positive-model anchor drift: no EXTENDS clause survives in {modname}")
    body = text[text.index("EXTENDS"):]
    header = (
        f"------------------------------- MODULE {modname} "
        f"-------------------------------\n(*\n  {header_desc}\n"
        f"  Expected violated property: {expected_inv}.\n"
        f"  Single-rule difference(s) from E12AsyncMutex noted below. Everything "
        f"else is identical.\n*)\n"
    )
    return {
        f"{modname}.tla": header + body,
        f"{modname}.cfg": CFG.format(inv=expected_inv),
    }


def validate_cases(cases: list) -> None:
    """Fail-closed CASES metadata check: unique module names (a duplicated
    entry would silently overwrite one artifact with another's bytes),
    non-empty description/invariant, and at least one edit per case."""
    seen: set[str] = set()
    for modname, desc, edits, inv in cases:
        if not modname.startswith(POSITIVE_MODULE + "Neg"):
            fail(f"malformed CASE metadata: module {modname!r} does not extend "
                 f"the positive module name")
        if modname in seen:
            fail(f"unexpected CASE duplication: {modname} appears twice in CASES")
        seen.add(modname)
        if not desc or not inv:
            fail(f"malformed CASE metadata: empty description or invariant for {modname}")
        if not edits:
            fail(f"malformed CASE metadata: {modname} has no edits")
        names = [action_name for action_name, _ in edits]
        if len(set(names)) != len(names):
            fail(f"malformed CASE metadata: {modname} edits the same action twice")


def expected_artifacts(cases: list, positive_text: str) -> dict:
    artifacts: dict[str, str] = {}
    for modname, desc, edits, inv in cases:
        rendered = render_case(modname, desc, edits, inv, positive_text)
        clash = artifacts.keys() & rendered.keys()
        if clash:
            fail(f"unexpected CASE duplication: {sorted(clash)} rendered twice")
        artifacts.update(rendered)
    return artifacts


def write_artifacts(artifacts: dict) -> None:
    for name, content in sorted(artifacts.items()):
        (HERE / name).write_text(content, encoding="utf-8")
    print(f"regenerated {len(artifacts)} artifacts "
          f"({len(artifacts) // 2} NEG modules, .tla + .cfg) in {HERE.name}/")


def check_artifacts(artifacts: dict) -> int:
    """Read-only freshness verification: compare the committed files against
    the in-memory render. NEVER writes to the working tree. Returns 0 iff
    every committed generated artifact is byte-identical to the render and no
    unexpected E12AsyncMutexNeg* artifact exists."""
    problems: list[str] = []
    expected_names = set(artifacts)
    for p in sorted(HERE.glob(ARTIFACT_GLOB)):
        if p.name not in expected_names:
            problems.append(f"unexpected generated artifact {p.name} "
                            f"(not produced by the current CASES)")
    for name, content in sorted(artifacts.items()):
        path = HERE / name
        if not path.is_file():
            problems.append(f"missing generated artifact {name}")
            continue
        if path.read_text(encoding="utf-8") != content:
            problems.append(f"stale generated artifact {name}")
    if problems:
        print("error: generated-negative freshness check FAILED:", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        print("regenerate with: python3 spec/tla/e12_async_mutex/_gen_neg.py",
              file=sys.stderr)
        return 1
    print(f"fresh: all {len(artifacts)} generated artifacts "
          f"({len(artifacts) // 2} NEG modules) match the current positive "
          f"model + generator")
    return 0


# ---------------------------------------------------------------------------
# Each defect is a SINGLE focused rule change. The defect body is the FULL
# replacement action (with the injected defect) so the generator does a clean
# whole-action substitution.
#
# NEG-M4 and NEG-M5 intentionally share the same defect CLASS (UnlockHandoff
# leaves owner = NoOwner) but carry DIFFERENT detection roles and must not be
# merged: M4 exercises the state property InvNoOwnerlessQueuedDemand (an
# ownerless mutex must not have eligible queued demand) while M5 exercises the
# history-backed transition property InvGrantOwnerCommit (a handoff must
# commit owner to the winner). See closeout §16 for the distinct CEX shapes.

# NEG-M1 NonOwnerUnlock: UnlockNoWaiter drops the `owner = actor` precondition,
# so a foreign Fiber can unlock. Expected: InvUnlockAuthority.
NEGM1 = r'''UnlockNoWaiter(actor) ==
    /\ ~destroyed
    /\ FIFOHead = None
    /\ owner' = NoOwner
    /\ Mark("UnlockNoWaiter", actor, None, None)
    /\ UNCHANGED <<queue, nodeState, epochFiber, deadlineDue, runnablePublished,
                   resolutionCount, publicationCount, destroyed,
                   admissionSawFree, admissionSawDue>>
'''

# NEG-M2 RecursiveAcquire: TryLockSuccess drops the owner = NoOwner precondition,
# so a recursive acquire can succeed while already owned by the actor.
# Expected: InvRecursiveForbidden.
NEGM2 = r'''TryLockSuccess(actor) ==
    /\ ~destroyed
    /\ FIFOHead = None
    /\ owner' = actor
    /\ Mark("TryLockSuccess", actor, None, None)
    /\ UNCHANGED <<queue, nodeState, epochFiber, deadlineDue, runnablePublished,
                   resolutionCount, publicationCount, destroyed,
                   admissionSawFree, admissionSawDue>>
'''

# NEG-M3 NonFIFOGrant: when at least two eligible waiters are queued, UnlockHandoff
# grants the SECOND eligible epoch instead of the FIFO head. (Triggered only with
# >= 2 eligible waiters, so single-head grants are unaffected.) The real FIFO head
# is latched into expectedFIFOHead; lastGrantedEpoch is the wrong epoch, so
# InvFIFOGrant fails. Expected: InvFIFOGrant.
NEGM3 = r'''UnlockHandoff(actor) ==
    /\ ~destroyed
    /\ owner = actor
    /\ Len(EligibleQueue) >= 2
    /\ expectedFIFOHead' = Head(EligibleQueue)
    /\ LET w == EligibleQueue[2] IN
       /\ nodeState[w] = "Registered"
       /\ nodeState' = [nodeState EXCEPT ![w] = "Woken"]
       /\ resolutionCount' = [resolutionCount EXCEPT ![w] = 1]
       /\ owner' = epochFiber[w]
       /\ runnablePublished' = [runnablePublished EXCEPT ![w] = TRUE]
       /\ publicationCount' = [publicationCount EXCEPT ![w] = 1]
       /\ queue' = RemoveFromQueue(queue, w)
       /\ lastAction' = "UnlockHandoff"
       /\ lastActor' = actor
       /\ lastTargetEpoch' = w
       /\ lastGrantedEpoch' = w
       /\ SnapPre
    /\ UNCHANGED <<epochFiber, deadlineDue, destroyed,
                   admissionSawFree, admissionSawDue>>
'''

# NEG-M4 HandoffFreeWindow: UnlockHandoff resolves the FIFO head Woken and
# publishes but does NOT commit ownership (owner := NoOwner). This creates a
# free window: owner = NoOwner while an eligible queued waiter remains.
# Expected: InvNoOwnerlessQueuedDemand.
NEGM4 = r'''UnlockHandoff(actor) ==
    /\ ~destroyed
    /\ owner = actor
    /\ FIFOHead # None
    /\ nodeState[FIFOHead] = "Registered"
    /\ epochFiber[FIFOHead] # None
    /\ expectedFIFOHead' = FIFOHead
    /\ LET w == FIFOHead IN
       /\ nodeState' = [nodeState EXCEPT ![w] = "Woken"]
       /\ resolutionCount' = [resolutionCount EXCEPT ![w] = 1]
       /\ owner' = NoOwner               \* DEFECT: no owner commit to winner
       /\ runnablePublished' = [runnablePublished EXCEPT ![w] = TRUE]
       /\ publicationCount' = [publicationCount EXCEPT ![w] = 1]
       /\ queue' = RemoveFromQueue(queue, w)
       /\ lastAction' = "UnlockHandoff"
       /\ lastActor' = actor
       /\ lastTargetEpoch' = w
       /\ lastGrantedEpoch' = w
       /\ SnapPre
    /\ UNCHANGED <<epochFiber, deadlineDue, destroyed,
                   admissionSawFree, admissionSawDue>>
'''

# NEG-M5 GrantWithoutOwnerCommit: UnlockHandoff does NOT commit owner to the
# winner (leaves owner = NoOwner). Expected: InvGrantOwnerCommit.
NEGM5 = r'''UnlockHandoff(actor) ==
    /\ ~destroyed
    /\ owner = actor
    /\ FIFOHead # None
    /\ nodeState[FIFOHead] = "Registered"
    /\ epochFiber[FIFOHead] # None
    /\ expectedFIFOHead' = FIFOHead
    /\ LET w == FIFOHead IN
       /\ nodeState' = [nodeState EXCEPT ![w] = "Woken"]
       /\ resolutionCount' = [resolutionCount EXCEPT ![w] = 1]
       /\ owner' = NoOwner
       /\ runnablePublished' = [runnablePublished EXCEPT ![w] = TRUE]
       /\ publicationCount' = [publicationCount EXCEPT ![w] = 1]
       /\ queue' = RemoveFromQueue(queue, w)
       /\ lastAction' = "UnlockHandoff"
       /\ lastActor' = actor
       /\ lastTargetEpoch' = w
       /\ lastGrantedEpoch' = w
       /\ SnapPre
    /\ UNCHANGED <<epochFiber, deadlineDue, destroyed,
                   admissionSawFree, admissionSawDue>>
'''

# NEG-M6 PublicationWithoutGrantCoupling: UnlockHandoff publishes but commits
# owner to the WRONG fiber (NoOwner or a non-winner), decoupling publication
# from ownership. Expected: InvGrantPublicationCoupling.
NEGM6 = r'''UnlockHandoff(actor) ==
    /\ ~destroyed
    /\ owner = actor
    /\ FIFOHead # None
    /\ nodeState[FIFOHead] = "Registered"
    /\ epochFiber[FIFOHead] # None
    /\ expectedFIFOHead' = FIFOHead
    /\ LET w == FIFOHead IN
       /\ nodeState' = [nodeState EXCEPT ![w] = "Woken"]
       /\ resolutionCount' = [resolutionCount EXCEPT ![w] = 1]
       /\ owner' = actor
       /\ runnablePublished' = [runnablePublished EXCEPT ![w] = TRUE]
       /\ publicationCount' = [publicationCount EXCEPT ![w] = 1]
       /\ queue' = RemoveFromQueue(queue, w)
       /\ lastAction' = "UnlockHandoff"
       /\ lastActor' = actor
       /\ lastTargetEpoch' = w
       /\ lastGrantedEpoch' = w
       /\ SnapPre
    /\ UNCHANGED <<epochFiber, deadlineDue, destroyed,
                   admissionSawFree, admissionSawDue>>
'''

# NEG-M7 AdmissionClosureFailure: LockAdmissionSuspend's precondition is
# INVERTED to suspend exactly when the mutex IS free and the epoch is the FIFO
# head (the case that should inline-acquire), and it latches admissionSawFree =
# TRUE. So a registered epoch that observed free is left Registered (Suspended)
# in the queue, violating InvAdmissionClosure. Expected: InvAdmissionClosure.
NEGM7 = r'''LockAdmissionSuspend(actor, epoch) ==
    /\ ~destroyed
    /\ nodeState[epoch] = "Detached"
    /\ owner = NoOwner
    /\ FIFOHead = None
    /\ actor # owner
    /\ queue' = Append(queue, epoch)
    /\ nodeState' = [nodeState EXCEPT ![epoch] = "Registered"]
    /\ epochFiber' = [epochFiber EXCEPT ![epoch] = actor]
    /\ deadlineDue' = [deadlineDue EXCEPT ![epoch] = FALSE]
    /\ admissionSawFree' = [admissionSawFree EXCEPT ![epoch] = TRUE]
    /\ admissionSawDue' = [admissionSawDue EXCEPT ![epoch] = FALSE]
    /\ Mark("LockAdmissionSuspend", actor, epoch, None)
    /\ UNCHANGED <<owner, runnablePublished, resolutionCount, publicationCount,
                   destroyed>>
'''

# NEG-M8 CancelRevokesHandoff: a late CancelAttemptTerminal of a Woken epoch
# mutates owner (reverts to NoOwner). Expected: InvGrantFinality.
NEGM8 = r'''CancelAttemptTerminal(epoch) ==
    /\ nodeState[epoch] \in {"Woken", "Cancelled", "Expired"}
    /\ IF preNodeState[epoch] = "Woken"
       THEN owner' = NoOwner
       ELSE owner' = owner
    /\ Mark("CancelAttemptTerminal", None, epoch, None)
    /\ UNCHANGED <<queue, nodeState, epochFiber, deadlineDue,
                   runnablePublished, resolutionCount, publicationCount, destroyed,
                   admissionSawFree, admissionSawDue>>
'''

# NEG-M9 DeadlineRevokesHandoff: a late ExpireAttemptTerminal of a Woken epoch
# republishes (increments publicationCount). Expected: InvGrantFinality.
NEGM9 = r'''ExpireAttemptTerminal(epoch) ==
    /\ nodeState[epoch] \in {"Woken", "Cancelled", "Expired"}
    /\ IF preNodeState[epoch] = "Woken"
       THEN /\ publicationCount' = [publicationCount EXCEPT ![epoch] = publicationCount[epoch] + 1]
            /\ runnablePublished' = [runnablePublished EXCEPT ![epoch] = TRUE]
       ELSE /\ publicationCount' = publicationCount
            /\ runnablePublished' = runnablePublished
    /\ Mark("ExpireAttemptTerminal", None, epoch, None)
    /\ UNCHANGED <<owner, queue, nodeState, epochFiber, deadlineDue,
                   resolutionCount, destroyed,
                   admissionSawFree, admissionSawDue>>
'''

# NEG-M10 ImmediatePublication: LockImmediate creates a runnable publication
# (sets publicationCount := 1) even though the Fiber never suspended. Expected:
# InvPublicationRequiresSuspensionOrHandoff.
NEGM10 = r'''LockImmediate(actor, epoch) ==
    /\ ~destroyed
    /\ nodeState[epoch] = "Detached"
    /\ owner = NoOwner
    /\ FIFOHead = None
    /\ owner' = actor
    /\ nodeState' = [nodeState EXCEPT ![epoch] = "Woken"]
    /\ epochFiber' = [epochFiber EXCEPT ![epoch] = actor]
    /\ deadlineDue' = [deadlineDue EXCEPT ![epoch] = FALSE]
    /\ resolutionCount' = [resolutionCount EXCEPT ![epoch] = 1]
    /\ runnablePublished' = [runnablePublished EXCEPT ![epoch] = TRUE]
    /\ publicationCount' = [publicationCount EXCEPT ![epoch] = 1]
    /\ admissionSawFree' = [admissionSawFree EXCEPT ![epoch] = TRUE]
    /\ admissionSawDue' = [admissionSawDue EXCEPT ![epoch] = FALSE]
    /\ Mark("LockImmediate", actor, epoch, epoch)
    /\ UNCHANGED <<queue, destroyed>>
'''

# NEG-M11 DestructionWhileOwnedOrQueued: Destroy drops the owner = NoOwner /\
# queue = <<>> precondition, so destruction can occur while owned. Expected:
# InvDestructionPrecondition.
NEGM11 = r'''Destroy ==
    /\ ~destroyed
    /\ destroyed' = TRUE
    /\ Mark("Destroy", None, None, None)
    /\ UNCHANGED <<owner, queue, nodeState, epochFiber, deadlineDue,
                   runnablePublished, resolutionCount, publicationCount,
                   admissionSawFree, admissionSawDue>>
'''

CASES = [
    ("E12AsyncMutexNegM1", "NEG-M1 NonOwnerUnlock: UnlockNoWaiter drops the owner = actor precondition; a foreign Fiber can unlock.",
     [("UnlockNoWaiter", NEGM1)], "InvUnlockAuthority"),
    ("E12AsyncMutexNegM2", "NEG-M2 RecursiveAcquire: TryLockSuccess drops the owner = NoOwner precondition; an owner can re-acquire.",
     [("TryLockSuccess", NEGM2)], "InvRecursiveForbidden"),
    ("E12AsyncMutexNegM3", "NEG-M3 NonFIFOGrant: with >= 2 eligible waiters, UnlockHandoff grants the second instead of the FIFO head.",
     [("UnlockHandoff", NEGM3)], "InvFIFOGrant"),
    ("E12AsyncMutexNegM4", "NEG-M4 HandoffFreeWindow: UnlockHandoff resolves FIFO head Woken + published but does NOT commit owner (owner := NoOwner). Free window: ownerless while eligible waiter queued.",
     [("UnlockHandoff", NEGM4)], "InvNoOwnerlessQueuedDemand"),
    ("E12AsyncMutexNegM5", "NEG-M5 GrantWithoutOwnerCommit: UnlockHandoff resolves + publishes but leaves owner = NoOwner.",
     [("UnlockHandoff", NEGM5)], "InvGrantOwnerCommit"),
    ("E12AsyncMutexNegM6", "NEG-M6 PublicationWithoutGrantCoupling: UnlockHandoff publishes but commits owner to the old actor (not the winner).",
     [("UnlockHandoff", NEGM6)], "InvGrantPublicationCoupling"),
    ("E12AsyncMutexNegM7", "NEG-M7 AdmissionClosureFailure: LockAdmissionSuspend's precondition is inverted to suspend when free + FIFO head, latching admissionSawFree=TRUE (strands a free mutex).",
     [("LockAdmissionSuspend", NEGM7)], "InvAdmissionClosure"),
    ("E12AsyncMutexNegM8", "NEG-M8 CancelRevokesHandoff: a late CancelAttemptTerminal of a Woken epoch reverts owner.",
     [("CancelAttemptTerminal", NEGM8)], "InvGrantFinality"),
    ("E12AsyncMutexNegM9", "NEG-M9 DeadlineRevokesHandoff: a late ExpireAttemptTerminal of a Woken epoch republishes.",
     [("ExpireAttemptTerminal", NEGM9)], "InvGrantFinality"),
    ("E12AsyncMutexNegM10", "NEG-M10 ImmediatePublication: LockImmediate creates a runnable publication though the Fiber never suspended.",
     [("LockImmediate", NEGM10)], "InvPublicationRequiresSuspensionOrHandoff"),
    ("E12AsyncMutexNegM11", "NEG-M11 DestructionWhileOwnedOrQueued: Destroy drops the owner=NoOwner / queue-empty precondition.",
     [("Destroy", NEGM11)], "InvDestructionPrecondition"),
]


def main() -> int:
    check_only = "--check" in sys.argv[1:]
    if not POSITIVE.is_file():
        fail(f"positive model not found: {POSITIVE}")
    validate_cases(CASES)
    artifacts = expected_artifacts(CASES, POSITIVE.read_text(encoding="utf-8"))
    if check_only:
        return check_artifacts(artifacts)
    write_artifacts(artifacts)
    return 0


if __name__ == "__main__":
    sys.exit(main())
