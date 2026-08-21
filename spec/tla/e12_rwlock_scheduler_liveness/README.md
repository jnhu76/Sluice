# e12-rwlock-scheduler-liveness — issue #161 combined liveness suite

The T22 multi-worker drain hang (`async_rwlock_test` /
`rwlock_mw_cancel_and_unlock_on_different_workers`, CI PR #160):
both workers asleep in the unbounded wake-domain park, `run_impl` blocked in
`join`, **after every fiber already completed**. This suite proves the
protocol-level root cause, proves the shape of the minimal repair, and
classifies which idle-count reset sites are genuine "contribution
invalidation events".

Tracking issue: **#161**. Verifier: `scripts/formal/verify-e12-sched-liveness.sh`
(8 positive gates PASS, 5 negative gates CEX). Owner doc:
`docs/architecture/issue-161-idle-dance-contribution-generation-gate.md`.

## Scope

The three `run()` invocations of T22 are collapsed into the initial state
(writer holds the lock and waits on the ready flag; R1 cancelled between
runs — its ticket sits in W0's inbox; R2 queued; flag TRUE). Fibers are
finite (application fairness), so the interesting behavior is entirely in
the scheduler protocol: pop, the unlocked pop-path idle reset, classify,
the mw_s1 fall-through, the idle dance, the park commit baseline, and the
cv-predicate wake. MW-S2 (backend work) is out of scope — T22 runs an
IdleBackend; the e9 suite owns the backend park domain.

## C++ site ↔ action ↔ property binding

| C++ site | Model action(s) | Load-bearing role |
|---|---|---|
| `scheduler.cpp:513-517` pop | `TryPop` | removes the ticket under `inbox_mtx` only — invisible to classify and to the park-commit scan |
| `scheduler.cpp:550` pop-path idle reset | `EraseIdleOnPop` | **unlocked, no mandatory follow-up signal — genuine invalidation site (B4)** |
| `scheduler.cpp:1211-1219` make_running | `StartFiber` | the ticket becomes a classify-visible observer only here |
| `scheduler.cpp:567` unlocked classify | `DrainClassify` (with the :562-568 drain) | drain publishes + signals + erases idle atomically under G |
| `scheduler.cpp:582` mw_s1 fall-through erase | `EraseIdleMwS1` | **unlocked, arbitrarily delayable — genuine invalidation site (B4)** |
| `scheduler.cpp:955-958` dance-block reclassify + erase | `ReclassifyMwS1` | self-guarded (:582 always precedes on the same path) |
| `scheduler.cpp:1035-1093` idle dance | `DanceContribute` | contribute; not-last signal (E9-LIFE-8); last-idle recheck/terminate |
| `scheduler.cpp:1065` reset-continue | `DanceContribute` middle branch | self-guarded (the eraser stays active and re-loops) |
| `park_wake.cpp:295-338` park commit | `ParkCommit` | recheck (`RefusePark`) then arm baseline — the refusal is the repair point |
| `park_wake.cpp:379-396` cv predicate | `CanLeavePark`/`LeavePark` | epoch / terminate / own-inbox backstop |
| `scheduler.cpp:1452-1483` route erase+signal | inside `DrainClassify`/`FiberStepWF` | G-atomic pair — self-guarded (the #115 closure) |
| E8 steal | `TrySteal` | transport; `DrainClassify` requires nothing stealable (the C++ pop→steal→drain order) |

Properties: invariants `NoReaderWriterOverlap`, `TerminalUniqueness`,
`PublicationUniqueness`, `NoLinkedTerminal`, `InboxImpliesTicket`,
`NoStrandedRunnable` (#115 class), `DrainStuckState` (the #161 target);
temporal `DrainEventuallyReturns`, `CancelUnlockScenarioEventuallyCompletes`,
`GrantedWaiterEventuallyResumes`, `RunnableEventuallyExecutes`. Fairness:
WF on every scheduler-controlled action; **SF on `TryPop`** — a two-worker
steal-war toggles the pop's enablement forever (each steal displaces the
other's pop window), so WF never fires; SF encodes the minimal scheduling
fact the C++ relies on (a worker scheduled infinitely often with its own
ticket eventually executes the straight-line pop). No fairness on any
producer; the ready flag is TRUE from Init — a state, not an event.

## The proven defect (M4 = the as-built protocol)

Counterexample (21 states, `E12SchedLivenessM4.cfg`), essence:

```text
s10-11  W0 classifies QUIESCENT while W1 sits in the pop window
        (R2 popped, invisible; running not yet incremented)
        -> W0 DanceContribute not-last, signal S_x, idle=1, contributed[W0]=1
s12     W1 EraseIdleOnPop (:550): idle 1->0   [W0's contribution orphaned;
        contributed[W0] stays 1 — the stale R4 1-bit flag]
s13-19  W1 runs WF (grants R2) and R2 to completion  [ALL WORK DONE,
        r2Acquired=TRUE]
s20-21  W1 re-dances not-last (prev=0), signal S_y, parks (baseline >= S_y)
s22-23  W0's delayed ParkCommit: ProgressPending=FALSE, idle(1)>contributed(1)
        FALSE  -> arms baseline := current epoch  [ABSORBS S_y]
        -> BOTH PARKED, idle=1 < live=2, no leave enabled -> stutter forever
```

The `:582`-erase variant (`E12SchedLivenessB4NoBumpMwS1Erase.cfg`) realizes
the same shape with the mw_s1 fall-through erase as the orphaning site.

## The repair (proven sufficient in the abstract)

`RepairContributionGeneration = TRUE`: every idle reset that can orphan an
outstanding contribution advances a generation counter; `DanceContribute`
records the generation; `ParkCommit` refuses while a LIVE contribution
(contributed=1) is no longer current. The refusing worker signals and
re-loops; the re-dance sees the eraser's fresh contribution and converges
(prev+1 >= live -> LAST -> terminate). Both M4 counterexamples disappear;
all safety and liveness properties hold.

## B4: reset-site classification (model experiments, repaired base)

| Site | Toggle | Bump off | Verdict |
|---|---|---|---|
| `scheduler.cpp:550` pop-path (unlocked) | `BumpPopErase` | `DrainStuckState` CEX | **genuine invalidation event** |
| `scheduler.cpp:582` mw_s1 fall-through (unlocked) | `BumpMwS1Erase` | `DrainStuckState` CEX | **genuine invalidation event** |
| `scheduler.cpp:958` dance-block recheck (under G) | `BumpRecheckErase` | PASS | self-guarded — :582 always precedes on the same path |
| `scheduler.cpp:1452` route erase (under G, atomic with signal) | `BumpPubErase` | PASS | self-guarded — the G-atomic erase+signal pair plus the park-commit scan (#115 closure) rescue any orphan |
| `scheduler.cpp:1065` dance reset-continue (under G) | `BumpDanceResetErase` | PASS | self-guarded — the eraser stays active and re-loops/re-dances |

The Live-resident erase (`:1027`) is out of scope (Drain-only scenario);
its convergence is carried by the not-last signal per the R4/E9-LIFE-8
analysis in the e9 suite.

**Minimal C++ refinement:** advance the dance/contribution generation ONLY
at `:550` and `:582` — the two UNLOCKED erases with no mandatory follow-up
signal and no forced re-loop.

## C++ refinement of the model's atomicity (implementation binding)

The model's `EraseIdle` (reset + generation advance) and `DanceContribute`
(record + count) are atomic actions; the C++ sites are unlocked RMW pairs
that can interleave. The shipped refinement follows three ordering rules
(full argument in `docs/architecture/issue-161-idle-dance-contribution-
generation-gate.md` §Gate 1):

1. the dancer records the generation strictly BEFORE its `fetch_add`;
2. the eraser `exchange(0)`s the count strictly BEFORE its conditional
   bump (and bumps only when a contribution was actually erased);
3. the park commit loads the generation strictly AFTER its
   `idle_workers_` load (the identity term is the last refusal disjunct).

Monotonicity then guarantees every true orphaning is caught (the orphaning
erase's bump is sequenced after the victim's earlier record, so a commit
that sees the erased count also sees the newer generation), while every
residual interleaving can only cause a false refusal — which converges by
the R4 conservative argument. The bridge regression is
`tests/issue161_idle_dance_orphan_test.cpp` (per-worker seams; deterministic
pre-fix FAIL, post-fix PASS).

## M1/M2/M3: documented in-scope closure

M1 (publication without wake signal), M2 (pure-baseline transport — no
own-inbox backstop, no commit scan), M3 (park commit without the progress
recheck) are **closed in this scenario**: every publication is
worker-executed (the drain or a running fiber), so the publisher's own
loop-top pops/steals the ticket, and the own-inbox predicate backstop
(E9 Section 10) covers the parked-owner case. Their hazard classes need a
NON-worker producer or an unbounded busy owner — the issue-#115 shape —
carried by the e9 suite's negative models (`BuggyPrePark` et al.) at their
abstraction. The M1/M2/M3 gates here are composition checks: the toggles
must not re-break convergence on the repaired base. M5 (grant without a
runnable ticket) fails as expected (liveness CEX: the run returns STALLED
with `r2Acquired` false).

## Modeling lessons (recorded for future audits)

1. The first counterexample TLC found exposed a fidelity shortcut: the
   mw_s1 fall-through must NOT model classify->erase->park directly — the
   C++ **reclassify under G at :955** routes a worker whose observer
   vanished to the dance, not the park. `ReclassifyMwS1` is that gate.
2. `DrainStuckState` needs the "no park leave enabled" conjunct: a
   freshly-signaled parked worker (baseline predates the signal) is a
   transient, converging state, not a stuck one.
3. The wake epoch is monotonic Nat here (not a 1-bit toggle): two signals
   between baselines must not alias to "no signal" — a toggle would
   over-approximate misses and could fabricate phantom stuck states.
