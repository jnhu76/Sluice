------------------------------- MODULE E12AsyncMutexNegM9 -------------------------------
(*
  NEG-M9 DeadlineRevokesHandoff: a late ExpireAttemptTerminal of a Woken epoch republishes.
  Expected violated property: InvGrantFinality.
  Single-rule difference(s) from E12AsyncMutex noted below. Everything else is identical.
*)
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Fibers, F1, F2, F3, Epochs, E1, E2, E3

(* Fibers is the (small, exhaustive) caller set. Epochs is the wait-epoch set.
   Each Epoch denotes one wait epoch. *)

VARIABLES
    \* ---- authoritative state ----
    \* @type: Fiber \cup {NoOwner}   SOLE ownership authority
    owner,
    \* @type: Seq(Epoch)             FIFO wait queue (only Registered = Suspended)
    queue,
    \* @type: [Epoch -> NodeState]   per-epoch lifecycle
    nodeState,
    \* @type: [Epoch -> Fiber]       the Fiber that registered this epoch
    epochFiber,
    \* @type: [Epoch -> BOOLEAN]     env-chosen: was deadline already due at admission
    deadlineDue,
    \* @type: [Epoch -> BOOLEAN]     runnable-ticket published (M9 publication authority)
    runnablePublished,
    \* @type: [Epoch -> 0..1]        terminal resolution count (M11: <= 1)
    resolutionCount,
    \* @type: [Epoch -> 0..1]        runnable publication count (M9: <= 1)
    publicationCount,
    \* @type: BOOLEAN                destroyed (terminal)
    destroyed,
    \* ---- HISTORY: last action record ----
    \* @type: ActionKind
    lastAction,
    \* @type: Fiber \cup {None}
    lastActor,
    \* @type: Epoch \cup {None}
    lastTargetEpoch,
    \* @type: Epoch \cup {None}
    lastGrantedEpoch,
    \* ---- HISTORY: pre-state ghost evidence (snapshots before last mutation) ----
    \* @type: Fiber \cup {NoOwner}   PREVIOUS ownership (NOT the acting Fiber)
    preOwner,
    \* @type: Seq(Epoch)
    preQueue,
    \* @type: [Epoch -> NodeState]
    preNodeState,
    \* @type: [Epoch -> BOOLEAN]
    prePublished,
    \* @type: [Epoch -> 0..1]
    prePublicationCount,
    \* @type: Epoch \cup {None}      FIFO head latched before queue mutation
    expectedFIFOHead,
    \* ---- latched admission evidence (prime-free history invariants) ----
    \* @type: [Epoch -> BOOLEAN]     admission observed free + FIFO-admissible
    admissionSawFree,
    \* @type: [Epoch -> BOOLEAN]     admission observed deadline already due
    admissionSawDue

NodeState == {"Detached", "Registered", "Woken", "Cancelled", "Expired"}
ActionKind == {"Init",
               "TryLockSuccess", "TryLockFailure",
               "LockImmediate", "LockAdmissionAcquire", "LockAdmissionSuspend",
               "LockUntilImmediate", "LockUntilAdmissionAcquire",
               "LockUntilAdmissionSuspend", "LockUntilDue",
               "UnlockNoWaiter", "UnlockHandoff",
               "CancelSuspended", "ExpireSuspended",
               "CancelAttemptTerminal", "ExpireAttemptTerminal",
               "Destroy"}

\* Sentinels. Fibers/Epochs are model values; an integer never equals a model
\* value, so integer sentinels are distinct from every Fiber/Epoch.
NoOwner == 999
None == 998

ASSUME /\ Fibers # {}
       /\ F1 \in Fibers
       /\ F2 \in Fibers
       /\ F3 \in Fibers
       /\ F1 # F2 /\ F2 # F3 /\ F1 # F3
       /\ Epochs # {}
       /\ E1 \in Epochs
       /\ E2 \in Epochs
       /\ E3 \in Epochs
       /\ E1 # E2 /\ E2 # E3 /\ E1 # E3

