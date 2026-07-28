#!/usr/bin/env python3
"""Generate NEG-M1..NEG-M11 from the correct E12AsyncMutex model.

Each NEG is a self-contained TLA+ module identical to E12AsyncMutex EXCEPT for
one single-rule defect (a modified action precondition or effect). The rest is
copied verbatim with the module renamed. A header comment records the defect +
the expected violated invariant.

This is a build aid (not part of the formal gate output committed as evidence;
the generated .tla/.cfg files are committed). Re-run after editing
E12AsyncMutex.tla to regenerate all negatives.

Expected violated invariant per NEG (matches docs/e12-async-mutex.md §16):
  NEG-M1  NonOwnerUnlock              -> InvUnlockAuthority
  NEG-M2  RecursiveAcquire            -> InvRecursiveForbidden
  NEG-M3  NonFIFOGrant                -> InvFIFOGrant
  NEG-M4  Barging                     -> InvNoBarging
  NEG-M5  GrantWithoutOwnerCommit     -> InvGrantOwnerCommit
  NEG-M6  PublicationWithoutGrantCoupling -> InvGrantPublicationCoupling
  NEG-M7  AdmissionClosureFailure     -> InvAdmissionClosure
  NEG-M8  CancelRevokesHandoff        -> InvGrantFinality
  NEG-M9  DeadlineRevokesHandoff      -> InvGrantFinality
  NEG-M10 ImmediatePublication        -> InvPublicationRequiresSuspensionOrHandoff
  NEG-M11 DestructionWhileOwnedOrQueued -> InvDestructionPrecondition
"""
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
CORRECT = (HERE / "E12AsyncMutex.tla").read_text()

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


def replace_action(text, name, new_body):
    """Replace the action definition `name == ...` with new_body (which must
    include the `name ==` line). The action block runs until the next top-level
    definition, a `----` separator, or a line comment introducing the next
    action. We anchor on the action signature line; the block ends at the first
    subsequent line that is a top-level definition (`X ==` or `X(...`) or a
    `----` rule."""
    lines = text.split("\n")
    start = next(i for i, ln in enumerate(lines)
                 if ln.startswith(name + "(") or ln.startswith(name + " =="))
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


def gen(modname, header_desc, edits, expected_inv):
    """edits: list of (action_name, defect_body) applied in order."""
    text = CORRECT.replace("MODULE E12AsyncMutex", f"MODULE {modname}", 1)
    for action_name, defect_body in edits:
        text = replace_action(text, action_name, defect_body)
    body = text[text.index("EXTENDS"):]
    header = (
        f"------------------------------- MODULE {modname} "
        f"-------------------------------\n(*\n  {header_desc}\n"
        f"  Expected violated property: {expected_inv}.\n"
        f"  Single-rule difference(s) from E12AsyncMutex noted below. Everything "
        f"else is identical.\n*)\n"
    )
    out = header + body
    (HERE / f"{modname}.tla").write_text(out)
    (HERE / f"{modname}.cfg").write_text(CFG.format(inv=expected_inv))
    print(f"  wrote {modname}.tla / .cfg  (expected: {expected_inv})")


# ---------------------------------------------------------------------------
# Each defect is a SINGLE focused rule change. The defect body is the FULL
# replacement action (with the injected defect) so the generator does a clean
# whole-action substitution.

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


def main():
    for modname, desc, edits, inv in CASES:
        gen(modname, desc, edits, inv)


if __name__ == "__main__":
    main()
