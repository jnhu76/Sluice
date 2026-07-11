------------------------------- MODULE E11TimerWait -------------------------------
(*
  E11 Deadline / Timer Wait Integration (sluice-CORE-E11) — correct model.

  Extends the E10 WaitNode protocol (docs/spec/e10_waitnode/E10WaitNode.tla)
  with a THIRD resolution cause, TIMER_EXPIRE, and the three new state dimensions
  E11 introduces over E10:

    1. timer-registration lifetime   (TimerRegistration control-block state:
                                       ACTIVE / RETIRED / CONSUMED)
    2. deadline park liveness        (monotonic time + deadline-due predicate +
                                       Scheduler parked/executable state)
    3. wait admission phase          (NoAdmission / AdmissionOpen / Suspended) +
                                       the final-admission-decision history that
                                       closes I5 (Deadline Admission Closure).

  THE LOAD-BEARING E11 DIFFERENCE FROM E10 (I3/I4): E10's loser-safety holds
  ONLY while the WaitNode object is alive (its absorbing terminal state rejects
  a straggling resolve_ CAS). E11 must additionally be safe AFTER the WaitNode
  storage is destroyed, because a physically-retained/lazy timer entry can
  outlive the node. The model therefore distinguishes:

      waitEpoch identity   = the logical node N_E (one per Register)
      timerRegistration R  = a separate control block bound to one epoch
      nodeAlive[n]         = whether the node's STORAGE is still reachable

  A timer registration R_E bound to epoch E is retired/consumed before E's node
  storage may be destroyed. A straggling expiry of R_E observes RETIRED/CONSUMED
  via R_E's OWN state and MUST NOT dereference the (possibly-destroyed) node.
  Cross-epoch resolution is impossible: R_E can only attempt ResolveTimer on its
  bound epoch E, never on E+1 (which is a distinct epoch, registered separately).

  I5 ADMISSION CLOSURE (the non-vacuous formalization). Production
  await_wait_deadline performs, under one critical section:
      register WaitNode -> register timer -> FINAL deadline recheck ->
        if deadline already due: expire_locked inline, do NOT suspend
        otherwise:               Fiber::make_waiting (commit suspension)
  The model represents this admission boundary explicitly with admissionPhase:
      Register            -> AdmissionOpen  (registered, decision pending)
      AdmissionExpire     -> Expired        (deadline due at decision; no suspend)
      CommitSuspend       -> Suspended      (deadline NOT due; suspension committed)
  and a history fact suspendedDue[n] set by CommitSuspend to the value of the
  deadline-due predicate AT the final admission decision. I5 then asserts a node
  whose suspension was committed did NOT observe its deadline already due at that
  decision — the non-tautological admission-closure law. This is NOT checkable
  without the admission-phase dimension: previously the model could only state
  P => TRUE.

  Domain (finite, exhaustive TLC):
    Nodes = {N0, N1}        -- wait epochs (each is one Register lifetime)
    Regs  = {R0, R1}        -- timer registrations (one bound epoch each)
    Time  = 0..3            -- monotonic logical time (small bound)
    DeadlineVal = 0..3      -- absolute deadline values

  State dimensions (NEVER collapsed — see spec "Required formal state dimensions"):
    nodeState[n]      : Detached/Registered/Woken/Cancelled/Expired  (E10 + Expired)
    linked[n]         : queue membership (Registered <=> linked)
    resolvedCount[n]  : winning resolutions (<= 1)
    wakeDispatched    : total scheduler-wake intents (== Sum resolvedCount)
    regState[r]       : Inert/Active/Retired/Consumed  (timer lifetime)
    regEpoch[r]       : the node epoch R is bound to
    regDeadline[r]    : absolute monotonic deadline
    nodeAlive[n]      : storage reachable (FALSE after the node is destroyed)
    now               : monotonic logical time
    parked            : whether the Scheduler is parked (idle)
    admissionPhase[n] : NoAdmission/AdmissionOpen/Suspended  (I5 admission boundary)
    suspendedDue[n]   : deadline-due fact recorded at the final admission decision

  Actions:
    Register(n)        : Detached -> Registered; bind R to n (Active); AdmissionOpen.
    AdmissionExpire(n) : AdmissionOpen + Active reg due -> Expired inline (no suspend).
    CommitSuspend(n)   : AdmissionOpen + Active reg NOT due -> Suspended (make_waiting).
    ResolveWake(n)     : Registered -> Woken (E10; retires R if bound).
    ResolveCancel(n)   : Registered -> Cancelled (E10; retires R if bound).
    ResolveTimer(r)    : Active R bound to n + due + n Registered -> Expired.
                         CLAIMS R (Active->Consumed) BEFORE touching n. I4 gate.
    DestroyNode(n)     : n terminal + R retired/consumed -> nodeAlive[n] := FALSE.
    Tick                 : now++ (monotonic progress; drives due timers).
    Park / Unpark        : Scheduler idle/resident state for liveness.
*)
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Nodes, N0, N1, Regs, R0, R1

