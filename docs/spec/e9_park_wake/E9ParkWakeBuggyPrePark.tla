----------------------- MODULE E9ParkWakeBuggyPrePark -----------------------
(*
  E9 Scheduler park-admission and unified wake-source protocol
  (sluice-CORE-E9), Model P3 (decoupled wake domains).

  THE LOAD-BEARING E9 QUESTION (ADR 9.4):

    When may an idle Scheduler Worker commit to parking, and which state
    publications create an obligation to wake parked Workers?

  ANSWER (P3, decoupled wake domains):

    - A Worker commits to parking only after a globally-coordinated
      admission: drain persistent readiness, reclassify, OBSERVE the wake
      epoch, and validate the epoch before sleeping. The wake epoch is the
      authority for "a wake-relevant publication happened after I decided
      to park"; the cv/notify is the physical delivery.
    - There are TWO park domains: BACKEND (ctx_.wait_one, at most one
      participant, the E7 MW-S2 rule) and SCHEDULER (wake_cv + wake
      epoch, any number of Workers). A Worker chooses the SCHEDULER
      domain whenever an external-wake-capable wait is registered (this
      is the MIXED-WAKE fix). The BACKEND domain is used only when no
      external-wake-capable wait is registered.
    - Every wake-relevant producer publishes persistent state FIRST and
      signals (advances wakeEpoch + notifies) SECOND. The signal is
      advisory; persistent state is authoritative.

  DOMAIN (finite, exhaustive TLC):
    Workers = {W0, W1}, Fibers = {F0}. (F1 not needed; one waiter is the
    load-bearing proof. Two Workers exercise the multi-parked state and
    the at-most-one-backend-participant rule.)

  STATE AXES (E9 spec 10):
    runnableVisible, runningVisible     (global executable work)
    backendOutstanding, backendReady    (backend progress)
    externalWaitRegistered, externalReady  (external Future source)
    wakeEpoch                           (monotonic; authority)
    workerPhase[w] in {Active, ParkCandidate, ParkCommitted, Parked}
    observedEpoch[w]                    (epoch at commit)
    backendWaitParticipant              (in {NONE, W0, W1})

  Persistent state (runnable/running/backend/external ready) is kept
  SEPARATE from the wake signal/epoch. The wake notification is NOT the
  source of truth.

  This model abstracts away Fiber identity beyond F0 (the external-wait
  Fiber) and collapses runnable/running into booleans. The E7/E8
  publication/steal protocols are CLOSED; E9 does not reopen them.
*)
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Workers, Fibers, W0, W1, F0, NONE

VARIABLES
    runnableVisible,
    runningVisible,
    backendOutstanding,
    backendReady,
    externalWaitRegistered,
    externalReady,
    wakeEpoch,
    workerPhase,
    observedEpoch,
    backendWaitParticipant

PhaseVal == {"Active", "ParkCandidate", "ParkCommitted", "Parked"}
PartVal  == {NONE, W0, W1}

ASSUME
    /\ Workers = {W0, W1}
    /\ Fibers  = {F0}
    /\ W0 \in Workers
    /\ W1 \in Workers
    /\ NONE \notin Workers
    /\ W0 # W1

(* =========================================================================
   Derived predicates
   ========================================================================= *)

ExecutableWork ==
    runnableVisible \/ runningVisible

SomeBackendWork ==
    backendOutstanding \/ backendReady

(* A wait registered in waiting_ready_ whose flag may be set by an
   external thread. In the E9 baseline every external-wait registration
   is external-wake-capable. *)
ExternalWakePossible ==
    externalWaitRegistered

(* The MW-S2 state: no executable work, backend progress possible. *)
MWS2 ==
    ~ExecutableWork /\ SomeBackendWork

(* The MIXED-WAKE state: MW-S2 AND an external-wake-capable wait is
   registered. This is the state E9 must not let strand external ready
   work behind a backend-only wait. *)
