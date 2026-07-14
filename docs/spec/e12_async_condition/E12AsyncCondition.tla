------------------------------- MODULE E12AsyncCondition -------------------------------
\* sluice::async::AsyncCondition -- first-scope Fiber-suspending async Condition
\* SAFETY model (E12-D-CONDITION-PREPARATION-CORRECTIVE-1, authority
\* docs/e12-condition.md). The gate is
\* scripts/verify-e12-async-condition-formal.sh; see README.md for the design.
\*
\* The load-bearing E12-D questions, closed by C-H1..C-H10:
\*   C-H1  Model A: mandatory Mutex reacquire before return.
\*   C-H2  one bound AsyncMutex at construction.
\*   C-H3  wake then ordinary AsyncMutex reacquire.
\*   C-H4  deadline governs only Condition epoch.
\*   C-H5  reacquire untimed and non-cancellable.
\*   C-H6  notify_all included in first scope.
\*   C-H7  one caller Condition node; stack-local reacquire node.
\*   C-H8  ordinary Mutex FIFO-tail reacquire ordering.
\*   C-H9  wait morph deferred.
\*   C-H10 notify_all is an atomic snapshot/drain.
\*
\* SCOPE. SAFETY-ONLY model. The model has two FIFO queues:
\*   - conditionQueue: Seq(ConditionEpoch) -- the Condition wait list
\*   - mutexQueue: Seq(MutexEpoch)        -- the unified Mutex wait list,
\*                                           containing both reacquire epochs
\*                                           (from Condition waiters) and ordinary
\*                                           epochs (from plain lock() callers)
\*
\* The wait-phase state machine (per Fiber):
\*   Idle -> ConditionWaiting -> ConditionResolved -> ReacquirePending
\*       -> MutexWaiting (if contended) -> Returned
\*
\* The due-inline exception (deadline already due at admission):
\*   Idle -> Returned (still owning Mutex, no Condition queue, no reacquire)
\*
\* Refinement map:
\*   WaitDueInline              <-> condition_wait_until due admission
\*   WaitAdmissionSuspend       <-> CONDITION-WAIT-PREPARE (combined seam)
\*   NotifyOne                  <-> condition notify_one
\*   NotifyAll                  <-> condition notify_all (snapshot-drain)
\*   CancelCondition            <-> condition cancel
\*   ExpireCondition            <-> E11 timer expiry on Condition node
\*   TerminalAttempt            <-> late cancel/expire on terminal node
\*   BeginReacquire             <-> resumed Fiber enters reacquire body
\*   ReacquireImmediate         <-> mutex lock admission: free + empty -> own
\*   ReacquireSuspend           <-> mutex lock admission: register+suspend
\*   OrdinaryLockImmediate      <-> ordinary mutex lock free+empty -> own
\*   OrdinaryLockSuspend        <-> ordinary mutex lock register+suspend
\*   MutexUnlockNoWaiter        <-> mutex unlock empty queue: owner := NoOwner
\*   MutexUnlockHandoff         <-> MUTEX-HANDOFF-ONE: owner := winner BEFORE publication
\*   ReturnOwned                <-> wait() function return (Mutex held)
\*   Destroy                    <-> ~AsyncCondition precondition assert
\*
\* Note: Mutex unlock actions are modeled on the bound AsyncMutex, NOT on a
\* separate Mutex primitive. The Condition owns one bound Mutex (C-H2).
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS
    Fibers, F1, F2, F3,
    ConditionEpochs, C1, C2, C3,
    ReacquireEpochs, R1, R2, R3,
    OrdinaryEpochs, O1, O2

\* Unified mutex epoch domain (reacquire + ordinary).
MutexEpochs == ReacquireEpochs \cup OrdinaryEpochs

VARIABLES
    \* ---- authoritative state ----
    \* @type: Fiber \cup {NoOwner}
    mutexOwner,
    \* @type: Seq(ConditionEpoch)
    conditionQueue,
    \* @type: Seq(MutexEpochs)
    mutexQueue,
    \* @type: [ConditionEpoch -> {Detached, Registered, Woken, Cancelled, Expired}]
    conditionNodeState,
    \* @type: [MutexEpochs -> {Detached, Registered, Woken}]
    mutexNodeState,
    \* @type: [OrdinaryEpochs -> Fiber]
    ordinaryEpochFiber,
    \* @type: [Fiber ->
    \*    {Idle, ConditionWaiting, ConditionResolved,
    \*     ReacquirePending, MutexWaiting, Returned}]
    waitPhase,
    \* @type: [Fiber -> {None, Woken, Cancelled, Expired}]
    conditionReason,
    \* @type: [ConditionEpoch -> BOOLEAN]
    deadlineDue,
    \* @type: [ConditionEpoch -> 0..1]
    conditionResolutionCount,
    \* @type: [ConditionEpoch -> 0..1]
    conditionPublicationCount,
    \* @type: [MutexEpochs -> 0..1]
    mutexResolutionCount,
    \* @type: [MutexEpochs -> 0..1]
    mutexPublicationCount,
    \* @type: BOOLEAN
    destroyed,
    \* ---- HISTORY: last action metadata ----
    \* @type: ActionKind
    lastAction,
    \* @type: Fiber \cup {None}
    lastActor,
    \* @type: ConditionEpoch \cup {MutexEpochs} \cup {None}
    lastTargetEpoch,
    \* @type: MutexEpochs \cup {None}
    lastGrantedEpoch,
    \* ---- HISTORY: pre-state snapshots ----
    \* @type: Fiber \cup {NoOwner}
    preMutexOwner,
    \* @type: Seq(ConditionEpoch)
    preConditionQueue,
    \* @type: Seq(MutexEpochs)
    preMutexQueue,
    \* @type: [ConditionEpoch -> {Detached, Registered, Woken, Cancelled, Expired}]
    preConditionNodeState,
    \* @type: [MutexEpochs -> {Detached, Registered, Woken}]
    preMutexNodeState,
    \* @type: [ConditionEpoch -> 0..1]
    preConditionPublicationCount,
    \* @type: [MutexEpochs -> 0..1]
    preMutexPublicationCount,
    \* @type: [Fiber -> {None, Woken, Cancelled, Expired}]
    preConditionReason,
    \* @type: MutexEpochs \cup {None}
    expectedFIFOHead,
    \* ---- notify_all snapshot ghost ----
    \* @type: Seq(ConditionEpoch)
    notifyAllSnapshot,
    \* ---- registration-order ghost (lost-notify proof) ----
    \* @type: [ConditionEpoch -> BOOLEAN]
    registrationCommitted