Vars == <<owner, queue, nodeState, epochFiber, deadlineDue, runnablePublished,
          resolutionCount, publicationCount, destroyed,
          lastAction, lastActor, lastTargetEpoch, lastGrantedEpoch,
          preOwner, preQueue, preNodeState, prePublished, prePublicationCount,
          expectedFIFOHead, admissionSawFree, admissionSawDue>>

isTerminal(e) == nodeState[e] \in {"Woken", "Cancelled", "Expired"}

\* An epoch is ELIGIBLE for a handoff grant iff it is Registered (= Suspended in
\* production terms). By the atomic-admission refinement, a Registered epoch in
\* the queue has committed make_waiting; there is no Registered-but-running state.
Eligible(e) == nodeState[e] = "Registered"

EligibleSet == {e \in Epochs : Eligible(e)}

\* Value-membership in the queue (queue is a Seq = function 1..Len -> Epoch).
InQueue(e) == \E i \in 1..Len(queue) : queue[i] = e

RemoveFromQueue(q, e) == SelectSeq(q, LAMBDA x : x # e)

\* The FIFO-ordered projection of the eligible set, in queue order.
EligibleQueue == SelectSeq(queue, LAMBDA e : Eligible(e))

FIFOHead == IF EligibleQueue = <<>> THEN None ELSE Head(EligibleQueue)

\* Same projection over the PRE-state queue (for history-backed invariants).
EligiblePreQueue == SelectSeq(preQueue, LAMBDA e : preNodeState[e] = "Registered")

\* An admissible immediate acquisition: mutex free AND no eligible queued waiter
\* has FIFO priority (no barging, M3).
Admissible == /\ owner = NoOwner
              /\ EligibleQueue = <<>>

-------------------------------------------------------------------------------
Init ==
    /\ owner = NoOwner
    /\ queue = <<>>
    /\ nodeState = [e \in Epochs |-> "Detached"]
    /\ epochFiber = [e \in Epochs |-> F1]   \* arbitrary default; meaningful only post-admission
    /\ deadlineDue = [e \in Epochs |-> FALSE]
    /\ runnablePublished = [e \in Epochs |-> FALSE]
    /\ resolutionCount = [e \in Epochs |-> 0]
    /\ publicationCount = [e \in Epochs |-> 0]
    /\ destroyed = FALSE
    /\ lastAction = "Init"
    /\ lastActor = None
    /\ lastTargetEpoch = None
    /\ lastGrantedEpoch = None
    /\ preOwner = NoOwner
    /\ preQueue = <<>>
    /\ preNodeState = [e \in Epochs |-> "Detached"]
    /\ prePublished = [e \in Epochs |-> FALSE]
    /\ prePublicationCount = [e \in Epochs |-> 0]
    /\ expectedFIFOHead = None
    /\ admissionSawFree = [e \in Epochs |-> FALSE]
    /\ admissionSawDue = [e \in Epochs |-> FALSE]

\* HISTORY helper: latch pre-action authoritative + history state BEFORE mutation.
\* preOwner is the PREVIOUS Mutex ownership, NOT the acting Fiber.
SnapPre ==
    /\ preOwner' = owner
    /\ preQueue' = queue
    /\ preNodeState' = nodeState
    /\ prePublished' = runnablePublished
    /\ prePublicationCount' = publicationCount

\* HISTORY helper: record a non-handoff action's last-action metadata + SnapPre.
Mark(act, actor, tgt, granted) ==
    /\ lastAction' = act
    /\ lastActor' = actor
    /\ lastTargetEpoch' = tgt
    /\ lastGrantedEpoch' = granted
    /\ expectedFIFOHead' = None
    /\ SnapPre

-------------------------------------------------------------------------------
\* ---- Non-epoch operations ----

\* TryLockSuccess: no Epoch, no WaitNode resolution. Free + empty queue -> own.
\* Recursive acquisition is forbidden (caller-contract violation in production,
\* §7): the acting Fiber must not already own the mutex.
TryLockSuccess(actor) ==
    /\ ~destroyed
    /\ owner = NoOwner
    /\ actor # owner
    /\ FIFOHead = None
    /\ owner' = actor
    /\ Mark("TryLockSuccess", actor, None, None)
    /\ UNCHANGED <<queue, nodeState, epochFiber, deadlineDue, runnablePublished,
                   resolutionCount, publicationCount, destroyed,
                   admissionSawFree, admissionSawDue>>

\* TryLockFailure: pure operation record. No authoritative mutation. (Retained
\* for safety checking; the production API must not operate after destruction.)
TryLockFailure(actor) ==
    /\ owner # NoOwner \/ FIFOHead # None \/ destroyed
    /\ Mark("TryLockFailure", actor, None, None)
    /\ UNCHANGED <<owner, queue, nodeState, epochFiber, deadlineDue,
                   runnablePublished, resolutionCount, publicationCount, destroyed,
                   admissionSawFree, admissionSawDue>>

-------------------------------------------------------------------------------
\* ---- Admission actions (atomic register + recheck + disposition) ----
\* Each refines the ENTIRE production critical section (register + recheck +
\* inline-Woken | inline-Expired | make_waiting) as ONE atomic step. A Registered
\* epoch in the queue IS a Suspended (make_waiting-committed) epoch.

\* LockImmediate: free + no queued waiter -> acquire without suspension.
LockImmediate(actor, epoch) ==
    /\ ~destroyed
    /\ nodeState[epoch] = "Detached"
    /\ owner = NoOwner
    /\ FIFOHead = None
    /\ owner' = actor
    /\ nodeState' = [nodeState EXCEPT ![epoch] = "Woken"]
    /\ epochFiber' = [epochFiber EXCEPT ![epoch] = actor]
    /\ deadlineDue' = [deadlineDue EXCEPT ![epoch] = FALSE]
    /\ resolutionCount' = [resolutionCount EXCEPT ![epoch] = 1]
    /\ admissionSawFree' = [admissionSawFree EXCEPT ![epoch] = TRUE]
    /\ admissionSawDue' = [admissionSawDue EXCEPT ![epoch] = FALSE]
    /\ Mark("LockImmediate", actor, epoch, epoch)
    /\ UNCHANGED <<queue, runnablePublished, publicationCount, destroyed>>
    \* no runnable publication: the Fiber is running, not suspended (M9)

\* LockAdmissionAcquire: speculatively register, recheck finds free + FIFO head
\* -> inline Woken, unlink speculative registration. Queue ends empty.
LockAdmissionAcquire(actor, epoch) ==
    /\ ~destroyed
    /\ nodeState[epoch] = "Detached"
    /\ owner = NoOwner
    /\ FIFOHead = None
    /\ nodeState' = [nodeState EXCEPT ![epoch] = "Woken"]
    /\ epochFiber' = [epochFiber EXCEPT ![epoch] = actor]
    /\ deadlineDue' = [deadlineDue EXCEPT ![epoch] = FALSE]
    /\ resolutionCount' = [resolutionCount EXCEPT ![epoch] = 1]
    /\ owner' = actor
    /\ queue' = <<>>   \* registered then immediately removed (speculative)
    /\ admissionSawFree' = [admissionSawFree EXCEPT ![epoch] = TRUE]
    /\ admissionSawDue' = [admissionSawDue EXCEPT ![epoch] = FALSE]
    /\ Mark("LockAdmissionAcquire", actor, epoch, epoch)
    /\ UNCHANGED <<runnablePublished, publicationCount, destroyed>>
    \* no runnable publication (Fiber running)

\* LockAdmissionSuspend: register, recheck finds owned OR older waiter -> suspend.
\* Post-state: owner /= NoOwner OR older eligible waiter; epoch Registered in queue.
LockAdmissionSuspend(actor, epoch) ==
    /\ ~destroyed
    /\ nodeState[epoch] = "Detached"
    /\ ~(owner = NoOwner /\ FIFOHead = None)
    /\ actor # owner   \* recursive lock forbidden (§7.2 caller-contract violation)
    /\ queue' = Append(queue, epoch)
    /\ nodeState' = [nodeState EXCEPT ![epoch] = "Registered"]
    /\ epochFiber' = [epochFiber EXCEPT ![epoch] = actor]
    /\ deadlineDue' = [deadlineDue EXCEPT ![epoch] = FALSE]
    /\ admissionSawFree' = [admissionSawFree EXCEPT ![epoch] = FALSE]
    /\ admissionSawDue' = [admissionSawDue EXCEPT ![epoch] = FALSE]
    /\ Mark("LockAdmissionSuspend", actor, epoch, None)
    /\ UNCHANGED <<owner, runnablePublished, resolutionCount, publicationCount,
                   destroyed>>
    \* no resolution, no publication YET (suspension committed; publication on wake)

\* LockUntilImmediate: free + due deadline -> acquire (resource-first, M7).
\* An immediately admissible ownership beats an already-due deadline.
LockUntilImmediate(actor, epoch) ==
    /\ ~destroyed
    /\ nodeState[epoch] = "Detached"
    /\ owner = NoOwner
    /\ FIFOHead = None
    /\ owner' = actor
    /\ nodeState' = [nodeState EXCEPT ![epoch] = "Woken"]
    /\ epochFiber' = [epochFiber EXCEPT ![epoch] = actor]
    /\ deadlineDue' = [deadlineDue EXCEPT ![epoch] = TRUE]
    /\ resolutionCount' = [resolutionCount EXCEPT ![epoch] = 1]
    /\ admissionSawFree' = [admissionSawFree EXCEPT ![epoch] = TRUE]
    /\ admissionSawDue' = [admissionSawDue EXCEPT ![epoch] = TRUE]
    /\ Mark("LockUntilImmediate", actor, epoch, epoch)
    /\ UNCHANGED <<queue, runnablePublished, publicationCount, destroyed>>

\* LockUntilAdmissionAcquire: registered, recheck finds admissible. A due deadline
\* loses to immediately admissible ownership (M7). Env-chosen due latched.
LockUntilAdmissionAcquire(actor, epoch) ==
    /\ ~destroyed
    /\ nodeState[epoch] = "Detached"
    /\ owner = NoOwner
    /\ FIFOHead = None
    /\ \E due \in BOOLEAN :
        /\ deadlineDue' = [deadlineDue EXCEPT ![epoch] = due]
        /\ admissionSawDue' = [admissionSawDue EXCEPT ![epoch] = due]
    /\ nodeState' = [nodeState EXCEPT ![epoch] = "Woken"]
    /\ epochFiber' = [epochFiber EXCEPT ![epoch] = actor]
    /\ resolutionCount' = [resolutionCount EXCEPT ![epoch] = 1]
    /\ owner' = actor
    /\ queue' = <<>>   \* registered then immediately removed
    /\ admissionSawFree' = [admissionSawFree EXCEPT ![epoch] = TRUE]
    /\ Mark("LockUntilAdmissionAcquire", actor, epoch, epoch)
    /\ UNCHANGED <<runnablePublished, publicationCount, destroyed>>

\* LockUntilAdmissionSuspend: not admissible + deadline NOT due -> suspend timed.
LockUntilAdmissionSuspend(actor, epoch) ==
    /\ ~destroyed
    /\ nodeState[epoch] = "Detached"
    /\ ~(owner = NoOwner /\ FIFOHead = None)
    /\ actor # owner   \* recursive lock forbidden (§7.2 caller-contract violation)
    /\ deadlineDue' = [deadlineDue EXCEPT ![epoch] = FALSE]
    /\ queue' = Append(queue, epoch)
    /\ nodeState' = [nodeState EXCEPT ![epoch] = "Registered"]
    /\ epochFiber' = [epochFiber EXCEPT ![epoch] = actor]
    /\ admissionSawFree' = [admissionSawFree EXCEPT ![epoch] =
                                (owner = NoOwner /\ FIFOHead = None)]
    /\ admissionSawDue' = [admissionSawDue EXCEPT ![epoch] = FALSE]
    /\ Mark("LockUntilAdmissionSuspend", actor, epoch, None)
    /\ UNCHANGED <<owner, runnablePublished, resolutionCount, publicationCount,
                   destroyed>>

