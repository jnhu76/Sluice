------------------------------- MODULE E12Event -------------------------------
(*
  sluice::async::Event -- persistent manual-reset Event TLA+ safety + liveness
  model (E12-A-EVENT-CORRECTIVE-1).

  The load-bearing E12-A question: can persistent readiness be composed with
  the E10 WaitQueue + E11 deadline semantics without duplicating the old
  waiting_ready_ subsystem, while preserving:
    E1  Single Resolution Winner      (inherited from E10, one resolve_ CAS)
    E2  Single Runnable Publication   (make_runnable is the publication guard)
    E3  Event Admission Closure       (a wait observing SET at admission cannot
                                       commit suspension as unresolved)
    E4  Persistent SET Liveness       (a Registered+Suspended node eventually
                                       becomes terminal under fairness)
    E5  Reset Non-Resolution          (reset alone does not change a Registered
                                       node to terminal)
    E6  Set-Epoch Isolation           (an old SET epoch's drain cannot wake a
                                       waiter admitted after a later RESET)

  Answer: YES.

  E12-A-EVENT-CORRECTIVE-1 (Corrective H): the formal model now represents the
  production fact that a set epoch is a MULTI-STEP SERIALIZED critical section:

      StartSet : acquire abstract global Event serialization, store SET, open a
                 set epoch E, record setEpochGeneration[E] = resetGeneration
      DrainOne : while E owns serialization, resolve ONE eligible Registered+
                 Suspended waiter through RESOURCE_WAKE, record wakeSetEpoch[n]=E
      FinishSet: release serialization, close E (activeSetEpoch <- None)

  Reset and Admission CANNOT complete while SetDrain owns serialization. This is
  the load-bearing mechanism the OLD single-atomic SetEvent could not express.

  History/refinement variables (H1, marked REFINEMENT below) are mechanically
  connected to real modeled actions (resetGeneration advances on ResetEvent;
  setEpochGeneration[E] is recorded on StartSet; registrationGeneration[n] on
  Register; wakeSetEpoch[n] on DrainOne/AdmissionWake). They are NOT production
  fields; global_mtx_ is the production serialization. resetGeneration is an
  abstract monotonic history generation.

  Refinement map (TLA+ concept -> production seam):
    eventSet          <-> Event::set_ (std::atomic<bool>)
    resetGeneration   <-> abstract history generation (advances on event_reset)
    Register          <-> Scheduler::await_event_wait admission (register + check)
    AdmissionWake     <-> WaitQueue::wake_node_locked (resolve_(Woken) at admission)
    CommitSuspend     <-> Fiber::make_waiting + context_switch
    StartSet          <-> Scheduler::event_set_broadcast store SET + open epoch
    DrainOne          <-> wake_wait_one_locked (one drain step)
    FinishSet         <-> event_set_broadcast drain loop end + release global_mtx_
    ResetEvent        <-> Scheduler::event_reset (store false)
    ResolveCancel     <-> Scheduler::cancel_wait (resolve_(Cancelled) + unlink)
    nodeState         <-> WaitNode::state_ (atomic CAS)

  Domain: Nodes = {N0, N1}, exhaustive. Two nodes suffice to model the
  stale-set/post-reset topology (Wold from the old epoch + Wnew admitted after
  reset).
*)
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Nodes, N0, N1, MaxGen

