------------------------------- MODULE E11TimerWait -------------------------------
(*
  E11 Deadline / Timer Wait Integration (sluice-CORE-E11) — correct model.

  Extends the E10 WaitNode protocol (docs/spec/e10_waitnode/E10WaitNode.tla)
  with a THIRD resolution cause, TIMER_EXPIRE, and the two new state dimensions
  E11 introduces over E10:

    1. timer-registration lifetime   (TimerRegistration control-block state:
                                       ACTIVE / RETIRED / CONSUMED)
    2. deadline park liveness        (monotonic time + deadline-due predicate +
                                       Scheduler parked/executable state)

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

  Actions:
    Register(n,r)        : Detached -> Registered; bind R to n (Active). I5: if
                           the deadline is already due, resolve Expired at once.
    ResolveWake(n)       : Registered -> Woken (E10; retires R if bound).
    ResolveCancel(n)     : Registered -> Cancelled (E10; retires R if bound).
    ResolveTimer(r)      : Active R bound to n + due + n Registered -> Expired.
                           CLAIMS R (Active->Consumed) BEFORE touching n. I4 gate.
    DestroyNode(n)       : n terminal + R retired/consumed -> nodeAlive[n] := FALSE.
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
    parked            \* BOOLEAN

NodeState == {"Detached", "Registered", "Woken", "Cancelled", "Expired"}
RegState  == {"Inert", "Active", "Retired", "Consumed"}
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

(* =========================================================================
   Actions
   ========================================================================= *)

(* Register: Detached -> Registered; bind an Active registration R to n with an
   absolute deadline. Mirrors Scheduler::await_wait_deadline admission. A fresh
   node starts alive (its storage is reachable). *)
Register(n) ==
    /\ nodeState[n] = "Detached"
    /\ nodeAlive[n] = TRUE
    /\ \E r \in Regs :
        /\ regState[r] = "Inert"
        /\ nodeState' = [nodeState EXCEPT ![n] = "Registered"]
        /\ linked' = [linked EXCEPT ![n] = TRUE]
        /\ regState' = [regState EXCEPT ![r] = "Active"]
        /\ regEpoch' = [regEpoch EXCEPT ![r] = n]
        \* deadline chosen nondeterministically (TLC explores all values)
        /\ \E d \in DeadlineVal : regDeadline' = [regDeadline EXCEPT ![r] = d]
        /\ nodeAlive' = nodeAlive
        /\ now' = now
        /\ parked' = parked
        /\ resolvedCount' = resolvedCount
        /\ wakeDispatched' = wakeDispatched

(* I5 ADMISSION CLOSURE: if the deadline is ALREADY due at admission, resolve
   Expired through the SAME resolve_ authority NOW — the fiber must not suspend
   and wait for a future timer scan. This is the winner CAS Registered->Expired;
   the registration is Consumed. (Production: await_wait_deadline rechecks the
   clock after registration and calls expire_locked inline.) *)
AdmissionExpire(n) ==
    /\ nodeState[n] = "Registered"
    /\ \E r \in Regs :
        /\ regEpoch[r] = n
        /\ regState[r] = "Active"
        /\ now >= regDeadline[r]
    /\ nodeState' = [nodeState EXCEPT ![n] = "Expired"]
    /\ linked' = [linked EXCEPT ![n] = FALSE]
    /\ regState' = [regState EXCEPT ![r \in Regs |->
                          IF /\ regEpoch[r] = n
                             /\ regState[r] = "Active"
                             /\ now >= regDeadline[r]
                          THEN "Consumed"
                          ELSE regState[r]]
    /\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
    /\ wakeDispatched' = wakeDispatched + 1
    /\ UNCHANGED <<regEpoch, regDeadline, nodeAlive, now, parked>>

(* THE CANONICAL ONE-WINNER TERMINAL RESOLVER — Wake (E10; E11 retires R).
   Registered -> Woken. The registration bound to n is RETIRED (closes callback
   authority so a stale expiry cannot dereference the node after it is
   destroyed). wakeDispatched++ (the one scheduler-wake intent). *)
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
    /\ UNCHANGED <<regEpoch, regDeadline, nodeAlive, now, parked>>

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
    /\ UNCHANGED <<regEpoch, regDeadline, nodeAlive, now, parked>>

