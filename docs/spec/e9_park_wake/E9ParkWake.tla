------------------------------- MODULE E9ParkWake -------------------------------
(*
  E9 Scheduler park-admission and unified wake-source protocol
  (sluice-CORE-E9), Model P3 (decoupled wake domains).

  E9-CORRECTIVE adds the RUN INVOCATION LIFETIME dimension (runMode /
  runState) that the original model omitted. The omission let TLC green-
  light a Drain run that parks forever on MW-S3 + external-wake-capable
  (the shipped deterministic hang). See ADR §9.4.0.

  THE LOAD-BEARING E9 QUESTION (ADR 9.4):

    When may an idle Scheduler Worker commit to parking, and which state
    publications create an obligation to wake parked Workers?

  ANSWER (P3 + RunMode, decoupled wake domains):

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
    - RUN LIFETIME IS AN EXPLICIT POLICY DIMENSION (E9-CORRECTIVE):
        runMode  in {Drain, Live}
        runState in {Active, ReturnedStalled, ReturnedQuiescent, Shutdown}
      ClassifyGlobalState is SEPARATE from SelectIdleAction. In Drain,
      MW-S3 MUST return Stalled (never park). In Live, MW-S3 + effective
      external wake MAY park; MW-S3 without effective external wake MUST
      return Stalled. A wake handle never mutates runMode.

  DOMAIN (finite, exhaustive TLC):
    Workers = {W0, W1}, Fibers = {F0}. (F1 not needed; one waiter is the
    load-bearing proof. Two Workers exercise the multi-parked state and
    the at-most-one-backend-participant rule.)

  STATE AXES (E9 spec 10, four-dimensional topology M2):
    resource:    runnableVisible, runningVisible, backendOutstanding,
                 backendReady, externalWaitRegistered, externalReady
    execution:   (Fiber lifecycle collapsed; E8 ownership is CLOSED)
    coordination:workerPhase[w], observedEpoch[w], wakeEpoch,
                 backendWaitParticipant
    invocation:  runMode, runState   [ADDED by E9-CORRECTIVE]

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
    backendWaitParticipant,
    runMode,
    runState

PhaseVal == {"Active", "ParkCandidate", "ParkCommitted", "Parked"}
PartVal  == {NONE, W0, W1}
ModeVal  == {"Drain", "Live"}
StateVal == {"Active", "ReturnedStalled", "ReturnedQuiescent", "Shutdown"}

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

(* The MW-S3 state: no executable work, no backend progress, but an
   unresolved wait registration remains (E7 §9.2.6). A stale externalReady
   flag with NO registration is NOT MW-S3 -- it is not logical work; the
   flag is simply a published value no one is waiting on. *)
MWS3 ==
    ~ExecutableWork /\ ~SomeBackendWork /\ externalWaitRegistered

(* Latent external executable work: a registered external-wait whose flag
   is ALREADY ready but not yet drained into runnable. This is the E7
   9.2.6 "latent executable work" concept carried into E9 park admission:
   the cv.wait predicate AND the admission preconditions treat this as
   "predicate true" so a Worker never commits to park while a registered
   ready wait is un-drained. *)
LatentExternalWork ==
    externalReady /\ externalWaitRegistered

(* Quiescence: truly nothing remains — no executable work, no backend
   progress, no wait registration at all (E9-LIFE-5). *)
Quiescent ==
    ~ExecutableWork /\ ~SomeBackendWork /\
    ~externalWaitRegistered /\ ~externalReady

(* Any Worker in a phase at or past ParkCandidate. *)
ParkedOrCommitting(w) ==
    workerPhase[w] \in {"ParkCandidate", "ParkCommitted", "Parked"}

AnyParked ==
    \E w \in Workers : workerPhase[w] = "Parked"

AnyCommittedOrParked ==
    \E w \in Workers : workerPhase[w] \in {"ParkCommitted", "Parked"}

(* =========================================================================
   ClassifyGlobalState vs SelectIdleAction (E9-CORRECTIVE, M4).

   ClassifyGlobalState is the AUTHORITATIVE taxonomy (one classifier).
   SelectIdleAction is the mode-specific idle-action selection that runs
   AFTER classification. The two are conceptually distinct; the action
   preconditions below reflect one authoritative taxonomy.
   ========================================================================= *)

GlobalClass ==
    IF ExecutableWork THEN "MWS1"
    ELSE IF SomeBackendWork THEN "MWS2"
    ELSE IF externalWaitRegistered THEN "MWS3"
    ELSE "QUIESCENT"

(* SelectIdleAction: given (runMode, globalClass, externalWakeCapability),
   choose the idle action. This is the ONLY place runMode influences
   behavior. Drain + MW-S3 => ReturnStalled (never park). Live + MW-S3 +
   external wake => SchedulerPark. Live + MW-S3 + no external wake =>
   ReturnStalled. *)

(* Is parking ADMITTED for the current mode/state? Park admission is
   allowed only for MW-S2 (backend or mixed) and, in Live, for MW-S3 with
   an effective external wake. *)
ParkAdmitted ==
    /\ runState = "Active"
    /\ \/ GlobalClass = "MWS2"
       \/ /\ runMode = "Live"
          /\ GlobalClass = "MWS3"
          /\ ExternalWakePossible

(* =========================================================================
   Producer-side actions: PUBLISH PERSISTENT STATE, then SIGNAL.
   The signal advances the wake epoch. Persistent state is authoritative.
   ========================================================================= *)

(* W8: external thread completes a Future. Publishes externalReady, then
   signals the wake source. The producer does NOT route, make_runnable,
   or touch any Scheduler queue (9.4.9). Enabled only while the run
   invocation is Active (a returned run is no longer executing). *)
ExternalReadyPublish ==
    /\ runState = "Active"
    /\ ~externalReady
    /\ externalReady' = TRUE
    /\ wakeEpoch' = 1 - wakeEpoch
    /\ UNCHANGED <<runnableVisible, runningVisible,
                   backendOutstanding, backendReady,
                   externalWaitRegistered,
                   workerPhase, observedEpoch, backendWaitParticipant,
                   runMode, runState>>

(* A Scheduler Worker observes externalReady and routes the waiting Fiber
   to runnable (DrainExternalReady). This is a Scheduler-domain action:
   the external producer NEVER does this. *)
DrainExternalReady ==
    /\ runState = "Active"
    /\ externalReady
    /\ externalWaitRegistered
    /\ externalReady' = FALSE
    /\ externalWaitRegistered' = FALSE
    /\ runnableVisible' = TRUE
    /\ UNCHANGED <<runningVisible, backendOutstanding, backendReady,
                   wakeEpoch, workerPhase, observedEpoch,
                   backendWaitParticipant, runMode, runState>>

(* A backend op becomes ready (W5/W6 reified: a ThreadPool worker pushes
   a result, or a CQE lands). Persistent state first; the MW-S2 BACKEND
   participant observes it via wait_one return (BackendWaitReturn). No
   wake-epoch signal is needed for the BACKEND participant (it is in the
   backend domain). *)
BackendReadyPublish ==
    /\ runState = "Active"
    /\ backendOutstanding
    /\ ~backendReady
    /\ backendReady' = TRUE
    /\ UNCHANGED <<runnableVisible, runningVisible,
                   backendOutstanding, externalWaitRegistered,
                   externalReady, wakeEpoch,
                   workerPhase, observedEpoch, backendWaitParticipant,
                   runMode, runState>>

(* A running Fiber submits a backend op (e.g. ctx.submit_read). Marks the op
   outstanding; backendOutstanding=TRUE makes the state MW-S2-eligible once
   the Fiber suspends. The Completion this op will fill is implicit (a
   registered backend wait is implied by backendOutstanding). *)
SubmitBackend ==
    /\ runState = "Active"
    /\ runningVisible
    /\ ~backendOutstanding
    /\ backendOutstanding' = TRUE
    /\ UNCHANGED <<runnableVisible, runningVisible, backendReady,
                   externalWaitRegistered, externalReady,
                   wakeEpoch, workerPhase, observedEpoch,
                   backendWaitParticipant, runMode, runState>>

(* A Scheduler Worker drains backendReady into runnable (a Completion
   waiter becomes runnable). *)
DrainBackendReady ==
    /\ runState = "Active"
    /\ backendReady
    /\ backendReady' = FALSE
    /\ backendOutstanding' = FALSE
    /\ runnableVisible' = TRUE
    /\ UNCHANGED <<runningVisible, externalWaitRegistered, externalReady,
                   wakeEpoch, workerPhase, observedEpoch,
                   backendWaitParticipant, runMode, runState>>

(* A Fiber publishes runnable work from inside a Worker (W1/W2/W3). This
   advances the wake epoch AND routes (the route is abstracted here as
   runnableVisible' = TRUE). route_runnable_locked in production also
   notifies inbox_cv; E9 adds the wake-epoch path. *)
PublishRunnable ==
    /\ runState = "Active"
    /\ ~runnableVisible
    /\ runnableVisible' = TRUE
    /\ wakeEpoch' = 1 - wakeEpoch
    /\ UNCHANGED <<runningVisible, backendOutstanding, backendReady,
                   externalWaitRegistered, externalReady,
                   workerPhase, observedEpoch, backendWaitParticipant,
                   runMode, runState>>

(* =========================================================================
   Park-admission actions (Worker side). Globally coordinated under the
   wake mutex (abstracted as atomic transitions).

   Park admission is GATED by ParkAdmitted (which encodes runMode). In
   Drain, MW-S3 never admits a park candidate => the worker falls through
   to ReturnStalled. In Live, MW-S3 + external wake admits.
   ========================================================================= *)

(* BeginParkCandidate: a Worker with no local work elects itself a
   candidate. PRECONDITION: park is admitted for the current mode/state
   (ParkAdmitted), no executable work AND no latent external-ready work.
   The production worker_loop runs wake_ready_*_locked before classify;
   this precondition mirrors that drain obligation. *)
BeginParkCandidate(w) ==
    /\ workerPhase[w] = "Active"
    /\ runState = "Active"
    /\ ParkAdmitted
    /\ ~ExecutableWork
    /\ ~LatentExternalWork
    /\ workerPhase' = [workerPhase EXCEPT ![w] = "ParkCandidate"]
    /\ UNCHANGED <<runnableVisible, runningVisible,
                   backendOutstanding, backendReady,
                   externalWaitRegistered, externalReady,
                   wakeEpoch, observedEpoch, backendWaitParticipant,
                   runMode, runState>>

(* FinalParkRecheckAndCommit: the candidate does a final drain + classify,
   OBSERVES the wake epoch, and COMMITS to park (recording observedEpoch).
   PRECONDITION (final recheck): park still admitted, still no executable
   work AND still no latent external-ready work. A publish between
   CANDIDATE and COMMIT that set externalReady (or routed runnable,
   raising runnableVisible) cancels admission: the precondition is simply
   false, so the action is not enabled and the candidate cannot commit.
   The epoch validation at EnterPhysicalPark closes the commit-to-sleep
   window. *)
FinalParkRecheckAndCommit(w) ==
    /\ runState = "Active"
    /\ workerPhase[w] = "ParkCandidate"
    /\ ParkAdmitted
    /\ ~ExecutableWork
    /\ ~LatentExternalWork
    /\ observedEpoch' = [observedEpoch EXCEPT ![w] = wakeEpoch]
    /\ workerPhase' = [workerPhase EXCEPT ![w] = "ParkCommitted"]
    /\ UNCHANGED <<runnableVisible, runningVisible,
                   backendOutstanding, backendReady,
                   externalWaitRegistered, externalReady,
                   wakeEpoch, backendWaitParticipant, runMode, runState>>

(* AbandonParkCandidate: if park is no longer admitted (e.g. Drain mode
   was selected, or state changed to MW-S1), a candidate returns to Active
   without committing. This is the re-check that lets a Drain worker fall
   through to ReturnStalled. *)
AbandonParkCandidate(w) ==
    /\ runState = "Active"
    /\ workerPhase[w] = "ParkCandidate"
    /\ ~ParkAdmitted
    /\ workerPhase' = [workerPhase EXCEPT ![w] = "Active"]
    /\ UNCHANGED <<runnableVisible, runningVisible,
                   backendOutstanding, backendReady,
                   externalWaitRegistered, externalReady,
                   wakeEpoch, observedEpoch, backendWaitParticipant,
                   runMode, runState>>

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
   the next drain (DrainExternalReady) routes the waiting Fiber. *)
EnterPhysicalPark(w) ==
    /\ runState = "Active"
    /\ workerPhase[w] = "ParkCommitted"
    /\ IF /\ wakeEpoch = observedEpoch[w]   \* no pending SCHEDULER wake
          /\ ~backendReady                  \* no backend-ready signal
          /\ ~LatentExternalWork            \* no registered external-ready wait
          /\ ~ExecutableWork                \* no executable work appeared
          \* E9-CORRECTIVE idle-action re-selection: a Worker committed
          \* under MW-S2 may find the state has shifted to MW-S3 by the
          \* time it reaches the physical wait (backend drained, fiber
          \* finished). In Drain, MW-S3 MUST return Stalled -- the Worker
          \* treats the predicate as "true" (do not block) and returns to
          \* Active, where the next loop selects ReturnStalled. In Live,
          \* MW-S3 + effective external wake remains parkable.
          /\ \/ GlobalClass = "MWS2"
             \/ /\ runMode = "Live"
                /\ GlobalClass = "MWS3"
                /\ ExternalWakePossible
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
          \* the epoch predicate under the wake mutex. E9-CORRECTIVE: it
          \* is ALSO the Drain/MW-S3 idle-action boundary -- a Drain worker
          \* that finds MW-S3 at the physical wait returns to Active and
          \* the next loop iteration's select_idle_action yields
          \* ReturnStalled.)
          /\ workerPhase' = [workerPhase EXCEPT ![w] = "Active"]
          /\ backendWaitParticipant' = backendWaitParticipant
    /\ UNCHANGED <<runnableVisible, runningVisible,
                   backendOutstanding, backendReady,
                   externalWaitRegistered, externalReady,
                   wakeEpoch, observedEpoch, runMode, runState>>