NodeState == {"Detached", "Registered", "Woken", "Cancelled", "Expired"}
MutexNodeState == {"Detached", "Registered", "Woken"}
ActionKind == {"Init",
               "WaitDueInline",
               "WaitAdmissionSuspend",
               "NotifyOne", "NotifyAll",
               "CancelCondition", "ExpireCondition",
               "TerminalAttempt",
               "BeginReacquire",
               "ReacquireImmediate", "ReacquireSuspend",
               "OrdinaryLockImmediate", "OrdinaryLockSuspend",
               "MutexUnlockNoWaiter", "MutexUnlockHandoff",
               "ReturnOwned",
               "Destroy"}

NoOwner == 999
None == 998

ASSUME /\ Fibers # {}
       /\ F1 \in Fibers /\ F2 \in Fibers /\ F3 \in Fibers
       /\ F1 # F2 /\ F2 # F3 /\ F1 # F3
       /\ ConditionEpochs # {}
       /\ C1 \in ConditionEpochs /\ C2 \in ConditionEpochs /\ C3 \in ConditionEpochs
       /\ C1 # C2 /\ C2 # C3 /\ C1 # C3
       /\ ReacquireEpochs # {}
       /\ R1 \in ReacquireEpochs /\ R2 \in ReacquireEpochs /\ R3 \in ReacquireEpochs
       /\ R1 # R2 /\ R2 # R3 /\ R1 # R3
       /\ OrdinaryEpochs # {}
       /\ O1 \in OrdinaryEpochs /\ O2 \in OrdinaryEpochs
       /\ O1 # O2
       \* Domain separation: no overlap between epoch types.
       /\ ConditionEpochs \cap ReacquireEpochs = {}
       /\ ConditionEpochs \cap OrdinaryEpochs = {}
       /\ ReacquireEpochs \cap OrdinaryEpochs = {}

\* Pre-determined ConditionEpoch-to-Fiber mapping.
CondFiber == [c \in ConditionEpochs |->
    IF c = C1 THEN F1 ELSE IF c = C2 THEN F2 ELSE F3]
\* Pre-determined ConditionEpoch-to-ReacquireEpoch mapping.
CondReacquire == [c \in ConditionEpochs |->
    IF c = C1 THEN R1 ELSE IF c = C2 THEN R2 ELSE R3]

\* Per-fiber helper: the ConditionEpoch that Fiber `f` uses.
condEpoch(f) == CHOOSE c \in ConditionEpochs : CondFiber[c] = f

\* Per-fiber helper: the ReacquireEpoch that follows Fiber `f`'s ConditionEpoch.
reacquireEpoch(f) == CondReacquire[condEpoch(f)]

AllMutexEpochs == MutexEpochs

\* ---- Frame tuple ----
Vars == <<mutexOwner, conditionQueue, mutexQueue,
           conditionNodeState, mutexNodeState,
           ordinaryEpochFiber, waitPhase, conditionReason,
           deadlineDue,
           conditionResolutionCount, conditionPublicationCount,
           mutexResolutionCount, mutexPublicationCount,
           destroyed,
           lastAction, lastActor, lastTargetEpoch, lastGrantedEpoch,
           preMutexOwner, preConditionQueue, preMutexQueue,
           preConditionNodeState, preMutexNodeState,
           preConditionPublicationCount, preMutexPublicationCount,
           preConditionReason, expectedFIFOHead,
           notifyAllSnapshot, registrationCommitted>>

\* ---- Helpers ----
isConditionTerminal(c) ==
    conditionNodeState[c] \in {"Woken", "Cancelled", "Expired"}

isMutexTerminal(m) ==
    mutexNodeState[m] \in {"Woken"}

ConditionEligible(c) ==
    conditionNodeState[c] = "Registered"

MutexEligible(m) ==
    mutexNodeState[m] = "Registered"

InConditionQueue(c) ==
    \E i \in 1..Len(conditionQueue) : conditionQueue[i] = c

InMutexQueue(m) ==
    \E i \in 1..Len(mutexQueue) : mutexQueue[i] = m

