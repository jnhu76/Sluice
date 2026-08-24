# E9 Scheduler Park Admission and Unified Wake-Source Protocol — TLA+ Model

Narrow TLA+ model of the E9 park-admission and wake-source protocol,
realizing the architecture decision in
`docs/adr/ADR-execution-model.md` §9.4 (Model P3, decoupled wake domains),
updated at the Phase G closeout (2026-08-15) to the R1–R4 G1-repair rules,
the split-wait park domain, and the Scheduler→backend interrupt bridge
(`docs/design/phase-g-backend-progress-wake.md`).

The load-bearing E9 question:

```text
When may an idle Scheduler Worker commit to parking, and which state
publications create an obligation to wake parked Workers?
```

Answer (P3 + Phase G): a Worker commits to park only after a globally-
coordinated admission (drain persistent readiness → classify → observe
wake epoch → validate before sleep). There are two park domains —
BACKEND (`ctx_.wait_one()`, at most one participant, the E7 MW-S2 rule)
and SCHEDULER (the wake source, any number of Workers). **Phase G
(P5-CORRECTIVE): a SPLIT-WAIT backend (ThreadPool / real io_uring) parks
the MW-S2 participant in the BACKEND domain for BOTH backend-only and
MIXED-WAKE** — its progress transport is prompt, and external Scheduler
publications reach that park through the BRIDGE
(`signal_wake_locked → interrupt_backend_waiters`, modeled as the
one-shot `bridgePending` with the D4-RM14 commit-to-park persistence).
A reference (non-split) backend keeps the E9 rule: MIXED-WAKE parks on
the SCHEDULER domain with the bounded observation return (DIV-05,
narrowed to reference backends). R1 refuses a scheduler-domain park
commit beside unguarded progress (no observer); R2 makes the backend
participant election transferable (lowest ALIVE worker); R3's retire
never loses runnable work and publishes an unconditional wake;
R4's first-counted idle park signals the domain (the E9-LIFE-8
convergence obligation; the contribution-aware damping is abstracted
to that obligation).

## Files

- `E9ParkWake.tla`                 — the correct protocol (P3 + RunMode +
  Phase G R1–R4 + bridge + terminate protocol).
- `E9ParkWake.cfg`                 — TLC config (safety, `SplitWait=TRUE`).
- `E9ParkWakeLiveness.cfg`         — TLC config (liveness Life2/4/7/8,
  `SplitWait=TRUE`).
- `E9ParkWakeReference.cfg`        — TLC config (safety, `SplitWait=FALSE`
  — the reference/legacy E9 rule retained for Fake/Sync backends).
- `E9ParkWakeReferenceLiveness.cfg`— TLC config (liveness, reference).
- `E9ParkWakeWitnessRetire.cfg`    — #189 non-vacuity witness (1/2):
  `RetireWorkerQuiescent` fires with the departure wake.
- `E9ParkWakeWitnessTerminate.cfg` — #189 non-vacuity witness (2/2): the
  all-retired `ReturnedQuiescent`-at-quiescence terminal chain.
- `E9ParkWakeWitnessPnpExit.cfg`   — #191 non-vacuity witness (1/2):
  `ParticipantNoProgressExit` fires with the departure wake.
- `E9ParkWakeWitnessPnpEndedRun.cfg`— #191 non-vacuity witness (2/2): the
  last-alive `ReturnedStalled` classification.