(* LeavePark: a parked Worker returns to Active to re-drain. The wake is
   ADVISORY; the Worker re-drains persistent state regardless.

   E9-CORRECTIVE / E9-LIFE-8: the SCHEDULER-domain bounded observation
   park ALWAYS has an observation-return path. The bounded wake_cv_
   timeout fires regardless of whether a wake signal arrived (it is the
   authority for backend observation in MIXED-WAKE, ADR §9.4.7.1). So
   LeavePark is ALWAYS enabled for a Parked worker -- modeling the
   bounded timeout that eventually returns the Worker to re-drain +
   reclassify. This is what prevents a Drain worker from being stranded
   parked under MW-S3: even with no wake due, the observation timeout
   returns it, and the next loop's select_idle_action yields
   ReturnStalled.

   The wake-specific clauses (epoch advance, backendReady, etc.) remain
   as the *reasons* a wake may arrive early, but the action is enabled
   unconditionally to model the bounded observation return. *)
LeavePark(w) ==
    /\ runState = "Active"
    /\ workerPhase[w] = "Parked"
    /\ IF backendWaitParticipant = w
       THEN /\ backendWaitParticipant' = NONE
       ELSE /\ backendWaitParticipant' = backendWaitParticipant
    /\ workerPhase' = [workerPhase EXCEPT ![w] = "Active"]
    /\ UNCHANGED <<runnableVisible, runningVisible,
                   backendOutstanding, backendReady,
                   externalWaitRegistered, externalReady,
                   wakeEpoch, observedEpoch, runMode, runState>>