RemoveFromConditionQueue(q, c) ==
    SelectSeq(q, LAMBDA x : x # c)

RemoveFromMutexQueue(q, m) ==
    SelectSeq(q, LAMBDA x : x # m)

EligibleConditionQueue ==
    SelectSeq(conditionQueue, LAMBDA c : ConditionEligible(c))

EligibleMutexQueue ==
    SelectSeq(mutexQueue, LAMBDA m : MutexEligible(m))

ConditionFIFOHead ==
    IF EligibleConditionQueue = <<>> THEN None ELSE Head(EligibleConditionQueue)

MutexFIFOHead ==
    IF EligibleMutexQueue = <<>> THEN None ELSE Head(EligibleMutexQueue)

\* Pre-state projections (for history-backed invariants).
EligiblePreConditionQueue ==
    SelectSeq(preConditionQueue, LAMBDA c : preConditionNodeState[c] = "Registered")

EligiblePreMutexQueue ==
    SelectSeq(preMutexQueue, LAMBDA m : preMutexNodeState[m] = "Registered")

\* admissible: Mutex free AND no eligible queued waiter has FIFO priority (C-H8).
MutexAdmissible ==
    /\ mutexOwner = NoOwner
    /\ EligibleMutexQueue = <<>>

\* The Fiber that owns a Mutex epoch (reacquire => derived from CondFiber+CondReacquire;
\* ordinary => from ordinaryEpochFiber variable).
mutexEpochFiber(m) ==
    IF m \in ReacquireEpochs
    THEN CondFiber[CHOOSE c \in ConditionEpochs : CondReacquire[c] = m]
    ELSE ordinaryEpochFiber[m]

\* Check whether a fiber has any active wait ongoing.
fiberHasActiveWait(f) ==
    waitPhase[f] \in {"ConditionWaiting", "ConditionResolved",
                       "ReacquirePending", "MutexWaiting"}

\* Conditions for safe destruction.
destructionSafe ==
    /\ conditionQueue = <<>>
    /\ \A f \in Fibers : ~fiberHasActiveWait(f)

\* ---- HISTORY helpers ----
SnapPre ==
    /\ preMutexOwner' = mutexOwner
    /\ preConditionQueue' = conditionQueue
    /\ preMutexQueue' = mutexQueue
    /\ preConditionNodeState' = conditionNodeState
    /\ preMutexNodeState' = mutexNodeState
    /\ preConditionPublicationCount' = conditionPublicationCount
    /\ preMutexPublicationCount' = mutexPublicationCount
    /\ preConditionReason' = conditionReason

Mark(act, actor, tgt, granted) ==
    /\ lastAction' = act
    /\ lastActor' = actor
    /\ lastTargetEpoch' = tgt
    /\ lastGrantedEpoch' = granted
    \* Preserve the handoff's explicit expectedFIFOHead assignment. MutexUnlockHandoff
    \* sets expectedFIFOHead' = w (the granted FIFO head) in its body; resetting it to
    \* None here would make the conjunction unsatisfiable (w # None) and silence the
    \* action entirely. Only non-handoff actions reset the ghost to None.
    /\ (act # "MutexUnlockHandoff") => expectedFIFOHead' = None
    /\ SnapPre

\* ===========================================================================
\* Init
\* ===========================================================================
Init ==
    /\ mutexOwner = NoOwner
    /\ conditionQueue = <<>>
    /\ mutexQueue = <<>>
    /\ conditionNodeState = [c \in ConditionEpochs |-> "Detached"]
    /\ mutexNodeState = [m \in AllMutexEpochs |-> "Detached"]
    /\ ordinaryEpochFiber = [o \in OrdinaryEpochs |-> F1]
    /\ waitPhase = [f \in Fibers |-> "Idle"]
    /\ conditionReason = [f \in Fibers |-> "None"]
    /\ deadlineDue = [c \in ConditionEpochs |-> FALSE]
    /\ conditionResolutionCount = [c \in ConditionEpochs |-> 0]
    /\ conditionPublicationCount = [c \in ConditionEpochs |-> 0]
    /\ mutexResolutionCount = [m \in AllMutexEpochs |-> 0]
    /\ mutexPublicationCount = [m \in AllMutexEpochs |-> 0]
    /\ destroyed = FALSE
    /\ lastAction = "Init"
    /\ lastActor = None
    /\ lastTargetEpoch = None
    /\ lastGrantedEpoch = None
    /\ preMutexOwner = NoOwner
    /\ preConditionQueue = <<>>
    /\ preMutexQueue = <<>>
    /\ preConditionNodeState = [c \in ConditionEpochs |-> "Detached"]
    /\ preMutexNodeState = [m \in AllMutexEpochs |-> "Detached"]
    /\ preConditionPublicationCount = [c \in ConditionEpochs |-> 0]
    /\ preMutexPublicationCount = [m \in AllMutexEpochs |-> 0]
    /\ preConditionReason = [f \in Fibers |-> "None"]
    /\ expectedFIFOHead = None
    /\ notifyAllSnapshot = <<>>
    /\ registrationCommitted = [c \in ConditionEpochs |-> FALSE]

\* ---- Common unchanged blocks ----
\* Variables NOT changed by a Condition-phase action.
CondPhaseUnchanged ==
    UNCHANGED <<mutexNodeState, ordinaryEpochFiber, mutexQueue,
                mutexResolutionCount, mutexPublicationCount>>

\* Variables NOT changed by a Mutex-phase action.
MutexPhaseUnchanged ==
    UNCHANGED <<conditionNodeState, conditionQueue,
                conditionResolutionCount, conditionPublicationCount,
                conditionReason, deadlineDue>>

\* ===========================================================================
\* Condition-phase actions
\* ===========================================================================

\* WaitDueInline: deadline already due at admission.
\* Resolve Expired inline, retain Mutex ownership, NO suspension, NO reacquire.
WaitDueInline(actor) ==
    /\ ~destroyed
    /\ waitPhase[actor] = "Idle"
    /\ mutexOwner = actor
    /\ LET c == condEpoch(actor) IN
       /\ conditionNodeState[c] = "Detached"
       /\ deadlineDue[c] = TRUE
       /\ conditionNodeState' = [conditionNodeState EXCEPT ![c] = "Expired"]
       /\ conditionResolutionCount' = [conditionResolutionCount EXCEPT ![c] = 1]
       /\ conditionReason' = [conditionReason EXCEPT ![actor] = "Expired"]
       /\ registrationCommitted' = [registrationCommitted EXCEPT ![c] = TRUE]
       /\ waitPhase' = [waitPhase EXCEPT ![actor] = "Returned"]
       /\ Mark("WaitDueInline", actor, c, None)
    /\ UNCHANGED <<mutexOwner, conditionQueue, mutexQueue,
                   mutexNodeState, ordinaryEpochFiber,
                   mutexResolutionCount, mutexPublicationCount,
                   conditionPublicationCount, deadlineDue,
                   conditionQueue, destroyed, notifyAllSnapshot>>

\* WaitAdmissionSuspend: register Condition node, release Mutex, suspend.
\* Register BEFORE release (lost-notify closure). The action is atomic under
\* global_mtx_ (the coordination domain).
WaitAdmissionSuspend(actor) ==
    /\ ~destroyed
    /\ waitPhase[actor] = "Idle"
    /\ mutexOwner = actor
    /\ LET c == condEpoch(actor) IN
       /\ conditionNodeState[c] = "Detached"
       \* Step 1: Register Condition node (BEFORE release).
       /\ conditionNodeState' = [conditionNodeState EXCEPT ![c] = "Registered"]
       /\ conditionQueue' = Append(conditionQueue, c)
       /\ registrationCommitted' = [registrationCommitted EXCEPT ![c] = TRUE]
       \* Step 2: Release Mutex (or handoff if queued waiters).
       /\ IF EligibleMutexQueue = <<>>
          THEN /\ mutexOwner' = NoOwner
               /\ UNCHANGED <<mutexNodeState, mutexQueue,
                              mutexResolutionCount, mutexPublicationCount>>
          ELSE \* handoff to FIFO head
               LET w == MutexFIFOHead IN
               /\ mutexNodeState' = [mutexNodeState EXCEPT ![w] = "Woken"]
               /\ mutexResolutionCount' = [mutexResolutionCount EXCEPT ![w] = 1]
               /\ mutexPublicationCount' =
                      [mutexPublicationCount EXCEPT ![w] = 1]
               /\ mutexOwner' = mutexEpochFiber(w)
               /\ mutexQueue' = RemoveFromMutexQueue(mutexQueue, w)
       \* Step 3: Commit ConditionWaiting phase.
       /\ waitPhase' = [waitPhase EXCEPT ![actor] = "ConditionWaiting"]
       /\ deadlineDue' = [deadlineDue EXCEPT ![c] = FALSE]
       /\ Mark("WaitAdmissionSuspend", actor, c, None)
    /\ UNCHANGED <<conditionPublicationCount,
                   conditionResolutionCount, ordinaryEpochFiber,
                   conditionReason, destroyed, notifyAllSnapshot>>

\* NotifyOne: resolve eligible FIFO Condition head Woken.
NotifyOne ==
    /\ ~destroyed
    /\ ConditionFIFOHead # None
    /\ LET c == ConditionFIFOHead IN
       /\ conditionNodeState[c] = "Registered"
       /\ conditionNodeState' = [conditionNodeState EXCEPT ![c] = "Woken"]
       /\ conditionResolutionCount' = [conditionResolutionCount EXCEPT ![c] = 1]
       /\ conditionPublicationCount' =
              [conditionPublicationCount EXCEPT ![c] = 1]
       /\ conditionQueue' = RemoveFromConditionQueue(conditionQueue, c)
       /\ LET f == CondFiber[c] IN
          /\ conditionReason' = [conditionReason EXCEPT ![f] = "Woken"]
          /\ waitPhase' = [waitPhase EXCEPT ![f] = "ConditionResolved"]
       /\ Mark("NotifyOne", None, c, None)
    /\ UNCHANGED <<mutexOwner, mutexQueue, mutexNodeState,
                   ordinaryEpochFiber, mutexResolutionCount,
                   mutexPublicationCount, deadlineDue, destroyed,
                   notifyAllSnapshot, registrationCommitted>>

\* CancelCondition: resolve one Registered Condition epoch as Cancelled.
CancelCondition(epoch) ==
    /\ ~destroyed
    /\ conditionNodeState[epoch] = "Registered"
    /\ InConditionQueue(epoch)
    /\ conditionNodeState' = [conditionNodeState EXCEPT ![epoch] = "Cancelled"]
    /\ conditionResolutionCount' = [conditionResolutionCount EXCEPT ![epoch] = 1]
    /\ conditionPublicationCount' =
          [conditionPublicationCount EXCEPT ![epoch] = 1]
    /\ conditionQueue' = RemoveFromConditionQueue(conditionQueue, epoch)
    /\ LET f == CondFiber[epoch] IN
       /\ conditionReason' = [conditionReason EXCEPT ![f] = "Cancelled"]
       /\ waitPhase' = [waitPhase EXCEPT ![f] = "ConditionResolved"]
    /\ Mark("CancelCondition", None, epoch, None)
    /\ UNCHANGED <<mutexOwner, mutexQueue, mutexNodeState,
                   ordinaryEpochFiber, mutexResolutionCount,
                   mutexPublicationCount, deadlineDue, destroyed,
                   notifyAllSnapshot, registrationCommitted>>

\* ExpireCondition: resolve one Registered Condition epoch as Expired (E11 timer).
ExpireCondition(epoch) ==
    /\ ~destroyed
    /\ conditionNodeState[epoch] = "Registered"
    /\ InConditionQueue(epoch)
    /\ conditionNodeState' = [conditionNodeState EXCEPT ![epoch] = "Expired"]
    /\ conditionResolutionCount' = [conditionResolutionCount EXCEPT ![epoch] = 1]
    /\ conditionPublicationCount' =
          [conditionPublicationCount EXCEPT ![epoch] = 1]
    /\ conditionQueue' = RemoveFromConditionQueue(conditionQueue, epoch)
    /\ LET f == CondFiber[epoch] IN
       /\ conditionReason' = [conditionReason EXCEPT ![f] = "Expired"]
       /\ waitPhase' = [waitPhase EXCEPT ![f] = "ConditionResolved"]
    /\ Mark("ExpireCondition", None, epoch, None)
    /\ UNCHANGED <<mutexOwner, mutexQueue, mutexNodeState,
                   ordinaryEpochFiber, mutexResolutionCount,
                   mutexPublicationCount, deadlineDue, destroyed,
                   notifyAllSnapshot, registrationCommitted>>

\* TerminalAttempt: late cancel/expire/notify against an already-terminal epoch.
\* Models C-H5 and C-H4 finality: a late attempt is a no-op loser.
TerminalAttempt(epoch) ==
    /\ isConditionTerminal(epoch)
    /\ Mark("TerminalAttempt", None, epoch, None)
    /\ UNCHANGED <<mutexOwner, conditionQueue, mutexQueue,
                   conditionNodeState, mutexNodeState,
                   ordinaryEpochFiber, waitPhase, conditionReason,
                   deadlineDue,
                   conditionResolutionCount, conditionPublicationCount,
                   mutexResolutionCount, mutexPublicationCount,
                   destroyed, notifyAllSnapshot, registrationCommitted>>

\* ===========================================================================
\* NotifyAll: atomic snapshot/drain
\* ===========================================================================
\* Snapshot = every eligible Condition waiter at the linearization point.
\* All snapshot members resolve Woken exactly once.
\* Waiters registered after the snapshot are excluded.
\* This is ONE atomic action under global_mtx_ (C-H10).
NotifyAll ==
    /\ ~destroyed
    /\ EligibleConditionQueue # <<>>
    /\ notifyAllSnapshot' = conditionQueue
    \* Snapshot epochs as set (pre-drain).
    /\ LET snapSet == {conditionQueue[i] : i \in 1..Len(conditionQueue)}
        IN
        \* Every snapshot member is resolved Woken; others unchanged.
        /\ conditionNodeState' = [c \in ConditionEpochs |->
            IF c \in snapSet THEN "Woken" ELSE conditionNodeState[c]]
        /\ conditionResolutionCount' = [c \in ConditionEpochs |->
            IF c \in snapSet THEN conditionResolutionCount[c] + 1
                              ELSE conditionResolutionCount[c]]
        /\ conditionPublicationCount' = [c \in ConditionEpochs |->
            IF c \in snapSet THEN conditionPublicationCount[c] + 1
                              ELSE conditionPublicationCount[c]]
        /\ conditionReason' = [f \in Fibers |->
            IF condEpoch(f) \in snapSet THEN "Woken" ELSE conditionReason[f]]
        /\ waitPhase' = [f \in Fibers |->
            IF condEpoch(f) \in snapSet THEN "ConditionResolved" ELSE waitPhase[f]]
    \* The queue is drained.
    /\ conditionQueue' = <<>>
    /\ Mark("NotifyAll", None, None, None)
    /\ UNCHANGED <<mutexOwner, mutexQueue, mutexNodeState,
                   ordinaryEpochFiber, mutexResolutionCount,
                   mutexPublicationCount, deadlineDue, destroyed,
                   registrationCommitted, ordinaryEpochFiber>>

