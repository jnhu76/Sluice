------------------------------- MODULE E8SuspendSwitch -------------------------------
(*
  E8 suspend-switch steal-exclusion protocol (sluice-CORE-E8 / I47-F2, MODEL-007a).

  Focused safety model of the window that E8OwnershipTransfer.tla deliberately
  fuses away: its atomic SuspendFiber linearizes register + make_waiting +
  PHYSICAL context_switch into one step, which hides exactly the transient this
  suite exists to examine —

      logical state already Waiting (or woken Runnable with a published
      runnable ticket on the victim's local_runnable)
      WHILE the victim worker's CPU is still executing the Fiber and
      fiber->ctx rsp/rbp/rip have NOT yet been saved by the in-flight
      context_switch.

  Question answered: what prevents another worker from stealing that ticket and
  resuming a context that was never saved?  As-built answer (production C++ is
  the fact source; the model conforms to the C++, never vice versa):

    WorkerState::suspend_switch_pending
      raised  UNDER global_mtx_ by Scheduler::commit_suspend_locked
              (src/async/scheduler.cpp: raise store(true, release) BEFORE
              fiber->make_waiting(), both inside the same global_mtx_ critical
              section — so no resolver can publish a ticket between
              Waiting-visibility and protection-publication);
      cleared AFTER the physical switch, on the SCHEDULER continuation in
              Scheduler::run_next_on (store(false, release)), the single clear
              point, holding no lock;
      read    by Scheduler::try_steal under global_mtx_ (load acquire); a true
              value makes the thief skip the WHOLE victim for that scan.

  AS-BUILT TRANSITION TABLE (full table: issue #172 Comment A).

    W0/F  CommitSuspend  [G held]     pending:=TRUE; Running->Waiting;
                                     physical phase InFlight (switch not done)
    R     Resolve         [G held]    Waiting->Runnable; ticket -> W0 local.
                                     NO pending guard (faithful: the resolver
                                     never reads the flag) — may fire while the
                                     physical switch is still in flight.
    W1    StealRefused    [G held]    sees pending==TRUE -> skip whole victim
    W0/F  SaveContext     [no lock]   fiber_ctx::context_switch stores
                                     rsp/rbp/rip into fiber->ctx (3 stores)
    W0    ClearPending    [no lock]   scheduler continuation, post-save:
                                     pending:=FALSE
    W1    StealTicket     [G held]    pending==FALSE: move ticket W0->W1
    W1    PopResumeOnThief [no lock]  make_running (fail-fast unless Runnable)
                                     + switch into fiber->ctx: Resumed on W1

  DELIBERATE SPLITS (the point of this suite; do NOT re-fuse):
    CommitSuspend (logical commit, one global_mtx_ CS) /
    SaveContext   (physical save, thread-local) /
    ClearPending  (authority withdrawal, thread-local, post-save).
  E8's atomic SuspendFiber fuses all three; the wake-before-save transient
  lives exactly in the gap this suite keeps open. The gap between SaveContext
  and ClearPending (Saved /\ pending) is also real and observable: a thief
  refuses then too (harmless over-protection) — preserved, not optimized away.

  Domain (finite, exhaustive TLC): 2 workers implied by role (W0 = victim,
  W1 = thief), ONE Fiber, ONE resolver. The executor is encoded in fiberState
  ("Running" = on W0, "Resumed" = on W1); ticket location names the queue; the
  owner record is redundant with ticket location in this scenario and is
  deliberately NOT modeled (no contribution to any property or guard).

  SAFETY ONLY. No fairness, no liveness, no timers/backends/select/cancel
  semantics, no WaitQueue capacity, no second Fiber. The resolver is abstracted
  as "holds legitimate resolve authority: may perform Waiting->Runnable +
  one-ticket publication".

  NEGATIVE CONTROLS (config-boolean flips, d1_uring_poison precedent; one
  defect per cfg, no generator, no staleness surface):
    GuardStealWithPending = FALSE  -> NEG-SS1 (H1: try_steal ignores the flag)
    RaiseBeforeVisibility = FALSE  -> NEG-SS2 (H2: the OLD P1-1 corrective —
                                       make_waiting under G, raise only AFTER
                                       G release; resolver may publish while
                                       the authority is still down)
    ClearOnlyAfterSave    = FALSE  -> NEG-SS3 (H3: the OLD Select path — clear
                                       before the physical save completed)

  WEAK-MEMORY BOUNDARY: this is a sequentially-consistent protocol abstraction
  of the as-built program order and global_mtx_ serialization boundaries. TLC
  does NOT prove the C++ release/acquire implementation. The C++ memory-model
  argument (raise store(true,release) at scheduler.cpp:1331, clear
  store(false,release) at scheduler.cpp:1308, thief load(acquire) at
  scheduler.cpp:1931; save stores and clear are same-thread program-ordered) is
  a separate implementation-level obligation recorded in the suite README.
*)

EXTENDS Naturals

CONSTANTS
    \* try_steal refuses a victim whose suspend_switch_pending is true.
    \* FALSE = the H1 defect: steal proceeds regardless of the flag.
    GuardStealWithPending,
    \* commit_suspend_locked raises the authority BEFORE make_waiting, inside
    \* the same global_mtx_ CS (store(true) precedes Waiting-visibility).
    \* FALSE = the H2 defect: the old P1-1 protocol — Waiting becomes visible
    \* first; the raise happens only later (RaiseAuthorityLate), outside G.
    RaiseBeforeVisibility,
    \* run_next_on clears the authority only AFTER the physical save.
    \* FALSE = the H3 defect: clear may precede SaveContext.
    ClearOnlyAfterSave

VARIABLES
    fiberState,      \* "Running"(on W0) | "Waiting" | "Runnable" | "Resumed"(on W1)
    switchPhase,     \* "NoSuspend" | "InFlight" | "Saved"   (physical ctx save)
    suspendPending,  \* BOOLEAN — victim's suspend_switch_pending
    ticketLocation,  \* "None" | "VictimLocal" | "ThiefLocal" (runnable ticket)
    sawStealRefusal  \* HISTORY ghost: a thief observed (ticket@victim /\ pending)

FiberStates == {"Running", "Waiting", "Runnable", "Resumed"}
SwitchPhases == {"NoSuspend", "InFlight", "Saved"}
TicketLocs == {"None", "VictimLocal", "ThiefLocal"}

vars == <<fiberState, switchPhase, suspendPending, ticketLocation, sawStealRefusal>>

(* =========================================================================
   Actions (each maps to one C++ anchor; see table above)
   ========================================================================= *)

(* A2: Scheduler::commit_suspend_locked — ONE global_mtx_ critical section.
   As-built (RaiseBeforeVisibility=TRUE): store(true) then make_waiting(),
   atomically w.r.t. every G-taking resolver/thief, so no resolver can publish
   between Waiting-visibility and protection-publication.
   The mutant (FALSE, old P1-1): only Running->Waiting happens here; the
   raise is deferred to RaiseAuthorityLate outside the CS. *)
CommitSuspend ==
    /\ fiberState = "Running"
    /\ switchPhase = "NoSuspend"
    /\ ~ suspendPending
    /\ fiberState' = "Waiting"
    /\ switchPhase' = "InFlight"
    /\ suspendPending' = IF RaiseBeforeVisibility THEN TRUE ELSE suspendPending
    /\ UNCHANGED <<ticketLocation, sawStealRefusal>>

(* Only enabled by the H2 mutant: the victim raises the authority AFTER G
   release (old P1-1 shape), so Waiting/Runnable visibility can precede the
   raise. Guarded on the physical phase (the victim executes it on its way to
   the switch, regardless of any resolver). *)
RaiseAuthorityLate ==
    /\ ~ RaiseBeforeVisibility
    /\ fiberState \in {"Waiting", "Runnable"}
    /\ switchPhase = "InFlight"
    /\ ~ suspendPending
    /\ suspendPending' = TRUE
    /\ UNCHANGED <<fiberState, switchPhase, ticketLocation, sawStealRefusal>>

(* C1: resolver publication (make_runnable + route_runnable_locked, under G).
   Faithful: NO pending guard — the resolver never reads the flag, so this may
   fire while the physical switch is still in flight (the R1 transient). *)
Resolve ==
    /\ fiberState = "Waiting"
    /\ fiberState' = "Runnable"
    /\ ticketLocation' = "VictimLocal"
    /\ UNCHANGED <<switchPhase, suspendPending, sawStealRefusal>>

(* D2: try_steal under G loads pending==true and skips the whole victim.
   Witness-only (records the independent pre-state fact ticket@victim /\
   pending); state otherwise unchanged. Proves the refusal path non-vacuous. *)
StealRefused ==
    /\ ticketLocation = "VictimLocal"
    /\ suspendPending
    /\ sawStealRefusal' = TRUE
    /\ UNCHANGED <<fiberState, switchPhase, suspendPending, ticketLocation>>

(* B1: the physical save — fiber_ctx::context_switch's three stores of
   rsp/rbp/rip into fiber->ctx (src/async/fiber_ctx.cpp). Thread-local, no
   lock; may occur with the Fiber logically Waiting OR already woken Runnable. *)
SaveContext ==
    /\ switchPhase = "InFlight"
    /\ switchPhase' = "Saved"
    /\ UNCHANGED <<fiberState, suspendPending, ticketLocation, sawStealRefusal>>

(* B2: run_next_on on the scheduler continuation — the SINGLE clear point,
   after the physical save, holding no lock. ClearOnlyAfterSave=FALSE is the
   H3 defect (the old Select path cleared on the Fiber continuation before the
   save was complete). *)
ClearPending ==
    /\ suspendPending
    /\ (ClearOnlyAfterSave => switchPhase = "Saved")
    /\ suspendPending' = FALSE
    /\ UNCHANGED <<fiberState, switchPhase, ticketLocation, sawStealRefusal>>

(* D3: try_steal commit — move ticket victim->thief (+ owner transfer, which
   is redundant with the move here). Guarded on Runnable (the C++ scan accepts
   only state==runnable) and, in the correct protocol, on ~suspendPending. *)
StealTicket ==
    /\ fiberState = "Runnable"
    /\ ticketLocation = "VictimLocal"
    /\ (GuardStealWithPending => ~ suspendPending)
    /\ ticketLocation' = "ThiefLocal"
    /\ UNCHANGED <<fiberState, switchPhase, suspendPending, sawStealRefusal>>

(* D4: run_next_on(W1, F) — make_running (I47-F3 fail-fast unless Runnable,
   encoded structurally as the guard) + context_switch into fiber->ctx. *)
PopResumeOnThief ==
    /\ fiberState = "Runnable"
    /\ ticketLocation = "ThiefLocal"
    /\ fiberState' = "Resumed"
    /\ ticketLocation' = "None"
    /\ UNCHANGED <<switchPhase, suspendPending, sawStealRefusal>>

(* Terminal states (Resumed) have no successor but this; keeps TLC's deadlock
   check green without inventing post-resume behavior. *)
Stutter ==
    /\ UNCHANGED vars

Next ==
    \/ Stutter
    \/ CommitSuspend
    \/ RaiseAuthorityLate
    \/ Resolve
    \/ StealRefused
    \/ SaveContext
    \/ ClearPending
    \/ StealTicket
    \/ PopResumeOnThief

(* Scenario entry: F is mid-flight Running on its owner W0 (spawn + pop are
   E7/E8 territory, already covered there; this suite starts at the race).
   Booleans are written in equality form: TLC's initial-state enumerator
   rejects negation-form constraints (~v) on variables. *)
Init ==
    /\ fiberState = "Running"
    /\ switchPhase = "NoSuspend"
    /\ suspendPending = FALSE
    /\ ticketLocation = "None"
    /\ sawStealRefusal = FALSE

Spec == Init /\ [][Next]_vars

(* =========================================================================
   Safety invariants (positive cfg checks all four)
   ========================================================================= *)

(* CORE (deliberately minimal-strength): the only forbidden thing is RESUMING
   an unsaved context on the thief. Pre-save ticket MOVEMENT is not claimed to
   be impossible — only resume-before-save is. *)
InvNoResumeBeforeContextSaved ==
    fiberState = "Resumed" => switchPhase = "Saved"

(* PROTOCOL AUTHORITY: while a suspension is committed but the physical save
   has not completed, the protection flag must be up (for any logical state
   the resolver may have produced meanwhile). *)
InvUnsavedSuspensionProtected ==
    (switchPhase = "InFlight" /\ fiberState \in {"Waiting", "Runnable"})
        => suspendPending

(* E7-T2 structure preserved through steal/consume: a live ticket implies a
   Runnable fiber (exactly-one-ticket publication shape). *)
InvTicketImpliesRunnable ==
    ticketLocation # "None" => fiberState = "Runnable"

(* The flag is only ever up inside a real suspension cycle (raise bound to
   commit; clear bound to the scheduler continuation). *)
InvPendingImpliesCommitted ==
    suspendPending => switchPhase # "NoSuspend"

Inv ==
    /\ InvNoResumeBeforeContextSaved
    /\ InvUnsavedSuspensionProtected
    /\ InvTicketImpliesRunnable
    /\ InvPendingImpliesCommitted

(* =========================================================================
   Reachability witnesses (dedicated cfgs expect each VIOLATED — proving the
   scene reachable; repository NoReach* convention)
   ========================================================================= *)

(* R1: the wake-before-save transient — the reason this suite exists. *)
NoReachWakeBeforeSave ==
    ~ ( /\ fiberState = "Runnable"
       /\ ticketLocation = "VictimLocal"
       /\ switchPhase = "InFlight"
       /\ suspendPending )

(* R2: a thief actually attempted the steal during the window and was refused. *)
NoReachStealRefusal ==
    ~ sawStealRefusal

(* R3: the safe post-save migration — save done, authority cleared, ticket
   stolen, thief resumed. This is the legitimate path the guard protects. *)
NoReachSafePostSaveMigration ==
    ~ ( /\ fiberState = "Resumed"
       /\ switchPhase = "Saved"
       /\ ~ suspendPending
       /\ ticketLocation = "None" )
=====