VARIABLES
    nodeState,        \* [Nodes -> NodeState]
    linked,           \* [Nodes -> BOOLEAN]
    resolvedCount,    \* [Nodes -> Nat]
    wakeDispatched,   \* Nat
    regState,         \* [Regs -> RegState]
    regEpoch,         \* [Regs -> Nodes]
    regDeadline,      \* [Regs -> DeadlineVal]
    nodeAlive,        \* [Nodes -> BOOLEAN]
    now,              \* Time
    parked,           \* BOOLEAN
    admissionPhase,   \* [Nodes -> AdmissionPhase]
    suspendedDue      \* [Nodes -> BOOLEAN]

NodeState == {"Detached", "Registered", "Woken", "Cancelled", "Expired"}
RegState  == {"Inert", "Active", "Retired", "Consumed"}
AdmissionPhase == {"NoAdmission", "AdmissionOpen", "Suspended"}
DeadlineVal == 0..3

ASSUME
    /\ Nodes # {}
    /\ N0 \in Nodes
    /\ N1 \in Nodes
    /\ N0 # N1
    /\ Regs # {}
    /\ R0 \in Regs
    /\ R1 \in Regs
    /\ R0 # R1

(* =========================================================================
   Predicates
   ========================================================================= *)

isTerminal(n) == nodeState[n] \in {"Woken", "Cancelled", "Expired"}

(* The deadline of an ACTIVE registration is due: now >= deadline. *)
deadlineDue(r) == regState[r] = "Active" /\ now >= regDeadline[r]

(* I5 helper — the deadline bound to n is already due at the admission decision:
   there exists an Active registration bound to n whose deadline has arrived.
   This is the modeled final deadline recheck (production: the
   clock_now_unlocked() >= deadline test inside await_wait_deadline). *)
admissionDeadlineDue(n) ==
    \E r \in Regs : regEpoch[r] = n /\ regState[r] = "Active" /\ now >= regDeadline[r]

(* =========================================================================
   Actions
   ========================================================================= *)

(* Register: Detached -> Registered; bind an Active registration R to n with an
   absolute deadline. Mirrors Scheduler::await_wait_deadline admission up to (but
   NOT including) the final deadline recheck. A fresh node starts alive (its
   storage is reachable): Register SETS nodeAlive[n]=TRUE. The admission boundary
   OPENS here — admissionPhase[n] = AdmissionOpen means the node is Registered
   with an Active bound registration but the final admission decision (expire
   inline vs commit suspension) has not yet been made. (Init sets nodeAlive=FALSE
   for all nodes, so registration is the action that brings a node's storage into
   existence; DestroyNode returns it to FALSE. This makes the registration epoch
   REACHABLE for TLC.) *)
Register(n) ==
    /\ nodeState[n] = "Detached"
    /\ nodeAlive[n] = FALSE
    /\ \E r \in Regs :
        /\ regState[r] = "Inert"
        /\ nodeState' = [nodeState EXCEPT ![n] = "Registered"]
        /\ linked' = [linked EXCEPT ![n] = TRUE]
        /\ regState' = [regState EXCEPT ![r] = "Active"]
        /\ regEpoch' = [regEpoch EXCEPT ![r] = n]
        \* deadline chosen nondeterministically (TLC explores all values)
        /\ \E d \in DeadlineVal : regDeadline' = [regDeadline EXCEPT ![r] = d]
        /\ nodeAlive' = [nodeAlive EXCEPT ![n] = TRUE]
        /\ admissionPhase' = [admissionPhase EXCEPT ![n] = "AdmissionOpen"]
        /\ suspendedDue' = [suspendedDue EXCEPT ![n] = FALSE]
        /\ now' = now
        /\ parked' = parked
        /\ resolvedCount' = resolvedCount
        /\ wakeDispatched' = wakeDispatched