\* ===========================================================================
\* Reacquire-phase actions
\* ===========================================================================

\* BeginReacquire: Fiber wakes from ConditionResolved, starts reacquire body.
\* The stack-local reacquire node is in Detached state (C-H7).
BeginReacquire(actor) ==
    /\ waitPhase[actor] = "ConditionResolved"
    /\ LET r == reacquireEpoch(actor) IN
       /\ mutexNodeState[r] = "Detached"
    /\ waitPhase' = [waitPhase EXCEPT ![actor] = "ReacquirePending"]
    /\ Mark("BeginReacquire", actor, None, None)
    /\ UNCHANGED <<mutexOwner, conditionQueue, mutexQueue,
                   conditionNodeState, mutexNodeState,
                   ordinaryEpochFiber, conditionReason, deadlineDue,
                   conditionResolutionCount, conditionPublicationCount,
                   mutexResolutionCount, mutexPublicationCount,
                   destroyed, notifyAllSnapshot, registrationCommitted>>

\* ReacquireImmediate: Mutex free + empty queue -> acquire without suspension.
\* No publication (the Fiber is running, not suspended).
ReacquireImmediate(actor) ==
    /\ ~destroyed
    /\ waitPhase[actor] = "ReacquirePending"
    /\ mutexOwner = NoOwner
    /\ EligibleMutexQueue = <<>>
    /\ LET r == reacquireEpoch(actor) IN
       /\ mutexNodeState[r] = "Detached"
       /\ mutexNodeState' = [mutexNodeState EXCEPT ![r] = "Woken"]
       /\ mutexResolutionCount' = [mutexResolutionCount EXCEPT ![r] = 1]
       /\ mutexOwner' = actor
    /\ Mark("ReacquireImmediate", actor, None, None)
    /\ UNCHANGED <<conditionQueue, mutexQueue,
                   conditionNodeState, ordinaryEpochFiber,
                   conditionReason, deadlineDue,
                   conditionResolutionCount, conditionPublicationCount,
                   mutexPublicationCount, waitPhase, destroyed,
                   notifyAllSnapshot, registrationCommitted>>

