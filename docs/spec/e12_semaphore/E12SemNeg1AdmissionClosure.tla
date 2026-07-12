------------------------------- MODULE E12SemNeg1AdmissionClosure -------------------------------
(*
  NEG-SEM-1 -- Admission Closure (E12-B-SEMAPHORE-PREPARATION-CORRECTIVE-1).

  Single-rule difference from E12Semaphore. The ONLY defect: AcquireSuspend's
  commit-suspend branch does NOT recheck whether an admissible permit appeared
  between AcquireRegister and the suspend commit, and latches
  admissionSawPermit[n] = (available > 0). So a node that observed an
  admissible permit can be left Registered + Suspended, stranding the permit.

  Broken protocol:
    AcquireRegister(n)             [available == 0]
    ReleaseStore                   [available becomes > 0]
    AcquireSuspend(n)  [DEFECT]    commits Suspended despite available > 0,
                                   latches admissionSawPermit[n] = TRUE

  Required counterexample: node n Registered + Suspended + admissionSawPermit[n]
  = TRUE. Violated expected property: InvAdmissionClosure.

  Everything else is identical to E12Semaphore.
*)
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Nodes, N0, N1, N2, MaxPermits, MaxInit, MaxDue

VARIABLES
    available,
    queue,
    nodeState,
    resolvedCount,
    wakeDispatched,
    admissionPhase,
    deadlineDue,
    admissionSawPermit,
    admissionSawDue,
    acceptedReleaseCount,
    acquiredCount,
    lastAction,
    lastGrantedNode,
    expectedFIFOHead,
    preAvailable,
    preAcceptedReleaseCount,
    preAcquiredCount

NodeState == {"Detached", "Registered", "Woken", "Cancelled", "Expired"}
AdmissionPhase == {"NoAdmission", "AdmissionOpen", "Suspended"}
ActionKind == {"Init", "Acquire", "ReleaseTransfer", "ReleaseStore",
               "ReleaseOverflow", "Cancel", "Expire", "Register", "Suspend"}
NoNode == 999

ASSUME /\ Nodes # {}
       /\ N0 \in Nodes
       /\ N1 \in Nodes
       /\ N2 \in Nodes
       /\ N0 # N1 /\ N1 # N2 /\ N0 # N2
       /\ MaxPermits \in 1..3
       /\ MaxInit \in 1..3
       /\ MaxDue \in 0..3

Vars == <<available, queue, nodeState, resolvedCount, wakeDispatched,
          admissionPhase, deadlineDue, admissionSawPermit, admissionSawDue,
          acceptedReleaseCount, acquiredCount,
          lastAction, lastGrantedNode, expectedFIFOHead,
          preAvailable, preAcceptedReleaseCount, preAcquiredCount>>

isTerminal(n) == nodeState[n] \in {"Woken", "Cancelled", "Expired"}

Eligible(n) ==
    /\ nodeState[n] = "Registered"
    /\ admissionPhase[n] = "Suspended"

EligibleSet ==
    {n \in Nodes : Eligible(n)}

InQueue(n) ==
    \E i \in 1..Len(queue) : queue[i] = n

