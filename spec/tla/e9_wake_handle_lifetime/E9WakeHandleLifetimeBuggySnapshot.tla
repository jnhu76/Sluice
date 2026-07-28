----------------- MODULE E9WakeHandleLifetimeBuggySnapshot -----------------
(*
  E9-LIFETIME-CORRECTIVE — NEGATIVE model: the shipped snapshot-release
  defect (HEAD 1e8333d).

  This model differs from E9WakeHandleLifetime.tla in EXACTLY ONE way:

    notify() RELEASES Control::mtx after the validation snapshot, BEFORE
    entering the Scheduler wake callback.

  The buggy NotifyAcquire+NotifyValidateRelease step therefore models:

      lock Control::mtx
      read alive (true) + scheduler (valid)   -- snapshot
      UNLOCK Control::mtx                     -- DEFECT: lease dropped

  and a SEPARATE NotifyBeginCallback action later dereferences the captured
  Scheduler pointer with NO lease held. In the gap the destructor can
  invalidate + destroy members; the callback then runs against destroyed
  Scheduler wake members -> use-after-free.

  The ONLY introduced defect is "lease released before callback." We do
  NOT additionally drop the alive check, skip invalidation, double-destroy,
  or bend mutex semantics to manufacture the counterexample (spec 9).

  Required counterexample: a reachable state with

      schedulerState = "Destroyed" /\ notifyPhase = "Callback"

  violating LIFE-Inv4 (callback-after-destruction). This reproduces the
  exact shipped defect: snapshot, release lease, invalidate, destroy,
  callback.
*)
EXTENDS Naturals, TLC

VARIABLES
    schedulerState,
    notifyPhase,
    controlOwner,
    destroyRequested,
    buggySnapshotValid

StateVal == {"Alive", "Invalidated", "Destroyed"}
PhaseVal == {"Idle", "Validating", "SnapshotReleased", "Callback",
             "ReturnedTrue", "ReturnedFalse"}
OwnerVal == {"None", "Notifier", "Destructor"}

vars == <<schedulerState, notifyPhase, controlOwner, destroyRequested,
          buggySnapshotValid>>

(* =====================================================================
   Notifier actions — DEFECTIVE: lease released before callback.
   ===================================================================== *)

(* NotifyAcquire: notify() locks Control::mtx. *)
NotifyAcquire ==
    /\ notifyPhase = "Idle"
    /\ controlOwner = "None"
    /\ controlOwner' = "Notifier"
    /\ notifyPhase' = "Validating"
    /\ UNCHANGED <<schedulerState, destroyRequested, buggySnapshotValid>>

(* NotifyRejectDead: validated snapshot is dead/null -> ReturnedFalse. *)
NotifyRejectDead ==
    /\ notifyPhase = "Validating"
    /\ controlOwner = "Notifier"
    /\ schedulerState # "Alive"
    /\ notifyPhase' = "ReturnedFalse"
    /\ UNCHANGED <<schedulerState, controlOwner, destroyRequested, buggySnapshotValid>>

(* BUGGY NotifyValidateRelease: validation passed (Alive) and the snapshot
   is captured, then Control::mtx is RELEASED. The captured pointer is
   kept in buggySnapshotValid. The callback will run with NO lease. *)
NotifyValidateRelease ==
    /\ notifyPhase = "Validating"
    /\ controlOwner = "Notifier"
    /\ schedulerState = "Alive"
    /\ controlOwner' = "None"
    /\ notifyPhase' = "SnapshotReleased"
    /\ buggySnapshotValid' = TRUE
    /\ UNCHANGED <<schedulerState, destroyRequested>>

(* BUGGY NotifyBeginCallback: enter the Scheduler wake callback using the
   captured snapshot. controlOwner is "None" (or whatever the destructor
   left it as) — the lease is NOT held. This is the use-after-free window. *)
NotifyBeginCallback ==
    /\ notifyPhase = "SnapshotReleased"
    /\ buggySnapshotValid
    /\ notifyPhase' = "Callback"
    /\ UNCHANGED <<schedulerState, controlOwner, destroyRequested, buggySnapshotValid>>

NotifyCallbackReturn ==
    /\ notifyPhase = "Callback"
    /\ notifyPhase' = "ReturnedTrue"
    /\ UNCHANGED <<schedulerState, controlOwner, destroyRequested, buggySnapshotValid>>

