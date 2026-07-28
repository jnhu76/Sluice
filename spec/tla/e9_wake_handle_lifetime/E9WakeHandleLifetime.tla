--------------------- MODULE E9WakeHandleLifetime ---------------------
(*
  E9-LIFETIME-CORRECTIVE — SchedulerWakeHandle callback-lifetime model.

  This is a NARROW lifetime model. It models exactly one thing:

    May a SchedulerWakeHandle::notify() callback dereference the Scheduler
    AFTER the Scheduler destructor has destroyed its wake members?

  Production defect at HEAD 1e8333d (E9-CORRECTIVE review): notify()
  acquires Control::mtx, snapshots (alive, scheduler), RELEASES the lock,
  and then calls scheduler->notify_external_wake() using the captured
  pointer. The destructor acquires the same lock, sets alive=false /
  scheduler=nullptr, releases, and continues to destroy Scheduler wake
  members. The interleaving

      N: acquire, validate alive, snapshot scheduler, RELEASE
      D: acquire, invalidate, RELEASE, DestroyMembers
      N: enter callback using the stale snapshot

  is legal: a callback runs against destroyed members. `alive` closes the
  SNAPSHOT race; it does NOT lease the callback target.

  LOCKED REPAIR: hold Control::mtx from the validity check through the
  entire Scheduler wake callback. The destructor must acquire the same
  mutex before invalidating. Therefore the two legal linearizations are:

      Notify wins:    N holds the lease through the callback; D's acquire
                      BLOCKS until the callback returns; invalidation and
                      member destruction happen strictly AFTER.
      Destructor wins: D invalidates and releases; N acquires, observes
                      dead/null, returns false — no Scheduler dereference.

  This model is the CORRECT callback-lease protocol. The buggy
  snapshot-release variant is E9WakeHandleLifetimeBuggySnapshot.tla, which
  differs ONLY in "lease released before callback."

  THIS MODEL DOES NOT REOPEN:
    - E9ParkWake (park admission, RunMode, MW classifier) — CLOSED.
    - The notify() external signal-only contract (9.4.9) — preserved.
    - signal_wake_locked / notify_external_wake semantics — unchanged.

  The Scheduler wake callback body is abstracted as one atomic lease-held
  step (NotifyCallbackReturn) whose precondition is exactly "Notifier owns
  the lease AND schedulerState = Alive." The lease mutex is modeled
  explicitly via controlOwner, so validation and callback are NOT collapsed
  into one unstructured atomic action — the ownership / lease state is a
  first-class variable.
*)
EXTENDS Naturals, TLC

(*
  State axes (spec 6):

    schedulerState in {Alive, Invalidated, Destroyed}
        Alive        = Control::alive && scheduler != nullptr (issued +
                       not-yet-invalidated Scheduler)
        Invalidated  = destructor has stored alive=false / scheduler=nullptr
                       but Scheduler wake members are still constructed
                       (destructor body still holds / has released the lock)
        Destroyed    = Scheduler wake members are past their lifetime
                       (destructor body returned; members destroyed)

    notifyPhase in {Idle, Validating, Callback, ReturnedTrue, ReturnedFalse}
        Idle          = no notify in flight
        Validating    = Notifier holds the lease, checking alive/scheduler
        Callback      = Notifier holds the lease, inside the Scheduler
                        wake callback (notify_external_wake)
        ReturnedTrue  = a notify delivered a wake and fully returned
        ReturnedFalse = a notify observed dead/null and returned false

    controlOwner in {None, Notifier, Destructor}
        Who currently holds Control::mtx (the callback lease).

    destroyRequested (bool)
        True once the owning thread has committed to destroying the
        Scheduler. Distinguishes "no destructor will ever run" from
        "destructor is in flight" for the liveness property LIFE-Inv7.
*)

VARIABLES
    schedulerState,
    notifyPhase,
    controlOwner,
    destroyRequested

