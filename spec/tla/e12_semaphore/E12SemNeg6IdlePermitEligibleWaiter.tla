------------------------------- MODULE E12SemNeg6IdlePermitEligibleWaiter -------------------------------
(*
  NEG-SEM-6: IdlePermitEligibleWaiter. ReleaseStore drops the EligibleSet={} guard, storing a permit while an eligible waiter is queued.
  Single-rule difference from E12Semaphore: the ONLY defect is in ReleaseStore.
  Expected first invariant violation: InvNoIdlePermitWithEligibleWaiter.
*)
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Nodes, N0, N1, N2, MaxPermits, MaxInit, MaxDue

(* Nodes is the (small, exhaustive) epoch set. MaxPermits bounds the permit
   counter; MaxInit bounds initialPermits; MaxDue bounds the deadlineDue
   generation used to force the precedence case. *)

VARIABLES
    \* @type: 0..MaxPermits          stored, unassigned permits
    available,
    \* @type: Seq(Node)              the FIFO wait queue (eligible set derives)
    queue,
    \* @type: [Node -> NodeState]    per-epoch lifecycle
    nodeState,
    \* @type: [Node -> 0..1]         terminal resolution count (P9: <= 1)
    resolvedCount,
    \* @type: Int                    total runnable publications (P10)
    wakeDispatched,
    \* @type: [Node -> AdmissionPhase]
    admissionPhase,
    \* @type: [Node -> BOOLEAN]      was the deadline already due at admission?
    deadlineDue,
    \* ---- latched admission evidence (so InvPermitFirstDeadline is prime-free) ----
    \* @type: [Node -> BOOLEAN]      admission observed an admissible permit
    admissionSawPermit,
    \* @type: [Node -> BOOLEAN]      admission observed a due deadline
    admissionSawDue,
    \* ---- history counters (for InvPermitConservation / InvGrantCommitCoupling) ----
    \* @type: Nat
    acceptedReleaseCount,
    \* @type: Nat
    acquiredCount,
    \* ---- HISTORY: ghost evidence so P2/P3/P4 are state invariants ----
    \* @type: ActionKind
    lastAction,
    \* @type: Node \cup {NoNode}
    lastGrantedNode,
    \* @type: Node \cup {NoNode}
    expectedFIFOHead,
    \* @type: 0..MaxPermits
    preAvailable,
    \* @type: Nat
    preAcceptedReleaseCount,
    \* @type: Nat
    preAcquiredCount

NodeState == {"Detached", "Registered", "Woken", "Cancelled", "Expired"}
AdmissionPhase == {"NoAdmission", "AdmissionOpen", "Suspended"}
ActionKind == {"Init", "Acquire", "ReleaseTransfer", "ReleaseStore",
               "ReleaseOverflow", "Cancel", "Expire", "Register", "Suspend"}
\* Sentinel for "no node". Nodes N0/N1/N2 are model values; an integer never
\* equals a model value, so 999 is a safe distinct sentinel.
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

\* A node is ELIGIBLE for a release grant iff it is Registered, Suspended (its
\* admission window committed), and still demands a permit. (Physical terminal
\* nodes awaiting cleanup do not count; under Conclusion A a live release does
\* not observe them linked.)
Eligible(n) ==
    /\ nodeState[n] = "Registered"
    /\ admissionPhase[n] = "Suspended"

EligibleSet ==
    {n \in Nodes : Eligible(n)}

\* Membership in the queue. (queue is a Seq(Node) = a function 1..Len -> Node;
\* x \in queue tests DOMAIN membership, NOT value membership, so we define the
\* value-membership predicate explicitly.)
InQueue(n) ==
    \E i \in 1..Len(queue) : queue[i] = n

