----------------------- MODULE E11TimerWaitNeg4CallbackAfterRetirement -----------------------
(*
  E11 NEG-4 — Timer Callback After WaitNode Retirement (BROKEN model).

  Defect: a non-timer resolution (RESOURCE_WAKE / CANCEL) resolves the wait and
  publishes WITHOUT retiring the bound timer registration. The registration
  stays ACTIVE while the node becomes terminal; the fiber resumes and the node's
  storage is destroyed (nodeAlive := FALSE); a later ResolveTimer (the stale
  expiry) then dereferences the destroyed node because it still sees an ACTIVE
  registration. This is the I4 (Timer Lifetime Closure) violation — the
  load-bearing E11 difference from E10 loser-safety.

  Broken protocol:
      non-timer resolution publishes/resumes
      WITHOUT retiring timer callback authority
      -> node destroyed while registration ACTIVE
      -> stale expiry dereferences freed storage (UAF)

  Required counterexample state (reachable):
      nodeAlive[n] = FALSE
      regState[r] = "Active"   (for regEpoch[r] = n)
      => InvTimerLifetimeClosure VIOLATED.

  MODEL DESIGN (M5). The defect is modeled at the source the closure protocol
  prescribes — the NON-TIMER RESOLVE PATH. ResolveWakeBuggy / ResolveCancelBuggy
  transition the node to terminal and publish, but do NOT retire the bound Active
  registration (they leave regState unchanged). DestroyNodeBuggy correspondingly
  does NOT require the bound registration to be retired/consumed (its guard only
  checks isTerminal). Together these allow the bad state: a terminal, destroyed
  node whose registration is still ACTIVE. The correct model retires the reg in
  the SAME step as the resolve (so isTerminal => reg Retired/Consumed) and
  DestroyNode requires that; the buggy model omits both, isolating the
  "callback authority not closed before node lifetime" defect as the one-rule
  difference at the resolve path.

  REACHABILITY: Register -> ResolveWakeBuggy (node Woken, reg stays Active) ->
  DestroyNodeBuggy (nodeAlive := FALSE, reg still Active) reaches
  nodeAlive[N]=FALSE /\ regState[R]=Active. InvTimerLifetimeClosure fires.
*)
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Nodes, N0, N1, Regs, R0, R1

VARIABLES
    nodeState, linked, resolvedCount, wakeDispatched,
    regState, regEpoch, regDeadline, nodeAlive, now, parked

NodeState == {"Detached", "Registered", "Woken", "Cancelled", "Expired"}
RegState  == {"Inert", "Active", "Retired", "Consumed"}
DeadlineVal == 0..3

ASSUME
    /\ Nodes # {} /\ N0 \in Nodes /\ N1 \in Nodes /\ N0 # N1
    /\ Regs # {} /\ R0 \in Regs /\ R1 \in Regs /\ R0 # R1

isTerminal(n) == nodeState[n] \in {"Woken", "Cancelled", "Expired"}

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
        /\ UNCHANGED <<now, parked, resolvedCount, wakeDispatched>>

(* DEFECT (the one-rule difference at the resolve path): a non-timer winner
   resolves the node and publishes but does NOT retire the bound Active
   registration. The registration stays ACTIVE across the node's transition to
   terminal and across the subsequent node destruction. (Correct model retires
   the reg here: regState' = IF regEpoch[r]=n /\ Active THEN Retired ELSE ...).
   This is "non-timer resolution publishes WITHOUT retiring timer callback
   authority" — the defect the closure protocol prescribes for NEG-4. *)
ResolveWakeBuggy(n) ==
    /\ nodeState[n] = "Registered"
    /\ nodeState' = [nodeState EXCEPT ![n] = "Woken"]
    /\ linked' = [linked EXCEPT ![n] = FALSE]
    /\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
    /\ wakeDispatched' = wakeDispatched + 1
    \* DEFECT: regState is NOT retired (UNCHANGED) — callback authority stays open.
    /\ UNCHANGED <<regState, regEpoch, regDeadline, nodeAlive, now, parked>>

ResolveCancelBuggy(n) ==
    /\ nodeState[n] = "Registered"
    /\ nodeState' = [nodeState EXCEPT ![n] = "Cancelled"]
    /\ linked' = [linked EXCEPT ![n] = FALSE]
    /\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
    /\ wakeDispatched' = wakeDispatched + 1
    /\ UNCHANGED <<regState, regEpoch, regDeadline, nodeAlive, now, parked>>

ResolveTimer(r) ==
    /\ regState[r] = "Active"
    /\ now >= regDeadline[r]
    /\ nodeState[regEpoch[r]] = "Registered"
    /\ regState' = [regState EXCEPT ![r] = "Consumed"]
    /\ nodeState' = [nodeState EXCEPT ![regEpoch[r]] = "Expired"]
    /\ linked' = [linked EXCEPT ![regEpoch[r]] = FALSE]
    /\ resolvedCount' = [resolvedCount EXCEPT ![regEpoch[r]] =
                                    resolvedCount[regEpoch[r]] + 1]
    /\ wakeDispatched' = wakeDispatched + 1
    /\ UNCHANGED <<regEpoch, regDeadline, nodeAlive, now, parked>>

(* DestroyNode requires only that the node is terminal (its lifetime ended). It
   does NOT require the bound registration to be retired/consumed — which is
   consistent with the buggy resolve path that never retired it. The correct
   model's DestroyNode additionally guards \A r : regEpoch[r]=n =>
   regState[r] \in {Retired,Consumed,Inert}; that guard is redundant in the
   correct model (isTerminal already implies it) but load-bearing here, where
   the buggy resolve leaves the reg Active. *)
DestroyNode(n) ==
    /\ isTerminal(n)
    /\ nodeAlive[n] = TRUE
    \* DEFECT: no retirement guard — the buggy resolve left the reg Active.
    /\ nodeAlive' = [nodeAlive EXCEPT ![n] = FALSE]
    /\ UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched,
                  regState, regEpoch, regDeadline, now, parked>>

Tick ==
    /\ now < 3
    /\ now' = now + 1
    /\ UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched,
                  regState, regEpoch, regDeadline, nodeAlive, parked>>

Stutter == UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched,
                       regState, regEpoch, regDeadline, nodeAlive, now, parked>>

Next ==
    \/ Stutter
    \/ \E n \in Nodes : Register(n)
    \/ \E n \in Nodes : ResolveWakeBuggy(n)
    \/ \E n \in Nodes : ResolveCancelBuggy(n)
    \/ \E r \in Regs : ResolveTimer(r)
    \/ \E n \in Nodes : DestroyNode(n)
    \/ Tick

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

(* I4: an ACTIVE registration implies its bound node storage is still alive
   (retirement/consumption happens before DestroyNode). The buggy model reaches
   nodeAlive[n] = FALSE /\ regState[r] = "Active" (bound to n), so an Active
   registration outlives the node's storage -> VIOLATED (stale-expiry UAF). *)
InvTimerLifetimeClosure ==
    \A r \in Regs :
        regState[r] = "Active" => nodeAlive[regEpoch[r]]

=============================================================================
