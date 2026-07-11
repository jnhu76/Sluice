---------------------- MODULE E12EventNeg2WakeOne ----------------------
(*
  NEG-EVENT-2 -- Wake-One Manual-Reset Strands Waiter.

  One-rule difference from E12Event. Identical EXCEPT SetEventBuggy resolves
  ONLY ONE registered Suspended waiter (the FIFO head), instead of draining
  all registered Suspended waiters. The broken protocol:

    W1 registered, W2 registered
    set -> SET
    resource wake only W1
    set-on-SET idempotent, reset absent, deadline absent, cancel absent
    W2 remains Registered forever while Event remains SET

  Required counterexample: W2 remains Registered+Suspended forever while
  Event remains SET. Violated expected property: EventSetDrainLivenessNonVacuous
  (E4 liveness).

  The defect: SetEventBuggy's drain resolves only one Suspended node (picks
  one), leaving the others stranded. The correct SetEvent resolves ALL
  Suspended nodes in one atomic drain.
*)
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Nodes, N0, N1

VARIABLES nodeState, linked, resolvedCount, wakeDispatched, eventSet,
          admissionPhase, admissionSawSet, wokenBySetEpoch

NodeState == {"Detached", "Registered", "Woken", "Cancelled", "Expired"}
AdmissionPhase == {"NoAdmission", "AdmissionOpen", "Suspended"}

ASSUME /\ Nodes # {}
       /\ N0 \in Nodes
       /\ N1 \in Nodes
       /\ N0 # N1

Vars == <<nodeState, linked, resolvedCount, wakeDispatched, eventSet,
          admissionPhase, admissionSawSet, wokenBySetEpoch>>

isTerminal(n) == nodeState[n] \in {"Woken", "Cancelled", "Expired"}

Drainable ==
    {n \in Nodes : nodeState[n] = "Registered" /\ admissionPhase[n] = "Suspended"}

Init ==
    /\ nodeState = [n \in Nodes |-> "Detached"]
    /\ linked = [n \in Nodes |-> FALSE]
    /\ resolvedCount = [n \in Nodes |-> 0]
    /\ wakeDispatched = 0
    /\ eventSet = "UNSET"
    /\ admissionPhase = [n \in Nodes |-> "NoAdmission"]
    /\ admissionSawSet = [n \in Nodes |-> FALSE]
    /\ wokenBySetEpoch = [n \in Nodes |-> 0]

Register(n) ==
    /\ nodeState[n] = "Detached"
    /\ admissionPhase[n] = "NoAdmission"
    /\ nodeState' = [nodeState EXCEPT ![n] = "Registered"]
    /\ linked' = [linked EXCEPT ![n] = TRUE]
    /\ admissionPhase' = [admissionPhase EXCEPT ![n] = "AdmissionOpen"]
    /\ UNCHANGED <<resolvedCount, wakeDispatched, eventSet,
                   admissionSawSet, wokenBySetEpoch>>

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
    /\ UNCHANGED <<eventSet, wokenBySetEpoch>>

CommitSuspend(n) ==
    /\ nodeState[n] = "Registered"
    /\ admissionPhase[n] = "AdmissionOpen"
    /\ eventSet = "UNSET"
    /\ admissionPhase' = [admissionPhase EXCEPT ![n] = "Suspended"]
    /\ admissionSawSet' = [admissionSawSet EXCEPT ![n] = FALSE]
    /\ UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched, eventSet,
                  wokenBySetEpoch>>

\* DEFECT: SetEventBuggy drains ONLY ONE Suspended node (picks one via \E),
\* instead of all. The correct SetEvent resolves ALL Suspended nodes.
SetEventBuggy ==
    /\ eventSet = "UNSET"
    /\ Cardinality(Drainable) >= 1
    /\ \E victim \in Drainable :
          /\ eventSet' = "SET"
          /\ nodeState' =
                 [n \in Nodes |->
                    IF n = victim THEN "Woken" ELSE nodeState[n]]
          /\ linked' =
                 [n \in Nodes |->
                    IF n = victim THEN FALSE ELSE linked[n]]
          /\ resolvedCount' =
                 [n \in Nodes |->
                    IF n = victim THEN resolvedCount[n] + 1 ELSE resolvedCount[n]]
          /\ wokenBySetEpoch' =
                 [n \in Nodes |->
                    IF n = victim THEN wokenBySetEpoch[n] + 1 ELSE wokenBySetEpoch[n]]
          /\ wakeDispatched' = wakeDispatched + 1

    /\ UNCHANGED <<admissionPhase, admissionSawSet>>

ResetEvent ==
    /\ eventSet = "SET"
    /\ eventSet' = "UNSET"
    /\ UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched,
                  admissionPhase, admissionSawSet, wokenBySetEpoch>>

ResolveCancel(n) ==
    /\ nodeState[n] = "Registered"
    /\ nodeState' = [nodeState EXCEPT ![n] = "Cancelled"]
    /\ linked' = [linked EXCEPT ![n] = FALSE]
    /\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
    /\ wakeDispatched' = wakeDispatched + 1
    /\ UNCHANGED <<eventSet, admissionPhase, admissionSawSet, wokenBySetEpoch>>

Stutter == UNCHANGED Vars

Next ==
    \/ Stutter
    \/ \E n \in Nodes : Register(n)
    \/ \E n \in Nodes : AdmissionWake(n)
    \/ \E n \in Nodes : CommitSuspend(n)
    \/ SetEventBuggy
    \/ ResetEvent
    \/ \E n \in Nodes : ResolveCancel(n)

Spec == Init /\ [][Next]_Vars

\* Fairness on the buggy drain (so it must eventually run when drainable).
FairSetEventBuggy == WF_Vars(SetEventBuggy)
LivenessSpecFair == Spec /\ FairSetEventBuggy

\* The expected violated property: EventSetDrainLivenessNonVacuous (E4).
\* A Suspended node that is NOT the victim remains Registered forever.
EventSetDrainLivenessNonVacuous ==
    \A n \in Nodes :
        [] ( (/\ nodeState[n] = "Registered"
              /\ admissionPhase[n] = "Suspended")
             => <> (isTerminal(n)) )

=============================================================================