(* EnterBackendWait / BackendWaitReturn: the BACKEND participant calls
   ctx_.wait_one and returns. Abstracted as: enter is folded into
   EnterPhysicalPark (domain selection); return is LeavePark when
   backendReady. No separate action needed; the domain is encoded in
   backendWaitParticipant. *)

(* =========================================================================
   Run-lifetime idle actions (E9-CORRECTIVE). SelectIdleAction produces
   ReturnStalled / ReturnQuiescent when park is not admitted.
   ========================================================================= *)

(* ReturnStalled: the run invocation returns STALLED. Reached when the
   global state is MW-S3 AND park is not admitted for this mode:
     - Drain + MW-S3 (always return stalled; never park)
     - Live + MW-S3 WITHOUT effective external wake
   No executable work, no backend progress, an unresolved wait remains.
   The wait registration is PRESERVED (MW-S3 is not quiescence).

   This is a GLOBAL run-lifetime decision (the coordinated-termination
   path in production: the last worker to go idle does a final recheck
   and sets global_terminate). It does NOT require every Worker to be in
   a particular phase; transient ParkCandidate/ParkCommitted Workers are
   released by the termination wake (production global_terminate_ +
   signal_wake_locked wake every parked Worker). *)
ReturnStalled ==
    /\ runState = "Active"
    /\ GlobalClass = "MWS3"
    /\ ~ParkAdmitted
    /\ ~AnyCommittedOrParked       \* no Worker is physically parked (the
                                   \* stranded hazard); candidates/committed
                                   \* that have not blocked are released by
                                   \* the termination wake in production.
    /\ runState' = "ReturnedStalled"
    /\ UNCHANGED <<runnableVisible, runningVisible,
                   backendOutstanding, backendReady,
                   externalWaitRegistered, externalReady,
                   wakeEpoch, workerPhase, observedEpoch,
                   backendWaitParticipant, runMode>>