\* LockUntilDue: not admissible + deadline ALREADY due -> Expired at admission.
\* No runnable publication (Fiber has not suspended).
LockUntilDue(actor, epoch) ==
    /\ ~destroyed
    /\ nodeState[epoch] = "Detached"
    /\ ~(owner = NoOwner /\ FIFOHead = None)
    /\ actor # owner   \* recursive lock forbidden (§7.2 caller-contract violation)
    /\ deadlineDue' = [deadlineDue EXCEPT ![epoch] = TRUE]
    /\ nodeState' = [nodeState EXCEPT ![epoch] = "Expired"]
    /\ epochFiber' = [epochFiber EXCEPT ![epoch] = actor]
    /\ resolutionCount' = [resolutionCount EXCEPT ![epoch] = 1]
    /\ queue' = <<>>   \* registered then immediately expired + removed
    /\ admissionSawFree' = [admissionSawFree EXCEPT ![epoch] = FALSE]
    /\ admissionSawDue' = [admissionSawDue EXCEPT ![epoch] = TRUE]
    /\ Mark("LockUntilDue", actor, epoch, None)
    /\ UNCHANGED <<owner, runnablePublished, publicationCount, destroyed>>

-------------------------------------------------------------------------------
\* ---- Unlock operations ----

\* UnlockNoWaiter: owner unlocks, queue empty -> owner := NoOwner. No publication.
UnlockNoWaiter(actor) ==
    /\ ~destroyed
    /\ owner = actor
    /\ FIFOHead = None
    /\ owner' = NoOwner
    /\ Mark("UnlockNoWaiter", actor, None, None)
    /\ UNCHANGED <<queue, nodeState, epochFiber, deadlineDue, runnablePublished,
                   resolutionCount, publicationCount, destroyed,
                   admissionSawFree, admissionSawDue>>