- `E9ParkWakeBuggyNoBridge.tla/.cfg` — negative model (Phase G): the
  model-level M1 mutation — wake publications advance `wakeEpoch` but
  never set `bridgePending`. Produces an `Inv8BridgeReachesBackendPark`
  counterexample (a parked participant that can never observe an
  external publication). GENERATED from the current positive model by
  `_gen_neg.py` (#192; freshness-gated, do not edit by hand).
- `E9ParkWakeNegRetireDead.tla/.cfg` — negative model (#189 fail-closed
  witness control): the EXACT pre-#189 defect — `RetireWorkerQuiescent`'s
  `UNCHANGED` reacquires `wakeEpoch, bridgePending` while the action body
  conjoins `BridgeEffect` (double-prime contradiction). Both witness
  invariants HOLD (PASS). GENERATED from the current positive model by
  `_gen_neg.py` (#192; freshness-gated, do not edit by hand).
- `E9ParkWakeNegParticipantDead.tla/.cfg` — negative model (#191 fail-closed
  witness control): the EXACT pre-#191 defect — `ParticipantNoProgressExit`
  conjoins `BridgeEffect` while its body assigns `bridgePending' = FALSE`
  (double-prime contradiction). Both witness invariants HOLD (PASS).
  GENERATED from the current positive model by `_gen_neg.py` (#191;
  freshness-gated, do not edit by hand).
- `_gen_neg.py`                  — generates the duplicated-snapshot
  negatives (`NegRetireDead`, `BuggyNoBridge`, `NegParticipantDead`) from
  the current `E9ParkWake.tla`; `--check` is the read-only freshness gate
  the verifier runs before any TLC invocation (#192, #191).
- `E9ParkWakeBuggyDrainParks.tla/.cfg` — negative model C (E9-CORRECTIVE,
  historical): the shipped Drain-park defect. `Life2Buggy`
  counterexample.
- `E9ParkWakeBuggyPrePark.tla/.cfg` — negative model A (historical): lost
  external wake. `Inv2NoLostWake` counterexample.
- `E9ParkWakeBuggyMixedSource.tla/.cfg` — negative model B (historical):
  blind backend wait. `Inv7StateForm` counterexample.
- `README.md`                      — this file + refinement map.

NOTE: BuggyPrePark and BuggyMixedSource predate E9-CORRECTIVE and model
the pre-Phase-G protocol; their correspondence to the original defects
remains valid. They are kept as historical negative controls; do not
delete them.

## Running

```
bash scripts/formal/verify-e9-park-wake.sh
```

(four positive gates + four reachability-witness gates + six negative
gates; TLC runs in an isolated mktemp workspace — never in this
directory.)

## Generated negatives + freshness gate (#192, #191)

`NegRetireDead`, `BuggyNoBridge`, and `NegParticipantDead` are FULL
DUPLICATED SNAPSHOTS of the positive model, so they are GENERATED from the
current `E9ParkWake.tla` by `_gen_neg.py` (each mutation is a declared
exact-fragment swap whose anchor must match exactly one location — zero or
multiple matches aborts generation fail-closed) and freshness-gated:
`verify-e9-park-wake.sh` runs `_gen_neg.py --check` BEFORE any TLC run,
so a stale, missing, or unexpected generated artifact fails the formal
gate without starting TLC (a stale negative would keep checking an
outdated mutation while CI stays green). Never edit those six files by
hand — regenerate with `python3 spec/tla/e9_park_wake/_gen_neg.py`.

`BuggyPrePark` / `BuggyMixedSource` (pre-Phase-G protocol snapshots) and
`BuggyDrainParks` (an EXTENDS-based minimal mutant that auto-tracks the
positive model) are intentionally NOT generated: the first two are
frozen historical controls, the third has no drift risk. A future
negative that is a single-rule mutation of the current model should
join the generator's CASES — mutation spec + cfg + verifier expectation —
not add another hand-copied snapshot.

## Model domain (finite, exhaustive TLC)

```
Workers = {W0, W1}
Fibers  = {F0}   (one external-wait Fiber is the load-bearing proof)
SplitWait ∈ {TRUE, FALSE}  (CONSTANT: production split-wait vs reference)
```

`wakeEpoch` is modeled as a 1-bit toggle (`1 - wakeEpoch`) rather than a
monotonic natural. (Consequence: two benign publishes can flip parity
back — this is why the park-return authority in this model is the
PERSISTENT per-domain wake state, not the epoch parity; production's
monotonic `wake_epoch_` makes "publication ⇒ returnable" literal.)

## State dimensions

```
runnableVisible, runningVisible          (global executable work)
backendOutstanding, backendReady         (backend progress)
externalWaitRegistered, externalReady    (external Future source)
wakeEpoch                                (1-bit toggle; commit-to-sleep
                                          window authority)
workerPhase[w] in {Active, ParkCandidate, ParkCommitted, Parked}
observedEpoch[w]                         (epoch at commit)
backendWaitParticipant                   (in {NONE, W0, W1})
bridgePending                            [PHASE G] one-shot control wake
                                          to the backend participant
workerAlive[w]                           [R2/R3] thread-in-loop liveness
idleCount                                [R4] counted idle parks (0..2)
terminateFlag                            [R3] global_terminate_ publication
runMode   in {Drain, Live}               [E9-CORRECTIVE: invocation policy]
runState  in {Active, ReturnedStalled, ReturnedQuiescent, Shutdown}
                                          [E9-CORRECTIVE: invocation lifetime]
```

Init explores BOTH run modes (the pre-closeout model fixed `runMode =
"Drain"` with no mutator, leaving every Live-conditioned property
vacuously true — fixed at the closeout).

## Results (2026-08-15 closeout, tla2tools from the repo bootstrap)

| model / config | result |
|----------------|--------|
| `E9ParkWake` safety, split-wait (14472 distinct states) | **all invariants PASS — no error, no deadlock** |
| `E9ParkWake` liveness, split-wait | **Life2/4/7/8 PASS** |
| `E9ParkWake` safety, reference (15376 distinct states) | **all invariants PASS** |
| `E9ParkWake` liveness, reference | **Life2/4/7/8 PASS** |
| `E9ParkWakeBuggyNoBridge` | **`Inv8BridgeReachesBackendPark` counterexample** |
| `E9ParkWakeBuggyDrainParks` | **`Life2Buggy` counterexample** |
| `E9ParkWakeBuggyPrePark` | **`Inv2NoLostWake` counterexample** |
| `E9ParkWakeBuggyMixedSource` | **`Inv7StateForm` counterexample** |

### Correct-model safety invariants (all PASS)

- **E9-Inv2** `Inv2NoLostWake` (Phase G, domain-aware): a parked Worker is
  returnable whenever ITS domain's wake authority owes a return — the
  backend participant on `backendReady` (its own transport) or
  `bridgePending` (the bridge); a scheduler-parked worker on
  scheduler-domain persistent publications or the invocation end. A quiet
  resident park is legal and stays.
- **E9-Inv4** `Inv4ExternalReadyWakes`: registered external-ready while
  parked ⇒ returnable (sub-case of Inv2 + Inv8 for the participant).
- **E9-Inv6** `Inv6OneBackendParticipant`: at most one backend
  participant, and it is ALIVE (R2: a retired worker can never remain the
  elected participant).
- **E9-Inv7** MIXED-WAKE authority — structural in `EnterPhysicalPark`'s
  BACKEND-branch precondition (`~ExternalWakePossible` at commit under
  `~SplitWait`). Under `SplitWait` the mixed-wake BACKEND park is the
  design; the blind-wait hazard is closed by Inv8 + Life7 instead.
- **Inv8BridgeReachesBackendPark** [PHASE G]: while the backend
  participant is parked and a scheduler-domain publication is due, the
  one-shot control wake is still pending (consumed exactly by the
  participant's own return). `backendReady` is excluded — it is the
  backend domain's own transport.
- **Inv9NoStrandedRunnable** [R1/R3, the G1 strand]: runnable work
  always has a reachable observer — a live not-yet-parked Worker, the
  backend participant, or an ended invocation. The deterministic
  production reproducer is `phase_g_g1_stranded_runnable_park_stall_-
  reproducer`.
- **Inv10BackendProgressHasObserver** [R1, split-wait domain]: accepted
  or ready backend work always has an observer. The REFERENCE domain is
  exempt by design (its bounded observation return IS the observation
  authority — DIV-05 narrowed to reference backends).
- **InvLife1** scoped to the reference domain under SplitWait (the
  Drain-MW-S3 park-return obligation is temporal there — Life2).

### Liveness properties (all PASS)

- **Life2** Drain MW-S3 eventually returns (no producer/backend
  fairness).
- **Life4** Live non-wakeable MW-S3 eventually returns.
- **Life7** external-ready eventually drained after publication — under
  `SplitWait` this is the BRIDGE's liveness obligation (the parked
  participant may be the only Worker; the bridge is then the only
  delivery path). The model-level M1 detector.
- **Life8BackendReadyEventuallyObserved** [PHASE G]: the parked
  participant eventually observes backend readiness (its own transport;
  no periodic wake, no reverse bridge).

Fairness: LeavePark, ReturnStalled/Quiescent, Abandon, EnterPhysicalPark,
both retire paths, election progress (Begin/Commit), and the unconditional
loop-top drains (FairDrain). NO producer/backend/shutdown fairness is
assumed for the return properties.

## Phase G refinements over the pre-closeout model

1. **SplitWait constant** — one model covers the production split-wait
   rule (bridge + backend-domain mixed park) and the reference E9 rule
   (scheduler-domain mixed park + bounded observation return).
2. **bridgePending** — the bridge + D4-RM13 one-shot + D4-RM14
   commit-to-park arm as one boolean: set by every wake publication while
   a committed/parked participant exists, consumed exactly by the
   participant's park return. A bump between the MW-S2 commit and the
   physical park is still delivered to the FIRST park.
3. **R1 refuse** — scheduler-domain park commits refuse beside unguarded
   progress (evaluated excluding the committing Worker's own in-flight
   admission, mirroring the production wake-domain park commit); the
   refusal signals the domain.
4. **R2 transferable election** — the BACKEND branch requires
   `w = LowestAlive`.
5. **R3 retire split** — `ParticipantNoProgressExit` (the interrupted
   0-progress participant exit; the only legal exit beside outstanding
   backend work — the E4/E5 caller-re-entry boundary) and
   `RetireWorkerQuiescent`; both publish `terminateFlag`
   (global_terminate_) and an unconditional wake, and never lose
   runnable work.
6. **R4 not-last signal** — the first counted idle park bundles the
   domain signal (the E9-LIFE-8 convergence obligation).
7. **Executor guards** — routing/draining actions require a live Active
   Worker (they run inside worker loops); the external producer and the
   backend's own readiness stay unguarded.
8. **Init explores both run modes** (fixes the pre-closeout vacuity).

## Refinement map (TLA+ → production; Phase G rows in bold)

| Formal concept/action | Production path | authority / domain |
| --------------------- | --------------- | ------------------ |
| `runMode` | `run(n)`→drain, `run_live(n)`→live | invocation lifetime |
| `runState` | run-return classification | invocation lifetime |
| `GlobalClass` | `Scheduler::classify_locked()` | global classifier |
| `ParkAdmitted` | idle-action branch (incl. `~terminateFlag`) | idle-action selection |
| `ReturnStalled` / `ReturnQuiescent` | run return paths (terminate publications) | run lifetime |
| `wakeEpoch` | `Scheduler::wake_epoch_` (monotonic in production) | commit-to-sleep window |
| `observedEpoch[w]` | `WorkerState::observed_epoch` | per-Worker park predicate |
| **`bridgePending`** | **`interrupt_backend_waiters` control epoch + `arm_committed_wait` floor (D4-RM14) + one-shot baseline (D4-RM13)** | **the Phase G bridge** |
| **`SplitWait`** | **`AsyncIoContext::has_split_wait_capability()` (wait_source() != null)** | **park-domain selection** |
| **R1 refuse (commit + Abandon signal)** | **`unguarded_progress_pending_locked()` recheck at `park_on_wake_source` commit** | **progress-observer invariant** |
| **R2 `LowestAlive`** | **MW-S2 Phase-A lowest-id alive election** | **transferable election** |
| **R3 retires + `terminateFlag`** | **worker-loop exit retire + `global_terminate_` + unconditional `signal_wake_locked`** | **departure publication** |
| **R4 `idleCount` + not-last signal** | **`idle_workers_` dance count + not-last `signal_wake_locked` (contribution-aware damping abstracted)** | **idle-dance convergence** |
| `BeginParkCandidate`/`FinalParkRecheckAndCommit` | MW-S2 two-phase admission + park commit | admission |
| `EnterPhysicalPark` | `park_on_wake_source` / `ctx_.wait_one()` | physical wait |
| `LeavePark` | park return → re-drain → reclassify | wake observation |
| `SignalWake` (BridgeEffect) | `signal_wake_locked` (+ bridge when gated) | wake-source signal |
| `ExternalReadyPublish` | external publication + `SchedulerWakeHandle::notify()` | external persistent + signal |
| `PublishRunnable` / drains | `route_runnable_locked` + drains (under `global_mtx_` + signal) | routing |
| `BackendReadyPublish` | backend terminal → `signal_progress` (NO wake-epoch advance; no reverse bridge) | backend persistent |
| `SubmitBackend` | `ctx_.submit_*` | backend op ingress |
| `ShutdownSignal` | stop path terminate + wake | termination wake |
| `backendWaitParticipant` | `admission_ == committed` + `admission_owner_` | at-most-one backend waiter |
| `ExternalWakePossible` | `external_wake_possible_locked()` | external-wake-capability test |

### Physical wake sets per blocking action (Phase G)

```
SCHEDULER-domain park (park_on_wake_source):
    wake epoch advance / terminateFlag (global_terminate_) / own local_runnable;
    bounded observation return ONLY for ~SplitWait reference parks
    (the 2ms interval is the reference backends' MIXED-WAKE progress
    authority — DIV-05 narrowed; split-wait parks are deadline-bounded
    only).

BACKEND-domain park (ctx_.wait_one, MW-S2; split-wait: backend-only AND
mixed):
    progress epoch / ring fd / ready cv (the backend's own transport);
    the BRIDGE: control epoch + control eventfd, fired by every Scheduler
    wake publication while the participant is committed/parked
    (backend_wait_active_ gate).
```

## What this model does NOT cover

- E10 WaitNode / cancellation-safe wait queue (covered by e10_waitnode).
- eventfd-in-ring (P6; deferred).
- wake_one routing refinement (notify_all baseline; ADR §9.4.8).
- Chase-Lev / lock-free deques (E16).
- Timers (E11, e11_timer_wait).
- The R4 contribution-aware damping refinement (anti-livelock
  optimization; its convergence obligation is modeled, the damping
  discipline is proven by the deterministic production tests).
- E4/E5 caller re-entry after a no-progress return (the run-return
  boundary is modeled; the caller's re-entry policy is out of scope).

## #196 trace-conformance pilot (2026-08-24): real C++ traces vs this model

`traces/` holds the canonical semantic-trace corpus and
`scripts/formal/e9_trace_validate.py` (driven by
`scripts/formal/verify-e9-trace-conformance.sh`, manifest suite
`e9-trace-conformance`) validates each trace against THIS module unchanged:
the validator generates a TLC replay wrapper (`EXTENDS E9ParkWake`) and TLC
answers whether SOME behavior of the model realizes the compiled action
sequence — the repository model is the authority (no second protocol
implementation). Claim: **TRACE-CONFORMANT (TESTED EXECUTIONS)** for the
corpus only; the event→action mapping table, terminal-collapse rules,
pre-history states, and the discovered model-scope boundaries (the E5-A2
ready-flag observation return under SplitWait=TRUE; delegation/quiescent/
drain-dance park classes; backend-domain parks) are documented in
`docs/verification/formal/e9-trace-conformance.md`. The C++ side is
`tests/e9_trace_conformance_test.cpp` (controller recorder, macro-guarded
call sites; production builds compile none of it).

## Reproducible verification

The committed `.cfg` files reproduce the gate above from the repo
bootstrap jar. The authoritative entry point is
`scripts/formal/verify-e9-park-wake.sh` (also run by
`python3 scripts/formal/verify.py smoke|all`); the TLA+ gate supplements,
and does not replace, the deterministic production tests
(`tests/phase_g_closeout_test.cpp`, `tests/phase_g_closeout_uring_test.cpp`,
`tests/phase_g_backend_progress_wake_test.cpp`).


## Audit note (2026-08-18): reference-config gate strength

> **Superseded by the #185 repair (2026-08-23, section below).** The
> unconditional `~SplitWait` escape this note describes was the B6 defect:
> it modeled ALL reference parks as timeout-bounded, which is stronger than
> the C++ truth. Post-#185 the escape is scoped to ENTRY-ARMED parks
> (`observationArmed[w]`), a causeless-return detector
> (`InvNoCauselessReturn`) is part of `Inv` in BOTH configs, and both the
> armed and un-armed reference park classes have reachability witnesses.
> The evidence-hierarchy point stands unchanged: the load-bearing
> no-lost-wake evidence remains the SplitWait=TRUE gates. Text below kept
> as the historical B6 record.

`LeaveParkEnabled` contains an unconditional `~SplitWait` disjunct for
scheduler-domain parks. This is INTENTIONAL: it models the E9-era bounded
observation rule (the reference topology's parks are timeout-bounded, so a
parked worker always eventually returns regardless of signals — the
"MIXED-WAKE progress authority" narrowed under DIV-05). Consequences, which
anyone citing these gates must know:

- `E9ParkWakeReference.cfg` safety gates (`Inv2NoLostWake`, `Inv4ExternalReadyWakes`,
  `InvLife1DrainNoMW3Park`) and `E9ParkWakeReferenceLiveness.cfg` (Life2/Life4)
  are SATISFIED LARGELY BY THE TIMEOUT, not by signal-correctness: a lost wake
  cannot strand a worker whose park is bounded. Do not cite the reference
  gates as lost-wake proofs.
- The load-bearing no-lost-wake evidence is the SplitWait=TRUE gates
  (`E9ParkWake.cfg` / `E9ParkWakeLiveness.cfg`), where the park is unbounded
  and only the bridge/epoch/persistent-state machinery returns the worker.
- `FairObservationTimeout == WF(EnterPhysicalPark)` is a legacy label: under
  SplitWait=TRUE there is no observation timeout; read it as
  "committed park entry eventually happens".

## Issue #189 repair (2026-08-23): `RetireWorkerQuiescent` revived

### Root cause (pre-fix, mechanically proven)

`RetireWorkerQuiescent` was unsatisfiable in every state: it conjoined
`BridgeEffect(1 - wakeEpoch)` — which primes `wakeEpoch'` and `bridgePending'`
(lines 266-269) — while its own `UNCHANGED` list pinned both variables
(`E9ParkWake.tla:594-610`). For `wakeEpoch \in {0,1}` the conjunction is
inconsistent, so the action had zero successors:

- TLC warning (pristine source): `The variable wakeEpoch was changed while
  it is specified as UNCHANGED at line 609, col 20 to line 609, col 28`.
- TLC action coverage: `RetireWorkerQuiescent ...: 0:0` (never enabled,
  zero successors).
- Guard-reachability probe: `NotReachRetireGuard` is **violated by the
  initial state** — the guard (`workerAlive ∧ runState="Active" ∧
  workerPhase[w]="Active" ∧ (Quiescent ∨ terminateFlag)`) is reachable, so
  the deadness is the primed-write vs `UNCHANGED` contradiction, not an
  unreachable guard.
- Consequence: `WF_vars(RetireWorkerQuiescent(w))` (`FairRetire`) was
  vacuous; any gate that appeared to prove retire-progress proved nothing.

### C++ as-built retirement path (the repair's authority)

Every `Scheduler::worker_loop` exit funnels through ONE retire epilogue
(`src/async/scheduler.cpp:1216-1250`): hold `global_mtx_` → `--live_loop_workers_`
→ `ws->active=false` → hold `ws->inbox_mtx`, move `local_runnable` to
`pending_spawn_`, release → **unconditional `signal_wake_locked()`** →
release. `signal_wake_locked()` (`scheduler_park_wake.cpp:128-157`) advances
`wake_epoch_`, notifies, and bridges to the backend participant when
`backend_wait_active_`. The exit classes (A last-idle quiescent, B
terminate-observed, C MW-S2 no-progress, D E14-F1 stop) all share the
epilogue; a fiber already running when termination lands is NOT preempted
(the loop-top pop+run at `scheduler.cpp:511-592` precedes the terminate
check), so runnable/running may remain after `global_terminate_`.

### Repair (faithful to the C++ epilogue)

1. **Keep `BridgeEffect(1 - wakeEpoch)`** — the C++ departure signal is
   unconditional — and **drop `wakeEpoch`/`bridgePending` from the action's
   `UNCHANGED`** (the contradiction).
2. **Faithful terminal classification** — `runState' = IF last-alive THEN
   (IF Quiescent THEN "ReturnedQuiescent" ELSE "ReturnedStalled") ELSE
   runState`. Reviving the action exposed a dormant model defect: the old
   `"ReturnedQuiescent"`-if-last-alive rule misclassified a
   terminate-observed exit at non-quiescence (e.g. an external ready or
   runnable published after a sibling's retire) as `ReturnedQuiescent`,
   violating `InvLife5QuiescenceClassifierDefined`. C++ never labels an
   observer exit "quiescent" (it just exits; `run()` returns void); the
   faithful label is `ReturnedStalled` at non-quiescence (the
   terminate-observed / E4-E5 caller-re-entry boundary).
3. **`InvLife3LiveExternalParkAdmitted` scoped to `~terminateFlag`** — the
   revival also exposed the legal-C++ state where a survivor keeps running
   (and suspending) its current fiber after termination and lands in
   MWS3 + `terminateFlag`; there parking is deliberately refused
   (`ParkAdmitted` requires `~terminateFlag`; the worker breaks at
   `scheduler.cpp:1150-1156`). The invariant's antecedent is now scoped to
   the non-terminated run, matching the model's own `ParkAdmitted` rule.
4. **`vars` dedupe** — the duplicate `terminateFlag` entry removed.
5. **Causal witness ghost `retireFired`** — set only inside
   `RetireWorkerQuiescent`'s body (which conjoins `BridgeEffect`), so a
   witness trace with `retireFired = TRUE` proves the full as-built retire
   step executed, including the departure wake.

### Non-vacuity witnesses (permanent gates)

- `E9ParkWakeWitnessRetire.cfg` — checks `NoReachRetireFired`; expected
  **VIOLATED**. The CEX is the causal chain: a worker alive in `Active`
  phase → `RetireWorkerQuiescent` fires → `workerAlive` TRUE→FALSE →
  `wakeEpoch` advances (departure wake) → the ghost `retireFired` flips.
- `E9ParkWakeWitnessTerminate.cfg` — checks `NoReachQuiescentTerminate`;
  expected **VIOLATED**. Pins the all-retired
  `ReturnedQuiescent`-at-quiescence terminal chain (reachable only through
  a last-alive `RetireWorkerQuiescent`). Each witness has its own cfg so
  TLC verifies both (it stops at the first violation otherwise).
- `E9ParkWakeNegRetireDead.tla/.cfg` — the EXACT pre-fix defect
  (contradiction reintroduced). GENERATED from the current positive
  model by `_gen_neg.py` (#192): the mutation restores `wakeEpoch,
  bridgePending` into `RetireWorkerQuiescent`'s UNCHANGED; the stale
  pre-#189 header comments of the old hand-copied snapshot are gone by
  regeneration. The witness invariants **HOLD** here
  (fail-closed): a reintroduced dead retire is detected. Reachable-state
  counts / semantic-graph cardinality match the pre-#189 model (46456
  generated / 14472 distinct, re-verified after generatorization; the
  mutant also carries the `retireFired`
  ghost and the post-fix `InvLife3` scope), and the positive invariants
  still pass on the mutant — the witness gate is the sole detector, which
  is why #189 existed.

### Fairness (`FairRetire`) non-vacuity

Under the `LivenessSpec`, TLC action coverage for
`RetireWorkerQuiescent` is **904:1720** (enabled/fired) — live, versus 0:0
pre-fix. The witness CEX's State-2 shape (`Quiescent ∧ terminateFlag ∧ the
other worker Active`) is a reachable state where the action is continuously
enabled (only stuttering otherwise), so `WF_vars(RetireWorkerQuiescent)`
is mechanically exercised. Honest note: no named liveness *property*
changes verdict if the action is disabled (probe: dropping the departure
wake fails nothing — `terminateFlag` itself is the scheduler-domain wake
authority, and at true quiescence no backend participant exists for the
bridge); the dedicated non-vacuity witness is the detector, not a liveness
property.

### State-space delta (PRE vs POST, safety)

| config | PRE (pristine, dead action) | POST (repaired) |
|--------|------------------------------|-----------------|
| split-wait safety | 46456 generated / 14472 distinct / depth 22 | 57944 / 18928 / depth 27 |
| reference safety | 51240 / 15376 | 62888 / 19872 |

The +4456 distinct split-wait states are ALL the retire/post-retire
families (verified from a state dump: every new state has ≥1 dead worker
and `terminateFlag = TRUE`, across `Active`-survivor, `ReturnedQuiescent`,
`ReturnedStalled`, and `Shutdown` classifications). The ghost splits the
same graph into 14472 (`retireFired=FALSE`) + 4456 (`retireFired=TRUE`)
states — the new families are disjoint from the pre-retire set, exactly as
expected for states only reachable through a retirement. No unexpected
families appear.

### Known separate defect (documented at #189, REPAIRED at #191)

`ParticipantNoProgressExit` was ALSO dead (pre-existing, and still 0:0
after the #189 repair), via a DISTINCT root cause: its body conjoined
`BridgeEffect(1 - wakeEpoch)` — whose bridge branch sets `bridgePending' =
TRUE` (a participant exists) — AND an explicit `bridgePending' = FALSE`
(one-shot consume), a double-prime contradiction. Its guard was reachable
(probe: 1055 states), so the interrupted-0-progress participant exit (exit
class C) was entirely unmodeled at the time. This was out of #189's scope
(different action, different root cause); it was registered in the debt
register and the manifest notes, tracked independently in issue #191, and
is now REPAIRED — see the next section ("Issue #191 repair").

## Issue #191 repair (2026-08-23): `ParticipantNoProgressExit` revived

### Root cause (pre-fix, mechanically proven)

`ParticipantNoProgressExit` was unsatisfiable in every state: it conjoined
`BridgeEffect(1 - wakeEpoch)` — whose bridge branch (lines 266-269) sets
`bridgePending' = IF BridgeFiresFromParticipant THEN TRUE ELSE bridgePending`
— while its own body assigned `bridgePending' = FALSE` (the one-shot consume
at line 605). For a participant (`backendWaitParticipant = w`),
`BridgeFiresFromParticipant` is TRUE, so the conjunction is
`bridgePending' = TRUE ∧ bridgePending' = FALSE`, which is inconsistent:

- TLC action coverage: `ParticipantNoProgressExit ...: 0:0` (never enabled,
  zero successors).
- Guard-reachability probe: `NotReachPnpExitGuard` is **violated** (1055
  states reach the guard), so the deadness is the double-prime contradiction,
  not an unreachable guard.
- Consequence: `WF_vars(ParticipantNoProgressExit(w))` (`FairRetire` on the
  participant side) was vacuous; the interrupted-0-progress participant exit
  (exit class C, the E4/E5 caller-re-entry boundary) was entirely unmodeled.

### C++ as-built participant exit path (the repair's authority)

The participant exit is the MW-S2 no-progress terminate path
(`src/async/scheduler.cpp:914-974`): `wait_one` returns 0 (no progress) →
`backend_wait_active_.store(false)` (participant gate CLEARED before any
signal, line 915) → Phase-D drain (lines 928-935) → classify `mw_s1`
(ExecutableWork) → `continue` (line 942, the `~ExecutableWork` guard) →
`external_wake_possible` → `continue` (line 957) → `global_terminate_.store(true)`
(line 964) → **`signal_wake_locked()`** (line 966, wake publication #1) →
`break` (line 974) → the common retire epilogue (lines 1216-1250) →
**`signal_wake_locked()`** (line 1249, wake publication #2).

Two distinct wake publications, both unconditional. The participant slot is
cleared BEFORE the first signal (line 915), so neither signal re-arms the
bridge (the `backend_wait_active_` gate is false).

### Repair (faithful to the C++ epilogue)

1. **Fuse the two wake publications into a single action** — matching #189's
   retire precedent (the common epilogue fuses the two C++ signals into one
   modeled action). The repaired action advances `wakeEpoch' = 1 - wakeEpoch`
   (direct, no `BridgeEffect`) and clears `backendWaitParticipant' = NONE` and
   `bridgePending' = FALSE` (the one-shot consume).
2. **`~ExecutableWork` guard added** — the C++ line 942 reclassifies to
   `mw_s1` and continues (refuses to exit) when executable work exists. The
   pre-fix model lacked this guard, so the action could fire beside runnable
   or running work (violating `Inv9NoStrandedRunnable`).
3. **Participant slot cleared before signal** — `backendWaitParticipant' = NONE`
   and `bridgePending' = FALSE` are assigned BEFORE the epoch advance, so the
   signal does not re-arm the bridge (the `BridgeFiresFromParticipant` guard
   is false when the action fires).
4. **Last-alive classification** — `IF (∀v ∈ Workers: v = w ∨ ~workerAlive[v])
   THEN runState' = "ReturnedStalled" ELSE runState' = runState`. The
   participant exit is beside outstanding backend work or a bridge obligation
   (the participant was parked waiting for it), so it is NEVER quiescent
   (quiescence requires no backend outstanding).
5. **Causal witness ghosts** — `participantExitFired` and
   `participantExitEndedRun` (the last-alive branch). Set only inside the
   action body (which conjoins the direct epoch advance), so a witness trace
   with the ghost flipped proves the full as-built participant exit executed.

### Inv8 and Inv10 refinements (D4-RM14 arm baseline + R2 transferable election)

Reviving `ParticipantNoProgressExit` exposed two invariant violations that
are NOT model defects — they are faithful encodings of the as-built C++
semantics:

- **Inv8BridgeReachesBackendPark** — the pre-repair invariant owed a bridge
  to ANY parked participant beside a scheduler-domain publication. The C++
  D4-RM14 arm baseline (`scheduler.cpp:770-795`) registers the commit-to-park
  persistence BEFORE the park is exposed, so a publication landing between
  the commit and the physical park is a PAST EVENT (the armed baseline makes
  the upcoming `wait_one()` observe it). The refined Inv8 narrows the owed
  set to DURING-RESIDENCY publications only (the `SchedulerDomainWakeDue`
  predicate excludes `terminateFlag`, which is a past event when the
  participant commits after another worker has already published termination).

- **Inv10BackendProgressHasObserver** — the pre-repair invariant required an
  observer beside backend work. The C++ R2 transferable election
  (`scheduler.cpp:665-670`) elects the lowest-id ALIVE worker as the
  participant, so a post-terminate participant (one worker has already exited
  via the no-progress path, publishing `global_terminate_`) is LEGAL — the
  surviving worker becomes the participant and observes the outstanding
  backend work (the E4/E5 caller-re-entry boundary owns it). The refined
  Inv10 exempts `terminateFlag` from the observer requirement (a post-terminate
  participant is legal, the outstanding work is the caller's responsibility).

### Non-vacuity witnesses (permanent gates)

- `E9ParkWakeWitnessPnpExit.cfg` — checks `NoReachParticipantExitFired`;
  expected **VIOLATED**. The CEX is the causal chain: a worker alive in
  `Parked` phase as the backend participant → `ParticipantNoProgressExit`
  fires → `workerAlive` TRUE→FALSE → `wakeEpoch` advances (departure wake) →
  `backendWaitParticipant` cleared → the ghost `participantExitFired` flips.
- `E9ParkWakeWitnessPnpEndedRun.cfg` — checks `NoReachPnpExitEndedRun`;
  expected **VIOLATED**. Pins the last-alive branch: the participant is the
  last alive worker, so its exit classifies the run `ReturnedStalled` (never
  `ReturnedQuiescent`, because a participant exit is beside outstanding
  backend work or bridge obligation). Each witness has its own cfg so TLC
  verifies both.
- `E9ParkWakeNegParticipantDead.tla/.cfg` — the EXACT pre-fix defect
  (double-prime contradiction reintroduced). GENERATED from the current
  positive model by `_gen_neg.py` (#191): the mutation restores
  `BridgeEffect(1 - wakeEpoch)` in place of the repaired direct epoch
  advance; the witness invariants **HOLD** here (fail-closed): a reintroduced
  dead participant is detected.

### Fairness (`FairRetire` on participant side) non-vacuity

Under the `LivenessSpec`, TLC action coverage for
`ParticipantNoProgressExit` is **162:176** (enabled/fired) — live, versus 0:0
pre-fix. The witness CEX's State-2 shape (`Parked ∧ participant ∧
~ExecutableWork ∧ ~ExternalWakePossible`) is a reachable state where the
action is continuously enabled (only stuttering otherwise), so
`WF_vars(ParticipantNoProgressExit)` is mechanically exercised.

### State-space delta (PRE vs POST, safety)

| config | PRE (pristine, dead action) | POST (repaired) |
|--------|------------------------------|-----------------|
| split-wait safety | 57944 generated / 18928 distinct / depth 27 | 64504 / 21664 / depth 27 |
| reference safety | 62888 / 19872 | 69496 / 22608 |

The +2736 distinct split-wait states are ALL the participant-exit and
post-exit families (verified from a state dump: every new state has ≥1 dead
worker who was the participant, `terminateFlag = TRUE`, and
`participantExitFired = TRUE`, across `Active`-survivor and
`ReturnedStalled` classifications). The ghost splits the same graph into
18928 (`participantExitFired=FALSE`) + 2736 (`participantExitFired=TRUE`)
states — the new families are disjoint from the pre-exit set, exactly as
expected for states only reachable through a participant exit. No unexpected
families appear.

### Adversarial probes (2G, #191)

One-rule mutants of the REPAIRED `ParticipantNoProgressExit`, each
reintroducing a specific adjacent defect, checked against the repaired
model:

- **A — exact pre-fix double-prime** (the #191 defect itself): caught by
  the permanent `NegParticipantDead` gate (both witness invariants HOLD,
  fail-closed).
- **B — departure wake BEFORE the participant slot is cleared**
  (self-bridge rearm): **CAUGHT by `Inv6OneBackendParticipant`** — the
  mutant keeps `backendWaitParticipant' = w` while `workerAlive'[w] =
  FALSE`, leaving a DEAD participant in the slot. This proves the
  as-built ordering (authority cleared before the departure signal,
  `scheduler.cpp:915` precedes `:966`/`:1249`) is structurally
  load-bearing, not cosmetic.
- **C — participant cleared with NO departure wake** (both production
  signals `:966`/`:1249` dropped): NOT CAUGHT — a documented model
  boundary. The fused exit publishes `terminateFlag` (persistent
  scheduler-domain state) atomically with the slot clear; the model's
  park-return authority is the PERSISTENT per-domain wake state, not the
  epoch parity (see "Model domain" above). The departure wake is a
  promptness signal; correctness is carried by persistent state plus the
  second epilogue signal. The model cannot express "wake dropped but
  terminate published" as a distinct violation because the fused action
  publishes both.
- **D — `~ExternalWakePossible` guard dropped** (participant may exit
  beside a registered external-wait): NOT CAUGHT — a documented model
  boundary. The guard is a C++ progress decision (`scheduler.cpp:957`
  re-loop); the model's terminal classification (exit class C → run ends
  → the E4/E5 caller-re-entry boundary owns the wait) subsumes the
  pending-wait handoff, so Life7's `runState # "Active"` escape holds.
  The model cannot distinguish "participant exits beside a registered
  external wait" from "run ends with the wait handed to the caller".
- **E — sibling publication between wait return and termination** (the
  D4-RM14 arm baseline / R2 transferable election families): this is the
  `Inv8`/`Inv10` refinement driver. The repaired model PASSES the refined
  invariants (post-terminate elected participants and pre-arm
  publications are legal C++), and the `BuggyNoBridge` permanent gate
  still catches the bridge-disabled defect (the owed set is narrower but
  the detector still fires on a genuinely stranded participant).
- **F — last-alive vs survivor-alive classification**: caught by the
  permanent `WitnessPnpEndedRun` gate (the last-alive branch is
  reachable and classifies `ReturnedStalled`).
- **G — #189 retire witness still works**: `WitnessRetire` /
  `WitnessTerminate` remain VIOLATED (permanent gates; the #189 repair
  did not regress).
- **H — #185 SplitWait semantics unchanged**: reference-config safety +
  liveness still PASS (permanent gates; the reference domain's bounded
  observation authority is untouched).

Probes C and D are honest boundaries, not silent gaps: the model's
persistent-state return authority and fused atomic exit intentionally
cannot distinguish those promptness/progress-decision defects from the
legal states they collapse into. The C++-level guarantee for C/D rests on
the deterministic production tests (the `phase_g_backend_progress_wake`
and no-progress-terminate tests) and the `scheduler.cpp:942/:957` guards
themselves.

## Issue #185 repair (2026-08-23): reference escape made faithful (`observationArmed`)

### Root cause (the B6 STOP classification, mechanically confirmed)

Pre-#185, `LeaveParkEnabled`'s scheduler-domain branch contained an
UNCONDITIONAL `\/ ~SplitWait` disjunct: every reference-config park was
modeled as always-returnable, standing for "reference parks are
timeout-bounded (E9 bounded observation, DIV-05)". That claim is STRONGER
than the C++ truth:

- The 2 ms `kParkBackstop` (`scheduler_park_wake.cpp:400`) applies only
  when `bounded_backend_observation` was armed at park entry or a deadline
  is active (`:455`, `:468`); with no deadline and no backend observation
  the park is UNBOUNDED — "No deadline and no backend observation:
  unbounded park (epoch / terminate / runnable predicate only). No periodic
  wake exists." (`:413-415`).
- Only the reference MIXED-WAKE park arms observation at entry:
  `park_on_wake_source(ws, /*bounded_backend_observation=*/true)`
  (`scheduler.cpp:818`). The general MW-S3 idle park does not
  (`scheduler.cpp:988-999`, `:1188`, and the explicit no-periodic-wake
  comment at `:1173-1178`).

So the unconditional escape licensed causeless returns for UN-ARMED
reference parks (a real C++ park of the unbounded class), and
`InvLife1DrainNoMW3Park` (then unscoped: every Drain+MW-S3 park must be
leave-enabled) held ONLY because of that unfaithful escape. Classification
(B6): CONTRACT/MODEL CLAIM MISMATCH — the model claimed more than the C++
guarantees; the repair follows the C++ (never vice versa), and no
invariant was weakened merely to regain green.

### C++ as-built park table (the repair's authority)

| park class (C++) | bound | return authority |
|------------------|-------|------------------|
| reference MIXED-WAKE park (`scheduler.cpp:818`, observation armed at entry) | 2 ms `kParkBackstop` | bounded observation re-check |
| MW-S3 idle / wake-domain park (`:988-999`, `:1188`, `:1173-1187`) | UNBOUNDED (unless a deadline) | cv predicate: epoch moved, `global_terminate_`, or local runnable |
| backend participant park (split-wait, Phase G domain) | unbounded | `backendReady` or the bridge interrupt |

### Repair (faithful to the C++)

1. **Ghost `observationArmed[w]`** — set to `ExternalWakePossible` at
   SCHEDULER-park entry (the entry capture of "an external wake-capable
   wait was registered", mirroring the C++ entry-time arming at
   `scheduler.cpp:818` and the `ready_flag_observation` entry capture at
   `:708`), cleared on park leave and on non-parking branches; carried
   through all `UNCHANGED` lists.
2. **`LeaveParkEnabled` escape scoped** — `(~SplitWait /\
   observationArmed[w])`: only ENTRY-ARMED reference parks carry the
   bounded-observation return. Un-armed reference parks return only on a
   real scheduler-domain cause (`SchedulerDomainWakeDue`) or run end,
   exactly the unbounded C++ park.
3. **`InvLife1DrainNoMW3Park` scoped to armed parks** — the consequent is
   now `(workerPhase[w] = "Parked" /\ observationArmed[w]) =>
   LeaveParkEnabled(w)`. Probe U (below) proves the unscoped law is FALSE
   against the faithful escape: the scoping is forced by C++ truth, not a
   convenience.
4. **`Inv10` comment corrected** — the 2 ms bound belongs ONLY to the
   entry-armed reference park; the general MW-S3 idle park is unbounded.
5. **`InvNoCauselessReturn` detector added to `Inv` (BOTH configs)** —
   forbids a scheduler-domain park returning with NO cause: not the
   backend participant wake, not a scheduler-domain wake, not entry-armed
   observation, run still Active.

### Non-vacuity witnesses (permanent gates)

- `E9ParkWakeWitnessRefObservation.cfg` — checks
  `NoReachRefObservationPark`; expected **VIOLATED**. An entry-armed
  reference park is reachable: the faithful escape is a real, exercised
  return class (the reference 2 ms observation authority).
- `E9ParkWakeWitnessRefUnbounded.cfg` — checks `NoReachUnboundedRefPark`;
  expected **VIOLATED**. An UN-armed reference park is reachable: the
  reference config genuinely explores the unbounded park (it was not
  retired from the graph to make the detector vacuous), and the detector
  is checked against real reachable states.
- `E9ParkWakeWitnessRefDrainMWS3.cfg` — checks
  `NoReachRefDrainMWS3ArmedPark`; expected **VIOLATED**. The scoped
  `InvLife1` antecedent (Drain + MW-S3 + armed park) is reachable: the
  law is non-vacuous after scoping.

### Fail-closed negatives (GENERATED by `_gen_neg.py`, SplitWait=FALSE)

Both are EXACT mutants — on each, the full safety set minus the detector
PASSES; only `InvNoCauselessReturn` fires.

- `NegOldEscape` — the pre-#185 unconditional `\/ ~SplitWait` escape
  reintroduced: an un-armed park returns with no wake due (causeless
  return). `InvNoCauselessReturn` **VIOLATED**.
- `NegNaiveEscape` — the naive "check `ExternalWakePossible` at leave"
  escape: a park armed at entry can be left after the flag has since
  cleared, again a causeless return. **VIOLATED**. This pins WHY the
  repair uses an entry-captured ghost rather than re-reading the live
  condition at return.

### Adversarial probes (2G, #185)

- **U — unscoped `InvLife1` vs the faithful escape**: VIOLATES in the
  reference config. The pre-#185 law is genuinely false once the escape
  is faithful; the scoping is load-bearing. (This is the mechanical
  confirmation of the B6 classification.)
- **G — ghost capture removed** (scheduler-park entry sets
  `observationArmed' = FALSE`): BOTH capture-dependent witnesses
  (`NoReachRefObservationPark`, `NoReachRefDrainMWS3ArmedPark`) HOLD —
  the witness gates fail closed if the capture is dropped; they genuinely
  test the entry capture, not incidental reachability.
- **Specificity** — both escape mutants PASS every other safety
  invariant: the detector is the sole catcher (EXACT negatives).
- **SplitWait=TRUE unchanged** — split safety/liveness PASS and the
  Phase-G bridge evidence (`Inv8` + `BuggyNoBridge`) is untouched
  (permanent gates in the 19-gate verifier run).
- **Detector soundness on the correct model** — both positive cfgs check
  the full `Inv` conjunction (which now includes the detector): PASS in
  both configs.

### State-space delta (PRE vs POST, safety)

| config | PRE (master 8c08e46) | POST (#185) |
|--------|------------------------|-------------|
| split-wait safety | 64504 generated / 21664 distinct / depth 27 | 71304 / 23984 / depth 27 |
| reference safety | 69496 / 22608 / depth 27 | 75904 / 24800 / depth 27 |

Under SplitWait=TRUE the transition relation is UNCHANGED (the escape
conjunct is FALSE there either way), so the +2320 distinct states are
purely the `observationArmed` ghost splitting states whose
scheduler-park entry-time `ExternalWakePossible` history differs. Under
the reference config the delta mixes those ghost splits with the REMOVED
causeless leave transitions (the un-armed park no longer has a causeless
out-edge). No state family disappears from the split graph.