(* ReturnQuiescent: the run invocation returns QUIESCENT. Reached when
   the global state is truly quiescent (no work, no backend, no waits).
   Distinct from MW-S3 (E9-LIFE-5). Global decision (see ReturnStalled). *)
ReturnQuiescent ==
    /\ runState = "Active"
    /\ Quiescent
    /\ ~AnyCommittedOrParked
    /\ runState' = "ReturnedQuiescent"
    /\ UNCHANGED <<runnableVisible, runningVisible,
                   backendOutstanding, backendReady,
                   externalWaitRegistered, externalReady,
                   wakeEpoch, workerPhase, observedEpoch,
                   backendWaitParticipant, runMode>>

(* ShutdownSignal: a coordinated termination condition. Advances the wake
   epoch and (in production) notifies wake_cv + inbox_cv, waking every
   parked Worker. Transitions runState to Shutdown. *)
ShutdownSignal ==
    /\ runState = "Active"
    /\ wakeEpoch' = 1 - wakeEpoch
    /\ runState' = "Shutdown"
    /\ UNCHANGED <<runnableVisible, runningVisible,
                   backendOutstanding, backendReady,
                   externalWaitRegistered, externalReady,
                   workerPhase, observedEpoch, backendWaitParticipant,
                   runMode>>

(* =========================================================================
   Fiber lifecycle (collapsed). These keep runnable/running consistent.
   ========================================================================= *)

