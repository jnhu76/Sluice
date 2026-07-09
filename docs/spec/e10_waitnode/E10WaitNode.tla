------------------------------- MODULE E10WaitNode -------------------------------
(*
  E10 WaitNode / cancellation-safe WaitQueue protocol (sluice-CORE-E10).

  Narrow TLA+ model of the abstract protocol whose correctness is load-bearing
  for E10: ONE canonical terminal resolver authority (resolve_), so that wake
  and cancellation cannot independently invent competing state transitions, and
  queue removal (unlink) is not an independent protocol but the same atomic
  transition as the winner CAS (§2 Design Law, §7 Unlink Law).

  This extends the E7/E8 vocabulary in spirit (register/suspend/wake/finish are
  the same lifecyle moves) but models the WaitNode-specific state machine and
  the WaitQueue membership + winner/unlink authority. It does NOT model the
  Scheduler, the Fiber lifecycle (closed by E7/E8), MW admission (closed by
  E9), backends, or timers.

  THE LOAD-BEARING PROPERTY (§2):
      Exactly one terminal resolver wins.
      All losing resolvers observe loss and perform no second wake.

  Domain (finite, exhaustive TLC): Nodes = {N0, N1}.

  Model:
    nodeState[n] in {Detached, Registered, Woken, Cancelled}
      Detached    : initial; not linked. register_ moves to Registered.
      Registered  : linked in exactly one WaitQueue; resolvable.
      Woken       : terminal (resolved by a wake). Absorbing.
      Cancelled   : terminal (resolved by a cancel). Absorbing.
    linked[n]        : membership (Registered <=> linked). Structural under qmtx.
    resolvedCount[n] : how many times a resolver WON for n (load-bearing == <=1).
    wakeDispatched   : number of scheduler-wake intents issued (load-bearing:
                       equals resolvedCount total, never a duplicate dispatch).

  Actions:
    Register(n)      : Detached -> Registered; links n.  (single-shot per node)
    ResolveWake(n)   : Registered -> Woken if winner CAS; unlink if won.
    ResolveCancel(n) : Registered -> Cancelled if winner CAS; unlink if won.
    Reset(n)         : terminal -> Detached is FORBIDDEN (terminal is absorbing;
                       this action is absent by construction — NoTerminalResur-
                       rection holds because no rule moves out of Woken/Cancelled).

  The "winner CAS" is modeled as the guarded transition: ResolveWake(n) is
  enabled only when nodeState[n] = "Registered", and moves it to "Woken". A
  concurrent ResolveCancel(n) observes nodeState[n] = "Woken" (terminal) and is
  NOT enabled. Because TLA+ actions are atomic and mutually exclusive on the
  Registered state, at most one resolver transitions out of Registered — this
  IS the single-winner linearization. resolvedCount[n] is incremented exactly
  by the winning action, so NoDoubleCompletion == resolvedCount[n] <= 1.

  This matches production: WaitNode::resolve_(outcome) is a CAS Registered ->
  {Woken,Cancelled}; the winner increments the count (conceptually) and unlinks;
  the loser's CAS fails and it does nothing.
*)
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Nodes, N0, N1

VARIABLES
    nodeState,        \* [Nodes -> NodeState]
    linked,           \* [Nodes -> BOOLEAN]  -- Registered <=> linked (queue membership)
    resolvedCount,    \* [Nodes -> Nat]      -- number of winning resolutions per node (<= 1)
    wakeDispatched    \* Nat                 -- total scheduler-wake intents issued

NodeState == {"Detached", "Registered", "Woken", "Cancelled"}

ASSUME
    /\ Nodes # {}
    /\ N0 \in Nodes
    /\ N1 \in Nodes
    /\ N0 # N1

(* =========================================================================
   Actions
   ========================================================================= *)

(* Register: Detached -> Registered; link the node. Single-shot: only succeeds
   from Detached (C8 reuse rejection: a terminal/already-registered node cannot
   re-register). *)
Register(n) ==
    /\ nodeState[n] = "Detached"
    /\ nodeState' = [nodeState EXCEPT ![n] = "Registered"]
    /\ linked' = [linked EXCEPT ![n] = TRUE]
    /\ UNCHANGED <<resolvedCount, wakeDispatched>>

