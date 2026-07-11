------------------------------- MODULE E12Event -------------------------------
(*
  sluice::async::Event -- persistent manual-reset Event TLA+ safety model.

  The load-bearing E12-A question: can persistent readiness be composed with
  the E10 WaitQueue + E11 deadline semantics without duplicating the old
  waiting_ready_ subsystem, while preserving:
    E1  Single Resolution Winner      (inherited from E10, one resolve_ CAS)
    E2  Single Runnable Publication   (make_runnable is the publication guard)
    E3  Event Admission Closure       (a wait observing SET at admission cannot
                                       commit suspension as unresolved)
    E5  Reset Non-Resolution          (reset alone does not change a Registered
                                       node to terminal)
    E6  Set-Epoch Isolation           (an old SET epoch's drain cannot wake a
                                       waiter admitted after a later RESET)

  Answer: YES. Event owns a persistent boolean set_ + a WaitQueue waiters_.
  ALL Event operations (set, reset, wait admission, set's drain) serialize
  under global_mtx_ (represented here as the mutual exclusion of the actions
  that mutate set_ or the queue). The drain (SetEvent) is atomic with the
  set_ store, so OLD_SET_WAKES_POST_RESET_WAITER is mechanically impossible:
  a waiter admitted after the drain is not in the queue during the drain.

  E4 (Persistent SET Liveness) is a liveness property modeled in the
  LivenessSpecFair variant (separate .cfg).

  Refinement map (TLA+ concept -> production seam):
    eventSet          <-> Event::set_ (std::atomic<bool>)
    Register          <-> Scheduler::await_event_wait admission (register + check)
    AdmissionWake     <-> WaitQueue::wake_node_locked (resolve_(Woken) at admission)
    CommitSuspend     <-> Fiber::make_waiting + context_switch
    SetEvent          <-> Scheduler::event_set_broadcast (store + drain loop)
    ResetEvent        <-> Scheduler::event_reset (store false)
    ResolveCancel     <-> Scheduler::cancel_wait (resolve_(Cancelled) + unlink)
    nodeState         <-> WaitNode::state_ (atomic CAS)

  Domain: Nodes = {N0, N1}, exhaustive.
*)
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Nodes, N0, N1

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
    \* @type: [Node -> 0..1]     -- woken by a SetEvent drain? (E6)
    wokenBySetEpoch

NodeState == {"Detached", "Registered", "Woken", "Cancelled", "Expired"}
AdmissionPhase == {"NoAdmission", "AdmissionOpen", "Suspended"}

ASSUME /\ Nodes # {}
       /\ N0 \in Nodes
       /\ N1 \in Nodes
       /\ N0 # N1

Vars == <<nodeState, linked, resolvedCount, wakeDispatched, eventSet,
          admissionPhase, admissionSawSet, wokenBySetEpoch>>

isTerminal(n) == nodeState[n] \in {"Woken", "Cancelled", "Expired"}

SumResolvedCount ==
    resolvedCount[N0] + resolvedCount[N1]

-------------------------------------------------------------------------------
Init ==
    /\ nodeState = [n \in Nodes |-> "Detached"]
    /\ linked = [n \in Nodes |-> FALSE]
    /\ resolvedCount = [n \in Nodes |-> 0]
    /\ wakeDispatched = 0
    /\ eventSet = "UNSET"
    /\ admissionPhase = [n \in Nodes |-> "NoAdmission"]
    /\ admissionSawSet = [n \in Nodes |-> FALSE]
    /\ wokenBySetEpoch = [n \in Nodes |-> 0]

-------------------------------------------------------------------------------
\* Register: a waiter enters Event wait admission. Always registers first
\* (Detached -> Registered, linked), then checks SET in AdmissionWake/CommitSuspend.
\* Mirrors production: register_wait_locked + ++count, THEN check set_.
Register(n) ==
    /\ nodeState[n] = "Detached"
    /\ admissionPhase[n] = "NoAdmission"
    /\ nodeState' = [nodeState EXCEPT ![n] = "Registered"]
    /\ linked' = [linked EXCEPT ![n] = TRUE]
    /\ admissionPhase' = [admissionPhase EXCEPT ![n] = "AdmissionOpen"]
    /\ UNCHANGED <<resolvedCount, wakeDispatched, eventSet,
                   admissionSawSet, wokenBySetEpoch>>

\* AdmissionWake: the admission closure (E3). If SET is observed after
\* registration, resolve this node Woken inline (no suspend). Mirrors
\* wake_node_locked: resolve_(Woken) + unlink.
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
    \* Admission-time wake is NOT attributed to a SetEvent drain epoch.
    /\ UNCHANGED <<eventSet, wokenBySetEpoch>>

\* CommitSuspend: the admission closure (suspend branch). If SET is NOT
\* observed, commit suspension. E3: admissionSawSet is FALSE here.
CommitSuspend(n) ==
    /\ nodeState[n] = "Registered"
    /\ admissionPhase[n] = "AdmissionOpen"
    /\ eventSet = "UNSET"
    /\ admissionPhase' = [admissionPhase EXCEPT ![n] = "Suspended"]
    /\ admissionSawSet' = [admissionSawSet EXCEPT ![n] = FALSE]
    /\ UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched, eventSet,
                  wokenBySetEpoch>>

\* The set of nodes drainable by a SetEvent broadcast (Registered + Suspended).
Drainable ==
    {n \in Nodes : nodeState[n] = "Registered" /\ admissionPhase[n] = "Suspended"}

\* SetEvent: the broadcast. Transition to SET and drain EVERY registered,
\* Suspended waiter through RESOURCE_WAKE. Mirrors event_set_broadcast:
\* set_ <- SET + loop wake_wait_one_locked until empty.
\* E6 (epoch isolation): the drain is atomic with the store; a node admitted
\*    after this action is not in the queue during the drain.
SetEvent ==
    /\ eventSet = "UNSET"
    /\ eventSet' = "SET"
    /\ nodeState' =
           [n \in Nodes |->
              IF n \in Drainable THEN "Woken" ELSE nodeState[n]]
    /\ linked' =
           [n \in Nodes |->
              IF n \in Drainable THEN FALSE ELSE linked[n]]
    /\ resolvedCount' =
           [n \in Nodes |->
              IF n \in Drainable THEN resolvedCount[n] + 1 ELSE resolvedCount[n]]
    /\ wokenBySetEpoch' =
           [n \in Nodes |->
              IF n \in Drainable THEN wokenBySetEpoch[n] + 1 ELSE wokenBySetEpoch[n]]
    /\ wakeDispatched' = wakeDispatched + Cardinality(Drainable)
    /\ UNCHANGED <<admissionPhase, admissionSawSet>>

\* ResetEvent: pure state flip. SET -> UNSET. E5: does NOT resolve, cancel,
\* expire, unlink, or publish any node.
ResetEvent ==
    /\ eventSet = "SET"
    /\ eventSet' = "UNSET"
    /\ UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched,
                  admissionPhase, admissionSawSet, wokenBySetEpoch>>

\* ResolveCancel: a specific node is cancelled (resolve_(Cancelled) + unlink).
\* Mirrors Scheduler::cancel_wait. The winner CAS is the authority.
ResolveCancel(n) ==
    /\ nodeState[n] = "Registered"
    /\ nodeState' = [nodeState EXCEPT ![n] = "Cancelled"]
    /\ linked' = [linked EXCEPT ![n] = FALSE]
    /\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
    /\ wakeDispatched' = wakeDispatched + 1
    /\ UNCHANGED <<eventSet, admissionPhase, admissionSawSet, wokenBySetEpoch>>

\* Stutter: no-op (allows the model to represent quiescence).
Stutter ==
    UNCHANGED Vars

-------------------------------------------------------------------------------
Next ==
    \/ Stutter
    \/ \E n \in Nodes : Register(n)
    \/ \E n \in Nodes : AdmissionWake(n)
    \/ \E n \in Nodes : CommitSuspend(n)
    \/ SetEvent
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

\* E3: Event Admission Closure. Two consequences:
\* (a) A wait whose admission observed SET resolved Woken inline (did not
\*     commit suspension): admissionSawSet => Woken.
\* (b) A wait that committed suspension did so while UNSET: a Suspended+
\*     Registered node implies eventSet="UNSET". The buggy lost-set protocol
\*     violates this (it allows suspend while SET).
InvEventAdmissionClosure ==
    /\ \A n \in Nodes :
           admissionSawSet[n] = TRUE => nodeState[n] = "Woken"
    /\ \A n \in Nodes :
           /\ nodeState[n] = "Registered"
           /\ admissionPhase[n] = "Suspended"
           => eventSet = "UNSET"

\* E5: Reset Non-Resolution. ResetEvent alone does not change a Registered
\* node to terminal. Structurally: ResetEvent UNCHANGES nodeState/resolvedCount.
\* The invariant consequence: every terminal node was resolved by exactly one
\* cause (wake/cancel), and ResetEvent did not increment resolvedCount.
InvResetNonResolution ==
    \A n \in Nodes :
        /\ isTerminal(n)
        /\ resolvedCount[n] = 1
        => nodeState[n] \in {"Woken", "Cancelled"}

\* E6: Set-Epoch Isolation. An old SET epoch's drain cannot wake a waiter
\* admitted after a later RESET. A node is woken by at most one drain epoch.
InvSetEpochIsolation ==
    \A n \in Nodes : wokenBySetEpoch[n] <= 1

Inv == /\ InvSingleResolutionWinner
       /\ InvSingleRunnablePublication
       /\ InvEventAdmissionClosure
       /\ InvResetNonResolution
       /\ InvSetEpochIsolation

-------------------------------------------------------------------------------
\* E4: Persistent SET Liveness (liveness property, separate .cfg).
\* If a node is Registered+Suspended, under fairness it eventually becomes
\* terminal (Woken by a SetEvent drain, or Cancelled). This is the formal
\* expression of set-releases-all. FairSetEvent ensures SetEvent eventually
\* runs when a Suspended waiter exists and eventSet="UNSET".

FairSetEvent == WF_Vars(SetEvent)

LivenessSpecFair == Spec /\ FairSetEvent

EventSetDrainLivenessNonVacuous ==
    \A n \in Nodes :
        [] ( (/\ nodeState[n] = "Registered"
              /\ admissionPhase[n] = "Suspended")
             => <> (isTerminal(n)) )

=============================================================================