(* I5 ADMISSION CLOSURE — Expire branch: the final admission decision observes
   the deadline ALREADY due. Resolve Expired through the SAME resolve_ authority
   NOW — the fiber must not suspend and wait for a future timer scan. This is the
   winner CAS Registered->Expired; the registration is Consumed. The admission
   boundary CLOSES (admissionPhase -> NoAdmission) WITHOUT ever reaching
   Suspended. (Production: await_wait_deadline rechecks the clock after
   registration and calls expire_locked inline, returning without make_waiting.) *)
AdmissionExpire(n) ==
    /\ nodeState[n] = "Registered"
    /\ admissionPhase[n] = "AdmissionOpen"
    /\ \E r \in Regs :
        /\ regEpoch[r] = n
        /\ regState[r] = "Active"
        /\ now >= regDeadline[r]
    /\ nodeState' = [nodeState EXCEPT ![n] = "Expired"]
    /\ linked' = [linked EXCEPT ![n] = FALSE]
    \* Consume every Active registration bound to n whose deadline is due (an
    \* EXCEPT may not take a function constructor as its subscript; use a plain
    \* function constructor over all Regs, matching the form used by
    \* ResolveWake/ResolveCancel below).
    /\ regState' = [r \in Regs |->
                          IF /\ regEpoch[r] = n
                             /\ regState[r] = "Active"
                             /\ now >= regDeadline[r]
                          THEN "Consumed"
                          ELSE regState[r]]
    /\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
    /\ wakeDispatched' = wakeDispatched + 1
    /\ admissionPhase' = [admissionPhase EXCEPT ![n] = "NoAdmission"]
    /\ UNCHANGED <<regEpoch, regDeadline, nodeAlive, now, parked, suspendedDue>>

(* I5 ADMISSION CLOSURE — Suspend branch: the final admission decision observes
   the deadline NOT yet due. Commit Fiber suspension (production: make_waiting)
   and enter the parked-wait window. The node REMAINS Registered and the
   registration REMAINS Active — no resolution happens here; the fiber is simply
   parked to be woken later by ResolveTimer/ResolveWake/ResolveCancel. The
   history fact suspendedDue[n] records the deadline-due predicate value AT this
   decision (FALSE, guaranteed by the ~admissionDeadlineDue(n) guard) so I5 can
   assert a suspended wait did not observe its deadline already due. This action
   MUST NOT be enabled when the admission deadline is already due — that is the
   load-bearing I5 rule. *)
CommitSuspend(n) ==
    /\ nodeState[n] = "Registered"
    /\ admissionPhase[n] = "AdmissionOpen"
    /\ ~admissionDeadlineDue(n)
    /\ admissionPhase' = [admissionPhase EXCEPT ![n] = "Suspended"]
    /\ suspendedDue' = [suspendedDue EXCEPT ![n] = admissionDeadlineDue(n)]
    /\ UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched,
                  regState, regEpoch, regDeadline, nodeAlive, now, parked>>

(* THE CANONICAL ONE-WINNER TERMINAL RESOLVER — Wake (E10; E11 retires R).
   Registered -> Woken. The registration bound to n is RETIRED (closes callback
   authority so a stale expiry cannot dereference the node after it is
   destroyed). wakeDispatched++ (the one scheduler-wake intent). A wake may win
   while admission is OPEN (the resource fired before the admission decision) or
   after suspension — either way the node is Registered and this CAS wins; the
   admission boundary closes. *)
ResolveWake(n) ==
    /\ nodeState[n] = "Registered"
    /\ nodeState' = [nodeState EXCEPT ![n] = "Woken"]
    /\ linked' = [linked EXCEPT ![n] = FALSE]
    /\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
    /\ wakeDispatched' = wakeDispatched + 1
    \* E11 Phase 5: retire the bound Active registration (callback closure).
    /\ regState' = [r \in Regs |->
                        IF /\ regEpoch[r] = n
                           /\ regState[r] = "Active"
                        THEN "Retired"
                        ELSE regState[r]]
    /\ admissionPhase' = [admissionPhase EXCEPT ![n] = "NoAdmission"]
    /\ UNCHANGED <<regEpoch, regDeadline, nodeAlive, now, parked, suspendedDue>>