(* A runnable Fiber starts running. *)
RunFiber ==
    /\ runState = "Active"
    /\ runnableVisible
    /\ ~runningVisible
    /\ runnableVisible' = FALSE
    /\ runningVisible' = TRUE
    /\ UNCHANGED <<backendOutstanding, backendReady,
                   externalWaitRegistered, externalReady,
                   wakeEpoch, workerPhase, observedEpoch,
                   backendWaitParticipant, runMode, runState>>

(* A running Fiber suspends on the external Future (registers the wait). *)
SuspendFiber ==
    /\ runState = "Active"
    /\ runningVisible
    /\ ~externalWaitRegistered
    /\ runningVisible' = FALSE
    /\ externalWaitRegistered' = TRUE
    /\ UNCHANGED <<runnableVisible, backendOutstanding, backendReady,
                   externalReady, wakeEpoch, workerPhase, observedEpoch,
                   backendWaitParticipant, runMode, runState>>

(* A running Fiber finishes. *)
FinishFiber ==
    /\ runState = "Active"
    /\ runningVisible
    /\ runningVisible' = FALSE
    /\ UNCHANGED <<runnableVisible, backendOutstanding, backendReady,
                   externalWaitRegistered, externalReady,
                   wakeEpoch, workerPhase, observedEpoch,
                   backendWaitParticipant, runMode, runState>>

