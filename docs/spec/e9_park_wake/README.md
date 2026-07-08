# E9 Scheduler Park Admission and Unified Wake-Source Protocol — TLA+ Model

Narrow TLA+ model of the E9 park-admission and wake-source protocol,
realizing the architecture decision in
`docs/adr/ADR-execution-model.md` §9.4 (Model P3, decoupled wake domains).

The load-bearing E9 question:

```text
When may an idle Scheduler Worker commit to parking, and which state
publications create an obligation to wake parked Workers?
```

Answer (P3): a Worker commits to park only after a globally-coordinated
admission (drain persistent readiness → classify → observe wake epoch →
validate before sleep). The wake epoch closes the commit-to-physical-wait
window; **persistent state is the lost-wake authority** (E9-Inv3). There
are two park domains — BACKEND (the E7 MW-S2 `ctx_.wait_one()` participant,
at most one) and SCHEDULER (the wake source, any number of Workers). A
Worker parks on the SCHEDULER domain whenever an external-wake-capable
wait is registered; this is the MIXED-WAKE fix (the MW-S2 participant
yields backend-wait privilege when external wake is possible).

## Files

- `E9ParkWake.tla`                 — the correct protocol (Model P3 +
  RunMode, E9-CORRECTIVE).
- `E9ParkWake.cfg`                 — TLC config (safety invariants).
- `E9ParkWakeLiveness.cfg`         — TLC config (liveness properties
  E9-LIFE-2/4/7 under `LivenessSpec`).
- `E9ParkWakeBuggyDrainParks.tla/.cfg` — negative model C (E9-CORRECTIVE):
  the shipped Drain-park defect. Park admission admits Drain+MW-S3+
  external-wake, and LeavePark is signal-only (no observation return).
  Produces a `Life2Buggy` counterexample reproducing the deterministic
  Drain-mode hang.
- `E9ParkWakeBuggyPrePark.tla/.cfg` — negative model A: lost external wake
  (the producer publishes the ready flag but drops the signal; LeavePark
  is signal-only). Produces an `Inv2NoLostWake` counterexample.
- `E9ParkWakeBuggyMixedSource.tla/.cfg` — negative model B: blind
  backend wait (the MW-S2 participant enters the BACKEND domain even when
  external wake is possible). Produces an `Inv7StateForm` counterexample.
- `README.md`                      — this file + refinement map.

NOTE: BuggyPrePark and BuggyMixedSource predate E9-CORRECTIVE and do not
model `runMode`/`runState`. Their correspondence to the original defects
(lost wake, blind mixed-source) remains valid; they document different
defects than BuggyDrainParks. They are kept; do not delete them.

## Running

```
java -cp /tmp/tla2tools.jar tlc2.TLC \
  -config docs/spec/e9_park_wake/E9ParkWake.cfg \
  docs/spec/e9_park_wake/E9ParkWake
java -cp /tmp/tla2tools.jar tlc2.TLC \
  -config docs/spec/e9_park_wake/E9ParkWakeLiveness.cfg \
  docs/spec/e9_park_wake/E9ParkWake
java -cp /tmp/tla2tools.jar tlc2.TLC \
  -config docs/spec/e9_park_wake/E9ParkWakeBuggyDrainParks.cfg \
  docs/spec/e9_park_wake/E9ParkWakeBuggyDrainParks
java -cp /tmp/tla2tools.jar tlc2.TLC \
  -config docs/spec/e9_park_wake/E9ParkWakeBuggyPrePark.cfg \
  docs/spec/e9_park_wake/E9ParkWakeBuggyPrePark
java -cp /tmp/tla2tools.jar tlc2.TLC \
  -config docs/spec/e9_park_wake/E9ParkWakeBuggyMixedSource.cfg \
  docs/spec/e9_park_wake/E9ParkWakeBuggyMixedSource
```

## Model domain (finite, exhaustive TLC)

```
Workers = {W0, W1}
Fibers  = {F0}   (one external-wait Fiber is the load-bearing proof)
```