\* UnlockHandoff: direct ownership handoff to the eligible FIFO head. Publishes
\* ONLY a Suspended (Registered) waiter. Atomic coupling (M4/M5):
\*   winner resolve Woken
\*   owner := winner Fiber          (BEFORE publication)
\*   runnablePublished := TRUE      (publication AFTER owner commit)
\* No intermediate owner := NoOwner.
UnlockHandoff(actor) ==
    /\ ~destroyed
    /\ owner = actor
    /\ FIFOHead # None
    /\ nodeState[FIFOHead] = "Registered"
    /\ epochFiber[FIFOHead] # None
    /\ expectedFIFOHead' = FIFOHead
    /\ LET w == FIFOHead IN
       /\ nodeState' = [nodeState EXCEPT ![w] = "Woken"]
       /\ resolutionCount' = [resolutionCount EXCEPT ![w] = 1]
       /\ owner' = epochFiber[w]               \* owner commit
       /\ runnablePublished' = [runnablePublished EXCEPT ![w] = TRUE]  \* publication
       /\ publicationCount' = [publicationCount EXCEPT ![w] = 1]
       /\ queue' = RemoveFromQueue(queue, w)
       /\ lastAction' = "UnlockHandoff"
       /\ lastActor' = actor
       /\ lastTargetEpoch' = w
       /\ lastGrantedEpoch' = w
       /\ SnapPre
    /\ UNCHANGED <<epochFiber, deadlineDue, destroyed,
                   admissionSawFree, admissionSawDue>>