\* ReacquireSuspend: Mutex is contended -> register on FIFO tail + suspend.
\* Uses the unified mutex queue (C-H8: ordinary FIFO tail).
ReacquireSuspend(actor) ==
    /\ ~destroyed
    /\ waitPhase[actor] = "ReacquirePending"
    /\ \/ mutexOwner # NoOwner
       \/ EligibleMutexQueue # <<>>
    /\ LET r == reacquireEpoch(actor) IN
       /\ mutexNodeState[r] = "Detached"
       /\ mutexNodeState' = [mutexNodeState EXCEPT ![r] = "Registered"]
       /\ mutexQueue' = Append(mutexQueue, r)
       /\ waitPhase' = [waitPhase EXCEPT ![actor] = "MutexWaiting"]
    /\ Mark("ReacquireSuspend", actor, None, None)
    /\ UNCHANGED <<mutexOwner, conditionQueue,
                   conditionNodeState, ordinaryEpochFiber,
                   conditionReason, deadlineDue,
                   conditionResolutionCount, conditionPublicationCount,
                   mutexResolutionCount, mutexPublicationCount,
                   destroyed, notifyAllSnapshot, registrationCommitted>>

\* ===========================================================================
\* Ordinary Mutex admission actions
\* ===========================================================================

OrdinaryLockImmediate(actor, o) ==
    /\ ~destroyed
    /\ o \in OrdinaryEpochs
    /\ waitPhase[actor] = "Idle"
    /\ mutexOwner = NoOwner
    /\ EligibleMutexQueue = <<>>
    /\ mutexNodeState[o] = "Detached"
    /\ mutexNodeState' = [mutexNodeState EXCEPT ![o] = "Woken"]
    /\ mutexResolutionCount' = [mutexResolutionCount EXCEPT ![o] = 1]
    /\ ordinaryEpochFiber' = [ordinaryEpochFiber EXCEPT ![o] = actor]
    /\ mutexOwner' = actor
    /\ Mark("OrdinaryLockImmediate", actor, o, None)
    /\ UNCHANGED <<conditionQueue, mutexQueue,
                   conditionNodeState, conditionReason, deadlineDue,
                   conditionResolutionCount, conditionPublicationCount,
                   mutexPublicationCount, waitPhase, destroyed,
                   notifyAllSnapshot, registrationCommitted>>