`wakeEpoch` is modeled as a 1-bit toggle (`1 - wakeEpoch`) rather than a
monotonic natural. Every invariant reasons about `wakeEpoch #
observedEpoch[w]` ("did a wake-relevant publication happen after I
observed?"), which a 1-bit toggle preserves exactly. A monotonic counter
would make the state space infinite and TLC non-terminating. (Consequence:
two benign publishes can flip parity back — this is why Inv2's authority
is the PERSISTENT wake predicate, not the epoch parity.)

## State dimensions

```
runnableVisible, runningVisible          (global executable work)
backendOutstanding, backendReady         (backend progress)
externalWaitRegistered, externalReady    (external Future source)
wakeEpoch                                (1-bit toggle; authority for the
                                          commit-to-sleep window)
workerPhase[w] in {Active, ParkCandidate, ParkCommitted, Parked}
observedEpoch[w]                         (epoch at commit)
backendWaitParticipant                   (in {NONE, W0, W1})
runMode   in {Drain, Live}               [E9-CORRECTIVE: invocation policy]
runState  in {Active, ReturnedStalled, ReturnedQuiescent, Shutdown}
                                          [E9-CORRECTIVE: invocation lifetime]
```

Persistent state is kept SEPARATE from the wake signal/epoch. The wake
notification is NOT the source of truth (E9-Inv3).

The `runMode`/`runState` axes (E9-CORRECTIVE) are the invocation/lifetime
dimension that the original model omitted. `ClassifyGlobalState`
(`GlobalClass`) is SEPARATE from `SelectIdleAction` (`ParkAdmitted`,
`ReturnStalled`, `ReturnQuiescent`): the classifier is one authoritative
taxonomy; runMode only selects the idle action after classification.

## Results

| model | distinct states | depth | result |
|-------|----------------:|------:|--------|
| `E9ParkWake` (correct, P3+RunMode) safety | 18456 | 25 | **all invariants PASS — no error** |
| `E9ParkWakeLiveness` (Life2/4/7) | 18528 | 25 | **all temporal properties PASS** |
| `E9ParkWakeBuggyDrainParks` (Drain-park defect) | 18528 | 11 | **`Life2Buggy` counterexample** (the hang) |
| `E9ParkWakeBuggyPrePark` (lost wake) | 206 | 6 | **`Inv2NoLostWake` counterexample** |
| `E9ParkWakeBuggyMixedSource` (blind backend) | 644 | 8 | **`Inv7StateForm` counterexample** |

### Correct-model safety invariants (all PASS)

- **E9-Inv1** Park commit requires no executable work — STRUCTURAL
  (FinalParkRecheckAndCommit precondition `~ExecutableWork`). Not a global
  state invariant: work may become visible after a valid park (spec §12
  warning). Inv2 catches the post-commit case.
- **E9-Inv2** `Inv2NoLostWake`: if any persistent wake is due
  (`PersistentWakeDue`), every parked Worker's `LeavePark` is enabled.
  Authority = persistent state (not epoch parity, because of the toggle).
- **E9-Inv3** Wake signal is not authority — structural (LeavePark and
  the producers never erase persistent state via the signal path).
- **E9-Inv4** `Inv4ExternalReadyWakes`: registered external-ready while
  parked ⇒ `LeavePark` enabled. (Sub-case of Inv2; stated for clarity.)
- **E9-Inv6** `Inv6OneBackendParticipant`: `backendWaitParticipant` is
  NONE or a single Worker (structural — single variable, not a set). At-
  most-one backend participant is by construction; other Workers may park
  in the SCHEDULER domain concurrently (P3).
- **E9-Inv7** MIXED-WAKE no blind backend wait — TRANSITION OBLIGATION,
  structural in EnterPhysicalPark's BACKEND-branch precondition
  `~ExternalWakePossible`. The state form (`Inv7StateForm`) is NOT a true
  invariant of the correct protocol (a Worker may lawfully enter BACKEND
  before an external wait is registered, then a Fiber running on another
  Worker may register one); it is kept ONLY so the BuggyMixedSource model
  can prove the defect makes the blind wait REACHABLE.
- **E9-Inv8/9/10** structural (shutdown advances the epoch; spurious wake
  re-drains via LeavePark's persistent clauses; park state is never
  conflated with quiescence — there is no Quiescent variable).

### E9-LIFE run-lifetime properties (E9-CORRECTIVE, all PASS)

- **E9-LIFE-1** `InvLife1DrainNoMW3Park` (DECISION OBLIGATION): a Drain
  run never STRANDS a Parked Worker under MW-S3 — every Parked Worker
  has `LeavePark` enabled (bounded observation return).
- **E9-LIFE-2** `Life2DrainMWS3Returns` (TEMPORAL): a Drain run that
  reaches MW-S3 eventually ends (Stalled/Quiescent/Shutdown) or leaves
  MW-S3 via legitimate progress. No producer/backend/shutdown fairness.
- **E9-LIFE-3** `InvLife3LiveExternalParkAdmitted` (DECISION OBLIGATION):
  in Live + MW-S3 + effective external wake, park IS admitted.
- **E9-LIFE-4** `Life4LiveNonWakeableMWS3Returns` (TEMPORAL): Live +
  MW-S3 without effective external wake ends the run or leaves MW-S3.
- **E9-LIFE-5** `InvLife5QuiescenceClassifierDefined` (STATE INVARIANT):
  `ReturnedQuiescent` implies no executable work, no backend, no wait
  registration (quiescence is classifier-defined, not park-defined).
- **E9-LIFE-6** (STRUCTURAL): no action mutates `runMode` because a wake
  handle is created/copied/retained/signaled/invalidated.
- **E9-LIFE-7** `Life7ExternalReadyEventuallyDrained` (TEMPORAL): after
  Live + parked + external-ready published, externalReady is eventually
  drained (or the run ends).
- **E9-LIFE-8** (TRANSITION OBLIGATION): the SCHEDULER-domain bounded
  observation park has an always-enabled `LeavePark` (the bounded
  `wake_cv_` timeout returns regardless of a signal). This is the
  authority for backend observation in MIXED-WAKE.
- **E9-LIFE-9** (STRUCTURAL): the external producer is signal-only
  (ExternalReadyPublish changes only `externalReady` + `wakeEpoch`).
- **E9-LIFE-10** (STRUCTURAL): E7/E8 publication/ownership protocols
  remain invariant (closed; E9 does not modify them).

### BuggyPrePark counterexample (lost wake)

```
State 5: SuspendFiber registers the external wait.
State 6-7: a Worker elects + commits + physically parks (externalReady
           still FALSE at commit — admission preconditions met).
State 8: ExternalReadyPublish sets externalReady=TRUE but does NOT
        advance wakeEpoch (the signal is LOST).
        => Inv2 violated: PersistentWakeDue is true (externalReady /\
           externalWaitRegistered), but LeavePark is signal-only and the
           signal was dropped — the parked Worker can never leave.
```

This is the classic pre-park / lost-notification race: a publication that
happens after the Worker committed to park but whose notification is
lost. The correct model closes it by (a) LeavePark re-draining persistent
state, and (b) the wake epoch being advanced by every wake-relevant
producer.

### BuggyMixedSource counterexample (blind backend wait)

```
State 4: SubmitBackend (backendOutstanding := TRUE).
State 5: SuspendFiber registers the external wait (backendOutstanding
           stays TRUE -> MIXED-WAKE).
State 6-7: a Worker elects + commits + physically parks.
State 8: EnterPhysicalPark chooses the BACKEND domain (defect: the
           ~ExternalWakePossible guard is omitted), so
           backendWaitParticipant = W0 while externalWaitRegistered = TRUE.
        => Inv7StateForm violated: ExternalWakePossible /\
           backendWaitParticipant # NONE. The MW-S2 participant is
           blocked on backend progress only; an external-ready
           publication cannot interrupt it (MIXED-WAKE strand).
```

The correct model's EnterPhysicalPark BACKEND branch requires
`~ExternalWakePossible`, forcing the participant onto the SCHEDULER
domain (wake-epoch wakeable) whenever an external wait is registered.

### BuggyDrainParks counterexample (E9-CORRECTIVE — the shipped hang)

```
State 1-3: PublishRunnable -> RunFiber -> SuspendFiber registers the
            external wait (runMode = Drain).
State 4-9:  the run reaches MW-S3 (externalWaitRegistered = TRUE,
            no executable work, no backend outstanding).
State 9-10: a Worker elects (BeginParkCandidateBuggy) + commits
            (FinalParkRecheckAndCommitBuggy) + enters physical park
            (EnterPhysicalParkBuggy). DEFECT: ParkAdmittedBuggy admits
            the Drain+MW-S3+external-wake park; the EnterPhysicalPark
            predicate has no Drain/MW-S3 idle-action re-selection.
State 11:   both Workers Parked under MW-S3, no producer acts.
            LeaveParkBuggy is signal-only -> not enabled (no wake due).
            => Life2Buggy violated: the Drain run is stuck with Parked
               workers under MW-S3 and NEVER reaches a terminal state.
               This is the deterministic Drain-mode hang.
```

The correct model closes this by (a) `ParkAdmitted` admitting MW-S3 park
ONLY in Live + effective external wake (Drain returns Stalled), (b)
`EnterPhysicalPark` re-selecting the idle action (a Drain worker that
finds MW-S3 at the physical wait returns to Active → ReturnStalled), and
(c) `LeavePark` always-enabled (the bounded observation return,
E9-LIFE-8), so no parked worker can be stranded.

## Liveness (E9-CORRECTIVE — now in the gate cfg)

`E9ParkWakeLiveness.cfg` checks the load-bearing temporal properties
under `LivenessSpec`:

```
WF_vars(LeavePark(w))      -- a woken Worker eventually leaves park
WF_vars(ReturnStalled)     -- the run eventually returns Stalled
WF_vars(ReturnQuiescent)   -- the run eventually returns Quiescent
WF_vars(AbandonParkCandidate(w))
WF_vars(EnterPhysicalPark(w))  -- the bounded observation return
```

NO producer/backend/shutdown fairness is assumed for the Drain-return
properties. `Life7ExternalReadyEventuallyDrained` is conditioned on
externalReady having ALREADY been published (producer fairness is not
used to make publication happen).

## Refinement map (TLA+ → production, E9 spec §17, E9-CORRECTIVE extended)

| Formal concept/action | Production path / proposed seam | authority / domain |
| --------------------- | ------------------------------- | ------------------ |
| `runMode` | explicit `RunMode{drain,live}` invocation policy; `run(n)`→drain, `run_live(n)`→live | invocation lifetime contract |
| `runState` | invocation classification (the `run()` return path: Active→ all-idle final recheck sets `global_terminate_` and returns) | invocation lifetime |
| `GlobalClass` (MW-S1/S2/S3/QUIESCENT) | `Scheduler::classify_locked()` (unchanged by runMode) | global classifier |
| `SelectIdleAction` / `ParkAdmitted` | the worker-loop idle-action branch after `classify_locked`, gated by `run_mode` | idle-action selection |
| `ReturnStalled` | Drain MW-S3 (and Live MW-S3 non-external) return/termination path | run lifetime |
| `ReturnQuiescent` | true quiescence return path | run lifetime |
| `wakeEpoch` | `Scheduler::wake_epoch_` (`std::atomic`, advanced under `wake_mtx_`) | wake-source authority (commit-to-sleep window) |
| `observedEpoch[w]` | `WorkerState::observed_epoch` (recorded under `wake_mtx_` at commit) | per-Worker park predicate |
| `BeginParkCandidate` | `worker_loop`: no-local-work branch → set `admission_ = candidate` under `global_mtx_` | E7 admission reused |
| `FinalParkRecheckAndCommit` | E7 Phase-B re-drain + reclassify + (E9 NEW) record `observed_epoch` + epoch validation | commit decision |
| `EnterPhysicalPark` | `park_on_wake_source(w)` (SCHEDULER domain) / `ctx_.wait_one()` (BACKEND domain); E9-CORRECTIVE: idle-action re-selection at the physical wait | physical wait |
| `LeavePark` | `wake_cv_.wait_for` bounded-timeout return OR signal return → re-drain, reclassify, loop | wake observation |
| `ObservationTimeout` | the bounded `wake_cv_.wait_for(... 2ms ...)` timeout — LOAD-BEARING for MIXED-WAKE backend observation (E9-LIFE-8) | bounded observation return |
| `SignalWake` | `Scheduler::signal_wake_locked()` under `wake_mtx_`: `++wake_epoch_`; `wake_cv_.notify_all()` | wake-source signal |
| `ExternalReadyPublish` | `Future::complete_with` (sets `ready_`) → `SchedulerWakeHandle::notify()` → `signal_wake_locked()` | external persistent + signal |
| `DrainExternalReady` | `Scheduler::wake_ready_flags_locked()` (erases reg, `make_runnable`, `route_runnable_locked` by `WaitReg.owner`) | Scheduler drains |
| `PublishRunnable` | existing `spawn`/`route_runnable_locked` + (E9 NEW) `signal_wake_locked()` | runnable publication + signal |
| `BackendReadyPublish` | `ThreadPoolBackend`/`UringAsyncBackend` reap → `Completion` ready | backend persistent |
| `DrainBackendReady` | `Scheduler::wake_ready_completions_locked()` (erases reg, `make_runnable`, route by `WaitReg.owner`) | Scheduler drains |
| `EnterBackendWait` | E7 MW-S2 committed participant `ctx_.wait_one()` — unchanged when ~external-wake-capable | E7 backend wait |
| `BackendWaitReturn` | `ctx_.wait_one()` return → re-drain | backend wake |
| `SubmitBackend` | Fiber body `ctx_.submit_*` (sets `Completion` outstanding, `ctx_.outstanding()>0`) | backend op ingress |
| `RunFiber`/`SuspendFiber`/`FinishFiber` | `run_next_on` / `await_*` / `fiber_entry_bridge` (E7/E8, unchanged) | Fiber lifecycle |
| `ShutdownSignal` | `global_terminate_ = true` + (E9) `signal_wake_locked()` + `inbox_cv.notify_all()` | termination wake |
| `backendWaitParticipant` | E7 `admission_ == committed` + `admission_owner_` (the single MW-S2 participant) | at-most-one backend waiter |
| `ExternalWakePossible` | `!waiting_ready_.empty()` (a registered ready-flag wait) | external-wake-capability test |
| `LatentExternalWork` | a `waiting_ready_` entry whose flag loads true (drained by `wake_ready_flags_locked`) | latent executable work |

### External-producer boundary (E9 §18, mandatory review)

The external producer touches ONLY:

```
Future::complete_with  (publishes ready_)
SchedulerWakeHandle::notify  (signal_wake_locked: ++wake_epoch_, notify)
```

It MUST NOT call `make_runnable`, `route_runnable_locked`, push to
`local_runnable`, erase a wait registration, or call any `AsyncIoContext`
method. The weak/generation-invalidated wake handle (ADR §9.4.10) makes a
post-Scheduler-destruction `notify()` a safe no-op.

### Physical wake sets per blocking action (E9-CORRECTIVE §16)

For every blocking action, the physical signal wake set is explicit:

```
SCHEDULER-domain bounded park (park_on_wake_source, wake_cv_.wait_for 2ms):
    direct signal wake set:
        wake epoch advance (route_runnable_locked / notify_external_wake)
        shutdown (global_terminate_)
    bounded observation return:
        the 2ms wake_cv_ timeout (LOAD-BEARING for MIXED-WAKE backend
        observation, E9-LIFE-8 / ADR §9.4.7.1)

BACKEND-domain park (ctx_.wait_one, MW-S2 backend-only):
    direct signal wake set:
        backend CV (ThreadPoolBackend cv_) / io_uring CQE / Fake poll
    (NOT the Scheduler wake source; backend readiness does not directly
     signal wake_cv_ in the E9 baseline.)

MIXED-WAKE (backendOutstanding + external wait registered):
    the participant parks on the SCHEDULER domain (NOT backend wait_one),
    so backend readiness is observed via the bounded observation return,
    NOT a direct physical signal to wake_cv_.
```

The unification across disjoint wake domains is **post-observation drain +
authoritative global reclassification**, NOT one physical wake primitive.
Do NOT claim unified backend-to-wake_cv notification (ADR §9.4.7.1).

## What this model does NOT cover

- E10 WaitNode / cancellation-safe wait queue (next frontier).
- A backend-facing interruptible wait seam (P5; reserved as E9-B1 only if
  P3 proves insufficient under the load-bearing tests).
- eventfd-in-ring (P6; deferred).
- wake_one routing refinement (notify_all baseline; ADR §9.4.8).
- Chase-Lev / lock-free deques (E16).
- Timers (E11).

## Reproducible verification

The committed `.cfg` files reproduce the gate above from a fixed
`tla2tools.jar`. No ad hoc flags. The E9 stability runner
(`scripts/verify-e9-stability.sh`, added in E9-C) covers the production
stress gates; the TLA+ gate is the formal correctness proof.