-------------------------------------------------------------------------------
\* ---- Cancel / Expire of a SUSPENDED waiter (publishes) ----

\* CancelSuspended: Registered queued epoch -> Cancelled, unlink, publish once.
\* owner unchanged.
CancelSuspended(epoch) ==
    /\ nodeState[epoch] = "Registered"
    /\ InQueue(epoch)
    /\ nodeState' = [nodeState EXCEPT ![epoch] = "Cancelled"]
    /\ resolutionCount' = [resolutionCount EXCEPT ![epoch] = 1]
    /\ runnablePublished' = [runnablePublished EXCEPT ![epoch] = TRUE]
    /\ publicationCount' = [publicationCount EXCEPT ![epoch] = 1]
    /\ queue' = RemoveFromQueue(queue, epoch)
    /\ Mark("CancelSuspended", None, epoch, None)
    /\ UNCHANGED <<owner, epochFiber, deadlineDue, destroyed,
                   admissionSawFree, admissionSawDue>>

\* ExpireSuspended: Registered queued epoch -> Expired, unlink, publish once.
\* owner unchanged.
ExpireSuspended(epoch) ==
    /\ nodeState[epoch] = "Registered"
    /\ InQueue(epoch)
    /\ nodeState' = [nodeState EXCEPT ![epoch] = "Expired"]
    /\ resolutionCount' = [resolutionCount EXCEPT ![epoch] = 1]
    /\ runnablePublished' = [runnablePublished EXCEPT ![epoch] = TRUE]
    /\ publicationCount' = [publicationCount EXCEPT ![epoch] = 1]
    /\ queue' = RemoveFromQueue(queue, epoch)
    /\ Mark("ExpireSuspended", None, epoch, None)
    /\ UNCHANGED <<owner, epochFiber, deadlineDue, destroyed,
                   admissionSawFree, admissionSawDue>>