StateVal == {"Alive", "Invalidated", "Destroyed"}
PhaseVal == {"Idle", "Validating", "Callback", "ReturnedTrue", "ReturnedFalse"}
OwnerVal == {"None", "Notifier", "Destructor"}

vars == <<schedulerState, notifyPhase, controlOwner, destroyRequested>>

(* =====================================================================
   Notifier actions (spec 7). The lease (controlOwner = Notifier) is held
   ACROSS validation and the callback; only NotifyRelease drops it.
   ===================================================================== *)

(* NotifyAcquire: notify() locks Control::mtx. Requires the mutex free. *)
NotifyAcquire ==
    /\ notifyPhase = "Idle"
    /\ controlOwner = "None"
    /\ controlOwner' = "Notifier"
    /\ notifyPhase' = "Validating"
    /\ UNCHANGED <<schedulerState, destroyRequested>>

(* NotifyRejectDead: a validated snapshot that is dead/null short-circuits
   to ReturnedFalse WITHOUT ever entering Callback. This is the destructor-
   wins linearization: the lease is acquired but the callback is never
   entered because alive=false / scheduler=nullptr. *)
NotifyRejectDead ==
    /\ notifyPhase = "Validating"
    /\ controlOwner = "Notifier"
    /\ schedulerState # "Alive"
    /\ notifyPhase' = "ReturnedFalse"
    /\ UNCHANGED <<schedulerState, controlOwner, destroyRequested>>

(* NotifyBeginCallback: validation passed (schedulerState = Alive) while
   the Notifier holds the lease; enter the Scheduler wake callback. *)
NotifyBeginCallback ==
    /\ notifyPhase = "Validating"
    /\ controlOwner = "Notifier"
    /\ schedulerState = "Alive"
    /\ notifyPhase' = "Callback"
    /\ UNCHANGED <<schedulerState, controlOwner, destroyRequested>>

(* NotifyCallbackReturn: the Scheduler wake callback (notify_external_wake
   -> signal_wake_locked) completes while the lease is still held. *)
NotifyCallbackReturn ==
    /\ notifyPhase = "Callback"
    /\ controlOwner = "Notifier"
    /\ notifyPhase' = "ReturnedTrue"
    /\ UNCHANGED <<schedulerState, controlOwner, destroyRequested>>

(* NotifyRelease: notify() unlocks Control::mtx; the callback has fully
   returned. Transitions back to Idle so repeated notifies are modelable. *)
NotifyRelease ==
    /\ notifyPhase \in {"ReturnedTrue", "ReturnedFalse"}
    /\ controlOwner = "Notifier"
    /\ controlOwner' = "None"
    /\ notifyPhase' = "Idle"
    /\ UNCHANGED <<schedulerState, destroyRequested>>

(* =====================================================================
   Destructor actions (spec 7).
   ===================================================================== *)

(* DestructorAcquire: ~Scheduler locks Control::mtx. Requires it free. *)
DestructorAcquire ==
    /\ controlOwner = "None"
    /\ schedulerState = "Alive"
    /\ destroyRequested' = TRUE
    /\ controlOwner' = "Destructor"
    /\ UNCHANGED <<schedulerState, notifyPhase>>

(* DestructorInvalidate: store alive=false / scheduler=nullptr while the
   destructor holds the lease. Scheduler wake members are still
   constructed here — the callback is only forbidden by ownership. *)
DestructorInvalidate ==
    /\ controlOwner = "Destructor"
    /\ schedulerState = "Alive"
    /\ schedulerState' = "Invalidated"
    /\ UNCHANGED <<notifyPhase, controlOwner, destroyRequested>>

(* DestructorRelease: ~Scheduler unlocks Control::mtx after invalidation.
   A concurrent notify may now acquire the lease; it will observe
   Invalidated and short-circuit to ReturnedFalse. *)
