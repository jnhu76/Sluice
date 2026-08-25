# Issue #115 — Runnable-Publication / Parked-Worker Liveness: Investigation

> **Archived 2026-08-25 (issue #167 Step 5).** Moved from
> `docs/investigations/`. Classification at move: CLOSED-HISTORY. Body
> preserved as-written; see `docs/history/README.md`.
>
> **Disposition adjudication (Phase D, issue #167):** the "deferred to
> application evidence" disposition is **superseded**. The scheduler
> wake-protocol change was implemented as a post-freeze evidence-derived
> corrective — `spawn()`/`spawn_on()` advance the wake epoch and the G1
> park-commit refusal re-check is unconditional for runnable tickets. Current
> evidence: the gate record
> (`docs/architecture/issue-115-runnable-publication-wake-gate.md`, Gate 0–4,
> 168/168), the fix attribution
> (`docs/post-freeze/post-freeze-final-report.md`), and the formal model
> `spawn-wake-epoch` (`docs/verification/formal/cpp-model-coverage.md`).
> #111 is the same root cause (this file §14, ISSUE_111_SAME_ROOT_CAUSE) and
> is administratively closed. No current obligation remains; the residual
> delegation window is current architecture (Phase-D MW-S2 bridge, phase-g
> design).

**Status:** RUNNABLE_WAKE_ROOT_CAUSE_FIXED
**Baseline master SHA:** `fbb3ea074db8c7c2aafc3c5e4599166e2381f458` (post #119)
**Fix branch:** `fix/issue-115-runnable-publication-wake`
**Compliance gate:** `docs/architecture/issue-115-runnable-publication-wake-gate.md`
**Classification:** post-freeze evidence-derived correctness fix (Issue #115)

---

## 1. Executive summary

`Scheduler::spawn()` and `Scheduler::spawn_on()` are NEW runnable publications,
but they published only the queue state and a target-inbox notification.
No production path ever waits on `inbox_cv` (grep proof: only the declaration
at `include/sluice/async/scheduler.hpp` and `notify` call sites exist — every
park is on the unified Scheduler wake domain `wake_cv_` or the backend
`wait_one` domain), and neither site advanced `wake_epoch_`. A worker already
committed to the unbounded wake-domain park therefore slept through the
publication: the predicate checks epoch / terminate / the worker's OWN inbox
only. A ticket queued on a busy worker with all steal-capable peers parked is
stranded permanently (no deadline, no poll).

**Repair (Candidate A):** both sites now run the canonical
`route_runnable_locked` publication tail — queue membership under
`global_mtx_`→`inbox_mtx_`, inbox released, then `signal_wake_locked()` under
`global_mtx_` (global→wake is the accepted order; the inbox→wake edge is
never taken, which is the inversion the park predicate's wake→inbox read
forbids).

## 2. Defect adjudication (Phase 1)

Classification: **PRODUCTION_RUNNABLE_WAKE_DEFECT** — proven mechanically on
master, no probabilistic sampling needed:

1. `src/async/scheduler.cpp` (pre-fix) `spawn()`/`spawn_on()`: push +
   `inbox_cv.notify_one()`, no `signal_wake_locked()`.
2. `inbox_cv` has no waiter in the entire production tree.
3. `park_on_wake_source` predicate (`src/async/scheduler_park_wake.cpp`):
   `wake_epoch_ != observed_epoch || global_terminate_ || own inbox non-empty`.
4. The G1 park-commit refusal (`unguarded_progress_pending_locked`) closes only
   the pre-commit window and only when NO observer exists; with the target
   worker running a Fiber (the #115 premise) the refusal never fires.

Independent historical corroboration: the comment block in
`tests/runnable_steal_test.cpp` (`steal_steal_run_suspend_wake_resume_on_thief`,
"WHY f0 MUST SPIN UNTIL f2_spawned") documents this exact strand as the CI
failure shape and works around it by holding the future thief inside user
fiber code until after the publication — i.e. the suite has been paying a
workaround tax for this frozen defect.

## 3. Exact pre-fix interleaving (Phase 1)

```text
W1: pops F1, enters user code, blocks (running observer; cannot drain queue)
W0: no local work; drain/classify -> mw_s1 (F1 running) -> park commit:
        G: unguarded_progress_pending? running_fiber_count>0 -> false (pass)
        wake_mtx_: baseline observed_epoch := E
publisher (any thread / a fiber on W1):
        spawn(F2) -> G -> inbox(W1): push F2; owner := W1
        inbox_cv.notify_one()        [inert: no waiter]
        [no wake_epoch_ advance]     [missing RP-2 wake obligation]
W0: wake_cv_.wait predicate: E==E, !terminate, own inbox empty -> SLEEPS
F2: Runnable + stealable + owner busy + all steal-capable peers asleep
    -> permanent strand
```

Pre-commit/post-commit distinction (required by the brief): the G1 refusal
protects `runnable visible -> worker attempts park` (and only with no
observer). It cannot protect `worker committed park -> runnable published
afterwards`; that direction's only transport is the wake-epoch advance —
which the two publication sites omitted.

## 4. Runnable publication path audit (Phase 2)

All queue publications live in `src/async/scheduler.cpp` (verified: no other
TU touches `local_runnable` outside comments/predicate reads).

| Publication path | Persistent state | Queue / owner | G held | inbox notify | wake epoch | Parked peer may need it? |
|---|---|---|---|---|---|---|
| `spawn()` | local_runnable push + fiber_owner_ | RR target | yes | yes (dead) | **MISSING → bypass** | **YES** (target busy) |
| `spawn_on()` | same | explicit worker | yes | yes (dead) | **MISSING → bypass** | **YES** |
| `route_runnable_locked()` | local_runnable push | owner / RR | yes | yes (dead) | yes | yes (canonical) |
| `publish_waiting_fiber_runnable_locked()` | → route_runnable_locked | owner | yes | inherited | yes | yes |
| Completion drain (reap → route) | → route_runnable_locked | owner | yes | inherited | yes | yes |
| `wake_ready_flags_locked()` | → route_runnable_locked | owner | yes | inherited | yes | yes |
| WaitQueue wake/cancel (`wake_wait_one_locked`, `cancel_wait`) | → publish_waiting… | owner | yes | inherited | yes | yes |
| Timer expiry (`pump_deadlines_locked` / `advance_clock`) | route + `signal_wake_locked` | owner | yes | inherited | yes | yes |
| Event / Mutex / Semaphore / Condition / RwLock wakes | → publish_waiting… | owner | yes | inherited | yes | yes |
| Select publication (`select_publish_locked`) | `route_runnable_locked` | caller owner | yes | inherited | yes | yes |
| Worker-loop epilogue rescue | move to pending_spawn_ + signal | pre-run domain | yes | no | yes | yes |
| `try_steal()` | MOVE ticket + owner transfer | thief | yes | yes (dead) | no — not needed | no: thief is live and self-executing (MOVE, not publication) |
| `run()` initial distribute | MOVE pending→inboxes | first-N RR | yes | yes (dead) | no — not needed | no: worker threads do not exist yet |
| `route_runnable()` (unlocked variant) | local_runnable push | owner | no | yes (dead) | no | **UNUSED — zero callers** (legacy; left untouched, flagged for follow-up cleanup) |

Conclusion: `spawn()`/`spawn_on()` were the only NEW-publication wake bypasses.
MOVEs (steal, distribute) legitimately carry no new obligation.

## 5. Deterministic reproducer (Phase 3)

`tests/issue115_runnable_publication_wake_test.cpp` (internal-testing variant;
registered in `xmake/tests/async_internal.lua`). One narrow test seam added:
`PhaseTag::scheduler_park_baseline_recorded` — pauses the parking worker
AFTER the baseline is recorded and BEFORE it takes `wake_mtx_` to enter
`cv.wait`, with no locks held (the exact mirror of the existing pre-baseline
`scheduler_park_commit` seam; included in `release_all_phases`). A publication
issued under this hold is strictly post-commit: the cv predicate is its only
possible transport.

Construction (cases A/B): W1 runs F1 blocked on a std::condition_variable
rendezvous (running observer, no busy-spin); W0 (pinned to finish its trivial
F0 only after F1 started, so it can never steal F1) commits its park and is
held at the seam; the coordinator then publishes F2 onto busy W1 — case A via
**production `spawn()`** (round-robin deterministically positioned on W1: F0
consumed slot 0 with a filler spawn through the same production path), case B
via `spawn_on(f2, 1)` — and releases the seam. Case C targets an idle/parked
worker (normal-delivery guard: own-inbox backstop + epoch signal coexist,
exactly-once). Case D (round 2, §6a) reuses the EXISTING pre-baseline
`scheduler_park_commit` seam — no new seam: W0 is held at the park-commit
boundary (post last-steal, pre G1-recheck/baseline, no locks held) while the
coordinator publishes F2 onto busy W1; the publication's epoch signal lands
entirely BEFORE the baseline W0 records on release, so it is absorbed and the
G1 persistent-state recheck is the only remaining transport.

Watchdogs are bounded (10 s), fail-closed, and rescue the run via the
production external-wake handle so a pre-fix failure reports cleanly instead
of hanging. No sleep-ordering anywhere.

## 6. Chosen repair (Phase 4)

**Candidate A** — publish the Scheduler wake after queue insertion, mirroring
the `route_runnable_locked` publication protocol (state first, inbox lock
released, then the wake signal). Not byte-identical: the retained legacy
`inbox_cv.notify_one()` fires after the release where route notifies inside
the inbox critical section — equivalent for a cv with no waiter. Not shared
code either: the spawn sites have no admission-demotion / terminate-clear
semantics, and route's are load-bearing there; a shared helper would couple
two different publications' G-scope side effects for zero invariant gain —
Candidate B rejected as over-abstraction for a two-site correction.

Candidates C (victim re-scan in the park predicate) and D (periodic timeout)
rejected per the brief: C expands the wake predicate to O(N) inbox locks under
`wake_mtx_` (the exact lock-order expansion the E9-CORRECTIVE predicate
redesign removed) and hides polling semantics; D masks the correctness bug.

## 6a. Round-2 repair (review follow-up): G1 refusal priority

The first round documented the pre-baseline absorbed window as residual risk
(§13, original wording). The PR review correctly re-adjudicated it as
**in-scope**: Issue #115's own title — "spawn_on onto a busy worker can strand
runnable fibers" — describes the final state that window produces (asleep
steal-capable worker + stranded ticket + busy owner), identically to the
post-baseline window round 1 fixed. A defect class is not closed while either
interleaving still reaches it; "Closes #115" and "residual risk" cannot both
hold for the same final state.

The window, precisely: the park G-section (`global_mtx_` held: G1 recheck,
then the baseline under nested `wake_mtx_`) serializes with every publication
(push under G→inbox, signal under G→wake), so a publication is either
entirely BEFORE the G-section — visible to the G1 recheck's queue scan — or
entirely AFTER the baseline — visible to the cv predicate. No gap exists.
Round 1 closed the AFTER side (the publication now advances the epoch, which
the predicate compares against the baseline). The BEFORE side stayed open
because `unguarded_progress_pending_locked()` ran its observer exemption
FIRST: with any fiber running anywhere (`running_fiber_count_ > 0`) it
returned "no progress pending" without ever scanning the queues, the baseline
then absorbed the already-signaled epoch, and the last steal-capable worker
slept — the strand.

**Repair** — re-order the checks in `unguarded_progress_pending_locked()`:
the runnable checks (`pending_spawn_` non-empty; any active participant's
`local_runnable` non-empty) run BEFORE the observer exemption. Semantics: a
running Fiber is an observer for ITSELF, never for another runnable ticket
queued behind it; when a stealable ticket exists, the last steal-capable
worker refuses the park, re-loops, steals it, and becomes the executor. The
observer exemption now covers only accepted backend work and resident waits,
whose designated observer is the MW-S2 participant (its bridge wakes on
backend progress — unchanged Phase-D design).

Why this is NOT the forbidden "scan all victim queues inside the cv
predicate": the scan is the G1 persistent-state recheck under `global_mtx_`
(the same domain every publication serializes under), where the queue scan
ALREADY existed below the exemption; the cv predicate (`wake_mtx_` held) still
reads only epoch + terminate + the worker's OWN inbox. No lock-order change:
G→inbox is the accepted publication edge this very function already used; no
new edge, no `wake_mtx_`→inbox expansion, no periodic tick.

No-spin argument: every refusal is followed either by a successful steal
(the refused worker's loop-top `try_steal` deterministically takes a ticket
that the recheck just observed non-empty under the same serialization) or by
queues that emptied between recheck and steal (another thief won); in the
latter case the next park's G1 finds no ticket and parks. Bounded
ping-pong, no livelock; the refusing path additionally signals the wake
domain (pre-existing G1 behavior) so a non-acting refuser cannot spin while
an electable sibling sleeps.

Deterministic proof: Case D (`issue115_absorbed_publication_refuses_park`,
§5) fails 3/3 on the round-1 HEAD (watchdog fail-closed on `progressed`) and
passes post-repair with F2 stolen and executed exactly once by the former
parker. Pre-fix/post-fix, both baseline sides:

```text
           runnable publication (spawn/spawn_on)
                          |
            +-------------+--------------+
            |                            |
     BEFORE the park G-section    AFTER the baseline
            |                            |
     G1 queue scan (now            wake_epoch advance
     UNCONDITIONAL for            (round-1 signal) ->
     runnable tickets) ->             cv predicate fires
     REFUSE park -> re-loop
     -> steal -> execute
            |                            |
            +-------------+--------------+
                          |
                       progress
```

## 7. Lock-order proof (RP-3)

```text
accepted edges (unchanged):  global -> inbox, global -> wake, global -> access,
                             wake -> own-inbox (predicate read)
forbidden edge:              inbox -> wake   (would cycle with the predicate)
fix shape:                   G held; inbox taken INSIDE G, released;
                             signal_wake_locked takes wake under G; notify after.
```

`signal_wake_locked` also calls `interrupt_backend_waiters` with `wake_mtx_`
already released and `global_mtx_` still held — the wait source is a leaf that
never acquires Scheduler locks (Phase-G design §4), so no new edge appears.

## 8. Why the wake cannot be lost (RP-1/RP-2 argument)

`std::condition_variable::wait(lk, pred)` is `while (!pred()) wait(lk)`
(cppreference): the predicate is evaluated BEFORE the first block and after
every wake. The predicate's authority is `wake_epoch_` under `wake_mtx_`.
For W0 parked on baseline E and a publication of F2 after the commit:

* publication stores queue state (under G→inbox, released), then advances
  `wake_epoch_` E→E+1 (under wake_mtx_), then notifies;
* if W0 is already in `cv.wait`: the notify (or any later notify / spurious
  wake) re-evaluates the predicate → `E+1 != E` → returns;
* if W0 has not entered `cv.wait` yet: the predicate is false→true at entry
  (epoch read under `wake_mtx_` sees the advanced value) → never blocks;
* therefore no interleaving of the post-commit publication with the physical
  wait can lose the wake. The proof uses no timeout, no fair scheduling, no
  periodic wake, and no assumption that the busy owner ever finishes.

## 9. Backend-wait bridge impact (Phase 5)

`signal_wake_locked`'s tail interrupts a parked MW-S2 participant when
`backend_wait_active_`. With the fix, a spawn during a backend park is such an
interrupt. Correct by construction: runnable work means the Scheduler must
re-evaluate; the participant's Phase-D drain re-classifies (mw_s1) and the
loop-top runs/steals the new ticket. One-shot per `wait_one` (D4-RM13),
commit-to-park handshake (D4-RM14) untouched; each interrupt maps to a real
publication (no interrupt→re-enter→same-interrupt loop: re-parking requires a
fresh classify, and the classify observes the consumed publication's state).
Phase-G suites re-run green (§11).

## 10. Fiber ownership invariants (Phase 7)

Unchanged: created→runnable exactly-once (`make_runnable` guard);
`fiber_owner_` recorded in the same inbox critical section; no duplicate
ticket (E7-T2 guard retained); steal remains MOVE + owner transfer;
`suspend_switch_pending` restrictions untouched. Owner SELECTION (RR) is
unchanged — the fix is wake transport only. Verified by the full E8 suite
(`runnable_steal_test` 3/3 standalone) and the 168/168 Debug gate.

## 11. Evidence

```text
Baseline (master fbb3ea0, pre-change):   Clang Debug ......... 167/167 PASS
Pre-fix reproducer (fix branch + seam + tests only):
    issue115_spawn_wakes_parked_peer_busy_target       3/3 FAIL (progressed) [strand]
    issue115_spawn_on_wakes_parked_peer_busy_target    3/3 FAIL (progressed) [strand]
    issue115_spawn_on_idle_target_delivers_once        FAIL (progressed)*
    (* A/B are seam-forced deterministic: the publication is held until W0's
    park baseline is recorded. C arms no seam — its pre-fix failure requires
    W1 to have parked before the coordinator's spawn, near-certain at the
    1 ms wait_flag granularity but not forced by construction; the 12/12
    pre-fix stress failures in §12 corroborate the mechanism.)
Round 2 (Case D, on the round-1 HEAD — signal fix in, G1 priority not yet):
    issue115_absorbed_publication_refuses_park         3/3 FAIL (progressed)
    [seam-forced: publication held strictly pre-baseline; the G1 observer
    exemption short-circuits the queue scan, the baseline absorbs the epoch]
Post-fix:
    issue115_runnable_publication_wake_test (A+B+C+D)  5/5 full runs PASS
    runnable_steal_test standalone                     3/3 PASS
    Clang Debug  xmake test -v ........................ 168/168 PASS (rc=0)
        [re-executed on the final HEAD after the round-2 G1 change]
    Clang Release xmake test -v ........................ 168/168 PASS (rc=0)
        [re-executed on the final HEAD after the round-2 G1 change]
    TSan: full suite 168/168 rc=0; focused race binaries
        (issue115, runnable_steal, phase_g_backend_progress_wake,
        phase_g_closeout, multi_worker_coord, scheduler_worker_topology_race,
        application_runtime_worker_topology) 7/7 rc=0, 0 ThreadSanitizer
        warnings — covers the modified race classes: spawn-publication vs
        park commit/baseline, publish vs steal, publish vs G1 recheck
        (round-2 class), wake signal vs parked waiter
    ASan+UBSan: full suite rc=0, 0 failures, 0 sanitizer diagnostics;
        runnable_steal_test repeated samples 0/15 failures (the #111
        historical exposure shape)
    Phase-G focused (Debug + TSan): phase_g_backend_progress_wake_test,
        phase_g_closeout_test — green (bridge/deadline/park-convergence
        invariants intact)
Mechanical: git diff --check clean; check-doc-links self-test + run PASS;
    verify-architecture-docs PASS; mechanical-facts self-test + run PASS
    (after live count rows updated: default-gate targets 167→168,
    scheduler.cpp 1952→1975→1986 (round-2 G1 reorder + header contract),
    scheduler_park_wake.cpp 1144→1153→1155 (round-2 G1 comment refresh));
    verify-completion-authority / verify-request-arena negative-compile
    probes PASS (12 + 6 cases)
```

## 12. Stress evidence (Phase 10)

Hostile scheduling on this WSL2 box — 24-way oversubscription (12 ×
runnable_steal_test + 12 × issue115 binary in parallel, `timeout` guarded),
Debug layout, signal temporarily disabled via a two-line probe edit to
reproduce the pre-fix production behavior (edit reverted; diff re-verified
marker-free and binaries re-run green):

```text
pre-fix  : issue115 binary 12/12 FAIL (rc=1 — the watchdog fail-closed on the
           strand under every schedule; the seam-forced interleaving is
           schedule-independent)
           runnable_steal_test 0/12 hangs (the suite's in-test workaround —
           f0 spinning until f2_spawned — closes the window by construction;
           the historical CI flake family was probabilistic and this sample
           did not hit it)
post-fix : 24/24 rc=0, zero watchdog activations, zero hangs
```

Stress is evidence, not proof: the deterministic seam reproducer (§5) is the
correctness proof.

Round-2 stress (final HEAD, after the G1 priority repair): 24 parallel
instances of the full issue115 binary (A+B+C+D, Release layout) — 0/24
failures, zero watchdog activations; runnable_steal_test 3/3 standalone.

## 12a. Performance sanity (Phase 11)

Per-spawn added work: one `wake_mtx_` critical section (`++wake_epoch_`), one
`wake_cv_.notify_all()` (waiter-bounded), one acquire load of
`backend_wait_active_` (the bridge's no-participant fast path). This is the
identical traffic class `route_runnable_locked` already emits on every wait
resolution, so spawn-rate wake traffic is bounded by the same worker-count
coalescing as routes; `signal_wake_locked` invokes nothing that publishes
runnables (no recursive wake loop), and a woken worker that finds nothing to
do re-classifies once and re-parks (the R4 damping analysis is unchanged —
spawn now behaves exactly like a route publication).

Empirical: full issue115 binary (three park/steal constructions) 0.56 s wall
post-fix vs 10.5 s+ per failing case pre-fix (watchdog); full Debug suite
6.4 s/168 tests post-fix vs 6.7 s/167 baseline. No wake storm, no idle
busy-spin observable.

## 13. Residual risk

* **Pre-baseline absorbed window — CLOSED by the round-2 repair (§6a).** The
  original round-1 wording kept it as accepted `route_runnable_locked`
  delegation semantics; the review follow-up correctly re-adjudicated it
  in-scope and the G1 refusal-priority change closed it for ALL runnable
  publications (spawn, spawn_on, route alike — they share the same G1
  recheck). Both sides of the baseline are now closed: pre-G-section
  publications are caught by the unconditional queue scan (refuse → re-loop
  → steal), post-baseline publications by the epoch-vs-baseline predicate.
  The remaining delegation window is ONLY accepted backend work and resident
  externally-resolved waits, whose designated observer is the MW-S2
  participant (Phase-D bridge design, unchanged, Phase-G suites green).
* **Dead-code wake bypass retained:** the unused unlocked
  `Scheduler::route_runnable()` (zero callers) keeps its notify-only shape.
  Flagged for a follow-up hygiene change; deleting it here would widen the
  diff without affecting any reachable behavior.
* **`inbox_cv` notifications are inert** in every publication path (no
  waiter). Retained verbatim to minimize the diff; documented at the two
  changed sites.
* **Per-park G1 cost:** with the observer exemption no longer first, a
  worker parking while ANY runnable ticket exists anywhere now scans the
  participant queues (bounded by worker count, mutex-free size reads under
  `global_mtx_`) before refusing — and refusal leads to a steal, not a
  re-scan loop (§6a no-spin argument). The parked-delegation fast path
  (fiber running, no tickets) still returns after one scan of empty queues.

## 14. Issue #111 relationship

The #111 family (`runnable_steal_test` ASan hang) was historically dodged by
the f0-spin workaround documented in T3's comment — the same interleaving this
fix removes the need for. The deterministic #115 failure (publication after a
committed unbounded park onto a busy owner, no wake epoch) explains that
family's mechanism: under ASan timing the workaround's spin window itself
could still lose to the park commit before `f2_spawned` was observed.
Classification: **ISSUE_111_SAME_ROOT_CAUSE** (administrative
close/supersede of #111 by #115 is indicated after merge; not done in this
patch per the brief).
