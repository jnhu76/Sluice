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
- `E9ParkWakeBuggyNoBridge.tla/.cfg` — negative model (Phase G): the
  model-level M1 mutation — wake publications advance `wakeEpoch` but
  never set `bridgePending`. Produces an `Inv8BridgeReachesBackendPark`
  counterexample (a parked participant that can never observe an
  external publication). GENERATED from the current positive model by
  `_gen_neg.py` (#192; freshness-gated, do not edit by hand).
- `_gen_neg.py`                  — generates the duplicated-snapshot
  negatives (`NegRetireDead`, `BuggyNoBridge`) from the current
  `E9ParkWake.tla`; `--check` is the read-only freshness gate the
  verifier runs before any TLC invocation (#192).
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

(four positive gates + two reachability-witness gates + five negative
gates; TLC runs in an isolated mktemp workspace — never in this
directory.)

## Generated negatives + freshness gate (#192)

`NegRetireDead` and `BuggyNoBridge` are FULL DUPLICATED SNAPSHOTS of the
positive model, so they are GENERATED from the current `E9ParkWake.tla`
by `_gen_neg.py` (each mutation is a declared exact-fragment swap whose
anchor must match exactly one location — zero or multiple matches aborts
generation fail-closed) and freshness-gated:
`verify-e9-park-wake.sh` runs `_gen_neg.py --check` BEFORE any TLC run,
so a stale, missing, or unexpected generated artifact fails the formal
gate without starting TLC (a stale negative would keep checking an
outdated mutation while CI stays green). Never edit those four files by
hand — regenerate with `python3 spec/tla/e9_park_wake/_gen_neg.py`.

`BuggyPrePark` / `BuggyMixedSource` (pre-Phase-G protocol snapshots) and
`BuggyDrainParks` (an EXTENDS-based minimal mutant that auto-tracks the
positive model) are intentionally NOT generated: the first two are
frozen historical controls, the third has no drift risk. A future
negative that is a single-rule mutation of the current model (e.g. the
#191 `ParticipantNoProgressExit` exact-old mutant) should join the
generator's CASES — mutation spec + cfg + verifier expectation — not add
another hand-copied snapshot.

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

## Reproducible verification

The committed `.cfg` files reproduce the gate above from the repo
bootstrap jar. The authoritative entry point is
`scripts/formal/verify-e9-park-wake.sh` (also run by
`python3 scripts/formal/verify.py smoke|all`); the TLA+ gate supplements,
and does not replace, the deterministic production tests
(`tests/phase_g_closeout_test.cpp`, `tests/phase_g_closeout_uring_test.cpp`,
`tests/phase_g_backend_progress_wake_test.cpp`).


## Audit note (2026-08-18): reference-config gate strength

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

### Known separate defect (documented, NOT repaired here)

`ParticipantNoProgressExit` is ALSO dead in every config (pre-existing, and
still 0:0 after this repair), via a DISTINCT root cause: its body conjoins
`BridgeEffect(1 - wakeEpoch)` — whose bridge branch sets `bridgePending' =
TRUE` (a participant exists) — AND an explicit `bridgePending' = FALSE`
(one-shot consume), a double-prime contradiction. Its guard IS reachable
(probe: 1055 states), so the interrupted-0-progress participant exit (exit
class C) is entirely unmodeled. This is out of #189's scope (different
action, different root cause); registered in the debt register and the
manifest notes, and tracked independently in issue #191.
