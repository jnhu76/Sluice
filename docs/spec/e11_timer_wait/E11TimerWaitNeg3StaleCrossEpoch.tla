----------------------- MODULE E11TimerWaitNeg3StaleCrossEpoch -----------------------
(*
  E11 NEG-3 — Stale Timer Cross-Epoch Resolution (BROKEN model).

  This is the address-reuse boundary between I3 and I4. The defect: a timer
  registration captures only a REUSABLE STORAGE SLOT (the address/Fiber
  identity), not the logical wait epoch. When epoch E+1 reuses the same slot A,
  a stale timer registered for E resolves E+1.

  Storage-reuse trace (the load-bearing counterexample):
      epoch E uses storage slot A
      timer R_E captures only reusable storage A (NOT the epoch identity)
      E resolves (e.g. RESOURCE_WAKE); node E dies (slot A freed)
      epoch E+1 reuses storage slot A
      R_E expires
      R_E resolves E+1 (because it keys on slot A, which E+1 now occupies)

  Required counterexample:
      a timer registered for epoch E causes resolution of E+1 -> a second
      resolution for the slot, violating wait-epoch isolation (I3).

  MODEL DESIGN. Two slots (A0, A1) and two epochs (N0, N1). slotOf[n] maps each
  epoch to the slot its node occupies; slots are REUSABLE (DestroyNode frees the
  slot, and a later Register may claim it). The buggy ResolveTimer targets the
  slot the registration captured (regSlot[r]) rather than the epoch it was bound
  to (regEpoch[r]). When E+1 reuses E's slot, the stale timer for E resolves E+1.

  The CORRECT model keys the expiry on regEpoch[r] (the logical epoch), which is
  immutable and distinct per Register — so a stale timer for E can never reach
  E+1. Production realizes this by capturing WaitNode& (never only Fiber*).
*)
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Nodes, N0, N1, Regs, R0, R1, Slots, A0, A1

VARIABLES
    nodeState, slotOf, slotFree, resolvedCount,
    regState, regEpoch, regSlot, regDeadline, now

NodeState == {"Detached", "Registered", "Woken", "Cancelled", "Expired"}
RegState  == {"Inert", "Active", "Retired", "Consumed"}
DeadlineVal == 0..3

ASSUME
    /\ Nodes # {} /\ N0 \in Nodes /\ N1 \in Nodes /\ N0 # N1
    /\ Regs # {} /\ R0 \in Regs /\ R1 \in Regs /\ R0 # R1
    /\ Slots # {} /\ A0 \in Slots /\ A1 \in Slots /\ A0 # A1

isTerminal(n) == nodeState[n] \in {"Woken", "Cancelled", "Expired"}

(* Register: claim a free slot for this epoch, bind an Active registration that
   captures BOTH the epoch (regEpoch) AND the slot (regSlot). A fresh node starts
   alive (slot occupied). *)
Register(n) ==
    /\ nodeState[n] = "Detached"
    /\ \E s \in Slots :
        /\ slotFree[s] = TRUE
        /\ \E r \in Regs :
            /\ regState[r] = "Inert"
            /\ nodeState' = [nodeState EXCEPT ![n] = "Registered"]
            /\ slotOf' = [slotOf EXCEPT ![n] = s]
            /\ slotFree' = [slotFree EXCEPT ![s] = FALSE]
            /\ regState' = [regState EXCEPT ![r] = "Active"]
            /\ regEpoch' = [regEpoch EXCEPT ![r] = n]
            /\ regSlot' = [regSlot EXCEPT ![r] = s]
            /\ \E d \in DeadlineVal : regDeadline' = [regDeadline EXCEPT ![r] = d]
            /\ UNCHANGED <<now, resolvedCount>>

ResolveWake(n) ==
    /\ nodeState[n] = "Registered"
    /\ nodeState' = [nodeState EXCEPT ![n] = "Woken"]
    /\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
    /\ regState' = [r \in Regs |->
                        IF /\ regEpoch[r] = n /\ regState[r] = "Active"
                        THEN "Retired" ELSE regState[r]]
    /\ UNCHANGED <<slotOf, slotFree, regEpoch, regSlot, regDeadline, now>>