OrdinaryLockSuspend(actor, o) ==
    /\ ~destroyed
    /\ o \in OrdinaryEpochs
    /\ waitPhase[actor] = "Idle"
    /\ \/ mutexOwner # NoOwner
       \/ EligibleMutexQueue # <<>>
    /\ mutexNodeState[o] = "Detached"
    /\ mutexNodeState' = [mutexNodeState EXCEPT ![o] = "Registered"]
    /\ mutexQueue' = Append(mutexQueue, o)
    /\ ordinaryEpochFiber' = [ordinaryEpochFiber EXCEPT ![o] = actor]
    /\ waitPhase' = [waitPhase EXCEPT ![actor] = "MutexWaiting"]
    /\ Mark("OrdinaryLockSuspend", actor, o, None)
    /\ UNCHANGED <<mutexOwner, conditionQueue,
                   conditionNodeState, conditionReason, deadlineDue,
                   conditionResolutionCount, conditionPublicationCount,
                   mutexResolutionCount, mutexPublicationCount,
                   destroyed, notifyAllSnapshot, registrationCommitted>>

\* ===========================================================================
\* Mutex unlock actions (bound AsyncMutex)
\* ===========================================================================

MutexUnlockNoWaiter(actor) ==
    /\ ~destroyed
    /\ mutexOwner = actor
    /\ EligibleMutexQueue = <<>>
    /\ mutexOwner' = NoOwner
    \* If the actor had returned from a condition wait, reset to Idle.
    /\ waitPhase' = [waitPhase EXCEPT ![actor] =
        IF waitPhase[actor] = "Returned" THEN "Idle" ELSE waitPhase[actor]]
    /\ Mark("MutexUnlockNoWaiter", actor, None, None)
    /\ UNCHANGED <<conditionQueue, mutexQueue,
                   conditionNodeState, mutexNodeState,
                   ordinaryEpochFiber, conditionReason,
                   deadlineDue,
                   conditionResolutionCount, conditionPublicationCount,
                   mutexResolutionCount, mutexPublicationCount,
                   destroyed, notifyAllSnapshot, registrationCommitted>>

\* MutexUnlockHandoff: unlock with eligible FIFO head -> direct ownership handoff.
\* Owner := winner BEFORE publication (inherited from E12-C M-H1).
\* Reuses the unified mutex queue (C-H8: FIFO, no distinction between reacquire
\* and ordinary epochs).
MutexUnlockHandoff(actor) ==
    /\ ~destroyed
    /\ mutexOwner = actor
    /\ MutexFIFOHead # None
    /\ LET w == MutexFIFOHead IN
       /\ mutexNodeState[w] = "Registered"
       /\ mutexNodeState' = [mutexNodeState EXCEPT ![w] = "Woken"]
       /\ mutexResolutionCount' = [mutexResolutionCount EXCEPT ![w] = 1]
       /\ expectedFIFOHead' = w
       \* Owner commit (before publication).
       /\ mutexOwner' = mutexEpochFiber(w)
       /\ mutexPublicationCount' =
              [mutexPublicationCount EXCEPT ![w] = 1]
        /\ mutexQueue' = RemoveFromMutexQueue(mutexQueue, w)
        \* Advance the unlocked fiber's phase.
        /\ IF w \in ReacquireEpochs
           THEN LET f == CondFiber[CHOOSE c \in ConditionEpochs :
                                              CondReacquire[c] = w] IN
                waitPhase' = [waitPhase EXCEPT ![actor] = "Idle", ![f] = "Returned"]
           ELSE LET f == ordinaryEpochFiber[w] IN
                waitPhase' = [waitPhase EXCEPT ![actor] = "Idle", ![f] = "Idle"]
       /\ Mark("MutexUnlockHandoff", actor, w, w)
    /\ UNCHANGED <<conditionQueue,
                   conditionNodeState, ordinaryEpochFiber,
                   conditionReason, deadlineDue,
                   conditionResolutionCount, conditionPublicationCount,
                   destroyed, notifyAllSnapshot, registrationCommitted>>

