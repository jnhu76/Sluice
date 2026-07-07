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

- `E9ParkWake.tla`                 — the correct protocol (Model P3).
- `E9ParkWake.cfg`                 — TLC config (safety invariants).
- `E9ParkWakeBuggyPrePark.tla/.cfg` — negative model A: lost external wake
  (the producer publishes the ready flag but drops the signal; LeavePark
  is signal-only). Produces an `Inv2NoLostWake` counterexample.
- `E9ParkWakeBuggyMixedSource.tla/.cfg` — negative model B: blind
  backend wait (the MW-S2 participant enters the BACKEND domain even when
  external wake is possible). Produces an `Inv7StateForm` counterexample.
- `README.md`                      — this file + refinement map.

## Running

```
java -cp /tmp/tla2tools.jar tlc2.TLC \
  -config docs/spec/e9_park_wake/E9ParkWake.cfg \
  docs/spec/e9_park_wake/E9ParkWake
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
```

Persistent state is kept SEPARATE from the wake signal/epoch. The wake
notification is NOT the source of truth (E9-Inv3).

## Results

| model | distinct states | depth | result |
|-------|----------------:|------:|--------|
| `E9ParkWake` (correct, P3) | 9216 | 21 | **all configured invariants PASS — no error** |
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

## Liveness (E9 spec §15) — documented, not in the gate cfg

The model defines `FairLeavePark`, `ExternalWakeLiveness`, and
`MixedWakeLiveness` as temporal properties under `LivenessSpec`. They are
NOT in the committed gate cfg because the safety invariants already
encode the load-bearing obligations (Inv2: a parked Worker's wake is
always enabled; Inv7 transition obligation: no blind backend wait), and
the finite-state 1-bit-epoch model's liveness would require a stricter
fairness discipline than the abstract protocol needs. The fairness
assumptions are documented here:

```
WF_<<workerPhase[w]>>(LeavePark(w))   -- a woken Worker eventually leaves
                                         park (scheduler fairness; the OS
                                         cv/notify does not strand a
                                         thread that has been signaled).

External producer eventually publishes: modeled as the existence of the
ExternalReadyPublish action; not a fairness assumption on the producer
(the producer may legitimately never complete -- MW-S3 quiescence).

Backend eventually completes: NOT assumed. Inv7's whole point is that
external wake must NOT depend on backend completion. The BuggyMixedSource
counterexample does not assume the backend completes.
```

## Refinement map (TLA+ → production, E9 spec §17)

| Formal concept/action | Production path / proposed seam | authority / domain |
| --------------------- | ------------------------------- | ------------------ |
| `wakeEpoch` | `Scheduler::wake_epoch_` (`std::atomic`, advanced under `wake_mtx_`) | wake-source authority (commit-to-sleep window) |
| `observedEpoch[w]` | `Scheduler::observed_epoch_[w]` (recorded under `wake_mtx_` at commit) | per-Worker park predicate |
| `BeginParkCandidate` | `worker_loop`: no-local-work branch → set `admission_ = candidate` under `global_mtx_` | E7 admission reused |
| `FinalParkRecheckAndCommit` | E7 Phase-B re-drain + reclassify + (E9 NEW) record `observed_epoch_` + epoch validation | commit decision |
| `EnterPhysicalPark` | E9 NEW `enter_physical_park(w)`: choose domain (BACKEND iff MW-S2 & ~external-wake-capable & no participant; else SCHEDULER); SCHEDULER domain = `wake_cv_.wait_for(lk, bounded_T, pred)` | physical wait |
| `LeavePark` | `wake_cv_` wake return OR `ctx_.wait_one()` return → re-drain, reclassify, loop | wake observation |
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