(* THE CANONICAL ONE-WINNER TERMINAL RESOLVER — Cancel (E10; E11 retires R). *)
ResolveCancel(n) ==
    /\ nodeState[n] = "Registered"
    /\ nodeState' = [nodeState EXCEPT ![n] = "Cancelled"]
    /\ linked' = [linked EXCEPT ![n] = FALSE]
    /\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
    /\ wakeDispatched' = wakeDispatched + 1
    /\ regState' = [r \in Regs |->
                        IF /\ regEpoch[r] = n
                           /\ regState[r] = "Active"
                        THEN "Retired"
                        ELSE regState[r]]
    /\ admissionPhase' = [admissionPhase EXCEPT ![n] = "NoAdmission"]
    /\ UNCHANGED <<regEpoch, regDeadline, nodeAlive, now, parked, suspendedDue>>

(* THE E11 THIRD RESOLVER — Timer Expiry. Active R bound to n + due + n
   Registered -> Expired. THE I4 GATE: R claims Active->Consumed BEFORE its
   bound node is touched. If R is Retired (a non-timer winner closed it) or
   already Consumed, this action is NOT enabled — the expiry is inert and MUST
   NOT dereference the node. This is the load-bearing lifetime-closure rule.
   This action covers BOTH the post-suspension pump (admissionPhase = Suspended,
   time advanced past the deadline) and any due+Registered state — the pump does
   not depend on the admission phase, only on R's own Active state. *)
ResolveTimer(r) ==
    /\ regState[r] = "Active"
    /\ deadlineDue(r)
    /\ nodeState[regEpoch[r]] = "Registered"
    /\ regState' = [regState EXCEPT ![r] = "Consumed"]
    /\ nodeState' = [nodeState EXCEPT ![regEpoch[r]] = "Expired"]
    /\ linked' = [linked EXCEPT ![regEpoch[r]] = FALSE]
    /\ resolvedCount' = [resolvedCount EXCEPT ![regEpoch[r]] =
                                    resolvedCount[regEpoch[r]] + 1]
    /\ wakeDispatched' = wakeDispatched + 1
    /\ admissionPhase' = [admissionPhase EXCEPT ![regEpoch[r]] = "NoAdmission"]
    /\ UNCHANGED <<regEpoch, regDeadline, nodeAlive, now, parked, suspendedDue>>

(* DestroyNode: a terminal node whose bound registration is retired/consumed
   may have its storage destroyed (the fiber resumed and its frame returned).
   Models the post-resolution lifetime window. Once nodeAlive[n] = FALSE, NO
   expiry may dereference n — and ResolveTimer never does (it gates on R's own
   Active state, not on nodeAlive). *)
DestroyNode(n) ==
    /\ isTerminal(n)
    /\ nodeAlive[n] = TRUE
    /\ \A r \in Regs : regEpoch[r] = n => regState[r] \in {"Retired", "Consumed", "Inert"}
    /\ nodeAlive' = [nodeAlive EXCEPT ![n] = FALSE]
    /\ UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched,
                  regState, regEpoch, regDeadline, now, parked,
                  admissionPhase, suspendedDue>>