\* ===========================================================================
\* ReturnOwned: complete wait call.
\* Precondition: Fiber holds Mutex and has terminal Condition reason (C-H1).
\* ===========================================================================
ReturnOwned(actor) ==
    /\ ~destroyed
    /\ mutexOwner = actor
    /\ conditionReason[actor] \in {"Woken", "Cancelled", "Expired"}
    /\ waitPhase[actor] \in {"ConditionResolved", "ReacquirePending"}
    /\ waitPhase' = [waitPhase EXCEPT ![actor] = "Returned"]
    /\ Mark("ReturnOwned", actor, None, None)
    /\ UNCHANGED <<mutexOwner, conditionQueue, mutexQueue,
                   conditionNodeState, mutexNodeState,
                   ordinaryEpochFiber, conditionReason, deadlineDue,
                   conditionResolutionCount, conditionPublicationCount,
                   mutexResolutionCount, mutexPublicationCount,
                   destroyed, notifyAllSnapshot, registrationCommitted>>

\* ===========================================================================
\* Destroy
\* ===========================================================================
Destroy ==
    /\ ~destroyed
    /\ destructionSafe
    /\ destroyed' = TRUE
    /\ Mark("Destroy", None, None, None)
    /\ UNCHANGED <<mutexOwner, conditionQueue, mutexQueue,
                   conditionNodeState, mutexNodeState,
                   ordinaryEpochFiber, waitPhase, conditionReason,
                   deadlineDue,
                   conditionResolutionCount, conditionPublicationCount,
                   mutexResolutionCount, mutexPublicationCount,
                   notifyAllSnapshot, registrationCommitted>>

\* ===========================================================================
\* Next and Spec
\* ===========================================================================
Stutter == UNCHANGED Vars

Next ==
    \/ Stutter
    \/ \E f \in Fibers : WaitDueInline(f)
    \/ \E f \in Fibers : WaitAdmissionSuspend(f)
    \/ NotifyOne
    \/ NotifyAll
    \/ \E c \in ConditionEpochs : CancelCondition(c)
    \/ \E c \in ConditionEpochs : ExpireCondition(c)
    \/ \E c \in ConditionEpochs : TerminalAttempt(c)
    \/ \E f \in Fibers : BeginReacquire(f)
    \/ \E f \in Fibers : ReacquireImmediate(f)
    \/ \E f \in Fibers : ReacquireSuspend(f)
    \/ \E f \in Fibers : \E o \in OrdinaryEpochs : OrdinaryLockImmediate(f, o)
    \/ \E f \in Fibers : \E o \in OrdinaryEpochs : OrdinaryLockSuspend(f, o)
    \/ \E f \in Fibers : MutexUnlockNoWaiter(f)
    \/ \E f \in Fibers : MutexUnlockHandoff(f)
    \/ \E f \in Fibers : ReturnOwned(f)
    \/ Destroy

Spec == Init /\ [][Next]_Vars

\* ===========================================================================
\* Invariants
\* ===========================================================================
\* ---- State invariants ----

InvType ==
    /\ mutexOwner \in Fibers \cup {NoOwner}
    /\ conditionQueue \in Seq(ConditionEpochs)
    /\ mutexQueue \in Seq(AllMutexEpochs)

InvConditionQueueWellFormed ==
    /\ \A i, j \in 1..Len(conditionQueue) : i # j =>
           conditionQueue[i] # conditionQueue[j]
    /\ \A c \in ConditionEpochs :
           InConditionQueue(c) => conditionNodeState[c] = "Registered"
    /\ \A c \in ConditionEpochs :
           isConditionTerminal(c) => ~InConditionQueue(c)

InvMutexQueueWellFormed ==
    /\ \A i, j \in 1..Len(mutexQueue) : i # j =>
           mutexQueue[i] # mutexQueue[j]
    /\ \A m \in AllMutexEpochs :
           InMutexQueue(m) => mutexNodeState[m] = "Registered"
    /\ \A m \in AllMutexEpochs :
           isMutexTerminal(m) => ~InMutexQueue(m)

InvSingleConditionResolution ==
    \A c \in ConditionEpochs : conditionResolutionCount[c] <= 1

InvSingleConditionPublication ==
    \A c \in ConditionEpochs : conditionPublicationCount[c] <= 1

InvSingleMutexResolution ==
    \A m \in AllMutexEpochs : mutexResolutionCount[m] <= 1

InvSingleMutexPublication ==
    \A m \in AllMutexEpochs : mutexPublicationCount[m] <= 1

\* A Condition waiter must not own the Mutex (release happened before suspend).
InvConditionWaiterDoesNotOwnMutex ==
    \A f \in Fibers :
        waitPhase[f] = "ConditionWaiting" => mutexOwner # f

\* No dual queue membership: a Fiber is never in Condition AND Mutex queues
\* simultaneously (the two epochs are sequential, C-H3).
InvNoDualQueueMembership ==
    \A f \in Fibers :
        ~( \E c \in ConditionEpochs :
               InConditionQueue(c) /\ CondFiber[c] = f
           /\ \E m \in AllMutexEpochs :
               InMutexQueue(m) /\ mutexEpochFiber(m) = f)

\* Returned always holds Mutex (C-H1).
InvReturnedOwnsMutex ==
    \A f \in Fibers :
        waitPhase[f] = "Returned" => mutexOwner = f

\* Reacquire by the same Fiber that waited (C-H1 identity preservation).
InvReacquireSameFiber ==
    \A c \in ConditionEpochs :
        isConditionTerminal(c)
        => mutexEpochFiber(CondReacquire[c]) = CondFiber[c]