-------------------------------------------------------------------------------
\* ---- Late terminal attempt actions (non-vacuous losers, M8) ----
\* A late cancel/expire against an already-terminal epoch is a no-op loser. These
\* make InvGrantFinality non-vacuous: a late attempt must NOT change owner or
\* republish a Woken winner.

CancelAttemptTerminal(epoch) ==
    /\ nodeState[epoch] \in {"Woken", "Cancelled", "Expired"}
    /\ Mark("CancelAttemptTerminal", None, epoch, None)
    /\ UNCHANGED <<owner, queue, nodeState, epochFiber, deadlineDue,
                   runnablePublished, resolutionCount, publicationCount, destroyed,
                   admissionSawFree, admissionSawDue>>

ExpireAttemptTerminal(epoch) ==
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

-------------------------------------------------------------------------------
\* ---- Destruction ----

Destroy ==
    /\ ~destroyed
    /\ owner = NoOwner
    /\ queue = <<>>
    /\ destroyed' = TRUE
    /\ Mark("Destroy", None, None, None)
    /\ UNCHANGED <<owner, queue, nodeState, epochFiber, deadlineDue,
                   runnablePublished, resolutionCount, publicationCount,
                   admissionSawFree, admissionSawDue>>

-------------------------------------------------------------------------------
\* Stutter: no-op (quiescence / post-Destroy absorption).
Stutter == UNCHANGED Vars

Next ==
    \/ Stutter
    \/ \E f \in Fibers : TryLockSuccess(f)
    \/ \E f \in Fibers : TryLockFailure(f)
    \/ \E f \in Fibers : \E e \in Epochs : LockImmediate(f, e)
    \/ \E f \in Fibers : \E e \in Epochs : LockAdmissionAcquire(f, e)
    \/ \E f \in Fibers : \E e \in Epochs : LockAdmissionSuspend(f, e)
    \/ \E f \in Fibers : \E e \in Epochs : LockUntilImmediate(f, e)
    \/ \E f \in Fibers : \E e \in Epochs : LockUntilAdmissionAcquire(f, e)
    \/ \E f \in Fibers : \E e \in Epochs : LockUntilAdmissionSuspend(f, e)
    \/ \E f \in Fibers : \E e \in Epochs : LockUntilDue(f, e)
    \/ \E f \in Fibers : UnlockNoWaiter(f)
    \/ \E f \in Fibers : UnlockHandoff(f)
    \/ \E e \in Epochs : CancelSuspended(e)
    \/ \E e \in Epochs : ExpireSuspended(e)
    \/ \E e \in Epochs : CancelAttemptTerminal(e)
    \/ \E e \in Epochs : ExpireAttemptTerminal(e)
    \/ Destroy

Spec == Init /\ [][Next]_Vars

-------------------------------------------------------------------------------
\* Invariants. Each is a PURE STATE PREDICATE (no primed variable).

\* ---- State invariants ----

InvType ==
    owner \in Fibers \cup {NoOwner}

InvQueueWellFormed ==
    /\ \* duplicate-free
       \A i, j \in 1..Len(queue) : i # j => queue[i] # queue[j]
    /\ \* only Registered (= Suspended) epochs are queued
       \A e \in Epochs : InQueue(e) => nodeState[e] = "Registered"
    /\ \* no terminal epoch is queued
       \A e \in Epochs : isTerminal(e) => ~InQueue(e)