VARIABLES
    \* @type: [Node -> {"Detached","Registered","Woken","Cancelled","Expired"}]
    nodeState,
    \* @type: [Node -> BOOLEAN]  -- the node is linked in the Event's WaitQueue
    linked,
    \* @type: [Node -> 0..1]     -- terminal resolution count (E1: <= 1)
    resolvedCount,
    \* @type: Int                -- total runnable publications (E2)
    wakeDispatched,
    \* @type: {"UNSET","SET"}    -- the persistent Event readiness state
    eventSet,
    \* @type: [Node -> {"NoAdmission","AdmissionOpen","Suspended"}]
    admissionPhase,
    \* @type: [Node -> BOOLEAN]  -- was SET observed at admission? (E3)
    admissionSawSet,
    \* @type: [Node -> 0..1]     -- woken by a SetEvent DRAIN (not admission)? (E6)
    wokenBySetDrain,

    \* ---- H1: global Event protocol owner / phase ----
    \* @type: {"Idle","SetDrain"}  -- abstract serialization owner (global_mtx_)
    protoPhase,

    \* ---- H1: refinement/history variables (NOT production fields) ----
    \* @type: Nat              -- abstract monotonic reset generation
    resetGeneration,
    \* @type: [Node -> Nat]    -- reset generation at which node n registered
    registrationGeneration,
    \* @type: Int              -- reset generation observed by the active set
    \*                            epoch, or -1 if no set drain is active. A single
    \*                            scalar suffices: at most one set epoch is active
    \*                            at a time (serialized by protoPhase), so there is
    \*                            no epoch-id reuse to corrupt the history tag.
    activeSetGen,
    \* @type: [Node -> Int]    -- reset generation of the set epoch that woke n
    \*                            via DRAIN, or -1 if n was not woken by a drain.
    \*                            (AdmissionWake does NOT set this — it is the
    \*                            admission-time late-waiter path, not a drain.)
    wakeEpochGen,

    \* ---- J: resolution-cause history (mechanically written by every terminal
    \*      resolution action) ----
    \* @type: [Node -> {"None","AdmissionSet","SetBroadcast","Cancel"}]
    resolutionCause

NodeState == {"Detached", "Registered", "Woken", "Cancelled", "Expired"}
AdmissionPhase == {"NoAdmission", "AdmissionOpen", "Suspended"}
ProtoPhase == {"Idle", "SetDrain"}
ResolutionCause == {"None", "AdmissionSet", "SetBroadcast", "Cancel"}

NoGen == 1000000

ASSUME /\ Nodes # {}
       /\ N0 \in Nodes
       /\ N1 \in Nodes
       /\ N0 # N1
       /\ MaxGen \in 1..10

Vars == <<nodeState, linked, resolvedCount, wakeDispatched, eventSet,
          admissionPhase, admissionSawSet, wokenBySetDrain,
          protoPhase, resetGeneration, registrationGeneration,
          activeSetGen, wakeEpochGen,
          resolutionCause>>

isTerminal(n) == nodeState[n] \in {"Woken", "Cancelled", "Expired"}

SumResolvedCount ==
    resolvedCount[N0] + resolvedCount[N1]

\* The set of nodes drainable by a SetEvent DRAIN (Registered + Suspended).
\* AdmissionWake (admission-time SET) is NOT a drain — it is attributed to the
\* AdmissionSet cause, not SetBroadcast.
Drainable ==
    {n \in Nodes : nodeState[n] = "Registered" /\ admissionPhase[n] = "Suspended"}

-------------------------------------------------------------------------------
Init ==
    /\ nodeState = [n \in Nodes |-> "Detached"]
    /\ linked = [n \in Nodes |-> FALSE]
    /\ resolvedCount = [n \in Nodes |-> 0]
    /\ wakeDispatched = 0
    /\ eventSet = "UNSET"
    /\ admissionPhase = [n \in Nodes |-> "NoAdmission"]
    /\ admissionSawSet = [n \in Nodes |-> FALSE]
    /\ wokenBySetDrain = [n \in Nodes |-> 0]
    /\ protoPhase = "Idle"
    /\ resetGeneration = 0
    /\ registrationGeneration = [n \in Nodes |-> 0]
    /\ activeSetGen = NoGen
    /\ wakeEpochGen = [n \in Nodes |-> NoGen]
    /\ resolutionCause = [n \in Nodes |-> "None"]

-------------------------------------------------------------------------------
\* Register: a waiter enters Event wait admission. Always registers first
\* (Detached -> Registered, linked), records its registration generation, then
\* checks SET in AdmissionWake/CommitSuspend. CANNOT run while a set drain owns
\* serialization (protoPhase must be Idle).
Register(n) ==
    /\ nodeState[n] = "Detached"
    /\ admissionPhase[n] = "NoAdmission"
    /\ protoPhase = "Idle"
    /\ nodeState' = [nodeState EXCEPT ![n] = "Registered"]
    /\ linked' = [linked EXCEPT ![n] = TRUE]
    /\ admissionPhase' = [admissionPhase EXCEPT ![n] = "AdmissionOpen"]
    /\ registrationGeneration' = [registrationGeneration EXCEPT ![n] = resetGeneration]
    /\ UNCHANGED <<resolvedCount, wakeDispatched, eventSet,
                   admissionSawSet, wokenBySetDrain,
                   protoPhase, resetGeneration, activeSetGen,
                   wakeEpochGen, resolutionCause>>