MixedWake ==
    MWS2 /\ ExternalWakePossible

(* Latent external executable work: a registered external-wait whose flag
   is ALREADY ready but not yet drained into runnable. This is the E7
   9.2.6 "latent executable work" concept carried into E9 park admission:
   the cv.wait predicate AND the admission preconditions treat this as
   "predicate true" so a Worker never commits to park while a registered
   ready wait is un-drained. *)
LatentExternalWork ==
    externalReady /\ externalWaitRegistered

(* Any Worker in a phase at or past ParkCandidate. *)
ParkedOrCommitting(w) ==
    workerPhase[w] \in {"ParkCandidate", "ParkCommitted", "Parked"}

AnyParked ==
    \E w \in Workers : workerPhase[w] = "Parked"

AnyCommittedOrParked ==
    \E w \in Workers : workerPhase[w] \in {"ParkCommitted", "Parked"}

(* =========================================================================
   Producer-side actions: PUBLISH PERSISTENT STATE, then SIGNAL.
   The signal advances the wake epoch. Persistent state is authoritative.
   ========================================================================= *)

(* W8: external thread completes a Future. Publishes externalReady, then
   signals the wake source. The producer does NOT route, make_runnable,
   or touch any Scheduler queue (9.4.9). *)
\* BUGGY (lost wake): the external producer publishes the persistent
\* ready flag but does NOT advance the wake epoch (no notification). The
\* signal that would wake a parked Worker is lost. (E9 spec 13.)
ExternalReadyPublish ==
    /\ ~externalReady
    /\ externalReady' = TRUE
    /\ UNCHANGED <<runnableVisible, runningVisible,
                   backendOutstanding, backendReady,
                   externalWaitRegistered,
                   wakeEpoch, workerPhase, observedEpoch,
                   backendWaitParticipant>>

(* A Scheduler Worker observes externalReady and routes the waiting Fiber
   to runnable (DrainExternalReady). This is a Scheduler-domain action:
   the external producer NEVER does this. *)
DrainExternalReady ==
    /\ externalReady
    /\ externalWaitRegistered
    /\ externalReady' = FALSE
    /\ externalWaitRegistered' = FALSE
    /\ runnableVisible' = TRUE
    /\ UNCHANGED <<runningVisible, backendOutstanding, backendReady,
                   wakeEpoch, workerPhase, observedEpoch,
                   backendWaitParticipant>>

(* A backend op becomes ready (W5/W6 reified: a ThreadPool worker pushes
   a result, or a CQE lands). Persistent state first; the MW-S2 BACKEND
   participant observes it via wait_one return (BackendWaitReturn). No
   wake-epoch signal is needed for the BACKEND participant (it is in the
   backend domain). *)
BackendReadyPublish ==
    /\ backendOutstanding
    /\ ~backendReady
    /\ backendReady' = TRUE
    /\ UNCHANGED <<runnableVisible, runningVisible,
                   backendOutstanding, externalWaitRegistered,
                   externalReady, wakeEpoch,
                   workerPhase, observedEpoch, backendWaitParticipant>>

(* A running Fiber submits a backend op (e.g. ctx.submit_read). Marks the op
   outstanding; backendOutstanding=TRUE makes the state MW-S2-eligible once
   the Fiber suspends. The Completion this op will fill is implicit (a
   registered backend wait is implied by backendOutstanding). *)
SubmitBackend ==
    /\ runningVisible
    /\ ~backendOutstanding
    /\ backendOutstanding' = TRUE
    /\ UNCHANGED <<runnableVisible, runningVisible, backendReady,
                   externalWaitRegistered, externalReady,
                   wakeEpoch, workerPhase, observedEpoch,
                   backendWaitParticipant>>

(* A Scheduler Worker drains backendReady into runnable (a Completion
   waiter becomes runnable). *)