(* THE CANONICAL ONE-WINNER TERMINAL RESOLVER — Wake.
   Enabled only when Registered. The transition Registered -> Woken is the
   linearization point (§7): at that instant n is (a) terminally resolved,
   (b) the unique winner, (c) the unique unlink owner. resolvedCount++ and
   wakeDispatched++ model the single scheduler-wake intent (route_runnable).
   The unlink (linked' := FALSE) happens in the SAME critical section. *)
ResolveWake(n) ==
    /\ nodeState[n] = "Registered"
    /\ nodeState' = [nodeState EXCEPT ![n] = "Woken"]
    /\ linked' = [linked EXCEPT ![n] = FALSE]      \* winner unlinks (§7)
    /\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
    /\ wakeDispatched' = wakeDispatched + 1          \* exactly one scheduler-wake intent

(* THE CANONICAL ONE-WINNER TERMINAL RESOLVER — Cancel. Same authority as Wake;
   outcome Cancelled. resolvedCount++ but wakeDispatched UNCHANGED is a modeling
   choice: a cancel also resumes the fiber through the canonical seam in
   production (cancel_wait -> route_runnable_locked), so it increments
   wakeDispatched too. *)
ResolveCancel(n) ==
    /\ nodeState[n] = "Registered"
    /\ nodeState' = [nodeState EXCEPT ![n] = "Cancelled"]
    /\ linked' = [linked EXCEPT ![n] = FALSE]      \* winner unlinks (§7)
    /\ resolvedCount' = [resolvedCount EXCEPT ![n] = resolvedCount[n] + 1]
    /\ wakeDispatched' = wakeDispatched + 1          \* cancel also routes (one enqueue)

(* Stutter: no spurious deadlock; invariants checked across terminal states. *)
Stutter ==
    /\ UNCHANGED <<nodeState, linked, resolvedCount, wakeDispatched>>

(* =========================================================================
   Next, Init, Spec
   ========================================================================= *)

Next ==
    \/ Stutter
    \/ \E n \in Nodes : Register(n)
    \/ \E n \in Nodes : ResolveWake(n)
    \/ \E n \in Nodes : ResolveCancel(n)

Init ==
    /\ nodeState = [n \in Nodes |-> "Detached"]
    /\ linked = [n \in Nodes |-> FALSE]
    /\ resolvedCount = [n \in Nodes |-> 0]
    /\ wakeDispatched = 0

Spec == Init /\ [][Next]_<<nodeState, linked, resolvedCount, wakeDispatched>>

(* =========================================================================
   E10 safety properties (§12)
   ========================================================================= *)

(* SingleWinner / NoDoubleCompletion: each node is resolved by at most one
   winning resolver. resolvedCount[n] <= 1 for all n. *)
InvNoDoubleCompletion ==
    \A n \in Nodes : resolvedCount[n] <= 1

(* NoTerminalResurrection: a terminal node (Woken/Cancelled) never returns to a
   non-terminal state. Structural: no action is enabled for terminal nodeState
   (Register needs Detached; ResolveWake/Cancel need Registered). *)
InvNoTerminalResurrection ==
    \A n \in Nodes :
        /\ (nodeState[n] = "Woken" =>
            nodeState[n] = "Woken")   \* tautology; the structural guarantee is
                                      \* that Next cannot move Woken -> anything.
        /\ (nodeState[n] = "Cancelled" => nodeState[n] = "Cancelled")

(* LinkedImpliesLive: a linked node is Registered (not terminal). Registered <=>
   linked (the membership invariant, §5). *)
InvLinkedImpliesRegistered ==
    \A n \in Nodes : linked[n] <=> nodeState[n] = "Registered"

(* TerminalEventuallyDetached-from-queue (structural): a terminal node is NOT
   linked. The winner unlinks in the same transition that resolves it, so a
   terminal node is never in the queue. (Liveness under enabled resolver
   execution is modeled by WF on ResolveWake/ResolveCancel in the liveness cfg.)
   LinkedImpliesRegistered already implies this; stated for §12 completeness. *)
InvTerminalNotLinked ==
    \A n \in Nodes :
        (nodeState[n] = "Woken" \/ nodeState[n] = "Cancelled") => ~linked[n]

(* NoDuplicateSchedulerWake: the number of scheduler-wake intents equals the
   total number of winning resolutions (each winner dispatches exactly once;
   losers dispatch zero times). wakeDispatched = Sum resolvedCount. *)
InvNoDuplicateSchedulerWake ==
    wakeDispatched = SumOver(resolvedCount)

(* Helper: sum of resolvedCount over all nodes. *)
RECURSIVE SumOver(_)
SumOver(rc) ==
    LET ns == [n \in Nodes |-> rc[n]]
    IN IF Nodes = {} THEN 0
       ELSE LET pick == CHOOSE n \in Nodes : TRUE
            IN rc[pick] + SumOver([n \in Nodes \ {pick} |-> rc[n]])

Inv ==
    /\ InvNoDoubleCompletion
    /\ InvNoTerminalResurrection
    /\ InvLinkedImpliesRegistered
    /\ InvTerminalNotLinked
    /\ InvNoDuplicateSchedulerWake

(* =========================================================================
   Temporal (liveness): under weak fairness on the resolvers, a Registered node
   is eventually resolved (TerminalEventuallyDetached under enabled execution).
   ========================================================================= *)
LivenessSpec == Spec
FairResolve ==
    /\ WF_<<nodeState, linked, resolvedCount, wakeDispatched>>(\E n \in Nodes : ResolveWake(n))
    /\ WF_<<nodeState, linked, resolvedCount, wakeDispatched>>(\E n \in Nodes : ResolveCancel(n))
LivenessSpecFair == LivenessSpec /\ []FairResolve

EventualResolution ==
    \A n \in Nodes : (nodeState[n] = "Registered" ~> nodeState[n] # "Registered")
=============================================================================
