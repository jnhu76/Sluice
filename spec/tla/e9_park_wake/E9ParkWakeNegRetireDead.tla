---------------------------- MODULE E9ParkWakeNegRetireDead -----------------------------
(*
  E9 Scheduler park-admission and unified wake-source protocol
  (sluice-CORE-E9), Model P3 (decoupled wake domains).

  E9-CORRECTIVE adds the RUN INVOCATION LIFETIME dimension (runMode /
  runState) that the original model omitted. The omission let TLC green-
  light a Drain run that parks forever on MW-S3 + external-wake-capable
  (the shipped deterministic hang). See ADR §9.4.0.

  PHASE G (P5-CORRECTIVE / G1 repair R1-R4, 2026-08-15 closeout) adds:

    - SPLIT-WAIT PARK DOMAIN (CONSTANT SplitWait): a split-wait backend
      (ThreadPool / real io_uring — wait_source() != null) parks the
      MW-S2 participant in the BACKEND domain for BOTH backend-only and
      MIXED-WAKE; its progress transport is prompt (ready epoch / ring
      fd). A non-split (reference) backend keeps the E9 rule: MIXED-WAKE
      parks on the SCHEDULER domain with the bounded observation return.
    - THE BRIDGE (signal_wake_locked -> interrupt_backend_waiters): every
      Scheduler wake publication (external ready, routing, retire,
      shutdown) ALSO delivers a one-shot control wake to a BACKEND-parked
      participant. Modeled as the boolean bridgePending: set by every
      wake-epoch-advancing producer while a committed/parked participant
      exists (the backend_wait_active_ gate window), consumed exactly by
      the participant's park return (D4-RM13 one-shot semantics). The
      D4-RM14 arm is the persistence of bridgePending across the
      commit-to-park-entry window: a bump landing between the MW-S2
      commit and the physical park is still delivered to the FIRST park
      (never rebaselined into a past event).
    - R1 (park-commit refuse): a SCHEDULER-domain park commit REFUSES
      when unguarded progress exists (runnable / accepted backend work
      with NO observer: no running fiber, no committed/parked backend
      participant, no admission in flight). The refuser signals the wake
      domain (wakes an electable sleeping sibling) and returns to Active
      to become the observer itself.
    - R2 (transferable election): the backend participant is the LOWEST
      alive worker, not a fixed worker 0 — after a worker retires, a
      survivor can still elect (the no-participant manifestation heals).
    - R3 (terminate-path retire): a worker leaving the loop publishes an
      UNCONDITIONAL wake (advance wakeEpoch), never loses runnable work
      (retire requires the worker's own view drained), and leaves the
      alive set; the last retire ends the run invocation.
    - R4 (idle-dance not-last signal): the first worker to enter the
      counted idle park signals the wake domain as part of its commit
      (the conservative "under-clear may cost an extra dance, never a
      missed signal" rule; the contribution-aware damping refinement is
      an anti-livelock optimization abstracted here to its convergence
      obligation).

  THE LOAD-BEARING E9 QUESTION (ADR 9.4):

    When may an idle Scheduler Worker commit to parking, and which state
    publications create an obligation to wake parked Workers?

  ANSWER (P3 + RunMode + Phase G, decoupled wake domains):

    - A Worker commits to parking only after a globally-coordinated
      admission: drain persistent readiness, reclassify, OBSERVE the wake
      epoch, and validate the epoch before sleeping. The wake epoch is the
      authority for "a wake-relevant publication happened after I decided
      to park"; the cv/notify is the physical delivery.
    - There are TWO park domains: BACKEND (ctx_.wait_one, at most one
      participant, the E7 MW-S2 rule) and SCHEDULER (wake_cv + wake
      epoch, any number of Workers). SplitWait selects BACKEND for all of
      MW-S2 (the bridge carries external wakes into it); non-split keeps
      the E9 rule (BACKEND only when no external-wake-capable wait is
      registered; the bounded observation return remains the reference
      backends' MIXED-WAKE progress authority).
    - Every wake-relevant producer publishes persistent state FIRST and
      signals (advances wakeEpoch + notifies) SECOND. The signal is
      advisory; persistent state is authoritative. Under SplitWait the
      same signal ALSO bridges into a parked backend participant.
    - RUN LIFETIME IS AN EXPLICIT POLICY DIMENSION (E9-CORRECTIVE):
        runMode  in {Drain, Live}
        runState in {Active, ReturnedStalled, ReturnedQuiescent, Shutdown}
      ClassifyGlobalState is SEPARATE from SelectIdleAction. In Drain,
      MW-S3 MUST return Stalled (never park). In Live, MW-S3 + effective
      external wake MAY park; MW-S3 without effective external wake MUST
      return Stalled. A wake handle never mutates runMode.

  DOMAIN (finite, exhaustive TLC):
    Workers = {W0, W1}, Fibers = {F0}. (F1 not needed; one waiter is the
    load-bearing proof. Two Workers exercise the multi-parked state, the
    at-most-one-backend-participant rule, the R2 transferable election,
    and the R3/R4 retire/dance convergence.)

  STATE AXES (E9 spec 10, four-dimensional topology M2):
    resource:    runnableVisible, runningVisible, backendOutstanding,
                 backendReady, externalWaitRegistered, externalReady
    execution:   (Fiber lifecycle collapsed; E8 ownership is CLOSED)
    coordination:workerPhase[w], observedEpoch[w], wakeEpoch,
                 backendWaitParticipant, bridgePending   [PHASE G]
    population:  workerAlive[w], idleCount               [R2/R3/R4]
    invocation:  runMode, runState   [E9-CORRECTIVE]

  Persistent state (runnable/running/backend/external ready) is kept
  SEPARATE from the wake signal/epoch. The wake notification is NOT the
  source of truth.

  This model abstracts away Fiber identity beyond F0 (the external-wait
  Fiber) and collapses runnable/running into booleans. The E7/E8
  publication/steal protocols are CLOSED; E9 does not reopen them. The
  R3 retire's pending_spawn re-seeding is collapsed into "runnable
  survives on a live worker or the run returns" (retire requires the
  retiring worker's own view drained). The R4 contribution-aware damping
  (a counted dancer may sleep holding its count) is a livelock
  optimization; the model keeps its convergence obligation (the
  not-last-signal) and the live-worker threshold.
*)
EXTENDS Naturals, Sequences, FiniteSets, TLC

CONSTANTS Workers, Fibers, W0, W1, F0, NONE, SplitWait

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
    bridgePending,
    workerAlive,
    idleCount,
    terminateFlag,
    runMode,
    runState,
    retireFired      \* causal history witness: RetireWorkerQuiescent executed

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
    /\ SplitWait \in {TRUE, FALSE}

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
   registered. Under SplitWait this state LAWFULLY parks in the BACKEND
   domain (the bridge carries external wakes into it); the pre-Phase-G
   "blind backend wait" hazard is closed by Inv8 + Life7 instead of the
   domain rule. *)
MixedWake ==
    MWS2 /\ ExternalWakePossible

(* The MW-S3 state: no executable work, no backend progress, but an
   unresolved wait registration remains (E7 §9.2.6). *)
MWS3 ==
    ~ExecutableWork /\ ~SomeBackendWork /\ externalWaitRegistered

(* Latent external executable work: a registered external-wait whose flag
   is ALREADY ready but not yet drained into runnable. *)
LatentExternalWork ==
    externalReady /\ externalWaitRegistered

(* Quiescence: truly nothing remains (E9-LIFE-5). *)
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

(* R1: an observer is anyone who will re-evaluate or consume progress —
   a running Fiber, the (committed or parked) backend participant, or
   any admission in flight. Unguarded progress is progress pending with
   NO observer: parking (on the SCHEDULER domain) beside it is the G1
   strand hazard. *)
ObserverExistsExcept(w) ==
       runningVisible
    \/ backendWaitParticipant # NONE
    \/ \E v \in Workers : v # w /\ workerPhase[v] \in {"ParkCandidate", "ParkCommitted"}

(* R1's refuse predicate as evaluated at Worker w's OWN park commit: w's
   own in-flight admission is NOT an observer (production evaluates it at
   the wake-domain park commit, after the MW-S2 admission cleared). *)
UnguardedProgressPendingExcept(w) ==
    (runnableVisible \/ SomeBackendWork) /\ ~ObserverExistsExcept(w)

(* Routing/draining actions execute INSIDE a Worker loop (production:
   route_runnable_locked / the loop-top and Phase-D drains run on worker
   threads). The abstract producer actions below therefore require a live
   Active Worker — with every Worker parked or retired there is no
   executor, and a phantom routing would fabricate exactly the strand
   class Inv9 exists to catch (the abstract external producer and the
   backend's own readiness are NOT Worker-executed and stay unguarded). *)
SomeActiveWorker ==
    \E w \in Workers : workerAlive[w] /\ workerPhase[w] = "Active"

(* R2: the transferable election — the lowest-id ALIVE worker. *)
LowestAlive ==
    IF workerAlive[W0] THEN W0
    ELSE IF workerAlive[W1] THEN W1
    ELSE NONE

(* The one-shot control wake to the backend participant (the bridge).
   bridgePending is set by every wake publication while a participant
   exists and consumed exactly once by the participant's park return. *)
BridgeFiresFromParticipant ==
    backendWaitParticipant # NONE

(* =========================================================================
   ClassifyGlobalState vs SelectIdleAction (E9-CORRECTIVE, M4).
   ========================================================================= *)

GlobalClass ==
    IF ExecutableWork THEN "MWS1"
    ELSE IF SomeBackendWork THEN "MWS2"
    ELSE IF externalWaitRegistered THEN "MWS3"
    ELSE "QUIESCENT"

(* Is parking ADMITTED for the current mode/state? *)
ParkAdmitted ==
    /\ runState = "Active"
    /\ ~terminateFlag
    /\ \/ GlobalClass = "MWS2"
       \/ /\ runMode = "Live"
          /\ GlobalClass = "MWS3"
          /\ ExternalWakePossible

(* =========================================================================
   Producer-side actions: PUBLISH PERSISTENT STATE, then SIGNAL.
   The signal advances the wake epoch; under SplitWait the same signal
   ALSO bridges into a committed/parked backend participant (Phase G).
   ========================================================================= *)

(* The bridge side effect shared by every wake-epoch-advancing producer:
   bump bridgePending iff a backend participant exists (the
   backend_wait_active_ gate window: the gate is armed in the same
   critical section as the MW-S2 commit and cleared only when the
   participant's wait_one returns). *)
BridgeEffect(newWakeEpoch) ==
    /\ wakeEpoch' = newWakeEpoch
    /\ bridgePending' = IF BridgeFiresFromParticipant THEN TRUE
                        ELSE bridgePending

(* W8: external thread completes a Future. Publishes externalReady, then
   signals the wake source (and bridges into a parked participant). *)
ExternalReadyPublish ==
    /\ runState = "Active"
    /\ ~externalReady
    /\ externalReady' = TRUE
    /\ BridgeEffect(1 - wakeEpoch)
    /\ UNCHANGED <<runnableVisible, runningVisible,
                   backendOutstanding, backendReady,
                   externalWaitRegistered,
                   workerPhase, observedEpoch, backendWaitParticipant,
                   workerAlive, idleCount,
                   terminateFlag, runMode, runState, retireFired>>

(* A Scheduler Worker observes externalReady and routes the waiting Fiber
   to runnable (DrainExternalReady). *)
DrainExternalReady ==
    /\ runState = "Active"
    /\ SomeActiveWorker
    /\ externalReady
    /\ externalWaitRegistered
    /\ externalReady' = FALSE
    /\ externalWaitRegistered' = FALSE
    /\ runnableVisible' = TRUE
    /\ BridgeEffect(1 - wakeEpoch)
    /\ UNCHANGED <<runningVisible, backendOutstanding, backendReady,
                   workerPhase, observedEpoch,
                   backendWaitParticipant, workerAlive, idleCount,
                   terminateFlag, runMode, runState, retireFired>>

(* A backend op becomes ready. Persistent state first; the MW-S2 BACKEND
   participant observes it via its wait_one return (LeavePark's backend
   branch). NO wake-epoch signal: backend progress is the backend
   domain's own transport (the anti-goal is a reverse bridge). *)
BackendReadyPublish ==
    /\ runState = "Active"
    /\ backendOutstanding
    /\ ~backendReady
    /\ backendReady' = TRUE
    /\ UNCHANGED <<runnableVisible, runningVisible,
                   backendOutstanding, externalWaitRegistered,
                   externalReady, wakeEpoch, bridgePending,
                   workerPhase, observedEpoch, backendWaitParticipant,
                   workerAlive, idleCount, terminateFlag, runMode, runState, retireFired>>

(* A running Fiber submits a backend op. *)
SubmitBackend ==
    /\ runState = "Active"
    /\ runningVisible
    /\ ~backendOutstanding
    /\ backendOutstanding' = TRUE
    /\ UNCHANGED <<runnableVisible, runningVisible, backendReady,
                   externalWaitRegistered, externalReady,
                   wakeEpoch, bridgePending, workerPhase, observedEpoch,
                   backendWaitParticipant, workerAlive, idleCount,
                   terminateFlag, runMode, runState, retireFired>>

(* A Scheduler Worker drains backendReady into runnable. *)
DrainBackendReady ==
    /\ runState = "Active"
    /\ SomeActiveWorker
    /\ backendReady
    /\ backendReady' = FALSE
    /\ backendOutstanding' = FALSE
    /\ runnableVisible' = TRUE
    /\ BridgeEffect(1 - wakeEpoch)
    /\ UNCHANGED <<runningVisible, externalWaitRegistered, externalReady,
                   workerPhase, observedEpoch,
                   backendWaitParticipant, workerAlive, idleCount,
                   terminateFlag, runMode, runState, retireFired>>

(* A Fiber publishes runnable work from inside a Worker (route + wake). *)
PublishRunnable ==
    /\ runState = "Active"
    /\ SomeActiveWorker
    /\ ~runnableVisible
    /\ runnableVisible' = TRUE
    /\ BridgeEffect(1 - wakeEpoch)
    /\ UNCHANGED <<runningVisible, backendOutstanding, backendReady,
                   externalWaitRegistered, externalReady,
                   workerPhase, observedEpoch, backendWaitParticipant,
                   workerAlive, idleCount, terminateFlag, runMode, runState, retireFired>>

(* =========================================================================
   Park-admission actions (Worker side). Globally coordinated under the
   wake mutex (abstracted as atomic transitions).
   ========================================================================= *)

(* BeginParkCandidate: a live Worker with no local work elects itself a
   candidate. *)
BeginParkCandidate(w) ==
    /\ workerAlive[w]
    /\ workerPhase[w] = "Active"
    /\ runState = "Active"
    /\ ParkAdmitted
    /\ ~ExecutableWork
    /\ ~LatentExternalWork
    /\ workerPhase' = [workerPhase EXCEPT ![w] = "ParkCandidate"]
    /\ UNCHANGED <<runnableVisible, runningVisible,
                   backendOutstanding, backendReady,
                   externalWaitRegistered, externalReady,
                   wakeEpoch, bridgePending, observedEpoch,
                   backendWaitParticipant, workerAlive, idleCount,
                   terminateFlag, runMode, runState, retireFired>>

(* FinalParkRecheckAndCommit: the candidate does a final drain + classify,
   OBSERVES the wake epoch, and COMMITS to park (recording observedEpoch).

   R1 (Phase G): a SCHEDULER-domain park commit REFUSES while unguarded
   progress exists (progress with no observer) — the refusing Worker
   returns through AbandonParkCandidate below, which SIGNALS the wake
   domain so an electable sibling re-checks (the progress-observer
   invariant is self-restoring at the park boundary). The BACKEND-domain
   commit is exempt: the committer becomes the observer by construction
   (its own admission is in flight / it elects the participant slot). *)
SchedulerDomainCommit(w) ==
    \/ (~SplitWait /\ ExternalWakePossible)
    \/ GlobalClass # "MWS2"
    \/ backendWaitParticipant # NONE
    \/ w # LowestAlive

FinalParkRecheckAndCommit(w) ==
    /\ runState = "Active"
    /\ workerPhase[w] = "ParkCandidate"
    /\ ParkAdmitted
    /\ ~ExecutableWork
    /\ ~LatentExternalWork
    /\ (SchedulerDomainCommit(w) => ~UnguardedProgressPendingExcept(w))
    /\ observedEpoch' = [observedEpoch EXCEPT ![w] = wakeEpoch]
    /\ workerPhase' = [workerPhase EXCEPT ![w] = "ParkCommitted"]
    /\ UNCHANGED <<runnableVisible, runningVisible,
                   backendOutstanding, backendReady,
                   externalWaitRegistered, externalReady,
                   wakeEpoch, bridgePending, backendWaitParticipant,
                   workerAlive, idleCount, terminateFlag, runMode, runState, retireFired>>

(* AbandonParkCandidate: the candidate returns to Active without
   committing. R1: the refusal beside unguarded progress must SIGNAL the
   wake domain (wake the sleeping electable sibling) — the bundled
   BridgeEffect below is the Phase G refusal signal. *)
AbandonParkCandidate(w) ==
    /\ workerAlive[w]
    /\ runState = "Active"
    /\ workerPhase[w] = "ParkCandidate"
    /\ \/ ~ParkAdmitted
       \/ ExecutableWork
       \/ UnguardedProgressPendingExcept(w)
    /\ IF UnguardedProgressPendingExcept(w)
       THEN BridgeEffect(1 - wakeEpoch)
       ELSE /\ wakeEpoch' = wakeEpoch
            /\ bridgePending' = bridgePending
    /\ workerPhase' = [workerPhase EXCEPT ![w] = "Active"]
    /\ UNCHANGED <<runnableVisible, runningVisible,
                   backendOutstanding, backendReady,
                   externalWaitRegistered, externalReady,
                   observedEpoch, backendWaitParticipant,
                   workerAlive, idleCount, terminateFlag, runMode, runState, retireFired>>

(* EnterPhysicalPark: the committed Worker releases the wake mutex and
   parks on its chosen domain. Domain selection (P3 + Phase G):
     - SplitWait: MW-S2 with no participant and THIS worker is the
       lowest alive worker => BACKEND domain (becomes the participant;
       MIXED-WAKE included — the bridge carries external wakes).
     - non-split: BACKEND only when MW-S2, NOT external-wake-possible,
       and no participant (the E9 reference rule).
     - else: SCHEDULER domain.

   FAITHFUL cv.wait SEMANTICS unchanged: the physical park blocks ONLY IF
   the predicate is currently false; a publication between COMMIT and
   this wait returns the Worker straight to Active.

   R4: a SCHEDULER-domain park entering the counted idle state from
   idleCount == 0 bundles the not-last idle signal (advance wakeEpoch)
   — the E9-LIFE-8 convergence obligation (conservatively signalled;
   the contribution-aware damping is abstracted away).
   The BACKEND-domain park enters with the D4-RM14 armed floor: any
   bridgePending set since the commit is delivered to THIS park (the
   LeavePark backend branch consumes it). *)
EnterPhysicalPark(w) ==
    /\ workerAlive[w]
    /\ runState = "Active"
    /\ workerPhase[w] = "ParkCommitted"
    /\ IF /\ wakeEpoch = observedEpoch[w]   \* no pending SCHEDULER wake
          /\ ~backendReady                  \* no backend-ready signal
          /\ ~LatentExternalWork            \* no registered external-ready wait
          /\ ~ExecutableWork                \* no executable work appeared
          /\ \/ GlobalClass = "MWS2"
             \/ /\ runMode = "Live"
                /\ GlobalClass = "MWS3"
                /\ ExternalWakePossible
       THEN
          \* Predicate false -> actually block. Choose the park domain.
          /\ IF /\ MWS2
                /\ backendWaitParticipant = NONE
                /\ w = LowestAlive
                /\ \/ SplitWait
                   \/ ~ExternalWakePossible
             THEN /\ backendWaitParticipant' = w
                  /\ workerPhase' = [workerPhase EXCEPT ![w] = "Parked"]
                  /\ idleCount' = idleCount
                  /\ wakeEpoch' = wakeEpoch
                  /\ bridgePending' = bridgePending
             ELSE /\ backendWaitParticipant' = backendWaitParticipant
                  /\ workerPhase' = [workerPhase EXCEPT ![w] = "Parked"]
                  /\ idleCount' = IF idleCount < 2 THEN idleCount + 1 ELSE idleCount
                  /\ IF idleCount = 0
                     THEN BridgeEffect(1 - wakeEpoch)   \* R4 not-last signal
                     ELSE /\ wakeEpoch' = wakeEpoch
                          /\ bridgePending' = bridgePending
       ELSE
          \* Predicate already true -> did not park; return to Active.
          /\ workerPhase' = [workerPhase EXCEPT ![w] = "Active"]
          /\ backendWaitParticipant' = backendWaitParticipant
          /\ idleCount' = idleCount
          /\ wakeEpoch' = wakeEpoch
          /\ bridgePending' = bridgePending
    /\ UNCHANGED <<runnableVisible, runningVisible,
                   backendOutstanding, backendReady,
                   externalWaitRegistered, externalReady,
                   observedEpoch, workerAlive, terminateFlag, runMode, runState, retireFired>>

(* LeavePark: a parked Worker returns to Active to re-drain.

   BACKEND participant (Phase G split-wait): the park has NO bounded
   timeout — it returns on real progress (backendReady) or the bridge
   interrupt (bridgePending, one-shot consumed here: a FUTURE park
   baselines fresh, D4-RM13). This is where the model would expose a
   lost bridge wake (Inv8 / Life7 / Life8).

   SCHEDULER domain: under SplitWait the wake-domain park is unbounded
   without an active deadline — the cv predicate (epoch moved, or the
   run ended) is the return authority. Under ~SplitWait the E9 bounded
   observation return remains (always enabled) — the reference
   backends' MIXED-WAKE progress authority (DIV-05, narrowed). *)
(* Domain-appropriate WAKE-DUE authority (Phase G). The production
   epochs are MONOTONIC; this model's 1-bit wakeEpoch toggle cannot serve
   as the return authority (two benign publications flip parity back —
   the ABA hazard the old model documented). Persistent state is the
   authority here, exactly as in production:
     - the BACKEND participant returns on ITS transport (backendReady)
       or the bridge interrupt (bridgePending);
     - a SCHEDULER-parked worker returns on Scheduler-domain publications
       (external ready, runnable routing, running) or the invocation end;
       backendReady does NOT wake the scheduler domain (no reverse
       bridge — R1's refuse rule + Inv10 keep an observer for backend
       progress instead). *)
BackendDomainWakeDue ==
    bridgePending \/ backendReady

SchedulerDomainWakeDue ==
       (externalReady /\ externalWaitRegistered)
    \/ runnableVisible
    \/ runningVisible
    \/ terminateFlag

LeaveParkEnabled(w) ==
    /\ workerPhase[w] = "Parked"
    /\ \/ (backendWaitParticipant = w /\ BackendDomainWakeDue)
       \/ (backendWaitParticipant # w
            /\ \/ ~SplitWait
               \/ SchedulerDomainWakeDue
               \/ runState # "Active")

LeavePark(w) ==
    /\ workerAlive[w]
    /\ runState = "Active"
    /\ LeaveParkEnabled(w)
    /\ IF backendWaitParticipant = w
       THEN /\ backendWaitParticipant' = NONE
            /\ bridgePending' = FALSE     \* one-shot consume (D4-RM13)
       ELSE /\ backendWaitParticipant' = backendWaitParticipant
            /\ bridgePending' = bridgePending
    /\ IF backendWaitParticipant # w
       THEN idleCount' = IF idleCount > 0 THEN idleCount - 1 ELSE idleCount
       ELSE idleCount' = idleCount
    /\ workerPhase' = [workerPhase EXCEPT ![w] = "Active"]
    /\ UNCHANGED <<runnableVisible, runningVisible,
                   backendOutstanding, backendReady,
                   externalWaitRegistered, externalReady,
                   wakeEpoch, observedEpoch, workerAlive,
                   terminateFlag, runMode, runState, retireFired>>

(* =========================================================================
   R3: terminate-path retire — TWO production exit paths, neither of which
   may abandon unguarded backend progress:

   1. ParticipantNoProgressExit: the MW-S2 participant's no-progress
      terminate — its wait_one returned on a control interrupt with
      NOTHING reaped (bridgePending, no backendReady) and no
      external-wake-possible wait remains. This is the E4/E5 caller-
      re-entry boundary: exiting beside outstanding backend work is legal
      HERE and only here (production: the interrupted 0-progress return
      with ~external_wake_possible -> global terminate). The departure
      publishes the UNCONDITIONAL R3 wake and leaves the alive set.

   2. RetireWorkerQuiescent: the last-idle terminate — only at true
      quiescence (no executable work, no backend work, no waits).

   A Worker that merely REFUSED to park beside unguarded backend progress
      (R1) satisfies NEITHER precondition: it must re-loop and elect as
      the observer (Inv10's structural obligation). *)
ParticipantNoProgressExit(w) ==
    /\ workerAlive[w]
    /\ runState = "Active"
    /\ workerPhase[w] = "Parked"
    /\ backendWaitParticipant = w
    /\ bridgePending
    /\ ~backendReady
    /\ ~ExternalWakePossible
    /\ BridgeEffect(1 - wakeEpoch)
    /\ backendWaitParticipant' = NONE
    /\ bridgePending' = FALSE
    /\ workerAlive' = [workerAlive EXCEPT ![w] = FALSE]
    /\ workerPhase' = [workerPhase EXCEPT ![w] = "Active"]
    /\ terminateFlag' = TRUE
    /\ runState' = IF \A v \in Workers : v = w \/ ~workerAlive[v]
                   THEN "ReturnedStalled"
                   ELSE runState
    /\ UNCHANGED <<runnableVisible, runningVisible,
                   backendOutstanding, backendReady,
                   externalWaitRegistered, externalReady,
                   observedEpoch, idleCount, runMode, retireFired>>

RetireWorkerQuiescent(w) ==
    /\ workerAlive[w]
    /\ runState = "Active"
    /\ workerPhase[w] = "Active"
    /\ \/ Quiescent
       \/ terminateFlag
    /\ BridgeEffect(1 - wakeEpoch)
    /\ workerAlive' = [workerAlive EXCEPT ![w] = FALSE]
    /\ terminateFlag' = TRUE
    /\ retireFired' = TRUE
    /\ runState' = IF \A v \in Workers : v = w \/ ~workerAlive[v]
                   THEN IF Quiescent
                        THEN "ReturnedQuiescent"
                        ELSE "ReturnedStalled"
                   ELSE runState
    /\ UNCHANGED <<runnableVisible, runningVisible,
                   backendOutstanding, backendReady,
                   externalWaitRegistered, externalReady,
                   wakeEpoch, bridgePending, workerPhase, observedEpoch,
                   backendWaitParticipant, idleCount, runMode>>

(* =========================================================================
   Run-lifetime idle actions (E9-CORRECTIVE).
   ========================================================================= *)

ReturnStalled ==
    /\ runState = "Active"
    /\ GlobalClass = "MWS3"
    /\ ~ParkAdmitted
    /\ terminateFlag' = TRUE
    /\ runState' = "ReturnedStalled"
    /\ UNCHANGED <<runnableVisible, runningVisible,
                   backendOutstanding, backendReady,
                   externalWaitRegistered, externalReady,
                   wakeEpoch, bridgePending, workerPhase, observedEpoch,
                   backendWaitParticipant, workerAlive, idleCount,
                   runMode, retireFired>>

ReturnQuiescent ==
    /\ runState = "Active"
    /\ Quiescent
    /\ terminateFlag' = TRUE
    /\ runState' = "ReturnedQuiescent"
    /\ UNCHANGED <<runnableVisible, runningVisible,
                   backendOutstanding, backendReady,
                   externalWaitRegistered, externalReady,
                   wakeEpoch, bridgePending, workerPhase, observedEpoch,
                   backendWaitParticipant, workerAlive, idleCount,
                   runMode, retireFired>>

(* ShutdownSignal: a coordinated termination condition. Advances the wake
   epoch (bridging into a parked participant) and ends the invocation. *)
ShutdownSignal ==
    /\ runState = "Active"
    /\ BridgeEffect(1 - wakeEpoch)
    /\ terminateFlag' = TRUE
    /\ runState' = "Shutdown"
    /\ UNCHANGED <<runnableVisible, runningVisible,
                   backendOutstanding, backendReady,
                   externalWaitRegistered, externalReady,
                   workerPhase, observedEpoch, backendWaitParticipant,
                   workerAlive, idleCount, runMode, retireFired>>

(* =========================================================================
   Fiber lifecycle (collapsed).
   ========================================================================= *)

RunFiber ==
    /\ runState = "Active"
    /\ SomeActiveWorker
    /\ runnableVisible
    /\ ~runningVisible
    /\ runnableVisible' = FALSE
    /\ runningVisible' = TRUE
    /\ UNCHANGED <<backendOutstanding, backendReady,
                   externalWaitRegistered, externalReady,
                   wakeEpoch, bridgePending, workerPhase, observedEpoch,
                   backendWaitParticipant, workerAlive, idleCount,
                   terminateFlag, runMode, runState, retireFired>>

SuspendFiber ==
    /\ runState = "Active"
    /\ runningVisible
    /\ ~externalWaitRegistered
    /\ runningVisible' = FALSE
    /\ externalWaitRegistered' = TRUE
    /\ UNCHANGED <<runnableVisible, backendOutstanding, backendReady,
                   externalReady, wakeEpoch, bridgePending, workerPhase,
                   observedEpoch, backendWaitParticipant, workerAlive,
                   idleCount, terminateFlag, runMode, runState, retireFired>>

FinishFiber ==
    /\ runState = "Active"
    /\ runningVisible
    /\ runningVisible' = FALSE
    /\ UNCHANGED <<runnableVisible, backendOutstanding, backendReady,
                   externalWaitRegistered, externalReady,
                   wakeEpoch, bridgePending, workerPhase, observedEpoch,
                   backendWaitParticipant, workerAlive, idleCount,
                   terminateFlag, runMode, runState, retireFired>>

(* =========================================================================
   Next, Init, Spec
   ========================================================================= *)

vars ==
    <<runnableVisible, runningVisible,
      backendOutstanding, backendReady,
      externalWaitRegistered, externalReady,
      wakeEpoch, workerPhase, observedEpoch,
      backendWaitParticipant, bridgePending, workerAlive, idleCount,
      terminateFlag, runMode, runState, retireFired>>

TerminalStutter ==
    /\ runState # "Active"
    /\ UNCHANGED vars

(* E9-Inv2: publish-before-sleep cannot be lost — DOMAIN-AWARE (Phase G).
   A parked Worker must be able to leave park whenever wake-relevant
   persistent state is due:
     - the BACKEND participant: backendReady (its own transport) or
       bridgePending (the bridge delivered an external/routing wake);
     - a SCHEDULER-parked worker under SplitWait: the wake epoch moved
       past its commit baseline (every wake-relevant Scheduler
       publication advances the epoch; a publication before the commit
       was visible to the commit recheck), or the invocation ended.
       backendReady does NOT wake the scheduler domain (no reverse
       bridge; R1's refuse rule keeps an observer for backend progress).
     - under ~SplitWait the E9 bounded observation return remains. *)
PersistentWakeDue ==
       (externalReady /\ externalWaitRegistered)
    \/ backendReady
    \/ runnableVisible
    \/ runningVisible

(* IdleResidentStutter: a fully-parked, nothing-due Live system is the
   legitimate resident state (split-wait parks are unbounded without an
   active deadline; there is no periodic wake). It is NOT a deadlock:
   time passes with nothing to observe. Producers remain enabled and
   fair; every conditioned liveness property holds. The G1-strand states
   (persistent wake due with no reachable observer) do NOT enable this
   action — TLC's deadlock check remains the strand detector. *)
AllQuietParked ==
    \A w \in Workers :
        ~workerAlive[w] \/ workerPhase[w] = "Parked"

IdleResidentStutter ==
    /\ runState = "Active"
    /\ AllQuietParked
    /\ ~PersistentWakeDue
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
    \/ \E w \in Workers : ParticipantNoProgressExit(w)
    \/ \E w \in Workers : RetireWorkerQuiescent(w)
    \/ ReturnStalled
    \/ ReturnQuiescent
    \/ RunFiber
    \/ SuspendFiber
    \/ FinishFiber
    \/ ShutdownSignal
    \/ TerminalStutter
    \/ IdleResidentStutter

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
    /\ bridgePending = FALSE
    /\ workerAlive = [w \in Workers |-> TRUE]
    /\ idleCount = 0
    /\ terminateFlag = FALSE
    /\ runMode \in {"Drain", "Live"}
    /\ runState = "Active"
    /\ retireFired = FALSE

Spec == Init /\ [][Next]_vars

(* =========================================================================
   E9 safety invariants (spec 12) + Phase G additions.
   ========================================================================= *)

(* A parked Worker is returnable whenever ITS domain's wake authority
   owes it a return: the participant on its transport/bridge dues, a
   scheduler-parked worker on scheduler-domain publications. A quiet
   resident park (nothing due in either domain) is legal and stays. *)
Inv2NoLostWake ==
    \A w \in Workers :
        (workerPhase[w] = "Parked"
         /\ \/ (backendWaitParticipant = w /\ BackendDomainWakeDue)
            \/ (backendWaitParticipant # w /\ SchedulerDomainWakeDue))
        => LeaveParkEnabled(w)

(* E9-Inv4: external ready publication creates a wake obligation for a
   REGISTERED wait, on EVERY park domain (the bridge carries it into the
   backend participant under SplitWait). *)
Inv4ExternalReadyWakes ==
    \A w \in Workers :
        (workerPhase[w] = "Parked" /\ externalReady /\ externalWaitRegistered)
        => LeaveParkEnabled(w)

(* E9-Inv6: at most one backend blocking participant, and it is ALIVE
   (R2: a retired worker can never remain the elected participant — the
   stale-election manifestation). *)
Inv6OneBackendParticipant ==
    /\ (backendWaitParticipant = NONE
       \/ backendWaitParticipant \in Workers)
    /\ (backendWaitParticipant = NONE
       \/ workerAlive[backendWaitParticipant])

(* E9-Inv7: MIXED-WAKE liveness authority is a STRUCTURAL obligation of
   EnterPhysicalPark's BACKEND-branch precondition, not a state formula:
   under ~SplitWait the branch requires ~ExternalWakePossible AT COMMIT.
   A wait registered AFTER the commit (a Fiber on another live Worker)
   is legal — that Worker (or its resident scheduler-domain park)
   observes the external publication, which is exactly what Life7
   checks; the registered-then-parked order is unprovable at state
   level. Under SplitWait the mixed-wake BACKEND park is the DESIGN; the
   blind-wait hazard is closed by the bridge (Inv8) + Life7. *)
Inv7MixedWakeNoBlindBackendWait ==
    TRUE

(* PHASE G Inv8: the bridge never drops a SCHEDULER-DOMAIN wake
   obligation — while the backend participant is parked and a scheduler
   publication (external ready, runnable routing, running) is due, the
   one-shot control wake must still be pending (it is consumed exactly
   by the participant's own return, which also clears the participant
   slot; production: the interrupt is published under the wait-source
   mutex before the gate is re-read). backendReady is EXCLUDED: it is
   the backend domain's OWN transport, observed without the bridge. *)
Inv8BridgeReachesBackendPark ==
    ~(SplitWait /\ backendWaitParticipant # NONE /\ SchedulerDomainWakeDue)
    \/ (workerPhase[backendWaitParticipant] = "Parked" => bridgePending)

(* PHASE G Inv9 (R1/R3, the G1 strand): runnable work is never stranded
   without a reachable observer — a live Worker not yet parked, the
   backend participant, or an ended invocation. A state where every
   alive Worker is parked on the SCHEDULER domain behind a stale epoch
   while runnable work sits on a retired Worker's queue is the G1
   violation (deterministically reproduced in production by
   phase_g_g1_stranded_runnable_park_stall_reproducer). *)
Inv9NoStrandedRunnable ==
    runnableVisible
    => (  \/ \E w \in Workers :
              workerAlive[w] /\ workerPhase[w] \in {"Active", "ParkCandidate", "ParkCommitted"}
        \/ backendWaitParticipant # NONE
        \/ runState # "Active")

(* PHASE G Inv10 (R1, the progress-observer invariant) — SPLIT-WAIT
   domain only: accepted or ready backend work ALWAYS has an observer —
   the backend participant, or a live Worker still able to become one.
   The all-parked scheduler-domain state beside unguarded backend
   progress is the G1 strand class (unbounded parks, no observer). The
   REFERENCE (non-split) domain is EXEMPT by design: its bounded
   observation return (the 2ms poll, DIV-05 narrowed to reference
   backends) IS the observation authority, so an all-parked scheduler
   domain beside outstanding work is legal there. *)
Inv10BackendProgressHasObserver ==
    ~SplitWait
    \/ ~SomeBackendWork
    \/ backendWaitParticipant # NONE
    \/ (\E w \in Workers :
           workerAlive[w]
           /\ workerPhase[w] \in {"Active", "ParkCandidate", "ParkCommitted"})
    \/ runState # "Active"

Inv ==
    /\ Inv2NoLostWake
    /\ Inv4ExternalReadyWakes
    /\ Inv6OneBackendParticipant
    /\ Inv7MixedWakeNoBlindBackendWait
    /\ Inv8BridgeReachesBackendPark
    /\ Inv9NoStrandedRunnable
    /\ Inv10BackendProgressHasObserver

(* =========================================================================
   E9-LIFE run-lifetime properties (E9-CORRECTIVE spec 7).
   ========================================================================= *)
InvLife1DrainNoMW3Park ==
    (~SplitWait /\ runMode = "Drain" /\ GlobalClass = "MWS3")
    => \A w \in Workers :
        (workerPhase[w] = "Parked" => LeaveParkEnabled(w))

InvLife3LiveExternalParkAdmitted ==
    (runMode = "Live" /\ GlobalClass = "MWS3" /\ ExternalWakePossible
     /\ runState = "Active" /\ ~terminateFlag)
    => ParkAdmitted

InvLife5QuiescenceClassifierDefined ==
    runState = "ReturnedQuiescent"
    => Quiescent

InvLife ==
    /\ InvLife1DrainNoMW3Park
    /\ InvLife3LiveExternalParkAdmitted
    /\ InvLife5QuiescenceClassifierDefined

(* =========================================================================
   Liveness (spec 15) + Phase G properties.
   ========================================================================= *)

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

FairRetire ==
    /\ \A w \in Workers : WF_vars (ParticipantNoProgressExit(w))
    /\ \A w \in Workers : WF_vars (RetireWorkerQuiescent(w))

(* R2 election progress: candidacy and commit are fair (a refusal cycle
   must not starve the election that repairs it). *)
FairElect ==
    /\ \A w \in Workers : WF_vars (BeginParkCandidate(w))
    /\ \A w \in Workers : WF_vars (FinalParkRecheckAndCommit(w))

(* The loop-top / Phase-D drains are unconditional worker-loop code: a
   woken Worker always re-drains (production-test-plan determinism
   basis; without this fairness the Life7 drain obligation could be
   starved by an infinite Active stutter). *)
FairDrain ==
    /\ WF_vars (DrainExternalReady)
    /\ WF_vars (DrainBackendReady)

(* NOTE: we do NOT assume external producer fairness (ExternalReadyPublish
   eventually happens) or backend fairness (backend eventually completes)
   or shutdown fairness for the Drain-return properties. *)

LivenessSpec ==
    Spec
    /\ FairLeavePark
    /\ FairReturnStalled
    /\ FairReturnQuiescent
    /\ FairAbandon
    /\ FairObservationTimeout
    /\ FairRetire
    /\ FairElect
    /\ FairDrain

(* E9-LIFE-2: Drain MW-S3 returns (no producer/backend fairness). *)
Life2DrainMWS3Returns ==
    [] ( (runMode = "Drain" /\ GlobalClass = "MWS3" /\ runState = "Active")
          => <> (runState # "Active" \/ GlobalClass # "MWS3") )

(* E9-LIFE-4: Live non-wakeable MW-S3 returns (no park forever). *)
Life4LiveNonWakeableMWS3Returns ==
    [] ( (runMode = "Live" /\ GlobalClass = "MWS3" /\ ~ExternalWakePossible
          /\ runState = "Active")
          => <> (runState # "Active" \/ GlobalClass # "MWS3") )

(* E9-LIFE-7: External-ready Live progress — CONDITIONED on the
   publication having happened. After Live + parked + external ready
   published, eventually externalReady is drained OR the run ends. Under
   SplitWait the parked participant may be the only Worker: the bridge
   (bridgePending -> LeavePark backend branch) is then the ONLY delivery
   path; this property is the bridge's liveness obligation (and the
   model-level M1 mutation detector). *)
Life7ExternalReadyEventuallyDrained ==
    [] ( (runMode = "Live" /\ externalReady /\ externalWaitRegistered
          /\ \E w \in Workers : workerPhase[w] = "Parked")
          => <> (~externalReady \/ runState # "Active") )

(* PHASE G Life8: backend-ready is eventually observed by the parked
   participant (its own transport; no periodic wake, no reverse bridge). *)
Life8BackendReadyEventuallyObserved ==
    [] ( (backendReady /\ backendWaitParticipant # NONE
          /\ workerPhase[backendWaitParticipant] = "Parked"
          /\ runState = "Active")
          => <> (backendWaitParticipant = NONE \/ ~backendReady
                 \/ runState # "Active") )

(* Combined liveness property set. *)
LifeProps ==
    /\ Life2DrainMWS3Returns
    /\ Life4LiveNonWakeableMWS3Returns
    /\ Life7ExternalReadyEventuallyDrained
    /\ Life8BackendReadyEventuallyObserved

(* =========================================================================
   Reachability / non-vacuity witnesses (#189). NoReach* are DELIBERATELY
   FALSE at the target: TLC's counterexample is the causal witness.

   NoReachRetireFired: the ghost retireFired is set ONLY inside
   RetireWorkerQuiescent's action body - which conjoins BridgeEffect - so a
   witness trace with retireFired = TRUE proves the full as-built retire
   step executed, INCLUDING the unconditional departure wake
   (wakeEpoch' = 1 - wakeEpoch; the C++ signal_wake_locked() in the worker
   epilogue, scheduler.cpp:1242-1249). Pre-#189 the action was
   unsatisfiable (BridgeEffect primed wakeEpoch'/bridgePending' while the
   action's own UNCHANGED pinned them); the witness fails closed.

   NoReachQuiescentTerminate: the terminal chain - every worker retired and
   the invocation classified ReturnedQuiescent at true quiescence. This
   final shape is reachable ONLY through a last-alive RetireWorkerQuiescent
   step (ReturnQuiescent never retires a worker; ParticipantNoProgressExit
   classifies ReturnedStalled), and it pins the C++-faithful terminal
   classification: a retire that ends the run at NON-quiescence must
   classify ReturnedStalled (the terminate-observed / E4-E5 caller-re-entry
   boundary), never ReturnedQuiescent (InvLife5). *)
NoReachRetireFired ==
    ~retireFired

NoReachQuiescentTerminate ==
    ~( /\ \A v \in Workers : ~workerAlive[v]
       /\ runState = "ReturnedQuiescent"
       /\ Quiescent )
====