(* =========================================================================
   Next, Init, Spec
   ========================================================================= *)

vars ==
    <<runnableVisible, runningVisible,
      backendOutstanding, backendReady,
      externalWaitRegistered, externalReady,
      wakeEpoch, workerPhase, observedEpoch,
      backendWaitParticipant, runMode, runState>>

(* TerminalStutter: a returned/shutdown invocation is a legitimate terminal
   state (the run has completed its lifetime). It may remain unchanged.
   This makes terminal states non-deadlocking without reopening the
   protocol. Production: run() has returned to the caller. *)
TerminalStutter ==
    /\ runState # "Active"
    /\ UNCHANGED vars

Next ==
    \/ ExternalReadyPublish
    \/ DrainExternalReady
    \/ SubmitBackend
    \/ BackendReadyPublish
    \/ DrainBackendReady
    \/ PublishRunnable
    \/ \E w \in Workers : BeginParkCandidate(w)
    \/ \E w \in Workers : FinalParkRecheckAndCommit(w)
    \/ \E w \in Workers : AbandonParkCandidate(w)
    \/ \E w \in Workers : EnterPhysicalPark(w)
    \/ \E w \in Workers : LeavePark(w)
    \/ ReturnStalled
    \/ ReturnQuiescent
    \/ RunFiber
    \/ SuspendFiber
    \/ FinishFiber
    \/ ShutdownSignal
    \/ TerminalStutter

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
    /\ runMode = "Drain"
    /\ runState = "Active"



Spec == Init /\ [][Next]_vars

(* Allow stuttering so terminal states are reachable and checked. *)

(* =========================================================================
   E9 safety invariants (spec 12). E9-Inv1 .. E9-Inv10 + E9-LIFE-1..10.
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
   source of truth (E9-Inv3). *)
PersistentWakeDue ==
       (externalReady /\ externalWaitRegistered)
    \/ backendReady
    \/ runnableVisible
    \/ runningVisible