InvSingleResolution ==
    \A e \in Epochs : resolutionCount[e] <= 1

InvSinglePublication ==
    \A e \in Epochs : publicationCount[e] <= 1

\* Publication flag and count are tightly coupled.
InvPublicationConsistency ==
    \A e \in Epochs : runnablePublished[e] = (publicationCount[e] = 1)

\* If no owner, no eligible waiter is queued (one-way; an owned Mutex may have an
\* empty queue). Do NOT use IsUnlocked <=> queue empty.
InvNoOwnerlessQueuedDemand ==
    owner = NoOwner => EligibleQueue = <<>>

\* A published epoch must be terminal.
InvPublishedEpochTerminal ==
    \A e \in Epochs :
        runnablePublished[e] = TRUE => nodeState[e] \in {"Woken", "Cancelled", "Expired"}

\* ---- History-backed transition properties ----

\* Only the owner may unlock.
InvUnlockAuthority ==
    (lastAction = "UnlockNoWaiter" \/ lastAction = "UnlockHandoff")
    => preOwner = lastActor

\* A successful acquisition requires the mutex was previously unowned.
InvRecursiveForbidden ==
    lastAction \in {"TryLockSuccess", "LockImmediate", "LockUntilImmediate",
                    "LockAdmissionAcquire", "LockUntilAdmissionAcquire"}
    => preOwner = NoOwner

\* A handoff grants exactly the eligible FIFO head latched before mutation.
InvFIFOGrant ==
    lastAction = "UnlockHandoff" => lastGrantedEpoch = expectedFIFOHead

\* Handoff targets the eligible FIFO head of the pre-state queue (the FIFO
\* grant rule, expressed over pre-state). Cancel/Expire may target ANY eligible
\* queued member (a caller cancels/expires its OWN node, which need not be the
\* head), so they are gated only by membership, not by head position. UnlockHandoff
\* is the only action with a FIFO-position obligation.
InvEligiblePreQueue ==
    lastAction = "UnlockHandoff"
    => /\ Len(EligiblePreQueue) > 0
       /\ lastTargetEpoch = Head(EligiblePreQueue)

\* No barging: an immediate/admission-acquire requires pre-state free AND no
\* eligible queued waiter.
InvNoBarging ==
    lastAction \in {"TryLockSuccess", "LockImmediate", "LockUntilImmediate",
                    "LockAdmissionAcquire", "LockUntilAdmissionAcquire"}
    => /\ preOwner = NoOwner
       /\ EligiblePreQueue = <<>>

\* An admission-acquire target must be the eligible FIFO head of the pre queue
\* (its speculative registration is admissible only at the head).
InvAdmissionFIFO ==
    lastAction \in {"LockAdmissionAcquire", "LockUntilAdmissionAcquire"}
    => /\ Len(EligiblePreQueue) > 0
       /\ lastTargetEpoch = Head(EligiblePreQueue)
       \/ preQueue = <<>>

\* Grant owner commit: a handoff commits ownership to the granted epoch's Fiber.
InvGrantOwnerCommit ==
    lastAction = "UnlockHandoff"
    => /\ lastGrantedEpoch # None
       /\ owner = epochFiber[lastGrantedEpoch]

\* Grant/publication coupling: handoff publishes iff ownership committed, and the
\* publication is exactly +1 over the pre-state count.
InvGrantPublicationCoupling ==
    lastAction = "UnlockHandoff"
    => /\ owner = epochFiber[lastGrantedEpoch]
       /\ runnablePublished[lastGrantedEpoch] = TRUE
       /\ prePublished[lastGrantedEpoch] = FALSE
       /\ publicationCount[lastGrantedEpoch] = prePublicationCount[lastGrantedEpoch] + 1

\* Admission closure: suspension occurs only when NOT admissible at the pre-state
\* (owned OR an older eligible waiter exists). Guards Head against empty queue.
InvAdmissionClosure ==
    (lastAction = "LockAdmissionSuspend" \/ lastAction = "LockUntilAdmissionSuspend")
    => /\ preOwner # NoOwner
       \/ /\ Len(EligiblePreQueue) > 0
          /\ Head(EligiblePreQueue) # lastTargetEpoch