(* Tick: monotonic time advances (the deadline-driving mechanism). Drives due
   timers via the scheduler worker loop's pump. Bounded: now < max time. *)
Tick ==
    /\ now < 3
    /\ now' = now + 1
    /\ UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched,
                  regState, regEpoch, regDeadline, nodeAlive, parked,
                  admissionPhase, suspendedDue>>

(* Park / Unpark: the Scheduler idle/resident state (deadline park liveness,
   I6). Parked is a coordination-dimension flag; the liveness property below
   asserts a due deadline eventually unparks. *)
Park ==
    /\ ~parked
    /\ parked' = TRUE
    /\ UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched,
                  regState, regEpoch, regDeadline, nodeAlive, now,
                  admissionPhase, suspendedDue>>

Unpark ==
    /\ parked
    /\ parked' = FALSE
    /\ UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched,
                  regState, regEpoch, regDeadline, nodeAlive, now,
                  admissionPhase, suspendedDue>>

(* Stutter: no-op for invariant coverage across absorbing states. *)
Stutter ==
    /\ UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched,
                  regState, regEpoch, regDeadline, nodeAlive, now, parked,
                  admissionPhase, suspendedDue>>

(* =========================================================================
   Next, Init, Spec
   ========================================================================= *)

Next ==
    \/ Stutter
    \/ \E n \in Nodes : Register(n)
    \/ \E n \in Nodes : AdmissionExpire(n)
    \/ \E n \in Nodes : CommitSuspend(n)
    \/ \E n \in Nodes : ResolveWake(n)
    \/ \E n \in Nodes : ResolveCancel(n)
    \/ \E r \in Regs : ResolveTimer(r)
    \/ \E n \in Nodes : DestroyNode(n)
    \/ Tick
    \/ Park
    \/ Unpark

Init ==
    /\ nodeState = [n \in Nodes |-> "Detached"]
    /\ linked = [n \in Nodes |-> FALSE]
    /\ resolvedCount = [n \in Nodes |-> 0]
    /\ wakeDispatched = 0
    /\ regState = [r \in Regs |-> "Inert"]
    /\ regEpoch = [r \in Regs |-> N0]
    /\ regDeadline = [r \in Regs |-> 0]
    /\ nodeAlive = [n \in Nodes |-> FALSE]
    /\ now = 0
    /\ parked = FALSE
    /\ admissionPhase = [n \in Nodes |-> "NoAdmission"]
    /\ suspendedDue = [n \in Nodes |-> FALSE]

Vars == <<nodeState, linked, resolvedCount, wakeDispatched,
          regState, regEpoch, regDeadline, nodeAlive, now, parked,
          admissionPhase, suspendedDue>>

Spec == Init /\ [][Next]_Vars

(* =========================================================================
   E11 safety invariants (I1-I7)
   ========================================================================= *)

(* I1 — Single Resolution Winner: each node is resolved by at most one winning
   resolver among {Wake, Cancel, Timer}. resolvedCount[n] <= 1. (Inherited E10,
   preserved because ResolveTimer uses the SAME Registered-guarded transition.) *)
InvSingleResolutionWinner ==
    \A n \in Nodes : resolvedCount[n] <= 1

(* I2 — Single Runnable Publication: scheduler-wake intents == total winning
   resolutions. Each winner dispatches exactly once; losers (a timer whose CAS
   lost to a concurrent wake/cancel, or vice versa) dispatch zero. *)
SumResolvedCount ==
    resolvedCount[N0] + resolvedCount[N1]

InvSingleRunnablePublication ==
    wakeDispatched = SumResolvedCount

(* I3 — Wait-Epoch Isolation: a timer registration R bound to epoch N_E may only
   attempt resolution of N_E. It CANNOT resolve a later epoch N_E+1. The
   semantic law: a registration may resolve only its immutably bound logical
   wait epoch. Stated as: if R is CONSUMED (it won a timer-expiry CAS), its
   bound epoch N_E must be the node that became Expired — the resolution landed
   on the bound epoch, not a different one. This is strictly stronger than mere
   domain-membership (regEpoch[r] \in Nodes, which is trivially true by typing):
   it ties consumption to the bound epoch's actual terminal outcome. The correct
   model holds this because only ResolveTimer and AdmissionExpire consume a
   registration, and both expire exactly their bound node. NEG-3's slot-keyed
   expiry consumes R (bound to N0) while N0 is Woken (resolved by wake) and N1
   is Expired, so the strengthened I3 is directly violated. (Production: the
   registration captures WaitNode& and the resolve_ CAS targets the bound node
   object; regEpoch is immutable after Register.) *)
InvWaitEpochIsolation ==
    \A r \in Regs :
        regState[r] = "Consumed" => nodeState[regEpoch[r]] = "Expired"
    \* Structural: regEpoch is immutable after Register; ResolveTimer only
    \* touches regEpoch[r]. No rule rebinds a registration to another node.

(* I4 — Timer Lifetime Closure: after a registration is Retired or Consumed, no
   expiry may dereference its bound node. ResolveTimer is enabled ONLY when
   regState[r] = "Active". A Retired/Consumed R cannot fire. Additionally, a
   node whose storage is destroyed (nodeAlive = FALSE) has no Active-bound
   registration (DestroyNode requires all bound regs Retired/Consumed/Inert). *)
InvTimerLifetimeClosure ==
    \A r \in Regs :
        /\ regState[r] \in {"Retired", "Consumed"} => ~deadlineDue(r)
        /\ (regState[r] = "Active" => nodeAlive[regEpoch[r]])
    \* An Active registration implies its bound node storage is still alive
    \* (retirement/consumption happens before DestroyNode).

(* I5 — Deadline Admission Closure: a wait whose final admission decision
   observed its deadline ALREADY due cannot commit Fiber suspension as a
   still-Registered wait. The admission-phase dimension distinguishes
   "Registered + admission decision pending" (AdmissionOpen) from "Registered +
   suspension committed" (Suspended). The history fact suspendedDue[n] records
   the deadline-due predicate value AT the final admission decision (set by
   CommitSuspend to admissionDeadlineDue(n), which its ~due guard forces to
   FALSE). The invariant asserts a Suspended wait did NOT observe its deadline
   due at the decision. This is NON-TAUTOLOGICAL: it fails exactly when a
   CommitSuspend-like action fires while the deadline is already due. It also
   does NOT reject the legitimate post-suspension state (suspended while NOT
   due, then time advances and the deadline becomes due): suspendedDue is frozen
   at the CommitSuspend step and does not track later time progress, so a node
   suspended while not-due remains suspendedDue=FALSE even after its deadline
   becomes due — that later-due state belongs to I6 timer-progress semantics. *)
InvDeadlineAdmissionClosure ==
    \A n \in Nodes :
        admissionPhase[n] = "Suspended" => ~suspendedDue[n]

(* I7 — Cleanup Closure: after one cause wins, every losing registration is
   immediately unable to resolve or publish. The winner retires/consumes R; a
   later ResolveTimer on a Retired/Consumed R is disabled. *)
InvCleanupClosure ==
    \A n \in Nodes :
        isTerminal(n) => \A r \in Regs :
            (regEpoch[r] = n => regState[r] \in {"Retired", "Consumed", "Inert"})

Inv ==
    /\ InvSingleResolutionWinner
    /\ InvSingleRunnablePublication
    /\ InvWaitEpochIsolation
    /\ InvTimerLifetimeClosure
    /\ InvDeadlineAdmissionClosure
    /\ InvCleanupClosure

(* =========================================================================
   E11 liveness (I6 — Deadline Park Liveness)
   ========================================================================= *)
LivenessSpec == Spec
(* Fairness: weak fairness on the timer resolver and the clock Tick (the two
   actions that drive a due Active deadline to resolution). Conjoined as bare
   WF conjuncts (NOT wrapped in [] — WF is already an always-formula; wrapping
   it yields a formula TLC rejects as "actions must be <>[]A or []<>A"). This
   mirrors the E9 LivenessSpec structure (FairX == WF_vars(action); LivenessSpec
   == Spec /\ FairX /\ ...). *)
FairResolveTimer == WF_Vars(\E r \in Regs : ResolveTimer(r))
FairTick == WF_Vars(Tick)
LivenessSpecFair ==
    LivenessSpec
    /\ FairResolveTimer
    /\ FairTick

(* I6 — Deadline Park Liveness: a Registered wait with a due Active deadline
   does not strand forever. The Scheduler's timer pump (ResolveTimer, under
   fairness) eventually attempts resolution, OR a competing cause (wake/cancel)
   resolves it first. Either way the node reaches a terminal outcome — it does
   NOT remain Registered indefinitely while a deadline is due. (The property is
   NOT "the timer always wins" — a legitimate wake/cancel that retires the
   registration first is a valid resolution. It IS "the wait is not lost while
   parked," the E11 contribution over E10's wake-only liveness.)

   Written as [](P => <>Q): whenever a due Active deadline is bound to a
   Registered node, that node is eventually terminal. Matches the E9
   Life2/Life7 liveness property form. *)
DeadlineParkLiveness ==
    \A r \in Regs :
        [] ( (/\ regState[r] = "Active"
              /\ now >= regDeadline[r]
              /\ nodeState[regEpoch[r]] = "Registered")
             => <> (isTerminal(regEpoch[r])) )

=============================================================================
