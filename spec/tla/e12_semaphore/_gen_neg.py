#!/usr/bin/env python3
"""Generate NEG-2..NEG-7 from the correct E12Semaphore model.

Each NEG is a self-contained TLA+ module identical to E12Semaphore EXCEPT for
one single-rule defect in one named action. The generator does a precise
multi-line string replacement of the action body; the rest is copied verbatim
with the module renamed. A header comment records the defect + expected
invariant.

This is a build aid (not part of the formal gate). NEG-1 is hand-written
(E12SemNeg1AdmissionClosure.tla) and serves as the validated template.
"""
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
CORRECT = (HERE / "E12Semaphore.tla").read_text()

CFG = """\
SPECIFICATION Spec
INVARIANT {inv}

CONSTANTS
N0 = N0
N1 = N1
N2 = N2
Nodes = {{N0, N1, N2}}
MaxPermits = 2
MaxInit = 2
MaxDue = 2
"""


def replace_action(text, name, new_body):
    """Replace the action definition `name == ...` with new_body (which must
    include the `name ==` line). The action block runs until the next top-level
    definition or a `----` separator."""
    lines = text.split("\n")
    start = next(i for i, ln in enumerate(lines)
                 if ln.startswith(name + " ==") or ln.startswith(name + "("))
    end = len(lines)
    for j in range(start + 1, len(lines)):
        ln = lines[j]
        if ln.startswith("----"):
            end = j
            break
        if ln and not ln[0].isspace() and " ==" in ln and not ln.startswith("\\*"):
            end = j
            break
    return "\n".join(lines[:start] + new_body.split("\n") + lines[end:])


def gen(modname, header_desc, action_name, defect_body, expected_inv):
    text = CORRECT.replace("MODULE E12Semaphore", f"MODULE {modname}", 1)
    text = replace_action(text, action_name, defect_body)
    # Strip the original big header comment block (up to EXTENDS), prepend ours.
    body = text[text.index("EXTENDS"):]
    header = (
        f"------------------------------- MODULE {modname} "
        f"-------------------------------\n(*\n  {header_desc}\n"
        f"  Single-rule difference from E12Semaphore: the ONLY defect is in "
        f"{action_name}.\n  Expected first invariant violation: "
        f"{expected_inv}.\n*)\n"
    )
    (HERE / f"{modname}.tla").write_text(header + body + "\n")
    (HERE / f"{modname}.cfg").write_text(CFG.format(inv=expected_inv))
    print(f"{modname}: {action_name} -> {expected_inv}")


# ---- defect bodies (full action definitions, single rule changed) ----

# NEG-2: ReleaseTransfer loses the grant permit (drops acquiredCount++).
NEG2 = """ReleaseTransfer ==
    /\\ FIFOHead # NoNode
    /\\ expectedFIFOHead' = FIFOHead
    /\\ LET n == FIFOHead IN
      /\\ nodeState' = [nodeState EXCEPT ![n] = "Woken"]
      /\\ admissionPhase' = [admissionPhase EXCEPT ![n] = "NoAdmission"]
      /\\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
      /\\ wakeDispatched' = wakeDispatched + 1
      /\\ queue' = RemoveFromQueue(queue, n)
      /\\ available' = available
      /\\ acceptedReleaseCount' = acceptedReleaseCount + 1
      /\\ UNCHANGED <<acquiredCount>>  \\* DEFECT: permit lost (no increment)
      /\\ lastGrantedNode' = n
      /\\ lastAction' = "ReleaseTransfer"
      /\\ preAvailable' = available
      /\\ preAcceptedReleaseCount' = acceptedReleaseCount
      /\\ preAcquiredCount' = acquiredCount
      /\\ UNCHANGED <<deadlineDue, admissionSawPermit, admissionSawDue>>"""

# NEG-3: ReleaseStore increments available by 2 (double-store) for one accepted release.
NEG3 = """ReleaseStore ==
    /\\ EligibleSet = {}
    /\\ available < MaxPermits
    /\\ available' = available + 2  \\* DEFECT: double-store (one release, two permits)
    /\\ acceptedReleaseCount' = acceptedReleaseCount + 1
    /\\ lastGrantedNode' = NoNode
    /\\ expectedFIFOHead' = NoNode
    /\\ lastAction' = "ReleaseStore"
    /\\ preAvailable' = available
    /\\ preAcceptedReleaseCount' = acceptedReleaseCount
    /\\ preAcquiredCount' = acquiredCount
    /\\ UNCHANGED <<queue, nodeState, resolvedCount, wakeDispatched,
                  admissionPhase, deadlineDue, admissionSawPermit,
                  admissionSawDue, acquiredCount>>"""

# NEG-4: ReleaseTransfer grants a non-FIFO-head eligible node.
NEG4 = """ReleaseTransfer ==
    /\\ Cardinality(EligibleSet) >= 2
    /\\ \\E n \\in EligibleSet :
        /\\ n # FIFOHead
        /\\ expectedFIFOHead' = FIFOHead
        /\\ nodeState' = [nodeState EXCEPT ![n] = "Woken"]
        /\\ admissionPhase' = [admissionPhase EXCEPT ![n] = "NoAdmission"]
        /\\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
        /\\ wakeDispatched' = wakeDispatched + 1
        /\\ queue' = RemoveFromQueue(queue, n)
        /\\ available' = available
        /\\ acceptedReleaseCount' = acceptedReleaseCount + 1
        /\\ acquiredCount' = acquiredCount + 1
        /\\ lastGrantedNode' = n
        /\\ lastAction' = "ReleaseTransfer"
        /\\ preAvailable' = available
        /\\ preAcceptedReleaseCount' = acceptedReleaseCount
        /\\ preAcquiredCount' = acquiredCount
        /\\ UNCHANGED <<deadlineDue, admissionSawPermit, admissionSawDue>>"""