RemoveFromQueue(q, n) ==
    SelectSeq(q, LAMBDA m : m # n)

EligibleQueue ==
    SelectSeq(queue, LAMBDA n : Eligible(n))

FIFOHead ==
    IF EligibleQueue = <<>> THEN NoNode ELSE Head(EligibleQueue)

ImmediateAdmissible ==
    /\ available > 0
    /\ EligibleSet = {}

-------------------------------------------------------------------------------
Init ==
    /\ available = 0
    /\ queue = <<>>
    /\ nodeState = [n \in Nodes |-> "Detached"]
    /\ resolvedCount = [n \in Nodes |-> 0]
    /\ wakeDispatched = 0
    /\ admissionPhase = [n \in Nodes |-> "NoAdmission"]
    /\ deadlineDue = [n \in Nodes |-> FALSE]
    /\ admissionSawPermit = [n \in Nodes |-> FALSE]
    /\ admissionSawDue = [n \in Nodes |-> FALSE]
    /\ acceptedReleaseCount = 0
    /\ acquiredCount = 0
    /\ lastAction = "Init"
    /\ lastGrantedNode = NoNode
    /\ expectedFIFOHead = NoNode
    /\ preAvailable = 0
    /\ preAcceptedReleaseCount = 0
    /\ preAcquiredCount = 0

SnapPre ==
    /\ preAvailable' = available
    /\ preAcceptedReleaseCount' = acceptedReleaseCount
    /\ preAcquiredCount' = acquiredCount

MarkNonRelease(act) ==
    /\ lastAction' = act
    /\ lastGrantedNode' = NoNode
    /\ expectedFIFOHead' = NoNode
    /\ SnapPre

-------------------------------------------------------------------------------
AcquireRegister(n) ==
    /\ nodeState[n] = "Detached"
    /\ admissionPhase[n] = "NoAdmission"
    /\ \E due \in BOOLEAN :
        /\ deadlineDue' = [deadlineDue EXCEPT ![n] = due]
        /\ admissionPhase' = [admissionPhase EXCEPT ![n] = "AdmissionOpen"]
        /\ nodeState' = [nodeState EXCEPT ![n] = "Registered"]
        /\ queue' = Append(queue, n)
        /\ MarkNonRelease("Register")
        /\ UNCHANGED <<available, resolvedCount, wakeDispatched,
                       admissionSawPermit, admissionSawDue,
                       acceptedReleaseCount, acquiredCount>>

AcquireImmediate(n) ==
    /\ nodeState[n] = "Registered"
    /\ admissionPhase[n] = "AdmissionOpen"
    /\ deadlineDue[n] = FALSE
    /\ ImmediateAdmissible
    /\ nodeState' = [nodeState EXCEPT ![n] = "Woken"]
    /\ admissionPhase' = [admissionPhase EXCEPT ![n] = "NoAdmission"]
    /\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
    /\ wakeDispatched' = wakeDispatched + 1
    /\ available' = available - 1
    /\ acquiredCount' = acquiredCount + 1
    /\ admissionSawPermit' = [admissionSawPermit EXCEPT ![n] = TRUE]
    /\ admissionSawDue' = [admissionSawDue EXCEPT ![n] = FALSE]
    /\ queue' = RemoveFromQueue(queue, n)
    /\ MarkNonRelease("Acquire")
    /\ UNCHANGED <<deadlineDue, acceptedReleaseCount>>

AcquireUntilImmediate(n) ==
    /\ nodeState[n] = "Registered"
    /\ admissionPhase[n] = "AdmissionOpen"
    /\ deadlineDue[n] = TRUE
    /\ ImmediateAdmissible
    /\ nodeState' = [nodeState EXCEPT ![n] = "Woken"]
    /\ admissionPhase' = [admissionPhase EXCEPT ![n] = "NoAdmission"]
    /\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
    /\ wakeDispatched' = wakeDispatched + 1
    /\ available' = available - 1
    /\ acquiredCount' = acquiredCount + 1
    /\ admissionSawPermit' = [admissionSawPermit EXCEPT ![n] = TRUE]
    /\ admissionSawDue' = [admissionSawDue EXCEPT ![n] = TRUE]
    /\ queue' = RemoveFromQueue(queue, n)
    /\ MarkNonRelease("Acquire")
    /\ UNCHANGED <<deadlineDue, acceptedReleaseCount>>

AcquireUntilDueNoPermit(n) ==
    /\ nodeState[n] = "Registered"
    /\ admissionPhase[n] = "AdmissionOpen"
    /\ deadlineDue[n] = TRUE
    /\ ~ImmediateAdmissible
    /\ nodeState' = [nodeState EXCEPT ![n] = "Expired"]
    /\ admissionPhase' = [admissionPhase EXCEPT ![n] = "NoAdmission"]
    /\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
    /\ wakeDispatched' = wakeDispatched + 1
    /\ admissionSawPermit' = [admissionSawPermit EXCEPT ![n] = FALSE]
    /\ admissionSawDue' = [admissionSawDue EXCEPT ![n] = TRUE]
    /\ queue' = RemoveFromQueue(queue, n)
    /\ MarkNonRelease("Expire")
    /\ UNCHANGED <<available, deadlineDue, acceptedReleaseCount, acquiredCount>>

\* DEFECT (NEG-1): the commit-suspend branch does NOT recheck whether an
\* admissible permit appeared, and latches admissionSawPermit = (available > 0).
\* So a node that saw an admissible permit can be left Registered+Suspended,
\* violating InvAdmissionClosure.
AcquireSuspend(n) ==
    /\ nodeState[n] = "Registered"
    /\ admissionPhase[n] = "AdmissionOpen"
    /\ admissionPhase' = [admissionPhase EXCEPT ![n] = "Suspended"]
    /\ admissionSawPermit' = [admissionSawPermit EXCEPT ![n] = (available > 0)]
    /\ admissionSawDue' = [admissionSawDue EXCEPT ![n] = deadlineDue[n]]
    /\ UNCHANGED <<nodeState, resolvedCount, wakeDispatched,
                   available, acquiredCount, queue, deadlineDue,
                   acceptedReleaseCount>>
    /\ MarkNonRelease("Suspend")

-------------------------------------------------------------------------------
ReleaseTransfer ==
    /\ FIFOHead # NoNode
    /\ expectedFIFOHead' = FIFOHead
    /\ LET n == FIFOHead IN
      /\ nodeState' = [nodeState EXCEPT ![n] = "Woken"]
      /\ admissionPhase' = [admissionPhase EXCEPT ![n] = "NoAdmission"]
      /\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
      /\ wakeDispatched' = wakeDispatched + 1
      /\ queue' = RemoveFromQueue(queue, n)
      /\ available' = available
      /\ acceptedReleaseCount' = acceptedReleaseCount + 1
      /\ acquiredCount' = acquiredCount + 1
      /\ lastGrantedNode' = n
      /\ lastAction' = "ReleaseTransfer"
      /\ preAvailable' = available
      /\ preAcceptedReleaseCount' = acceptedReleaseCount
      /\ preAcquiredCount' = acquiredCount
      /\ UNCHANGED <<deadlineDue, admissionSawPermit, admissionSawDue>>

ReleaseStore ==
    /\ EligibleSet = {}
    /\ available < MaxPermits
    /\ available' = available + 1
    /\ acceptedReleaseCount' = acceptedReleaseCount + 1
    /\ lastGrantedNode' = NoNode
    /\ expectedFIFOHead' = NoNode
    /\ lastAction' = "ReleaseStore"
    /\ preAvailable' = available
    /\ preAcceptedReleaseCount' = acceptedReleaseCount
    /\ preAcquiredCount' = acquiredCount
    /\ UNCHANGED <<queue, nodeState, resolvedCount, wakeDispatched,
                  admissionPhase, deadlineDue, admissionSawPermit,
                  admissionSawDue, acquiredCount>>

ReleaseOverflow ==
    /\ EligibleSet = {}
    /\ available = MaxPermits
    /\ lastGrantedNode' = NoNode
    /\ expectedFIFOHead' = NoNode
    /\ lastAction' = "ReleaseOverflow"
    /\ preAvailable' = available
    /\ preAcceptedReleaseCount' = acceptedReleaseCount
    /\ preAcquiredCount' = acquiredCount
    /\ UNCHANGED <<available, acceptedReleaseCount, acquiredCount,
                  queue, nodeState, resolvedCount, wakeDispatched,
                  admissionPhase, deadlineDue, admissionSawPermit,
                  admissionSawDue>>

-------------------------------------------------------------------------------
ResolveCancel(n) ==
    /\ nodeState[n] = "Registered"
    /\ InQueue(n)
    /\ nodeState' = [nodeState EXCEPT ![n] = "Cancelled"]
    /\ admissionPhase' = [admissionPhase EXCEPT ![n] = "NoAdmission"]
    /\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
    /\ wakeDispatched' = wakeDispatched + 1
    /\ queue' = RemoveFromQueue(queue, n)
    /\ MarkNonRelease("Cancel")
    /\ UNCHANGED <<available, acceptedReleaseCount, acquiredCount,
                  deadlineDue, admissionSawPermit, admissionSawDue>>

Stutter ==
    UNCHANGED Vars

-------------------------------------------------------------------------------
Next ==
    \/ Stutter
    \/ \E n \in Nodes : AcquireRegister(n)
    \/ \E n \in Nodes : AcquireImmediate(n)
    \/ \E n \in Nodes : AcquireUntilImmediate(n)
    \/ \E n \in Nodes : AcquireUntilDueNoPermit(n)
    \/ \E n \in Nodes : AcquireSuspend(n)
    \/ ReleaseTransfer
    \/ ReleaseStore
    \/ ReleaseOverflow
    \/ \E n \in Nodes : ResolveCancel(n)

Spec == Init /\ [][Next]_Vars

-------------------------------------------------------------------------------
InvPermitConservation ==
    available + acquiredCount = 0 + acceptedReleaseCount

InvPermitBounds ==
    /\ 0 <= available
    /\ available <= MaxPermits
    /\ MaxPermits > 0

InvQueueWellFormed ==
    /\ \A i, j \in 1..Len(queue) :
        i # j => queue[i] # queue[j]
    /\ \A n \in Nodes :
        InQueue(n) => nodeState[n] = "Registered"
    /\ \A n \in Nodes :
        InQueue(n) => admissionPhase[n] \in {"AdmissionOpen", "Suspended"}
    /\ \A n \in Nodes :
        isTerminal(n) => ~InQueue(n)

InvSingleResolution ==
    \A n \in Nodes : resolvedCount[n] <= 1

InvSinglePublication ==
    wakeDispatched = resolvedCount[N0] + resolvedCount[N1] + resolvedCount[N2]

InvGrantCommitCoupling ==
    acquiredCount = Cardinality({n \in Nodes : nodeState[n] = "Woken"})

InvFIFOGrant ==
    lastAction = "ReleaseTransfer" => lastGrantedNode = expectedFIFOHead

\* The target invariant for NEG-1.
InvAdmissionClosure ==
    \A n \in Nodes :
        admissionSawPermit[n] = TRUE => nodeState[n] = "Woken"

InvOverflowNonMutation ==
    lastAction = "ReleaseOverflow" =>
        /\ available = preAvailable
        /\ acceptedReleaseCount = preAcceptedReleaseCount
        /\ acquiredCount = preAcquiredCount

InvNoIdlePermitWithEligibleWaiter ==
    available > 0 => EligibleSet = {}

InvReleaseDisposition ==
    /\ (lastAction = "ReleaseTransfer" =>
            /\ acceptedReleaseCount = preAcceptedReleaseCount + 1
            /\ acquiredCount = preAcquiredCount + 1
            /\ available = preAvailable)
    /\ (lastAction = "ReleaseStore" =>
            /\ acceptedReleaseCount = preAcceptedReleaseCount + 1
            /\ acquiredCount = preAcquiredCount
            /\ available = preAvailable + 1)
    /\ (lastAction = "ReleaseOverflow" =>
            /\ acceptedReleaseCount = preAcceptedReleaseCount
            /\ acquiredCount = preAcquiredCount
            /\ available = preAvailable)

InvPermitFirstDeadline ==
    \A n \in Nodes :
        (admissionSawPermit[n] = TRUE /\ admissionSawDue[n] = TRUE)
        => nodeState[n] = "Woken"

=============================================================================
