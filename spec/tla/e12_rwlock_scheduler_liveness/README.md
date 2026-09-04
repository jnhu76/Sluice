# e12-rwlock-scheduler-liveness — issue #161 combined liveness suite

The T22 multi-worker drain hang (`async_rwlock_test` /
`rwlock_mw_cancel_and_unlock_on_different_workers`, CI PR #160):
both workers asleep in the unbounded wake-domain park, `run_impl` blocked in
`join`, **after every fiber already completed**. This suite proves the
protocol-level root cause, proves the shape of the minimal repair, and
classifies which idle-count reset sites are genuine "contribution
invalidation events".

Tracking issue: **#161**. Verifier: `scripts/formal/verify-e12-sched-liveness.sh`
(8 positive gates PASS, 6 negative gates CEX, 1 fail-closed reachability
witness). Owner doc:
`docs/history/closeout/issue-161-idle-dance-contribution-generation-gate.md`.

**Split-window model round (this revision):** the two unlocked erase sites
are now modeled as the TWO steps the C++ performs — the `exchange(0)`
(`EraseIdleOnPop` / `EraseIdleMwS1`) and the arbitrarily-delayable
conditional bump (`BumpPopGen` / `BumpMwS1Gen`), with the intermediate
`PopBumpPending` / `MwS1BumpPending` stages. The round had two outcomes:

1. the ROUTE-PUBLICATION erase (`route_runnable_locked`, `BumpPubErase`)
   was RECLASSIFIED to a genuine invalidation site — with the split
   interleaving live, `E12SchedLivenessB4NoBumpPubErase.cfg` finds the
   M4 stuck shape with the route erase as the orphaning site (its old
   "self-guarded" verdict was an artifact of the fused-atomicity model);
2. the split window itself (a park commit reading the erased count with
   the still-current generation) is REACHABLE — witness
   `E12SchedLivenessSplitWindow.cfg` violates `SplitWindowNeverArmed` —
   and the repaired-constants safety gate proves the window costs only a
   transient park. This is why the C++ refinement argument is the honest
   dichotomy, not a visibility claim (see below).

**Review-correction round (this revision):** the formal model now matches
the C++ refinement exactly, not more strongly.

- The positive safety/liveness gates (`E12SchedLivenessSafety.cfg` /
  `E12SchedLivenessLiveness.cfg`) run at the EXACT as-built repaired
  constants: the three GENUINE invalidation sites bump (`pop`, `mw_s1`,
  `route-publication`); the `:958` recheck and `:1065` reset-continue
  sites do NOT (C++ uses plain `store(0)` there). The over-strong
  all-bumps-on exploration remains, as `E12SchedLivenessSplitWindowSafety.cfg`
  (safety on the same constants as the split-window witness).
- `EraseIdleBumping` advances the generation only when the erase actually
  erased a contribution (`idleCount > 0`), matching C++ `if (erased != 0)`.
  A zero-count erase invalidates nothing and must not bump.
- `ReclassifyMwS1`'s mw_s1-still branch is no longer pinned shut by a
  contradictory `UNCHANGED` (the outer frame also pinned `idleCount` /
  `contributionGen`, which the branch's erase modifies — with
  `BumpRecheckErase` on, the transition was unsatisfiable and TLC silently
  deleted the recheck path from the state space). The branch is now live.
- M2's verdict is unified as **DOCUMENTED PASS** in the closed T22 scope
  (module header, cfg, verifier, and README agree); the mutant cfgs'
  duplicate constant assignments are removed.

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
| `scheduler.cpp:550` pop-path idle reset | `EraseIdleOnPop` + `BumpPopGen` | **unlocked, no mandatory follow-up signal — genuine invalidation site (B4); modeled as the split exchange/bump pair (the C++ window)** |
| `scheduler.cpp:1211-1219` make_running | `StartFiber` | the ticket becomes a classify-visible observer only here |
| `scheduler.cpp:567` unlocked classify | `DrainClassify` (with the :562-568 drain) | drain publishes + signals + erases idle atomically under G |
| `scheduler.cpp:582` mw_s1 fall-through erase | `EraseIdleMwS1` + `BumpMwS1Gen` | **unlocked, arbitrarily delayable — genuine invalidation site (B4); split exchange/bump pair** |
| `scheduler.cpp:955-958` dance-block reclassify + erase | `ReclassifyMwS1` | self-guarded (:582 always precedes on the same path) |
| `scheduler.cpp:1035-1093` idle dance | `DanceContribute` | contribute; not-last signal (E9-LIFE-8); last-idle recheck/terminate |
| `scheduler.cpp:1065` reset-continue | `DanceContribute` middle branch | self-guarded (the eraser stays active and re-loops) |
| `park_wake.cpp:295-338` park commit | `ParkCommit` | recheck (`RefusePark`) then arm baseline — the refusal is the repair point |
| `park_wake.cpp:379-396` cv predicate | `CanLeavePark`/`LeavePark` | epoch / terminate / own-inbox backstop |
| `scheduler.cpp:1452-1483` route erase+signal | `EraseIdleBumping(BumpPubErase)` inside `DrainClassify`/`FiberStepWF` | **RECLASSIFIED genuine (split-window round): a dance contribution made before a routed grant can be orphaned here with the contributor's 1-bit flag still claiming it** |
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
the same shape with the mw_s1 fall-through erase as the orphaning site, and
the route-publication variant (`E12SchedLivenessB4NoBumpPubErase.cfg`,
split-window round) realizes it with the route erase as the orphaning
site: a not-last DanceContribute → WF's unlock grants R2 and the route
erase zeroes the live count → work completes → the eraser's final
not-last dance → the orphaned contributor's late ParkCommit arms a
baseline after the last signal → both Parked.

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
| `scheduler.cpp:550` pop-path (unlocked, split) | `BumpPopErase` | `DrainStuckState` CEX | **genuine invalidation event** |
| `scheduler.cpp:582` mw_s1 fall-through (unlocked, split) | `BumpMwS1Erase` | `DrainStuckState` CEX | **genuine invalidation event** |
| `scheduler.cpp:1452` route publication (under G) | `BumpPubErase` | `DrainStuckState` CEX | **genuine invalidation event — RECLASSIFIED by the split-window round (the fused-atomicity model had masked the trace; the old "self-guarded" PASS row was an artifact)** |
| `scheduler.cpp:958` dance-block recheck (under G) | `BumpRecheckErase` | PASS | self-guarded — :582 always precedes on the same path |
| `scheduler.cpp:1065` dance reset-continue (under G) | `BumpDanceResetErase` | PASS | self-guarded — the eraser stays active and re-loops/re-dances |

The Live-resident erase (`:1027`) is out of scope (Drain-only scenario);
its convergence is carried by the not-last signal per the R4/E9-LIFE-8
analysis in the e9 suite.

**Minimal C++ refinement:** advance the dance/contribution generation at
the pop-path erase, the mw_s1 fall-through erase, AND the
route-publication erase — the three genuine invalidation events (each an
`exchange(0)` that bumps only when a contribution was actually erased).

## C++ refinement of the model's atomicity (implementation binding)

The G-section erase sites stay single atomic actions (`EraseIdleBumping`);
the two UNLOCKED sites are modeled as the split exchange/bump pairs the
C++ performs. The shipped refinement follows three ordering rules (full
argument in `docs/history/closeout/issue-161-idle-dance-contribution-
generation-gate.md` §Gate 1):

1. the dancer records the generation strictly BEFORE its `fetch_add`;
2. the eraser `exchange(0)`s the count strictly BEFORE its conditional
   bump (and bumps only when a contribution was actually erased);
3. the park commit loads the generation strictly AFTER its
   `idle_workers_` load (the identity term is the last refusal disjunct).

Rule 3 does NOT make the generation load observe the bump whenever the
idle load observes the erased count — `idle_workers_` and `dance_epoch_`
are distinct atomics with independent modification orders, so the claim
"every true orphaning is caught by monotonicity" (an earlier draft of
this section) is FALSE as a C++ argument. The witness gate
(`E12SchedLivenessSplitWindow.cfg`, `SplitWindowNeverArmed` violated)
keeps that honest: the split window — a park commit reading the erased
count together with the still-current generation — is REACHABLE. Its
safety is the two-case dichotomy: (a) generation mismatch → refuse and
converge by the R4 conservative argument; (b) generation match with the
count already erased → the eraser's protocol is incomplete, its re-dance
not-last signal is G-serialized strictly AFTER the committing worker's
arming (the commit holds `global_mtx_` across both loads and the arming,
and observing the erased count proves no re-dance contribution is
G-visible yet), so the park is transient — the window reorders who wakes
but cannot rebuild the terminal M4 stuck state, whose mechanism needs
the absorbed signal to PRECEDE the arming. The repaired-constants safety
gate on the SAME split constants proves exactly this (DrainStuckState
holds). Bridge regressions: `tests/issue161_idle_dance_orphan_test.cpp`
(pop-path site, per-worker seams) and
`tests/issue161_pub_erase_orphan_test.cpp` (route-publication site,
per-worker seams + the baseline seam; deterministic pre-fix FAIL,
post-fix PASS).

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
4. Split-window round: modeling two unlocked RMWs as ONE atomic action
   fuses a window the C++ really has — and the fused model can silently
   VINDICATE a site the split model convicts (the route-publication
   erase's "self-guarded" verdict was exactly this artifact, compounded
   by an unfaithful LoopTop `contributed` reset). When the implementation
   performs N distinct atomics, the model needs N distinct steps plus a
   reachability witness for the window, and the safety gate on the same
   constants must show what the window costs (here: a transient park).
