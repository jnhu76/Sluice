# Spawn Wake-Epoch Obligation — spawn onto a BUSY worker (MODEL-007d)

Focused TLA+ safety model of the #115 protocol: a runnable ticket
published onto a BUSY worker's `local_runnable` must publish the Scheduler
wake obligation (advance `wake_epoch_`), because a peer worker that has
already committed the unbounded wake-domain park observes a cross-worker
publication ONLY through the epoch predicate (the cv predicate checks
epoch / terminate / **own** inbox — another worker's queue is invisible).

Issue #176 (child of umbrella #171; historical defect: issue #115).
C++ is the fact source.

## C++ binding

| Model construct | C++ (baseline c1e93f9) |
|---|---|
| `SpawnB` (fused push + notify + epoch advance) | `Scheduler::spawn` / `spawn_on` / `route_runnable_locked` (`src/async/scheduler.cpp:247-330, 1517+`) — one `global_mtx_` scope; inbox lock released before `signal_wake_locked()` |
| `wakeEpoch' = wakeEpoch + 1` | `signal_wake_locked` (`src/async/scheduler_park_wake.cpp:128-157`): `++wake_epoch_` under `wake_mtx_`, `wake_cv_.notify_all`, Phase G bridge |
| `Park` (ticket-absence guard) | the G1 arm-then-recheck commit + `unguarded_progress_pending_locked` runnable-first (`src/async/scheduler.cpp:1856-1894`) |
| `observedEpochW0 := wakeEpoch` | the baseline record under nested `wake_mtx_` inside the G scope |
| `WakeObserve` | the unbounded-park cv predicate (`wake_epoch_ != observed_epoch` ∨ terminate ∨ own-inbox backstop) |
| `StealB` | `try_steal` (transport + owner transfer, E8's domain) |
| `inboxNotified` (no consumer) | historical: the pre-#170 `inbox_cv.notify_one()` (notify-only, never a waiter); the C++ notify was deleted by issue #170, so the ghost now records only the #115 pre-fix transport that `NegNoSignal` mutates |
| W1 pinned busy | the #115 precondition: the owner sits inside an unbounded fiber execution and cannot drain its queue |

## Boundary

2 workers: W0 = park candidate / thief; W1 = pinned busy (environment
constant). One spawned ticket B. Safety/accounting only.

Non-goals: W1 completion escape hatch; self-targeted delivery (the
own-inbox predicate backstop is e9-park-wake's modeled path); the
terminate clause; the idle dance (#161's own suite); deadlines and
bounded backend observation; the Phase G backend bridge; the
refuse-branch signal (its consumer would be a third worker).

Memory-model boundary: `wake_epoch_` lives entirely under `wake_mtx_`
(SC mutex domain); the modeled protocol has no lock-free atomics. TLC
proves the SC abstraction only — no C++ weak-memory claim.

## Laws (positive cfg, all PASS)

| invariant | meaning |
|---|---|
| `InvWakeObligation` | a committed-parked peer and a cross-worker published ticket coexist only if the publication advanced the epoch past the parked baseline — the violating state IS the persistent stranded shape. The comparison mirrors the production predicate `wake_epoch_ != observed_epoch` directly (inequality, not `>`; extensionally equal in this finite monotonic model with one bounded epoch advance; wraparound/multiple cycles outside the focused domain) |
| `InvBaselineSound` | the parked baseline never exceeds the current epoch (finite monotonic abstraction law; the production predicate is the inequality in `InvWakeObligation`) |
| `InvStealRequiresAwake` | transport only by a looping worker |
| `InvConsumedRequiresPublication` | no consumption without a publication |

## Negative controls (one-rule cfg flips; both are EXACT historical defects)

| gate | defect | named CEX | specificity |
|---|---|---|---|
| NegNoSignal | #115 pre-fix publication: push + inert inbox notify, no epoch advance | `InvWakeObligation` | other laws PASS |
| NegNoRecheck | pre-G1 commit: the baseline is armed regardless of a live stealable ticket (consumed-baseline stall) | `InvWakeObligation` | excludes entailed co-victim `InvStealRequiresAwake` (a recheck-less commit can park on top of the worker's own stolen ticket — the same pre-G1 shape) |

Dropped with argument (issue #176 Comment A): a separate "notify wrong
transport only" gate is representationally identical to NegNoSignal —
`inboxNotified` has no consumer by C++ fact (issue #170); and
"signal-before-state" has no interleaving window (`global_mtx_` serializes
the publication against the park commit; the only unsynchronized observer
— the cv predicate — reads only the epoch for cross-worker tickets).

## Reachability (6 witnesses, each a NoReach* CEX)

`NoReachParked` (committed park at baseline 0) · `NoReachPublishedWhileParked`
(B on the busy worker's queue while W0 parked) · `NoReachEpochAdvanced`
(the publication advanced the epoch) · `NoReachRescued` (consumption
chain) · `NoReachRescuedAfterWake` (the strong post-park rescue chain:
cv predicate fired, then steal, then consume — the witness that makes the
wake→observe→steal→progress path non-vacuous) · `NoReachParkRefuse` (the
commit-recheck refusal branch).

## Results

TLC 2.19 (tla2tools 1.7.4), exhaustive, 1 worker:

- positive: 24 states generated, 11 distinct, depth 6, no error.
- both historical negatives: exact named CEX; both specificity gates PASS;
  all 6 reachability gates CEX as expected.
- `bash scripts/formal/verify-spawn-wake-epoch.sh` → 12/12, PASS.

## C++ bridge (no new tests)

`tests/issue115_runnable_publication_wake_test.cpp` — the deterministic
causal regression: W1 pinned inside user fiber code, W0 held at the
`scheduler_park_baseline_recorded` seam (post-baseline, pre-cv-entry),
production `spawn()` / `spawn_on()` publishing strictly post-commit;
pre-fix fails via bounded watchdog + external-handle rescue, post-fix W0
wakes, steals, executes. This test is the exact C++ realization of the
NegNoSignal counterexample trace and the NoReachRescuedAfterWake witness.

## Verdict

**AS-BUILT MODELED.** No C++ defect candidate. Allowed claims: the SC
abstraction satisfies the four laws; both historical defect behaviors
(#115 pre-fix, pre-G1 consumed baseline) violate the named law; the
as-built two-layer protection (publication signals + commit recheck)
excludes both. Forbidden claims: C++ weak-memory correctness, liveness
under adversarial scheduling, implementation bug-freedom, and
"TLC green ⇒ C++ correct".