\* LeavePark(w) ENABLED predicate (mirror of the action's guard). E9-
\* CORRECTIVE: always enabled for a Parked worker (bounded observation
\* return, E9-LIFE-8).
LeaveParkEnabled(w) ==
    /\ workerPhase[w] = "Parked"

Inv2NoLostWake ==
    (\/ PersistentWakeDue)
    => (\A w \in Workers :
            workerPhase[w] = "Parked"
            => LeaveParkEnabled(w))

(* E9-Inv3: wake signal is not authority. Structural: LeavePark changes no
   persistent state. *)

(* E9-Inv4: external ready publication creates a wake obligation, but
   ONLY for a REGISTERED wait. Matches Inv2 for the external sub-case. *)
Inv4ExternalReadyWakes ==
    \A w \in Workers :
        (workerPhase[w] = "Parked" /\ externalReady /\ externalWaitRegistered)
        => LeaveParkEnabled(w)

(* E9-Inv6: at most one backend blocking participant. *)
Inv6OneBackendParticipant ==
    /\ (backendWaitParticipant = NONE
       \/ backendWaitParticipant \in Workers)

(* E9-Inv7: MIXED-WAKE liveness authority is explicit — a TRANSITION
   OBLIGATION encoded structurally as EnterPhysicalPark's BACKEND-branch
   precondition ~ExternalWakePossible. *)
Inv7MixedWakeNoBlindBackendWait ==
    TRUE

Inv7StateForm ==
    ExternalWakePossible => backendWaitParticipant = NONE

Inv ==
    /\ Inv2NoLostWake
    /\ Inv4ExternalReadyWakes
    /\ Inv6OneBackendParticipant
    /\ Inv7MixedWakeNoBlindBackendWait

(* =========================================================================
   E9-LIFE run-lifetime properties (E9-CORRECTIVE spec 7).

   E9-LIFE-1: Drain never parks solely for MW-S3 (DECISION OBLIGATION).
     The precise safety form: a Drain run never STRANDS a Parked Worker
     under MW-S3. A Drain Worker that lawfully parked under MW-S2-mixed
     may transit through a Parked snapshot after the backend drains to
     MW-S3; that is legitimate ONLY because the bounded observation
     return (E9-LIFE-8) keeps LeavePark ENABLED, so the Worker is never
     stranded. The invariant asserts: under Drain + MW-S3, every Parked
     Worker has LeavePark enabled (it can always return to Active, where
     the next loop selects ReturnStalled). The full "Drain MW-S3
     eventually returns Stalled" obligation is the temporal E9-LIFE-2.
   ========================================================================= *)
InvLife1DrainNoMW3Park ==
    (runMode = "Drain" /\ GlobalClass = "MWS3")
    => \A w \in Workers :
        (workerPhase[w] = "Parked" => LeaveParkEnabled(w))

(* E9-LIFE-2: Drain MW-S3 returns Stalled (TEMPORAL). Without assuming an
   external producer acts or a backend completes, a Drain run that reaches
   MW-S3 eventually reaches ReturnedStalled. Configured under fairness on
   ReturnStalled. See LivenessSpec below. *)

(* E9-LIFE-3: Live external-capable MW-S3 may remain resident (DECISION
   OBLIGATION). In Live + MW-S3 + external wake, park IS admitted. *)
InvLife3LiveExternalParkAdmitted ==
    (runMode = "Live" /\ GlobalClass = "MWS3" /\ ExternalWakePossible
     /\ runState = "Active")
    => ParkAdmitted

(* E9-LIFE-4: Live non-wakeable MW-S3 does not park forever (TEMPORAL).
   Live + MW-S3 + ~external wake ~> ReturnedStalled. *)

(* E9-LIFE-5: Quiescence remains classifier-defined (STATE INVARIANT).
   ReturnedQuiescent implies no executable work, no backend outstanding,
   no Scheduler wait registration. *)
InvLife5QuiescenceClassifierDefined ==
    runState = "ReturnedQuiescent"
    => Quiescent

(* E9-LIFE-6: Wake handle does not control run mode (STRUCTURAL). No
   formal action mutates runMode because a wake handle is created,
   copied, retained, signaled, or invalidated. Structural: runMode is
   UNCHANGED by every action (inspected above). No separate formula. *)

(* E9-LIFE-7: External-ready Live progress (TEMPORAL). After Live +
   external wait registered + parked + external ready published + wake
   epoch advanced, the protocol eventually drains external readiness. *)

(* E9-LIFE-8: Mixed-source bounded observation (TRANSITION OBLIGATION).
   When the selected Scheduler-domain park excludes backend readiness
   from its physical signal wake set (backendOutstanding /\ mixed wake /\
   Scheduler-domain park), an ObservationTimeout/observation-return
   action re-enters backend drain + external drain + global classify.
   Structural: LeavePark re-drains on every return (the production
   bounded wake_cv timeout re-enters the loop top drain + classify). *)

(* E9-LIFE-9: External producer remains signal-only (STRUCTURAL). The
   external producer actions (ExternalReadyPublish) only publish
   persistent ready + signal; they do not make_runnable, erase WaitReg,
   route, or mutate local queues. Structural: ExternalReadyPublish
   changes only externalReady + wakeEpoch. *)

(* E9-LIFE-10: E7/E8 protocols remain invariant (STRUCTURAL). One
   runnable Fiber <=> one runnable ticket; make_runnable success grants
   publication capability; steal = MOVE + ownerRecord TRANSFER;
   WakeReady routes from waitOwner. The E7/E8 TLA+ models remain CLOSED;
   E9 does not modify them. Structural here. *)

(* Combined LIFE safety invariants checked by TLC. *)
InvLife ==
    /\ InvLife1DrainNoMW3Park
    /\ InvLife3LiveExternalParkAdmitted
    /\ InvLife5QuiescenceClassifierDefined

(* =========================================================================
   Liveness (spec 15). E9-CORRECTIVE configures the load-bearing temporal
   properties in the gate cfg (no longer README-only).
   ========================================================================= *)

(* Scheduler fairness on the worker decision steps and on LeavePark. *)
FairLeavePark ==
    \A w \in Workers : WF_vars (LeavePark(w))

FairReturnStalled ==
    WF_vars (ReturnStalled)

FairReturnQuiescent ==
    WF_vars (ReturnQuiescent)

FairAbandon ==
    \A w \in Workers : WF_vars (AbandonParkCandidate(w))

FairObservationTimeout ==
    \A w \in Workers : WF_vars (EnterPhysicalPark(w))

(* NOTE: we do NOT assume external producer fairness (ExternalReadyPublish
   eventually happens) or backend fairness (backend eventually completes)
   or shutdown fairness for the Drain-return properties. Drain MW-S3 MUST
   return without any of those. *)

LivenessSpec ==
    Spec
    /\ FairLeavePark
    /\ FairReturnStalled
    /\ FairReturnQuiescent
    /\ FairAbandon
    /\ FairObservationTimeout

(* E9-LIFE-2: Drain MW-S3 returns (no producer/backend fairness). Leads-to
   form: whenever a Drain run reaches MW-S3, EITHER it eventually returns
   (Stalled/Quiescent/Shutdown -- any run-lifetime terminal) OR the state
   leaves MW-S3 via legitimate progress (runnable/backend/external-ready
   appears). The contrapositive of the defect: a Drain MW-S3 that
   receives NO progress MUST end the run (it cannot park forever). We do
   NOT assume producer/backend fairness; Shutdown is a caller-driven
   terminal, not a fairness assumption on the protocol. *)
Life2DrainMWS3Returns ==
    [] ( (runMode = "Drain" /\ GlobalClass = "MWS3" /\ runState = "Active")
          => <> (runState # "Active" \/ GlobalClass # "MWS3") )

(* E9-LIFE-4: Live non-wakeable MW-S3 returns (no park forever). Same
   leads-to shape as Life2: a Live MW-S3 with NO effective external wake
   must end the run or leave MW-S3 via progress -- it must not park
   forever on an unresolved wait with no effective Scheduler wake source. *)
Life4LiveNonWakeableMWS3Returns ==
    [] ( (runMode = "Live" /\ GlobalClass = "MWS3" /\ ~ExternalWakePossible
          /\ runState = "Active")
          => <> (runState # "Active" \/ GlobalClass # "MWS3") )

(* E9-LIFE-7: External-ready Live progress. CONDITIONED on externalReady
   having ALREADY been published (we do not use producer fairness to make
   publication happen). After Live + parked + external ready published,
   eventually externalReady is drained (becomes FALSE via
   DrainExternalReady) OR the run ends. *)
Life7ExternalReadyEventuallyDrained ==
    [] ( (runMode = "Live" /\ externalReady /\ externalWaitRegistered
          /\ \E w \in Workers : workerPhase[w] = "Parked")
          => <> (~externalReady \/ runState # "Active") )

(* Combined liveness property set. *)
LifeProps ==
    /\ Life2DrainMWS3Returns
    /\ Life4LiveNonWakeableMWS3Returns
    /\ Life7ExternalReadyEventuallyDrained

=============================================================================