\* Deadline precedence: a timed immediate/acquire observed free AND due resolves
\* Woken (resource-first; immediately admissible beats a due deadline).
InvDeadlinePrecedence ==
    lastAction \in {"LockUntilImmediate", "LockUntilAdmissionAcquire"}
    => /\ admissionSawFree[lastTargetEpoch] = TRUE
       /\ nodeState[lastTargetEpoch] = "Woken"

\* Grant finality: a late terminal cancel/expire attempt against a Woken epoch
\* preserves owner, the Woken outcome, and the publication count.
InvGrantFinality ==
    (lastAction = "CancelAttemptTerminal" \/ lastAction = "ExpireAttemptTerminal")
    /\ preNodeState[lastTargetEpoch] = "Woken"
    => /\ owner = preOwner
       /\ nodeState[lastTargetEpoch] = "Woken"
       /\ publicationCount[lastTargetEpoch] = prePublicationCount[lastTargetEpoch]

\* Publication discipline (M9): a publication is created ONLY by a
\* suspension-resolving action (handoff/cancel/expire), and the published epoch
\* is the action's target (lastGrantedEpoch for handoff, lastTargetEpoch for
\* cancel/expire). Non-publishing actions do not create a publication; this is
\* expressed by checking that WHEN the last action is a publishing action, the
\* targeted epoch received exactly one fresh publication (count = preCount + 1
\* and flag was FALSE -> TRUE). This avoids the stale-count trap: a persistent
\* publicationCount > 0 must not be blamed on a later non-publishing action.
InvPublicationRequiresSuspensionOrHandoff ==
    /\ (lastAction = "UnlockHandoff" =>
           /\ prePublished[lastGrantedEpoch] = FALSE
           /\ runnablePublished[lastGrantedEpoch] = TRUE
           /\ publicationCount[lastGrantedEpoch] = prePublicationCount[lastGrantedEpoch] + 1)
    /\ (lastAction = "CancelSuspended" =>
           /\ prePublished[lastTargetEpoch] = FALSE
           /\ runnablePublished[lastTargetEpoch] = TRUE
           /\ publicationCount[lastTargetEpoch] = prePublicationCount[lastTargetEpoch] + 1)
    /\ (lastAction = "ExpireSuspended" =>
           /\ prePublished[lastTargetEpoch] = FALSE
           /\ runnablePublished[lastTargetEpoch] = TRUE
           /\ publicationCount[lastTargetEpoch] = prePublicationCount[lastTargetEpoch] + 1)
    /\ (lastAction \in {"TryLockSuccess", "TryLockFailure", "LockImmediate",
                        "LockAdmissionAcquire", "LockUntilImmediate",
                        "LockUntilAdmissionAcquire", "UnlockNoWaiter",
                        "CancelAttemptTerminal", "ExpireAttemptTerminal",
                        "Destroy"}
        => \A e \in Epochs : publicationCount[e] = prePublicationCount[e])

\* Destruction precondition.
InvDestructionPrecondition ==
    lastAction = "Destroy" => /\ preOwner = NoOwner
                               /\ preQueue = <<>>

-------------------------------------------------------------------------------
Inv == /\ InvType
       /\ InvQueueWellFormed
       /\ InvSingleResolution
       /\ InvSinglePublication
       /\ InvPublicationConsistency
       /\ InvNoOwnerlessQueuedDemand
       /\ InvPublishedEpochTerminal
       /\ InvUnlockAuthority
       /\ InvRecursiveForbidden
       /\ InvFIFOGrant
       /\ InvEligiblePreQueue
       /\ InvNoBarging
       /\ InvAdmissionFIFO
       /\ InvGrantOwnerCommit
       /\ InvGrantPublicationCoupling
       /\ InvAdmissionClosure
       /\ InvDeadlinePrecedence
       /\ InvGrantFinality
       /\ InvPublicationRequiresSuspensionOrHandoff
       /\ InvDestructionPrecondition

=============================================================================