\* AdmissionWake: the admission closure (E3). If SET is observed after
\* registration, resolve this node Woken inline (no suspend). This is the
\* AdmissionSet cause (NOT a SetBroadcast drain). Mirrors wake_node_locked.
AdmissionWake(n) ==
    /\ nodeState[n] = "Registered"
    /\ admissionPhase[n] = "AdmissionOpen"
    /\ eventSet = "SET"
    /\ nodeState' = [nodeState EXCEPT ![n] = "Woken"]
    /\ linked' = [linked EXCEPT ![n] = FALSE]
    /\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
    /\ wakeDispatched' = wakeDispatched + 1
    /\ admissionPhase' = [admissionPhase EXCEPT ![n] = "NoAdmission"]
    /\ admissionSawSet' = [admissionSawSet EXCEPT ![n] = TRUE]
    /\ resolutionCause' = [resolutionCause EXCEPT ![n] = "AdmissionSet"]
    /\ UNCHANGED <<eventSet, wokenBySetDrain,
                  protoPhase, resetGeneration, registrationGeneration,
                  activeSetGen, wakeEpochGen>>

\* CommitSuspend: the admission closure (suspend branch). If SET is NOT
\* observed, commit suspension. E3: admissionSawSet is FALSE here.
CommitSuspend(n) ==
    /\ nodeState[n] = "Registered"
    /\ admissionPhase[n] = "AdmissionOpen"
    /\ eventSet = "UNSET"
    /\ admissionPhase' = [admissionPhase EXCEPT ![n] = "Suspended"]
    /\ admissionSawSet' = [admissionSawSet EXCEPT ![n] = FALSE]
    /\ UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched, eventSet,
                  wokenBySetDrain,
                  protoPhase, resetGeneration, registrationGeneration,
                  activeSetGen, wakeEpochGen,
                  resolutionCause>>

-------------------------------------------------------------------------------
\* H2: the MULTI-STEP SERIALIZED SetEvent protocol.

\* StartSet: acquire abstract global Event serialization, store SET, open a set
\* epoch, record the reset generation observed when this epoch opened. Can only
\* start when the protocol is Idle and the Event is UNSET.
StartSet ==
    /\ protoPhase = "Idle"
    /\ eventSet = "UNSET"
    /\ protoPhase' = "SetDrain"
    /\ eventSet' = "SET"
    /\ activeSetGen' = resetGeneration
    /\ UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched,
                  admissionPhase, admissionSawSet, wokenBySetDrain,
                  resetGeneration, registrationGeneration, wakeEpochGen,
                  resolutionCause>>

\* DrainOne: while the active set epoch owns serialization, resolve ONE eligible
\* Registered+Suspended waiter through RESOURCE_WAKE (the SetBroadcast cause).
\* Records wakeEpochGen[n] = activeSetGen (the load-bearing epoch tag).
DrainOne(n) ==
    /\ protoPhase = "SetDrain"
    /\ activeSetGen # NoGen
    /\ nodeState[n] = "Registered"
    /\ admissionPhase[n] = "Suspended"
    /\ nodeState' = [nodeState EXCEPT ![n] = "Woken"]
    /\ linked' = [linked EXCEPT ![n] = FALSE]
    /\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
    /\ wakeDispatched' = wakeDispatched + 1
    /\ wokenBySetDrain' = [wokenBySetDrain EXCEPT ![n] = wokenBySetDrain[n] + 1]
    /\ wakeEpochGen' = [wakeEpochGen EXCEPT ![n] = activeSetGen]
    /\ resolutionCause' = [resolutionCause EXCEPT ![n] = "SetBroadcast"]
    /\ UNCHANGED <<eventSet, admissionPhase, admissionSawSet,
                  protoPhase, resetGeneration, registrationGeneration,
                  activeSetGen>>