DestructorRelease ==
    /\ controlOwner = "Destructor"
    /\ schedulerState = "Invalidated"
    /\ controlOwner' = "None"
    /\ UNCHANGED <<schedulerState, notifyPhase, destroyRequested>>

(* DestroyMembers: the destructor body has returned; Scheduler wake members
   (wake_mtx_, wake_epoch_, wake_cv_) are destroyed. Requires the lease is
   free (no Notifier holds it) — guaranteed because DestructorAcquire is
   impossible while controlOwner = Notifier, and DestructorRelease only
   fires after invalidation. *)
DestroyMembers ==
    /\ controlOwner = "None"
    /\ schedulerState = "Invalidated"
    /\ schedulerState' = "Destroyed"
    /\ UNCHANGED <<notifyPhase, controlOwner, destroyRequested>>

(* =====================================================================
   Next / Init / Spec.
   ===================================================================== *)

Next ==
    \/ NotifyAcquire
    \/ NotifyRejectDead
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

Spec == Init /\ [][Next]_vars

(* =====================================================================
   Formal properties (spec 8).

   LIFE-Inv1 — Callback requires live Scheduler.
   LIFE-Inv2 — Callback owns the lease.
   LIFE-Inv3 — Destructor cannot be mid-acquire during a callback (the
               destructor does not hold the lease while a callback runs).
   LIFE-Inv4 — Destroyed Scheduler has no active callback.
   LIFE-Inv5 — post-invalidation notify terminates ReturnedFalse without
               entering Callback.
   ===================================================================== *)

LifeInv1CallbackRequiresAlive ==
    notifyPhase = "Callback" => schedulerState = "Alive"

LifeInv2CallbackOwnsLease ==
    notifyPhase = "Callback" => controlOwner = "Notifier"

LifeInv3NoDestructorDuringCallback ==
    notifyPhase = "Callback" => controlOwner # "Destructor"

LifeInv4DestroyedNoCallback ==
    schedulerState = "Destroyed" => notifyPhase # "Callback"