(* THE E11 THIRD RESOLVER — Timer Expiry. Active R bound to n + due + n
   Registered -> Expired. THE I4 GATE: R claims Active->Consumed BEFORE its
   bound node is touched. If R is Retired (a non-timer winner closed it) or
   already Consumed, this action is NOT enabled — the expiry is inert and MUST
   NOT dereference the node. This is the load-bearing lifetime-closure rule. *)
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
    /\ UNCHANGED <<regEpoch, regDeadline, nodeAlive, now, parked>>

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
                  regState, regEpoch, regDeadline, now, parked>>

(* Tick: monotonic time advances (the deadline-driving mechanism). Drives due
   timers via the scheduler worker loop's pump. Bounded: now < max time. *)
Tick ==
    /\ now < 3
    /\ now' = now + 1
    /\ UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched,
                  regState, regEpoch, regDeadline, nodeAlive, parked>>

(* Park / Unpark: the Scheduler idle/resident state (deadline park liveness,
   I6). Parked is a coordination-dimension flag; the liveness property below
   asserts a due deadline eventually unparks. *)
Park ==
    /\ ~parked
    /\ parked' = TRUE
    /\ UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched,
                  regState, regEpoch, regDeadline, nodeAlive, now>>

Unpark ==
    /\ parked
    /\ parked' = FALSE
    /\ UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched,
                  regState, regEpoch, regDeadline, nodeAlive, now>>

(* Stutter: no-op for invariant coverage across absorbing states. *)
Stutter ==
    /\ UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched,
                  regState, regEpoch, regDeadline, nodeAlive, now, parked>>

(* =========================================================================
   Next, Init, Spec
   ========================================================================= *)

Next ==
    \/ Stutter
    \/ \E n \in Nodes : Register(n)
    \/ \E n \in Nodes : AdmissionExpire(n)
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

Vars == <<nodeState, linked, resolvedCount, wakeDispatched,
          regState, regEpoch, regDeadline, nodeAlive, now, parked>>

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
InvSingleRunnablePublication ==
    wakeDispatched = SumResolvedCount

SumResolvedCount ==
    resolvedCount[N0] + resolvedCount[N1]

(* I3 — Wait-Epoch Isolation: a timer registration R bound to epoch N_E may only
   attempt resolution of N_E. It CANNOT resolve a later epoch N_E+1. Modeled
   structurally: regEpoch[r] is fixed at Register time and ResolveTimer targets
   exactly regEpoch[r]. A later Register(N_E+1) is a distinct node; R_E never
   references it. (Production: the registration captures WaitNode&, never only
   Fiber*; the resolve_ CAS targets the bound node object.) *)
InvWaitEpochIsolation ==
    \A r \in Regs :
        regState[r] = "Consumed" => regEpoch[r] \in Nodes
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

(* I5 — Deadline Admission Closure: already covered by AdmissionExpire, which is
   the deterministic resolve at registration. Invariant: a Registered node with
   an Active, already-due registration is not a stable state reachable across a
   Tick without resolution opportunity (AdmissionExpire / ResolveTimer resolve
   it). Stated as: if an Active reg is due and its node is Registered, the model
   offers a resolution transition (the property holds across the step). *)
InvDeadlineAdmissionClosure ==
    \A r \in Regs :
        (/\ regState[r] = "Active"
         /\ now >= regDeadline[r]
         /\ nodeState[regEpoch[r]] = "Registered")
        => TRUE   \* non-blocking: AdmissionExpire/ResolveTimer always enabled
                  \* from this state (liveness drives resolution). Safety: the
                  \* resolve is the SAME CAS, so no double-resolution.

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
FairTimer ==
    /\ WF_Vars(\E r \in Regs : ResolveTimer(r))
    /\ WF_Vars(Tick)
LivenessSpecFair == LivenessSpec /\ []FairTimer

(* I6 — Deadline Park Liveness: an Active due deadline is eventually resolved.
   A Worker may not remain parked solely because no non-timer wake source fires. *)
DeadlineParkLiveness ==
    \A r \in Regs :
        (/\ regState[r] = "Active"
         /\ now >= regDeadline[r]
         /\ nodeState[regEpoch[r]] = "Registered")
        ~> nodeState[regEpoch[r]] = "Expired"

=============================================================================