\* No ownerless Mutex demand: if Mutex is free, no eligible waiters queued.
InvNoOwnerlessMutexDemand ==
    mutexOwner = NoOwner => EligibleMutexQueue = <<>>

\* Due-inline preserves ownership.
InvDueInlineRetainsOwnership ==
    lastAction = "WaitDueInline"
    => /\ preMutexOwner = lastActor
       /\ mutexOwner = lastActor

\* Condition resolution finality: cancel/expire targets a Registered node.
InvConditionResolvedFinality ==
    (lastAction = "CancelCondition" \/ lastAction = "ExpireCondition")
    => preConditionNodeState[lastTargetEpoch] = "Registered"

\* Late terminal attempt does not change state (C-H5 finality).
InvTerminalAttemptFinality ==
    lastAction = "TerminalAttempt"
    => /\ isConditionTerminal(lastTargetEpoch)
       /\ conditionNodeState[lastTargetEpoch] =
              preConditionNodeState[lastTargetEpoch]
       /\ conditionPublicationCount[lastTargetEpoch] =
              preConditionPublicationCount[lastTargetEpoch]

\* FIFO grant on Mutex handoff (C-H8).
InvFIFOGrant ==
    lastAction = "MutexUnlockHandoff" => lastGrantedEpoch = expectedFIFOHead

\* Handoff targets the eligible FIFO head of the pre-state queue.
InvEligiblePreMutexQueue ==
    lastAction = "MutexUnlockHandoff"
    => /\ Len(EligiblePreMutexQueue) > 0
       /\ lastTargetEpoch = Head(EligiblePreMutexQueue)

\* Grant owner commit: handoff commits ownership to the granted epoch's Fiber.
InvGrantOwnerCommit ==
    lastAction = "MutexUnlockHandoff"
    => /\ lastGrantedEpoch # None
       /\ mutexOwner = mutexEpochFiber(lastGrantedEpoch)

\* C-H8: ordinary and reacquire FIFO ordering -- the handoff selects the unified
\* eligible FIFO head without regard to epoch kind.
InvOrdinaryAndReacquireFIFO ==
    lastAction = "MutexUnlockHandoff"
    => /\ Len(EligiblePreMutexQueue) > 0
       /\ LET w == Head(EligiblePreMutexQueue) IN
          mutexEpochFiber(w) = mutexOwner

\* NotifyAll snapshot completeness: every waiter in the captured snapshot
\* resolved Woken.
InvNotifyAllSnapshotComplete ==
    lastAction = "NotifyAll"
    => /\ \A i \in 1..Len(notifyAllSnapshot) :
           LET c == notifyAllSnapshot[i] IN
           conditionNodeState[c] = "Woken"

\* NotifyAll excludes late waiters.
InvNotifyAllExcludesLateWaiter ==
    lastAction = "NotifyAll"
    => /\ \A c \in ConditionEpochs :
           conditionNodeState[c] = "Registered"
           => \/ InConditionQueue(c)  \* still queued (registered AFTER)
              \/ \* OR already terminal before the snapshot
                 isConditionTerminal(c)
       \* A waiter that was NOT in the snapshot remains unaffected.
       /\ \A c \in {notifyAllSnapshot[i] : i \in 1..Len(notifyAllSnapshot)} :
          isConditionTerminal(c)

\* Lost-notify closure: registration committed BEFORE Mutex release.
\* The action WaitAdmissionSuspend always registers first, then releases.
\* This invariant checks the ghost evidence that registration happened.
InvNoLostNotifyWindow ==
    lastAction = "WaitAdmissionSuspend"
    => registrationCommitted[lastTargetEpoch] = TRUE

\* NotifyOne is FIFO (C-H8).
InvNotifyOneFIFO ==
    lastAction = "NotifyOne"
    => /\ Len(EligiblePreConditionQueue) > 0
       /\ lastTargetEpoch = Head(EligiblePreConditionQueue)
       /\ conditionNodeState[lastTargetEpoch] = "Woken"

\* Destruction precondition.
InvDestructionPrecondition ==
    lastAction = "Destroy"
    => /\ preConditionQueue = <<>>
       /\ \A f \in Fibers :
              waitPhase[f] \in {"Idle", "Returned"}

\* ReacquireSuspend admission closure: we only suspend when the Mutex is not
\* immediately admissible.
InvReacquireAdmissionClosure ==
    lastAction = "ReacquireSuspend"
    => \/ preMutexOwner # NoOwner
       \/ Len(EligiblePreMutexQueue) > 0

\* ---- Full invariant conjunction ----
Inv == /\ InvType
       /\ InvConditionQueueWellFormed
       /\ InvMutexQueueWellFormed
       /\ InvSingleConditionResolution
       /\ InvSingleConditionPublication
       /\ InvSingleMutexResolution
       /\ InvSingleMutexPublication
       /\ InvConditionWaiterDoesNotOwnMutex
       /\ InvNoDualQueueMembership
       /\ InvReturnedOwnsMutex
       /\ InvReacquireSameFiber
       /\ InvNoOwnerlessMutexDemand
       /\ InvDueInlineRetainsOwnership
       /\ InvConditionResolvedFinality
       /\ InvTerminalAttemptFinality
       /\ InvFIFOGrant
       /\ InvEligiblePreMutexQueue
       /\ InvGrantOwnerCommit
       /\ InvOrdinaryAndReacquireFIFO
       /\ InvNotifyAllSnapshotComplete
       /\ InvNotifyAllExcludesLateWaiter
       /\ InvNoLostNotifyWindow
       /\ InvNotifyOneFIFO
       /\ InvDestructionPrecondition
       /\ InvReacquireAdmissionClosure

=============================================================================