(* NotifyRelease: not strictly needed (the buggy path released the lease
   early), but kept so the model can return to Idle for repeated traces. *)
NotifyRelease ==
    /\ notifyPhase \in {"ReturnedTrue", "ReturnedFalse"}
    /\ notifyPhase' = "Idle"
    /\ controlOwner' = "None"
    /\ buggySnapshotValid' = FALSE
    /\ UNCHANGED <<schedulerState, destroyRequested>>

(* =====================================================================
   Destructor actions — IDENTICAL to the correct model.
   ===================================================================== *)

DestructorAcquire ==
    /\ controlOwner = "None"
    /\ schedulerState = "Alive"
    /\ destroyRequested' = TRUE
    /\ controlOwner' = "Destructor"
    /\ UNCHANGED <<schedulerState, notifyPhase, buggySnapshotValid>>

DestructorInvalidate ==
    /\ controlOwner = "Destructor"
    /\ schedulerState = "Alive"
    /\ schedulerState' = "Invalidated"
    /\ UNCHANGED <<notifyPhase, controlOwner, destroyRequested, buggySnapshotValid>>

DestructorRelease ==
    /\ controlOwner = "Destructor"
    /\ schedulerState = "Invalidated"
    /\ controlOwner' = "None"
    /\ UNCHANGED <<schedulerState, notifyPhase, destroyRequested, buggySnapshotValid>>

DestroyMembers ==
    /\ controlOwner = "None"
    /\ schedulerState = "Invalidated"
    /\ schedulerState' = "Destroyed"
    /\ UNCHANGED <<notifyPhase, controlOwner, destroyRequested, buggySnapshotValid>>

(* =====================================================================
   Next / Init / Spec.
   ===================================================================== *)

Next ==
    \/ NotifyAcquire
    \/ NotifyRejectDead
    \/ NotifyValidateRelease
    \/ NotifyBeginCallback
    \/ NotifyCallbackReturn
    \/ NotifyRelease
    \/ DestructorAcquire
    \/ DestructorInvalidate
    \/ DestructorRelease
    \/ DestroyMembers

Init ==
    /\ schedulerState = "Alive"
    /\ notifyPhase = "Idle"
    /\ controlOwner = "None"
    /\ destroyRequested = FALSE
    /\ buggySnapshotValid = FALSE

Spec == Init /\ [][Next]_vars

(* =====================================================================
   Properties.
   ===================================================================== *)

TypeOK ==
    /\ schedulerState \in StateVal
    /\ notifyPhase \in PhaseVal
    /\ controlOwner \in OwnerVal
    /\ destroyRequested \in BOOLEAN
    /\ buggySnapshotValid \in BOOLEAN

(* LIFE-Inv1 — Callback requires live Scheduler. The BUGGY model VIOLATES
   this: a callback can dereference a Destroyed scheduler. *)
LifeInv1CallbackRequiresAlive ==
    notifyPhase = "Callback" => schedulerState = "Alive"

(* LIFE-Inv2 — Callback owns the lease. The BUGGY model VIOLATES this:
   the lease is released before the callback, so controlOwner is NOT
   "Notifier" during Callback. *)
LifeInv2CallbackOwnsLease ==
    notifyPhase = "Callback" => controlOwner = "Notifier"

(* LIFE-Inv4 — Destroyed Scheduler has no active callback. The BUGGY model
   VIOLATES this: the use-after-free state is exactly
   schedulerState = Destroyed /\ notifyPhase = Callback. *)
LifeInv4DestroyedNoCallback ==
    schedulerState = "Destroyed" => notifyPhase # "Callback"

LifeInv5PostInvalidationNoCallback ==
    schedulerState # "Alive" => notifyPhase # "Callback"

LifeInv6CallbackReleasesBeforeDestruction ==
    schedulerState = "Destroyed" => notifyPhase \in {"Idle", "ReturnedTrue", "ReturnedFalse"}

Inv ==
    /\ TypeOK
    /\ LifeInv1CallbackRequiresAlive
    /\ LifeInv2CallbackOwnsLease
    /\ LifeInv4DestroyedNoCallback
    /\ LifeInv5PostInvalidationNoCallback
    /\ LifeInv6CallbackReleasesBeforeDestruction

=============================================================================