# NEG-5: ReleaseOverflow mutates available (instead of leaving it unchanged).
NEG5 = """ReleaseOverflow ==
    /\\ EligibleSet = {}
    /\\ available = MaxPermits
    /\\ available' = available + 1  \\* DEFECT: overflow mutates the counter
    /\\ lastGrantedNode' = NoNode
    /\\ expectedFIFOHead' = NoNode
    /\\ lastAction' = "ReleaseOverflow"
    /\\ preAvailable' = available
    /\\ preAcceptedReleaseCount' = acceptedReleaseCount
    /\\ preAcquiredCount' = acquiredCount
    /\\ UNCHANGED <<acceptedReleaseCount, acquiredCount,
                  queue, nodeState, resolvedCount, wakeDispatched,
                  admissionPhase, deadlineDue, admissionSawPermit,
                  admissionSawDue>>"""

# NEG-6: ReleaseStore drops the EligibleSet={} guard (stores while an eligible
# waiter is queued).
NEG6 = """ReleaseStore ==
    /\\ available < MaxPermits
    /\\ available' = available + 1
    /\\ acceptedReleaseCount' = acceptedReleaseCount + 1
    /\\ lastGrantedNode' = NoNode
    /\\ expectedFIFOHead' = NoNode
    /\\ lastAction' = "ReleaseStore"
    /\\ preAvailable' = available
    /\\ preAcceptedReleaseCount' = acceptedReleaseCount
    /\\ preAcquiredCount' = acquiredCount
    /\\ UNCHANGED <<queue, nodeState, resolvedCount, wakeDispatched,
                  admissionPhase, deadlineDue, admissionSawPermit,
                  admissionSawDue, acquiredCount>>"""

# NEG-7: AcquireUntilImmediate latches admissionSawPermit+admissionSawDue but
# resolves Expired (deadline beats an admissible permit).
NEG7 = """AcquireUntilImmediate(n) ==
    /\\ nodeState[n] = "Registered"
    /\\ admissionPhase[n] = "AdmissionOpen"
    /\\ deadlineDue[n] = TRUE
    /\\ ImmediateAdmissible
    /\\ nodeState' = [nodeState EXCEPT ![n] = "Expired"]  \\* DEFECT: Expired, not Woken
    /\\ admissionPhase' = [admissionPhase EXCEPT ![n] = "NoAdmission"]
    /\\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
    /\\ wakeDispatched' = wakeDispatched + 1
    /\\ admissionSawPermit' = [admissionSawPermit EXCEPT ![n] = TRUE]
    /\\ admissionSawDue' = [admissionSawDue EXCEPT ![n] = TRUE]
    /\\ queue' = RemoveFromQueue(queue, n)
    /\\ MarkNonRelease("Acquire")
    /\\ UNCHANGED <<available, deadlineDue, acceptedReleaseCount, acquiredCount>>"""

gen("E12SemNeg2ReleaseLoss",
    "NEG-SEM-2: ReleaseLoss. ReleaseTransfer increments acceptedReleaseCount "
    "but drops acquiredCount++ (the grant permit is lost).",
    "ReleaseTransfer", NEG2, "InvPermitConservation")

gen("E12SemNeg3DoubleStore",
    "NEG-SEM-3: DoubleStore. ReleaseStore increments available by 2 for one "
    "accepted release (double-store).",
    "ReleaseStore", NEG3, "InvPermitConservation")

gen("E12SemNeg4NonFIFOGrant",
    "NEG-SEM-4: NonFIFOGrant. ReleaseTransfer grants an eligible node that is "
    "NOT the FIFO head (requires >= 2 eligible waiters to be reachable).",
    "ReleaseTransfer", NEG4, "InvFIFOGrant")

gen("E12SemNeg5OverflowMutation",
    "NEG-SEM-5: OverflowMutation. ReleaseOverflow mutates available++ instead "
    "of leaving it unchanged.",
    "ReleaseOverflow", NEG5, "InvOverflowNonMutation")

gen("E12SemNeg6IdlePermitEligibleWaiter",
    "NEG-SEM-6: IdlePermitEligibleWaiter. ReleaseStore drops the EligibleSet={} "
    "guard, storing a permit while an eligible waiter is queued.",
    "ReleaseStore", NEG6, "InvNoIdlePermitWithEligibleWaiter")

gen("E12SemNeg7DeadlinePrecedence",
    "NEG-SEM-7: DeadlinePrecedence. AcquireUntilImmediate latches "
    "admissionSawPermit+admissionSawDue but resolves Expired instead of Woken "
    "(deadline beats an admissible permit).",
    "AcquireUntilImmediate", NEG7, "InvPermitFirstDeadline")