\* Remove node n from the queue, preserving order of the rest.
RemoveFromQueue(q, n) ==
    SelectSeq(q, LAMBDA m : m # n)

\* The FIFO-ordered projection of the eligible set, in queue order.
\* queue is a Seq(Node); SelectSeq preserves order.
EligibleQueue ==
    SelectSeq(queue, LAMBDA n : Eligible(n))

\* The FIFO head of the eligible set, or NoNode if none.
FIFOHead ==
    IF EligibleQueue = <<>> THEN NoNode ELSE Head(EligibleQueue)

\* An admissible immediate acquire: a stored permit exists AND no eligible
\* waiter has FIFO priority (no barging, A2).
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

\* HISTORY helper: latch the pre-action authoritative + history state.
SnapPre ==
    /\ preAvailable' = available
    /\ preAcceptedReleaseCount' = acceptedReleaseCount
    /\ preAcquiredCount' = acquiredCount

\* HISTORY helper: mark a non-release action (no disposition to record).
MarkNonRelease(act) ==
    /\ lastAction' = act
    /\ lastGrantedNode' = NoNode
    /\ expectedFIFOHead' = NoNode
    /\ SnapPre

-------------------------------------------------------------------------------
\* Admission epoch opener. The caller (environment) selects a deadline-due bit
\* for this epoch; the admission window then resolves inline (immediate
\* permit) or commits suspension. We model the deadline-due bit as an
\* environment-chosen input here so BOTH halves of precedence are reachable.
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

\* Immediate acquire (no deadline): an admissible permit exists and no
\* eligible waiter has FIFO priority. Resolve Woken inline; consume one permit.
\* (A2 no-barging is encoded by ImmediateAdmissible requiring EligibleSet={}.)
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

\* Permit-first deadline admission (A4): an admissible permit exists AND the
\* deadline is already due -> resolve Woken (permit beats due deadline).
\* Latches admissionSawPermit + admissionSawDue atomically with the Woken
\* resolution so InvPermitFirstDeadline is a prime-free state predicate.
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

\* No-permit + deadline-due admission (A4): no immediately admissible permit
\* AND the deadline is already due -> resolve Expired. (Production authority
\* for Expired is E11; modeled here to prove the other half of precedence.)
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

\* Suspend commit WITH the admission recheck (closes the register/suspend
\* window): if a permit became admissible between AcquireRegister and now,
\* consume it and resolve Woken; otherwise commit Suspended. This is what
\* makes admission closure NON-VACUOUS: a node can be observed Registered +
\* admissionPhase=AdmissionOpen while available>0 only between Register and
\* the next suspend/immediate action, and InvAdmissionClosure pins the
\* outcome of that observation.
AcquireSuspend(n) ==
    /\ nodeState[n] = "Registered"
    /\ admissionPhase[n] = "AdmissionOpen"
    /\ IF ImmediateAdmissible
       \* Recheck path: consume the permit that appeared, resolve Woken inline.
       THEN /\ nodeState' = [nodeState EXCEPT ![n] = "Woken"]
            /\ admissionPhase' = [admissionPhase EXCEPT ![n] = "NoAdmission"]
            /\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
            /\ wakeDispatched' = wakeDispatched + 1
            /\ available' = available - 1
            /\ acquiredCount' = acquiredCount + 1
            /\ admissionSawPermit' = [admissionSawPermit EXCEPT ![n] = TRUE]
            /\ admissionSawDue' = [admissionSawDue EXCEPT ![n] = deadlineDue[n]]
            /\ queue' = RemoveFromQueue(queue, n)
            /\ UNCHANGED <<deadlineDue, acceptedReleaseCount>>
       \* Commit-suspend path: no admissible permit; park the epoch.
       ELSE /\ admissionPhase' = [admissionPhase EXCEPT ![n] = "Suspended"]
            /\ UNCHANGED <<nodeState, resolvedCount, wakeDispatched,
                           available, acquiredCount,
                           admissionSawPermit, admissionSawDue,
                           queue, deadlineDue, acceptedReleaseCount>>
    /\ MarkNonRelease("Suspend")

-------------------------------------------------------------------------------
\* Release is ATOMIC. Three mutually exclusive dispositions.

\* ReleaseTransfer: an eligible FIFO waiter exists. Transfer the pending permit
\* directly to the FIFO head. available UNCHANGED (no decrement, no underflow).
\* HISTORY: latch expectedFIFOHead BEFORE mutating queue, and record the grant.
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

\* ReleaseStore: no eligible waiter AND available < max. Store the pending
\* permit. HISTORY: no grant node.
ReleaseStore ==
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
\* ResolveCancel: a specific queued node is cancelled (resolve_(Cancelled) +
\* unlink). Mirrors Scheduler::cancel_wait. Conclusion A: cancel is
\* global_mtx_-serialized and unlinks the node in the same critical section;
\* a release arriving after observes the node already gone.
ResolveCancel(n) ==
    /\ nodeState[n] = "Registered"
    /\ InQueue(n)  \* membership gate (must be queued)
    /\ nodeState' = [nodeState EXCEPT ![n] = "Cancelled"]
    /\ admissionPhase' = [admissionPhase EXCEPT ![n] = "NoAdmission"]
    /\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
    /\ wakeDispatched' = wakeDispatched + 1
    /\ queue' = RemoveFromQueue(queue, n)
    /\ MarkNonRelease("Cancel")
    /\ UNCHANGED <<available, acceptedReleaseCount, acquiredCount,
                  deadlineDue, admissionSawPermit, admissionSawDue>>

\* Stutter: no-op (quiescence).
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
\* Invariants. Each is a PURE STATE PREDICATE (no primed variable). Actions
\* freely use primes; invariants do not.

\* P1: Permit Conservation. (initialPermits is modeled as 0 here; the law is
\* stated relative to the model's counters. acceptedReleaseCount counts only
\* transfers and stores.)
InvPermitConservation ==
    available + acquiredCount = 0 + acceptedReleaseCount

\* P-bounds: Permit Bounds.
InvPermitBounds ==
    /\ 0 <= available
    /\ available <= MaxPermits
    /\ MaxPermits > 0

\* P8: Queue Well-Formedness. queue is duplicate-free; every queued node is
\* Registered in an admissible admission phase; no terminal / Detached node
\* is queued. (Mirrors the production intrusive-queue invariants.)
InvQueueWellFormed ==
    /\ \* duplicate-free
       \A i, j \in 1..Len(queue) :
           i # j => queue[i] # queue[j]
    /\ \* only Registered nodes are queued
       \A n \in Nodes :
           InQueue(n) => nodeState[n] = "Registered"
    /\ \* queued nodes are in an admissible admission phase
       \A n \in Nodes :
           InQueue(n) => admissionPhase[n] \in {"AdmissionOpen", "Suspended"}
    /\ \* no terminal node is queued
       \A n \in Nodes :
           isTerminal(n) => ~InQueue(n)

\* P9: Single Resolution (inherited E10).
InvSingleResolution ==
    \A n \in Nodes : resolvedCount[n] <= 1

\* P10: Single Publication.
InvSinglePublication ==
    wakeDispatched = resolvedCount[N0] + resolvedCount[N1] + resolvedCount[N2]

\* InvGrantCommitCoupling: acquiredCount equals the number of Woken nodes.
\* Holds because each Node is ONE wait epoch (no multi-epoch reuse).
InvGrantCommitCoupling ==
    acquiredCount = Cardinality({n \in Nodes : nodeState[n] = "Woken"})

\* P3: FIFO Grant (history-backed state invariant). The last release transfer
\* granted exactly the FIFO head latched before queue mutation.
InvFIFOGrant ==
    lastAction = "ReleaseTransfer" => lastGrantedNode = expectedFIFOHead

\* P6: Admission Closure. A node that observed an admissible permit at
\* admission (admissionSawPermit) resolved Woken, not Registered/Suspended/
\* Cancelled/Expired. (Neg-1 forces a Registered+Suspended node that saw a
\* permit.)
InvAdmissionClosure ==
    \A n \in Nodes :
        admissionSawPermit[n] = TRUE => nodeState[n] = "Woken"

\* P4: Overflow Non-Mutation (history-backed). The last overflow release
\* changed no authoritative counter.
InvOverflowNonMutation ==
    lastAction = "ReleaseOverflow" =>
        /\ available = preAvailable
        /\ acceptedReleaseCount = preAcceptedReleaseCount
        /\ acquiredCount = preAcquiredCount

\* P5: No Idle Permit With Eligible Waiter (the stable-state invariant).
InvNoIdlePermitWithEligibleWaiter ==
    available > 0 => EligibleSet = {}

\* P2: Release Disposition (history-backed). Every accepted release did
\* exactly one of transfer or store (with the precise counter deltas).
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

\* P7: Permit-First Deadline (latched admission evidence; prime-free). A wait
\* that observed BOTH an admissible permit AND a due deadline at admission
\* resolved Woken, not Expired.
InvPermitFirstDeadline ==
    \A n \in Nodes :
        (admissionSawPermit[n] = TRUE /\ admissionSawDue[n] = TRUE)
        => nodeState[n] = "Woken"

Inv == /\ InvPermitConservation
       /\ InvPermitBounds
       /\ InvQueueWellFormed
       /\ InvSingleResolution
       /\ InvSinglePublication
       /\ InvGrantCommitCoupling
       /\ InvFIFOGrant
       /\ InvAdmissionClosure
       /\ InvOverflowNonMutation
       /\ InvNoIdlePermitWithEligibleWaiter
       /\ InvReleaseDisposition
       /\ InvPermitFirstDeadline

=============================================================================

