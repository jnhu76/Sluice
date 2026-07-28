----------------------- MODULE E11TimerWaitNeg6DeadlineLostAtAdmission -----------------------
(*
  E11 NEG-6 — Due Deadline Lost at Admission (BROKEN model).

  Defect: the final admission decision commits Fiber suspension EVEN WHEN the
  deadline is ALREADY due at that decision. Production await_wait_deadline
  rechecks the clock after registration and, if the deadline is already due,
  resolves Expired inline WITHOUT calling make_waiting. The buggy model drops
  that recheck from the suspend path: CommitSuspendBuggy omits the
  ~admissionDeadlineDue(n) guard, so it may suspend a wait whose deadline has
  already arrived. The wait is then "parked" on a due deadline — exactly the
  "already-due deadline lost during wait admission" defect (I5).

  Broken protocol:
      register WaitNode (deadline already due)
      register timer
      final admission decision: CommitSuspend EVEN THOUGH deadline already due
      -> Fiber suspended on a wait whose deadline had already arrived
      -> admission phase = Suspended
      -> node state = Registered
      -> the deadline was due at the admission decision (suspendedDue = TRUE)

  Required counterexample state (reachable):
      admissionPhase[n] = "Suspended"
      suspendedDue[n] = TRUE        \* deadline was due when suspension committed
      nodeState[n] = "Registered"   \* still parked, not resolved at admission
      => InvDeadlineAdmissionClosure VIOLATED.

  MODEL DESIGN (M5 one-rule difference). Identical to E11TimerWait EXCEPT
  CommitSuspendBuggy is missing the /\ ~admissionDeadlineDue(n) guard that the
  correct model's CommitSuspend requires. Every other rule — Register,
  AdmissionExpire, ResolveWake/Cancel/Timer, DestroyNode, Tick, park topology —
  is the correct protocol. The buggy suspend sets suspendedDue[n] to the actual
  deadline-due fact (admissionDeadlineDue(n)), which is now unconstrained, so a
  due-at-admission suspension records suspendedDue = TRUE and trips the I5
  invariant. AdmissionExpire (the correct admission-expire branch) is left in
  the model so the correct path is also reachable; the defect is solely that the
  suspend path is NOT closed against an already-due deadline.

  REACHABILITY. Register(N0) with regDeadline=0 (now=0 => already due). The
  correct model forces AdmissionExpire here (CommitSuspend disabled). The buggy
  model ALSO allows CommitSuspendBuggy(N0): admissionPhase -> Suspended,
  suspendedDue -> TRUE, node stays Registered, reg stays Active. The state
  admissionPhase[N0]="Suspended" /\ suspendedDue[N0]=TRUE is reachable in two
  steps, violating InvDeadlineAdmissionClosure. The defect is NOT manufactured
  by destroying nodes, letting timers resolve arbitrary epochs, disabling time,
  or initializing suspendedDue=TRUE — it is exactly the missing admission
  recheck on the suspend path.
*)
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Nodes, N0, N1, Regs, R0, R1

VARIABLES
    nodeState, linked, resolvedCount, wakeDispatched,
    regState, regEpoch, regDeadline, nodeAlive, now, parked,
    admissionPhase, suspendedDue

NodeState == {"Detached", "Registered", "Woken", "Cancelled", "Expired"}
RegState  == {"Inert", "Active", "Retired", "Consumed"}
AdmissionPhase == {"NoAdmission", "AdmissionOpen", "Suspended"}
DeadlineVal == 0..3

ASSUME
    /\ Nodes # {} /\ N0 \in Nodes /\ N1 \in Nodes /\ N0 # N1
    /\ Regs # {} /\ R0 \in Regs /\ R1 \in Regs /\ R0 # R1

isTerminal(n) == nodeState[n] \in {"Woken", "Cancelled", "Expired"}
deadlineDue(r) == regState[r] = "Active" /\ now >= regDeadline[r]
admissionDeadlineDue(n) ==
    \E r \in Regs : regEpoch[r] = n /\ regState[r] = "Active" /\ now >= regDeadline[r]