\* FinishSet: release serialization, close the active set epoch. The drain is
\* complete; protoPhase returns to Idle so Reset/Admission can proceed.
FinishSet ==
    /\ protoPhase = "SetDrain"
    /\ protoPhase' = "Idle"
    /\ activeSetGen' = NoGen
    /\ UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched, eventSet,
                  admissionPhase, admissionSawSet, wokenBySetDrain,
                  resetGeneration, registrationGeneration,
                  wakeEpochGen, resolutionCause>>

-------------------------------------------------------------------------------
\* ResetEvent: pure state flip. SET -> UNSET. Advances resetGeneration (the
\* abstract history generation). CANNOT run while a set drain owns serialization.
\* E5: does NOT resolve, cancel, expire, unlink, or publish any node.
\* State-space bound: resetGeneration is capped at MaxGen (a model bound, NOT a
\* production limit — production's resetGeneration is unbounded). The cap keeps
\* the finite-state model checkable while preserving the generation ORDERING
\* needed to express set/reset epoch isolation.
ResetEvent ==
    /\ protoPhase = "Idle"
    /\ eventSet = "SET"
    /\ resetGeneration < MaxGen
    /\ eventSet' = "UNSET"
    /\ resetGeneration' = resetGeneration + 1
    /\ UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched,
                  admissionPhase, admissionSawSet, wokenBySetDrain,
                  protoPhase, registrationGeneration, activeSetGen,
                  wakeEpochGen, resolutionCause>>

\* ResolveCancel: a specific node is cancelled (resolve_(Cancelled) + unlink).
\* CANNOT run while a set drain owns serialization. Mirrors Scheduler::cancel_wait.
ResolveCancel(n) ==
    /\ nodeState[n] = "Registered"
    /\ protoPhase = "Idle"
    /\ nodeState' = [nodeState EXCEPT ![n] = "Cancelled"]
    /\ linked' = [linked EXCEPT ![n] = FALSE]
    /\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
    /\ wakeDispatched' = wakeDispatched + 1
    /\ resolutionCause' = [resolutionCause EXCEPT ![n] = "Cancel"]
    /\ UNCHANGED <<eventSet, admissionPhase, admissionSawSet, wokenBySetDrain,
                  protoPhase, resetGeneration, registrationGeneration,
                  activeSetGen, wakeEpochGen>>

\* Stutter: no-op (allows the model to represent quiescence).
Stutter ==
    UNCHANGED Vars

-------------------------------------------------------------------------------
Next ==
    \/ Stutter
    \/ \E n \in Nodes : Register(n)
    \/ \E n \in Nodes : AdmissionWake(n)
    \/ \E n \in Nodes : CommitSuspend(n)
    \/ StartSet
    \/ \E n \in Nodes : DrainOne(n)
    \/ FinishSet
    \/ ResetEvent
    \/ \E n \in Nodes : ResolveCancel(n)

Spec == Init /\ [][Next]_Vars

-------------------------------------------------------------------------------
\* Invariants (E1, E2, E3, E5, E6)

\* E1: one wait epoch -> at most one terminal resolution (inherited E10).
InvSingleResolutionWinner ==
    \A n \in Nodes : resolvedCount[n] <= 1

\* E2: one winning epoch -> at most one runnable publication.
InvSingleRunnablePublication ==
    wakeDispatched = SumResolvedCount

\* E3: Event Admission Closure. A wait whose admission observed SET resolved
\* Woken inline (did not commit suspension): admissionSawSet => Woken.
\* (The multi-step set protocol means a Registered+Suspended node may coexist
\*  transiently with eventSet=SET DURING a drain — the drain is resolving it.
\*  The admission-closure property that a lost-set bug violates is therefore
\*  expressed as (a): a wait that SAW SET at admission cannot be left
\*  Registered/Suspended. The lost-set counterexample is pinned by NEG-EVENT-1's
\*  own invariant, which checks the buggy protocol admits a Registered+Suspended
\*  node whose admission observed UNSET while a draining set epoch cannot reach
\*  it. The correct model's CommitSuspend guard (eventSet="UNSET") plus the
\*  multi-step drain's serialization is what makes OLD_SET_STRAND impossible.)
InvEventAdmissionClosure ==
    \A n \in Nodes :
        admissionSawSet[n] = TRUE => nodeState[n] = "Woken"

\* E5 (Corrective J): Reset Non-Resolution. ResetEvent must NEVER create a
\* terminal resolution. The resolutionCause history is written by EVERY modeled
\* terminal-resolution action (AdmissionWake, DrainOne, ResolveCancel). Reset is
\* NOT among the causes. A terminal node's cause must never be "Reset" (which is
\* not even a value in ResolutionCause here — ResetEvent cannot write it). This
\* is the strengthened property with real checking power: the cause variable is
\* mechanically coupled to terminal resolution, not a constant forced to pass.
InvResetNonResolution ==
    \A n \in Nodes :
        isTerminal(n) => resolutionCause[n] # "None" /\ resolutionCause[n] # "Reset"

\* E6 (Corrective H3): Set-Epoch Isolation — the REAL semantic law. A set-epoch
\* DRAIN may RESOURCE_WAKE only a wait epoch that was ALREADY registered when
\* that set epoch's drain linearized. A waiter woken by a drain must have been
\* registered at a reset generation <= the wake epoch's generation.
\*
\* Meaning: a waiter admitted AFTER a later reset (registrationGeneration >
\* activeSetGen at drain time) CANNOT be woken by an older set epoch's drain
\* (the stale-set/post-reset defect). This replaces the vacuous
\* `wokenBySetEpoch[n] <= 1` (a terminal Woken node is absorbing and could never
\* exceed one regardless of staleness).
\*
\* The "<=" (not "==") form is correct: a waiter that survives a set/reset
\* cycle (registered at gen 0, reset to gen 1, then woken by the gen-1 set
\* epoch) has registrationGeneration=0 <= wakeEpochGen=1 — a LEGAL wake. The
\* stale defect is registrationGeneration > wakeEpochGen (the waiter registered
\* strictly after the waking epoch's generation), which this forbids.
\*
\* The property PERMITS the admission-time late-waiter path: AdmissionWake
\* (cause AdmissionSet) does NOT set wakeEpochGen (it stays NoGen), so it is not
\* classified as a stale drain resolution. Only SetBroadcast-drained nodes carry
\* the wakeEpochGen tag and are bound by this law.
InvSetEpochIsolation ==
    \A n \in Nodes :
        wakeEpochGen[n] # NoGen
        => registrationGeneration[n] <= wakeEpochGen[n]

Inv == /\ InvSingleResolutionWinner
       /\ InvSingleRunnablePublication
       /\ InvEventAdmissionClosure
       /\ InvResetNonResolution
       /\ InvSetEpochIsolation

-------------------------------------------------------------------------------
\* E4: Persistent SET Liveness (liveness property, separate .cfg).
\* If a node is Registered+Suspended, under fairness it eventually becomes
\* terminal (Woken by a set-epoch drain, or Cancelled). The multi-step set
\* protocol (StartSet -> DrainOne -> FinishSet) must be able to make progress:
\* fairness is placed on each step so a stuck drain cannot strand a waiter.
\* EventSetDrainLivenessNonVacuous: from Registered+Suspended, eventually
\* terminal.

FairStartSet == WF_Vars(StartSet)
FairDrainOne == WF_Vars(\E n \in Nodes : DrainOne(n))
FairFinishSet == WF_Vars(FinishSet)
FairCancel == WF_Vars(\E n \in Nodes : ResolveCancel(n))

LivenessSpecFair == Spec /\ FairStartSet /\ FairDrainOne /\ FairFinishSet /\ FairCancel

EventSetDrainLivenessNonVacuous ==
    \A n \in Nodes :
        [] ( (/\ nodeState[n] = "Registered"
              /\ admissionPhase[n] = "Suspended")
             => <> (isTerminal(n)) )

=============================================================================