(* LIFE-Inv5 — a notify that validates AFTER invalidation must reach
   ReturnedFalse without entering Callback. Stated as a state invariant:
   if the scheduler is not Alive at validate time, the next phase is
   ReturnedFalse (NotifyRejectDead is the only enabled validation exit
   when schedulerState # "Alive"). Expressed directly: once
   schedulerState # "Alive", no state has notifyPhase = "Callback" reachable
   from a fresh validate. The invariant form below forbids any Callback
   phase once invalidation has occurred. *)
LifeInv5PostInvalidationNoCallback ==
    schedulerState # "Alive" => notifyPhase # "Callback"

(* LIFE-Inv6 — a successful notify callback completes before destruction.
   The precise safety claim is: a callback that began while Alive returns
   (reaches ReturnedTrue) BEFORE the Scheduler reaches Destroyed. Because
   the lease (controlOwner = Notifier) blocks DestructorAcquire, and
   DestroyMembers requires controlOwner = None, a Callback phase can only
   transition forward to ReturnedTrue; the destructor cannot progress past
   Invalidated while the lease is held. Stated as a state invariant that
   captures the binding between callback completion and destruction order:
   once Destroyed, no callback is in flight (LIFE-Inv4) AND a successful
   callback cannot be mid-callback. The temporal binding is LIFE-Inv7.

   A late notify may legitimately begin Validating after destruction
   (production: a post-destruction notify() acquires Control::mtx on the
   still-live shared control block, sees alive=false, returns false). That
   path never enters Callback — captured by LIFE-Inv5. So the invariant
   forbids only Callback-while-Destroyed (already LIFE-Inv4) and is stated
   here as the callback-vs-destruction binding for traceability. *)
LifeInv6CallbackReleasesBeforeDestruction ==
    notifyPhase = "Callback" => schedulerState = "Alive"

TypeOK ==
    /\ schedulerState \in StateVal
    /\ notifyPhase \in PhaseVal
    /\ controlOwner \in OwnerVal
    /\ destroyRequested \in BOOLEAN

Inv ==
    /\ TypeOK
    /\ LifeInv1CallbackRequiresAlive
    /\ LifeInv2CallbackOwnsLease
    /\ LifeInv3NoDestructorDuringCallback
    /\ LifeInv4DestroyedNoCallback
    /\ LifeInv5PostInvalidationNoCallback
    /\ LifeInv6CallbackReleasesBeforeDestruction

(* =====================================================================
   Liveness (spec 8, LIFE-Inv7).

   Under legitimate fairness of the destructor's mutex acquisition: once
   a callback has returned (notifyPhase = ReturnedTrue) AND destruction has
   been requested, the Scheduler eventually reaches Destroyed. We do NOT
   assume global WF(Next).

   Minimal fairness assumptions (mutex-semantics obligations, NOT producer
   fairness):
     - WF on every Notifier step that is enabled while the Notifier HOLDS
       the lease (Validate->RejectDead/BeginCallback, Callback->Return,
       Returned->Release): a thread that holds a mutex always eventually
       releases it. This is the load-bearing assumption — without it a
       holder could sit on the lease forever, blocking destruction.
     - SF on the destructor chain (Acquire/Invalidate/Release/
       DestroyMembers): spec 8 phrases the obligation as "legitimate
       fairness of the destructor's mutex acquisition." A spinning
       Notifier makes DestructorAcquire intermittently enabled, so weak
       fairness admits a starvation cycle; strong fairness ("if enabled
       infinitely often, eventually occurs") is the correct obligation.
       We do NOT assume producer fairness (an infinite stream of distinct
       notify() calls); SF only guarantees progression when the lease
       becomes free infinitely often.

   We deliberately do NOT assume fairness on NotifyAcquire itself: a
   Notifier that NEVER calls notify() does not block destruction (the
   lease is free and the destructor proceeds under SF). The only Notifier
   steps made fair are the ones fired AFTER the lease is acquired.
   ===================================================================== *)

FairNotifyRejectDead ==
    WF_vars(NotifyRejectDead)

FairNotifyBeginCallback ==
    WF_vars(NotifyBeginCallback)

FairNotifyCallbackReturn ==
    WF_vars(NotifyCallbackReturn)

FairNotifyReleaseAfterCallback ==
    WF_vars(NotifyRelease)

FairDestructorAcquire ==
    SF_vars(DestructorAcquire)

FairDestructorInvalidate ==
    SF_vars(DestructorInvalidate)

FairDestructorRelease ==
    SF_vars(DestructorRelease)

FairDestroyMembers ==
    SF_vars(DestroyMembers)

LivenessSpec ==
    Spec
    /\ FairNotifyRejectDead
    /\ FairNotifyBeginCallback
    /\ FairNotifyCallbackReturn
    /\ FairNotifyReleaseAfterCallback
    /\ FairDestructorAcquire
    /\ FairDestructorInvalidate
    /\ FairDestructorRelease
    /\ FairDestroyMembers(* LIFE-Inv7 — after a callback returns AND destruction is requested, the
   Scheduler eventually reaches Destroyed. *)
Life7DestroyedAfterCallbackAndRequest ==
    [] ( (notifyPhase = "ReturnedTrue" /\ destroyRequested)
          => <> (schedulerState = "Destroyed") )

(* LIFE-Inv7b — symmetric: once destruction is requested, the Scheduler
   eventually reaches Destroyed (destructor progresses through invalidation
   and member destruction once it can acquire the lease). This is the
   "destructor eventually progresses after callback return" obligation
   independent of which event (callback return vs request) happened first. *)
Life7bDestroyProgressesAfterRequest ==
    [] ( destroyRequested => <> (schedulerState = "Destroyed") )

LifeProps ==
    /\ Life7DestroyedAfterCallbackAndRequest
    /\ Life7bDestroyProgressesAfterRequest

=============================================================================