Register(n) ==
    /\ nodeState[n] = "Detached"
    /\ nodeAlive[n] = FALSE
    /\ \E r \in Regs :
        /\ regState[r] = "Inert"
        /\ nodeState' = [nodeState EXCEPT ![n] = "Registered"]
        /\ linked' = [linked EXCEPT ![n] = TRUE]
        /\ regState' = [regState EXCEPT ![r] = "Active"]
        /\ regEpoch' = [regEpoch EXCEPT ![r] = n]
        /\ \E d \in DeadlineVal : regDeadline' = [regDeadline EXCEPT ![r] = d]
        /\ nodeAlive' = [nodeAlive EXCEPT ![n] = TRUE]
        /\ admissionPhase' = [admissionPhase EXCEPT ![n] = "AdmissionOpen"]
        /\ suspendedDue' = [suspendedDue EXCEPT ![n] = FALSE]
        /\ UNCHANGED <<now, parked, resolvedCount, wakeDispatched>>

(* The correct admission-expire branch is retained so the model is otherwise the
   correct protocol. *)
AdmissionExpire(n) ==
    /\ nodeState[n] = "Registered"
    /\ admissionPhase[n] = "AdmissionOpen"
    /\ \E r \in Regs :
        /\ regEpoch[r] = n
        /\ regState[r] = "Active"
        /\ now >= regDeadline[r]
    /\ nodeState' = [nodeState EXCEPT ![n] = "Expired"]
    /\ linked' = [linked EXCEPT ![n] = FALSE]
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

(* DEFECT (the one-rule difference at the admission boundary): CommitSuspendBuggy
   commits Fiber suspension WITHOUT rechecking the deadline. The correct model
   guards /\ ~admissionDeadlineDue(n); this buggy action omits that guard, so it
   may suspend a wait whose deadline is ALREADY due. suspendedDue[n] is set to
   the actual deadline-due fact, which the omitted guard would have forced to
   FALSE — here it becomes TRUE, directly tripping InvDeadlineAdmissionClosure.
   (Correct model: CommitSuspend with /\ ~admissionDeadlineDue(n) guard.) *)
CommitSuspendBuggy(n) ==
    /\ nodeState[n] = "Registered"
    /\ admissionPhase[n] = "AdmissionOpen"
    \* DEFECT: the /\ ~admissionDeadlineDue(n) guard is OMITTED.
    /\ admissionPhase' = [admissionPhase EXCEPT ![n] = "Suspended"]
    /\ suspendedDue' = [suspendedDue EXCEPT ![n] = admissionDeadlineDue(n)]
    /\ UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched,
                  regState, regEpoch, regDeadline, nodeAlive, now, parked>>

ResolveWake(n) ==
    /\ nodeState[n] = "Registered"
    /\ nodeState' = [nodeState EXCEPT ![n] = "Woken"]
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

DestroyNode(n) ==
    /\ isTerminal(n)
    /\ nodeAlive[n] = TRUE
    /\ \A r \in Regs : regEpoch[r] = n => regState[r] \in {"Retired", "Consumed", "Inert"}
    /\ nodeAlive' = [nodeAlive EXCEPT ![n] = FALSE]
    /\ UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched,
                  regState, regEpoch, regDeadline, now, parked,
                  admissionPhase, suspendedDue>>

Tick ==
    /\ now < 3
    /\ now' = now + 1
    /\ UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched,
                  regState, regEpoch, regDeadline, nodeAlive, parked,
                  admissionPhase, suspendedDue>>

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

Stutter == UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched,
                       regState, regEpoch, regDeadline, nodeAlive, now, parked,
                       admissionPhase, suspendedDue>>

Next ==
    \/ Stutter
    \/ \E n \in Nodes : Register(n)
    \/ \E n \in Nodes : AdmissionExpire(n)
    \/ \E n \in Nodes : CommitSuspendBuggy(n)
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

(* I5 — Deadline Admission Closure: a Suspended wait must not have observed its
   deadline already due at the final admission decision. The buggy
   CommitSuspendBuggy (no ~due guard) reaches admissionPhase[n]="Suspended" /\
   suspendedDue[n]=TRUE with the node still Registered on a due deadline — the
   suspended-on-an-already-due-deadline defect. This is exactly the I5 violation
   that the previous P => TRUE tautology could never demonstrate. *)
InvDeadlineAdmissionClosure ==
    \A n \in Nodes :
        admissionPhase[n] = "Suspended" => ~suspendedDue[n]

=============================================================================