DrainBackendReady ==
    /\ backendReady
    /\ backendReady' = FALSE
    /\ backendOutstanding' = FALSE
    /\ runnableVisible' = TRUE
    /\ UNCHANGED <<runningVisible, externalWaitRegistered, externalReady,
                   wakeEpoch, workerPhase, observedEpoch,
                   backendWaitParticipant>>

(* A Fiber publishes runnable work from inside a Worker (W1/W2/W3). This
   advances the wake epoch AND routes (the route is abstracted here as
   runnableVisible' = TRUE). route_runnable_locked in production also
   notifies inbox_cv; E9 adds the wake-epoch path. *)
PublishRunnable ==
    /\ ~runnableVisible
    /\ runnableVisible' = TRUE
    /\ wakeEpoch' = 1 - wakeEpoch
    /\ UNCHANGED <<runningVisible, backendOutstanding, backendReady,
                   externalWaitRegistered, externalReady,
                   workerPhase, observedEpoch, backendWaitParticipant>>

(* =========================================================================
   Park-admission actions (Worker side). Globally coordinated under the
   wake mutex (abstracted as atomic transitions).
   ========================================================================= *)

(* BeginParkCandidate: a Worker with no local work elects itself a
   candidate. PRECONDITION: no executable work AND no latent external-
   ready work (a registered ready wait is latent executable work, E7
   9.2.6; it must be drained -- DrainExternalReady -- before parking is
   even considered). The production worker_loop runs wake_ready_*_locked
   before classify; this precondition mirrors that drain obligation. *)
BeginParkCandidate(w) ==
    /\ workerPhase[w] = "Active"
    /\ ~ExecutableWork
    /\ ~LatentExternalWork
    /\ workerPhase' = [workerPhase EXCEPT ![w] = "ParkCandidate"]
    /\ UNCHANGED <<runnableVisible, runningVisible,
                   backendOutstanding, backendReady,
                   externalWaitRegistered, externalReady,
                   wakeEpoch, observedEpoch, backendWaitParticipant>>

(* FinalParkRecheckAndCommit: the candidate does a final drain + classify,
   OBSERVES the wake epoch, and COMMITS to park (recording observedEpoch).
   PRECONDITION (final recheck): still no executable work AND still no
   latent external-ready work. A publish between CANDIDATE and COMMIT that
   set externalReady (or routed runnable, raising runnableVisible) cancels
   admission: the precondition is simply false, so the action is not
   enabled and the candidate cannot commit. The epoch validation at
   EnterPhysicalPark closes the commit-to-sleep window. *)
FinalParkRecheckAndCommit(w) ==
    /\ workerPhase[w] = "ParkCandidate"
    /\ ~ExecutableWork
    /\ ~LatentExternalWork
    /\ observedEpoch' = [observedEpoch EXCEPT ![w] = wakeEpoch]
    /\ workerPhase' = [workerPhase EXCEPT ![w] = "ParkCommitted"]
    /\ UNCHANGED <<runnableVisible, runningVisible,
                   backendOutstanding, backendReady,
                   externalWaitRegistered, externalReady,
                   wakeEpoch, backendWaitParticipant>>

(* EnterPhysicalPark: the committed Worker releases the wake mutex and
   parks on its chosen domain. Domain selection (P3):
     - if no other Worker is the backend participant AND the state is
       MWS2 AND NOT ExternalWakePossible: BACKEND domain (becomes the
       backend participant).
     - else: SCHEDULER domain.

   FAITHFUL cv.wait SEMANTICS: the physical park is a condition_variable
   wait with a predicate. The Worker actually blocks (transitions to
   Parked) ONLY IF the predicate is currently false (no pending wake, no
   ready work). If a publication advanced the epoch / set ready state
   between COMMIT and this wait, the predicate is already true and the
   wait returns immediately -- the Worker goes straight back to Active
   without ever being Parked. This is the publish-after-commit-before-
   sleep closure (E9-Inv2 / spec 8.3 case c).

   LATENT-WORK CLOSURE (E7 9.2.6 carried into E9): a REGISTERED wait
   whose source is already ready is latent executable work. The cv.wait
   predicate MUST treat (externalReady /\ externalWaitRegistered) as
   "predicate true" -- the Worker does not park; it returns to Active and
   the next drain (DrainExternalReady) routes the waiting Fiber. Without
   this, the model (and production) could park while a registered ready
   flag is already set -- the load-bearing lost-wake hazard. *)
EnterPhysicalPark(w) ==
    /\ workerPhase[w] = "ParkCommitted"
    /\ IF /\ wakeEpoch = observedEpoch[w]   \* no pending SCHEDULER wake
          /\ ~backendReady                  \* no backend-ready signal
          /\ ~LatentExternalWork            \* no registered external-ready wait
          /\ ~ExecutableWork                \* no executable work appeared
       THEN
          \* Predicate false -> actually block. Choose the park domain.
          /\ IF /\ MWS2
                /\ ~ExternalWakePossible
                /\ backendWaitParticipant = NONE
             THEN /\ backendWaitParticipant' = w
                  /\ workerPhase' = [workerPhase EXCEPT ![w] = "Parked"]
             ELSE /\ backendWaitParticipant' = backendWaitParticipant
                  /\ workerPhase' = [workerPhase EXCEPT ![w] = "Parked"]
       ELSE
          \* Predicate already true -> did not park; return to Active to
          \* re-drain. (This is the pre-wait notification race closed by
          \* the epoch predicate under the wake mutex.)
          /\ workerPhase' = [workerPhase EXCEPT ![w] = "Active"]
          /\ backendWaitParticipant' = backendWaitParticipant
    /\ UNCHANGED <<runnableVisible, runningVisible,
                   backendOutstanding, backendReady,
                   externalWaitRegistered, externalReady,
                   wakeEpoch, observedEpoch>>

(* LeavePark: a parked Worker is woken (wake source depends on domain)
   and returns to Active to re-drain. The wake is ADVISORY; the Worker
   re-drains persistent state regardless. Enabled by an epoch advance for
   SCHEDULER-parked Workers, or by backendReady for the BACKEND
   participant. Spurious wake is also allowed (the predicate is loose). *)
\* BUGGY: LeavePark wakes ONLY on a wake-source signal (epoch advance or
\* backendReady) -- it does NOT re-scan persistent state. Combined with
\* the lost-signal defect above, a parked Worker whose external-ready
\* publication dropped the signal can NEVER leave park: persistent wake
\* is due (Inv2 requires it be able to leave) but the signal-only wake
\* path is dead.
LeavePark(w) ==
    /\ workerPhase[w] = "Parked"
    /\ (\/ wakeEpoch # observedEpoch[w]
        \/ backendReady)
    /\ IF backendWaitParticipant = w
       THEN /\ backendWaitParticipant' = NONE
       ELSE /\ backendWaitParticipant' = backendWaitParticipant
    /\ workerPhase' = [workerPhase EXCEPT ![w] = "Active"]
    /\ UNCHANGED <<runnableVisible, runningVisible,
                   backendOutstanding, backendReady,
                   externalWaitRegistered, externalReady,
                   wakeEpoch, observedEpoch>>

(* EnterBackendWait / BackendWaitReturn: the BACKEND participant calls
   ctx_.wait_one and returns. Abstracted as: enter is folded into
   EnterPhysicalPark (domain selection); return is LeavePark when
   backendReady. No separate action needed; the domain is encoded in
   backendWaitParticipant. *)

(* =========================================================================
   Fiber lifecycle (collapsed). These keep runnable/running consistent.
   ========================================================================= *)

(* A runnable Fiber starts running. *)
RunFiber ==
    /\ runnableVisible
    /\ ~runningVisible
    /\ runnableVisible' = FALSE
    /\ runningVisible' = TRUE
    /\ UNCHANGED <<backendOutstanding, backendReady,
                   externalWaitRegistered, externalReady,
                   wakeEpoch, workerPhase, observedEpoch,
                   backendWaitParticipant>>

(* A running Fiber suspends on the external Future (registers the wait). *)
SuspendFiber ==
    /\ runningVisible
    /\ ~externalWaitRegistered
    /\ runningVisible' = FALSE
    /\ externalWaitRegistered' = TRUE
    /\ UNCHANGED <<runnableVisible, backendOutstanding, backendReady,
                   externalReady, wakeEpoch, workerPhase, observedEpoch,
                   backendWaitParticipant>>

(* A running Fiber finishes. *)
FinishFiber ==
    /\ runningVisible
    /\ runningVisible' = FALSE
    /\ UNCHANGED <<runnableVisible, backendOutstanding, backendReady,
                   externalWaitRegistered, externalReady,
                   wakeEpoch, workerPhase, observedEpoch,
                   backendWaitParticipant>>

(* =========================================================================
   Shutdown.
   ========================================================================= *)

(* ShutdownSignal: a coordinated termination condition. Advances the wake
   epoch and (in production) notifies wake_cv + inbox_cv, waking every
   parked Worker. Modeled as a flag via wakeEpoch odd/even is overkill;
   we model shutdown's effect on parking by allowing LeavePark on epoch
   advance. A dedicated terminal flag is not needed for the safety props. *)
ShutdownSignal ==
    /\ wakeEpoch' = 1 - wakeEpoch
    /\ UNCHANGED <<runnableVisible, runningVisible,
                   backendOutstanding, backendReady,
                   externalWaitRegistered, externalReady,
                   workerPhase, observedEpoch, backendWaitParticipant>>

(* =========================================================================
   Next, Init, Spec
   ========================================================================= *)

Next ==
    \/ ExternalReadyPublish
    \/ DrainExternalReady
    \/ SubmitBackend
    \/ BackendReadyPublish
    \/ DrainBackendReady
    \/ PublishRunnable
    \/ \E w \in Workers : BeginParkCandidate(w)
    \/ \E w \in Workers : FinalParkRecheckAndCommit(w)
    \/ \E w \in Workers : EnterPhysicalPark(w)
    \/ \E w \in Workers : LeavePark(w)
    \/ RunFiber
    \/ SuspendFiber
    \/ FinishFiber
    \/ ShutdownSignal

Init ==
    /\ runnableVisible = FALSE
    /\ runningVisible = FALSE
    /\ backendOutstanding = FALSE
    /\ backendReady = FALSE
    /\ externalWaitRegistered = FALSE
    /\ externalReady = FALSE
    /\ wakeEpoch = 0
    /\ workerPhase = [w \in Workers |-> "Active"]
    /\ observedEpoch = [w \in Workers |-> 0]
    /\ backendWaitParticipant = NONE

Spec == Init /\ [][Next]_<<runnableVisible, runningVisible,
                          backendOutstanding, backendReady,
                          externalWaitRegistered, externalReady,
                          wakeEpoch, workerPhase, observedEpoch,
                          backendWaitParticipant>>

(* Allow stuttering so terminal states are reachable and checked. *)

(* =========================================================================
   E9 safety invariants (spec 12). E9-Inv1 .. E9-Inv10.
   ========================================================================= *)

(* E9-Inv1: Park commit requires no executable work (DECISION OBLIGATION,
   not a global state invariant). The spec 12 explicitly warns: "Do not
   incorrectly assert Parked => no runnable, because work may become
   visible after a valid park." The obligation is encoded STRUCTURALLY as
   FinalParkRecheckAndCommit's precondition ~ExecutableWork: a Worker may
   only COMMIT when no executable work is visible at the commit instant.
   After commit, work may appear (publish-after-commit-before-sleep);
   EnterPhysicalPark's faithful cv.wait predicate and the wake-epoch
   protocol (Inv2) catch that. No separate state formula is asserted. *)

(* E9-Inv2: publish-before-sleep cannot be lost. The authoritative test is
   the PERSISTENT wake predicate, NOT the epoch parity. A parked Worker
   must be able to leave park whenever wake-relevant persistent state is
   true: registered external-ready, backend-ready, runnable, or running
   work. The epoch (a 1-bit toggle here) only closes the commit-to-
   physical-wait window; it is NOT the sole lost-wake authority because
   two benign publishes can flip parity back. Persistent state is the
   source of truth (E9-Inv3).

   Formally: in any reachable state where some Worker is Parked, EITHER
   its persistent wake predicate is already true OR its epoch differs
   from observed (a publication happened after commit and the Worker has
   not yet observed it). The disjunction guarantees LeavePark is enabled. *)
PersistentWakeDue ==
       (externalReady /\ externalWaitRegistered)
    \/ backendReady
    \/ runnableVisible
    \/ runningVisible

(* Inv2: if ANY wake-relevant publication is currently due, then EVERY
   parked Worker can leave park (its persistent predicate is true or its
   epoch is stale). This is the real lost-wake obligation: a publication
   must not strand a parked Worker. A parked Worker with NO publication
   due (a quiet MW-S3 / quiescent park) is legitimate and need not have a
   wake reason -- E7 9.2.9 permits parking in MW-S3; if the external
   producer never publishes, the Worker may park indefinitely. *)
\* LeavePark(w) ENABLED predicate (mirror of the action's guard).
\* Mirror of THIS model's signal-only LeavePark (the defective one).
LeaveParkEnabled(w) ==
    /\ workerPhase[w] = "Parked"
    /\ (\/ wakeEpoch # observedEpoch[w]
        \/ backendReady)

Inv2NoLostWake ==
    (\/ PersistentWakeDue)
    => (\A w \in Workers :
            workerPhase[w] = "Parked"
            => LeaveParkEnabled(w))

(* E9-Inv3: wake signal is not authority. A coalesced/spurious wake does
   not erase persistent state. Structural: LeavePark changes no persistent
   state. State-level: persistent flags are untouched by epoch changes. *)

(* E9-Inv4: external ready publication creates a wake obligation, but
   ONLY for a REGISTERED wait. Matches Inv2 for the external sub-case. *)
Inv4ExternalReadyWakes ==
    \A w \in Workers :
        (workerPhase[w] = "Parked" /\ externalReady /\ externalWaitRegistered)
        => LeaveParkEnabled(w)

(* E9-Inv5: runnable publication creates a wake obligation (coalescing ok).
   Structural: PublishRunnable advances wakeEpoch. At least one effective
   observer is made able to resume. Subsumed by Inv2 for the parked case. *)

(* E9-Inv6: at most one backend blocking participant. The constraint is
   ONLY on the BACKEND domain: at most one Worker may be the
   backendWaitParticipant. Other Workers MAY park in the SCHEDULER domain
   concurrently (P3: idle non-participants park on the wake source). So
   Inv6 does NOT forbid a second parked Worker; it forbids a second
   BACKEND participant. Since backendWaitParticipant is a single variable
   (not a set), at-most-one is structural; the invariant checks it is
   either NONE or a real Worker. *)
Inv6OneBackendParticipant ==
    /\ (backendWaitParticipant = NONE
       \/ backendWaitParticipant \in Workers)

(* E9-Inv7: MIXED-WAKE liveness authority is explicit. This is a
   TRANSITION OBLIGATION (when external wake is possible, a Worker must
   NOT ENTER the BACKEND park domain), not a global state invariant: a
   Worker that lawfully entered the BACKEND domain before any external
   wait was registered remains there, and an external wait may be
   registered later by a Fiber that ran. So the obligation is encoded
   STRUCTURALLY as EnterPhysicalPark's BACKEND-branch precondition
   ~ExternalWakePossible. The state-level consequence that does hold is
   weaker: a Worker may only BECOME backendWaitParticipant when no
   external wait was registered at park time. No global state formula
   asserts backendWaitParticipant = NONE under ExternalWakePossible. *)
Inv7MixedWakeNoBlindBackendWait ==
    TRUE

(* State form of Inv7, kept ONLY to verify the negative mixed-source model
   actually reaches the blind-backend-wait state. This is NOT a true
   invariant of the correct protocol (a Worker may lawfully enter the
   BACKEND domain before an external wait is registered, then a Fiber
   running on another Worker may register one). It is excluded from the
   correct-model cfg; the BuggyMixedSource cfg checks it to confirm the
   domain-guard defect makes the blind wait REACHABLE. *)
Inv7StateForm ==
    ExternalWakePossible => backendWaitParticipant = NONE

(* E9-Inv8: park state is not logical quiescence. Structural: there is no
   "Quiescent" state variable; classification is via ExecutableWork etc.
   A state with all Workers Parked and externalWaitRegistered is not
   terminal (LeavePark/SuspendFiber etc. remain enabled after a wake). *)

(* E9-Inv9: spurious wake is safe. Structural: LeavePark's predicate
   includes runnableVisible (a spurious wake with no new state still
   returns to Active and re-drains); no duplicate publication arises
   because DrainExternalReady/DrainBackendReady require the ready flag. *)

(* E9-Inv10: shutdown can wake parked Workers. Structural: ShutdownSignal
   advances wakeEpoch, enabling LeavePark for every parked Worker. *)

(* Combined invariant checked by TLC. Inv1 is structural (see above). *)
Inv ==
    /\ Inv2NoLostWake
    /\ Inv4ExternalReadyWakes
    /\ Inv6OneBackendParticipant
    /\ Inv7MixedWakeNoBlindBackendWait

(* =========================================================================
   Liveness (spec 15). The load-bearing temporal property: if external
   work becomes ready while a Worker is parked, the Worker eventually
   leaves park (under fairness on LeavePark). Distinguished from scheduler
   fairness and external-producer fairness.
   ========================================================================= *)

(* Environmental fairness: the external producer, once it has an
   outstanding ready publication obligation, eventually signals. This is
   WF on ExternalReadyPublish restricted to when externalReady is FALSE
   and a wake is owed. Modeled simply as WF on the action; the producer
   is assumed to eventually act. *)

(* Scheduler fairness: a woken Worker eventually leaves park. *)
FairLeavePark ==
    \A w \in Workers : WF_<<workerPhase[w]>> (LeavePark(w))

LivenessSpec == Spec /\ FairLeavePark

(* The temporal property: whenever externalReady is true and some Worker
   is parked, eventually that Worker is not parked (it woke and will
   re-drain). Stated as [] (condition => <> outcome). *)
ExternalWakeLiveness ==
    [] ( ((\E w \in Workers : workerPhase[w] = "Parked") /\ externalReady)
         => <> ((\A w \in Workers : workerPhase[w] # "Parked") /\ ~externalReady) )

(* MIXED-WAKE liveness: in the MIXED-WAKE state, if externalReady becomes
   true, the parked Worker is eventually woken (NOT blocked behind
   backend). *)
MixedWakeLiveness ==
    [] ( ((\E w \in Workers : workerPhase[w] = "Parked") /\
          externalReady /\ backendOutstanding)
         => <> (\A w \in Workers : workerPhase[w] # "Parked") )

(* =========================================================================
   Termination helper: a bounded-form check that the protocol does not
   deadlock (some action is always enabled). Stutter is allowed so TLC
   reaches terminal states, but Next must remain fair where it matters.
   ========================================================================= *)
=============================================================================