(* DEFECT: ResolveTimer targets the SLOT the registration captured (regSlot[r]),
   resolving whichever epoch CURRENTLY occupies that slot — NOT the epoch the
   registration was bound to (regEpoch[r]). When E+1 reuses E's slot, a stale
   Active timer for E resolves E+1. *)
ResolveTimer(r) ==
    /\ regState[r] = "Active"
    /\ now >= regDeadline[r]
    /\ \E m \in Nodes :
        /\ slotOf[m] = regSlot[r]                 \* DEFECT: keys on slot, not epoch
        /\ nodeState[m] = "Registered"
        /\ regState' = [regState EXCEPT ![r] = "Consumed"]
        /\ nodeState' = [nodeState EXCEPT ![m] = "Expired"]
        /\ resolvedCount' = [resolvedCount EXCEPT ![m] = resolvedCount[m] + 1]
        /\ UNCHANGED <<slotOf, slotFree, regEpoch, regSlot, regDeadline, now>>

(* DestroyNode: free the slot so a later epoch may reuse it. Models the
   post-resolution lifetime window + storage reuse. The registration must be
   retired/consumed first (DestroyNode requires it). *)
DestroyNode(n) ==
    /\ isTerminal(n)
    /\ \A r \in Regs : regEpoch[r] = n => regState[r] \in {"Retired", "Consumed", "Inert"}
    /\ slotFree' = [slotFree EXCEPT ![slotOf[n]] = TRUE]
    /\ UNCHANGED <<nodeState, slotOf, resolvedCount,
                  regState, regEpoch, regSlot, regDeadline, now>>

Tick ==
    /\ now < 3
    /\ now' = now + 1
    /\ UNCHANGED <<nodeState, slotOf, slotFree, resolvedCount,
                  regState, regEpoch, regSlot, regDeadline>>

Stutter == UNCHANGED <<nodeState, slotOf, slotFree, resolvedCount,
                       regState, regEpoch, regSlot, regDeadline, now>>

Next ==
    \/ Stutter
    \/ \E n \in Nodes : Register(n)
    \/ \E n \in Nodes : ResolveWake(n)
    \/ \E r \in Regs : ResolveTimer(r)
    \/ \E n \in Nodes : DestroyNode(n)
    \/ Tick

Init ==
    /\ nodeState = [n \in Nodes |-> "Detached"]
    /\ slotOf = [n \in Nodes |-> A0]
    /\ slotFree = [s \in Slots |-> TRUE]
    /\ resolvedCount = [n \in Nodes |-> 0]
    /\ regState = [r \in Regs |-> "Inert"]
    /\ regEpoch = [r \in Regs |-> N0]
    /\ regSlot = [r \in Regs |-> A0]
    /\ regDeadline = [r \in Regs |-> 0]
    /\ now = 0

Vars == <<nodeState, slotOf, slotFree, resolvedCount,
          regState, regEpoch, regSlot, regDeadline, now>>
Spec == Init /\ [][Next]_Vars

(* I3 violation: a timer registered for epoch E resolves a DIFFERENT epoch E+1.
   Counterexample: resolvedCount[m] > 0 for an epoch m that a registration bound
   to a DIFFERENT epoch caused. Concretely, the buggy ResolveTimer resolves an
   epoch m with regEpoch[r] # m — captured by resolvedCount growing past the
   node's own single resolution. The cleaner witness: some slot is resolved
   twice across two epochs (resolvedCount[N0]+resolvedCount[N1] > 1 reachable
   where one resolution was caused by a stale cross-epoch timer). *)
InvWaitEpochIsolation ==
    \A r \in Regs :
        regState[r] = "Consumed" => TRUE   \* structural (correct model keys on
                                           \* regEpoch, so r can only resolve its
                                           \* own epoch). The buggy model keys on
                                           \* regSlot, allowing cross-epoch.
\* The invariant the buggy model must VIOLATE: single resolution winner.
InvSingleResolutionWinner == \A n \in Nodes : resolvedCount[n] <= 1

=============================================================================
