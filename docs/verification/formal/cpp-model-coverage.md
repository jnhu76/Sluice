# C++ ↔ TLA+ Model Coverage Matrix

> Current developer document (2026-08-18 formal realignment audit,
> `audit/formal-cpp-tla-realignment`). This is the authoritative map of what
> each TLA+ suite abstracts, which C++ code owns the protocol, where the
> executable regression bridge lives, and what remains debt. The suite IDs
> and gate counts match `spec/tla/manifest.json`; per-suite details live in
> each `spec/tla/*/README.md`.
>
> Governing principle: **TLA+ serves the C++ design** (AGENTS.md §17). The
> C++ public contract and production implementation are the source of truth;
> models abstract load-bearing races, they do not verify the implementation.

Status vocabulary: `GOOD` (audited, faithful, no repair needed) ·
`REPAIRED` (this audit changed the suite/harness) · `PARTIAL` (real
abstraction, materially incomplete scope) · `DEBT` (unmodeled, recorded) ·
`RETIRED` (removed).

## Matrix

| Protocol | C++ authority | TLA suite | Safety | Liveness | Negative | C++ bridge | Status |
| -------- | ------------- | --------- | ------ | -------- | -------- | ---------- | ------ |
| Bounded blocking-I/O worker pool (submit/dequeue/complete/get/shutdown) | `src/blocking_io_pool.cpp`, `include/sluice/detail/blocking_io_pool_impl.hpp` | blocking-io-pool | 6 inv (queue/worker bounds, task lifecycle, quiescence) | StarvationFree under WF(Dequeue/Complete) — worker diligence + callable-termination boundary assumptions (documented in the spec doc) | **added by audit**: BuggyUnboundedDequeue (worker bound), BuggyShutdownDiscardsQueued (drain-on-close) | `BlockingIoPoolTest` (bounds, shutdown drain, wait_idle) | REPAIRED (dangling binding path, missing README, zero negatives, vacuous `Linearizable` gate) |
| io_uring transport ledger poison/recovery (Class-A retire, control-deferral, no-submit-after-poison) | `src/async/uring_backend.cpp` (transport ledger, cookie identity, `poison_and_recover_locked`) | d1-uring-poison | 9 inv (logical identity, ledger bounds, poison quiescence) | none (safety-only, deliberate) | 3 cfg-flip faults, all name-asserted | `tests/uring_submit_failure_test.cpp`, D1/D4 mutation evidence docs | GOOD (adjacent arena submission path now covered by request-arena) |
| RequestArena/RequestSlot explicit-I/O accepted-request lifecycle (five-stage admission, Scheme-B arbitration, terminal exactly-once, generation reuse, pin/reap, borrow-through-reap, Decision-11 verbatim) | `include/sluice/async/detail/request_arena.hpp`, `request_slot.hpp` | **request-arena (NEW, closes the formal-debt gap)** | 17 inv (accounting, borrow window, winner/pub exactly-once, pin phase/eligibility, gen advance, canceled-source) | acked-backend_ready ⇒ published; pin ⇒ acked (WF Enqueue/Reap only) | NEG-RA-1..6 (double terminal, stale cancel, direct publish, no-gen-increment, reap-ignores-pin, running-cancel-stores) + 2 wrong-prop controls + W1–W5 witnesses | `request_arena_test`, `request_lifecycle_scheme_b_test`, `request_waiter_borrow_lease_test`, `request_arena_cancel_intent_test`, `request_arena_death_test`, `threadpool_backend_scheme_b_race_test` | REPAIRED (debt → modeled; residual multi-slot ready-ring ORDER recorded as PARTIAL in the gap entry) |
| Runnable-ticket publication (one ticket per runnable fiber) | `src/async/scheduler.cpp` (spawn/wake/route, `fiber.hpp` state machine) | e7-publication | 9 inv (ticket multiplicity, waiting/runnable registration coupling) | none (safety scope; progress is e7-multiworker/e9) | BuggyDuplicatePublish → `InvDoneNoTicket` (name-asserted) | `fiber.hpp` make_runnable CAS + scheduler wake tests | REPAIRED (dangling owner_doc removed; inbox transport models dead storage — recorded, retirement deferred to e8 fold-in) |
| Multi-worker admission classification + blocking-park commit (MW-S1/S2/S3, re-drain before commit) | `src/async/scheduler.cpp` (`classify_locked_impl`, Phase A/B elect-commit) | e7-multiworker-progress | MWInv2/3/6/7 | none (liveness carried by e9) | BuggyAdmission (commit skips re-drain), BuggyOutstanding (classifier from registrations) — both name-asserted | scheduler classification/park-commit tests | GOOD (~70% overlaps e9 for admission safety; retained for MWInv7 + the two negatives) |
| Runnable-ownership transfer / work stealing (owner/exec/wait triple authority) | `src/async/scheduler.cpp` (`try_steal`, `fiber_owner_`, wake routing), `scheduler_park_wake.cpp` | e8-ownership-transfer | 11 inv (`InvLocalMatchesOwner` etc.) | none | BuggyOwner (steal without owner transfer) → `InvLocalMatchesOwner` — **now name-asserted by the audit** | scheduler steal/routing tests | REPAIRED (empty binding filled; verifier upgraded from any-cex to named) |
| Scheduler/backend wake convergence (park domains, epoch, split-wait bridge, R1–R4) | `src/async/scheduler_park_wake.cpp`, `scheduler.cpp` (park commit, `signal_wake_locked`, bridge) | e9-park-wake | Inv2/4/6/8/9/10 + Life1/3/5 | Life2/4/7/8 under worker/self fairness (producers deliberately unfair — justified) | DrainParks (temporal), MixedSource, PrePark (pre-Phase-G snapshots), NoBridge (one-line Phase-G mutant) | scheduler park/wake deterministic tests, `threadpool_wait_drain_deadlock_test` | REPAIRED (reference-config vacuity: the unconditional `~SplitWait` LeavePark disjunct made Inv2/Inv4/InvLife1 and Life2/Life4 near-trivial under SplitWait=FALSE — see suite README note; empty binding filled) |
| Wake-handle notify-vs-destructor lease (Control block lifetime) | `src/async/scheduler_park_wake.cpp` (`notify`), `scheduler.cpp` (dtor invalidation) | e9-wake-handle-lifetime | LifeInv1–6 | Life7/7b (WF notifier chain, SF destructor — SF documented as stronger than `std::mutex` guarantees, boundary assumption) | BuggySnapshot (release-before-callback) → `LifeInv4DestroyedNoCallback` — **now name-asserted** | wake-handle lifetime seam tests | REPAIRED (verifier named assertion; binding filled) |
| WaitNode terminal resolution (exactly-once resolve, single publication) | `include/sluice/async/wait_node.hpp`, `wait_queue.hpp`, `scheduler_park_wake.cpp` | e10-waitnode | 5 inv (**tautological `InvNoTerminalResurrection` and redundant `InvTerminalNotLinked` removed from the gate by this audit**) | EventualResolution under WF(resolvers) — caller-action fairness, documented as conditional | BuggyNoWinner → `InvNoDoubleCompletion` (name-asserted) | wait-node/wake-one tests | REPAIRED (cfg pruned; README stale "NOT executed" claim fixed; binding filled) |
| Deadline timer wait (admission expiry, epoch isolation, lifetime closure, bounded park) | `src/async/scheduler_timer.cpp` | e11-timer-wait | I1–I7 | I6 DeadlineParkLiveness under WF(Tick, ResolveTimer) — best-grounded fairness in the tree (steady_clock + pump call sites) | NEG-1..6 all real defect classes (name-asserted) | timer tests (`advance_clock` deterministic driver) | GOOD (strongest negative set of the primitive suites) |
| Manual-reset event set-drain / reset epoch isolation | `src/async/scheduler_event.cpp` | e12-event | E1/E2/E3/E5/E6 (**dead variable `wokenBySetDrain` removed by this audit**) | E4 drain liveness under WF(drain loop) — justified (setter's own call frame) | NEG-1/2/3 real; NEG-4 labeled ceremonial ghost-coupling probe (retained to pin E5) | event tests | REPAIRED (dead variable; NEG-4 relabeled; binding filled) |
| Semaphore permit conservation / FIFO transfer / admission recheck / overflow | `src/async/scheduler_semaphore.cpp` | e12-semaphore | 12 inv (P1–P10) | none — justified: `sem_release` transfers synchronously under `global_mtx_`; revisit if release ever splits | NEG-SEM-1..7 (Neg3 `+2` store is the one ceremonial mutant; wrong-prop gate proves specificity) | semaphore tests | GOOD (binding filled) |
| Async mutex FIFO handoff (owner commit before publication, no-barging) | `src/async/scheduler_mutex.cpp` | e12-async-mutex | 21 inv | none — FIFO order is safety (`InvFIFOGrant`); grant liveness deferred | NEG-M1..M11 (M4≈M5 near-duplicate pair, both retained — distinct laws) | async mutex tests | GOOD (binding filled) |
| Condition-variable lost-notification closure + unified reacquire/ordinary FIFO | `src/async/scheduler_condition.cpp` | e12-async-condition | 26 inv + 2 reachability (mixed-FIFO orders) | none — no-lost-notify encoded as safety | NEG-C1..C10 (C6 ghost-mutation is the one ceremonial mutant) | condition tests | GOOD (binding filled) |
| Bounded MPMC queue lease move + close/drain topology (B1–B7) | `src/async/queue_port.cpp` | e12-queue | A1–A12 + B1–B7 | none — close drains synchronously in one CS (revisit if async drain) | 7 real negatives + scenes R1–R8 (B3/B6 end-to-end) | queue tests | GOOD (binding filled) |
| Rwlock writer-fair FIFO reconcile (unlock/cancel/expire head-prefix grant) | `src/async/scheduler_rwlock.cpp` (`rwlock_grant_from_head_locked`) | e12-rwlock | RW1–RW10 **+ `InvNoStrandedGrantableHead` (added by this audit)** | none (safety-only) | ReaderBypass **+ NoReconcile (added by this audit — release drops grant-from-head, the two-step unlock regression)** | `tests/async_rwlock_test.cpp`: `rwlock_t6_cancel_and_head_reconcile`, `rwlock_t12_last_reader_grants_writer`, `rwlock_head_writer_cancel_grants_reader_prefix_immediately`, `rwlock_cancel_reconcile_preserves_fifo`, `rwlock_mw_concurrent_last_reader_unblock_grants_writer` | REPAIRED (audit-closed negative gap) |
| Multi-arm select winner linearization (contract layer) | `src/async/select.cpp` (+ `select_event.cpp`, `select_timer.cpp`) | e13-select-core | layered + refinement PROPERTY chain + R1–R12 witnesses | none (safety + refinement scope) | none here (safety suite owns them) | select tests | GOOD (bindings filled) |
| Select winner/loser finalization + restoration (fault suite) | same three C++ files | e13-select-safety | multi-layer aggregates + non-interference | none | 29 FAULT-constant mutants + restore gates + per-law non-vacuity (best negative architecture in the tree) | select tests | GOOD (bindings filled) |
| ApplicationRuntime lifecycle (start/stop/drain/close ownership, driver epoch wake) | `src/async/application_runtime.cpp` | e16-application-runtime | Inv1–Inv25 subset | Live1/2/3/5/6 under WF(driver/owner chains); **documented environment assumptions**: task-body termination, backend completion; ReapTaskIO's worker-scheduling dependency unmodeled (partially compensated by the #116 forced-re-entry mirror) | NEG-E16-1..6 (all real; wrong-property control present) + R2/R3/R11–R15/R17–R19 witnesses (R19 = post-stop admission-commit, ahead-accurate vs current C++) | `application_runtime*` tests | REPAIRED (binding + owner_docs filled; Inv15/Live4 exclusions documented in README by this audit) |

## Cross-suite duplication ledger (deliberate inheritance, not accidental)

- **E10 core (resolve-CAS exactly-once + single publication)** is re-proved
  in e11, e12-event, e12-semaphore, e12-async-mutex, e12-async-condition.
  Each README declares the inheritance; each instantiation adds primitive-
  specific laws. Cheap regression redundancy — retained.
- **Admission closure** (register → recheck → inline-resolve-or-suspend) has
  the same shape in e11 (deadline), e12-event (SET), e12-semaphore (permit),
  e12-async-mutex (owner). Different predicates, same lost-wake idiom —
  retained per-primitive.
- **e7-publication ⊂ e8**: e8's authority triple implies e7's ticket
  multiplicity laws on a smaller domain. e7 retained for the 3-fiber domain
  and the historical duplicate-publication negative; folding e7 into e8 via
  a `StealEnabled` CONSTANT is the recorded simplification path (not taken
  this round — historical effort is not a reason to keep a bad abstraction,
  but neither is churning a passing gate without a driving defect).
- **e9 pre-Phase-G negative snapshots** (BuggyPrePark, BuggyMixedSource):
  still gate the right named properties; re-expressing them as one-line
  mutants of the current model would delete ~1140 duplicated lines — recorded
  as cleanup debt, not blocking.

## Fairness assumptions register (what WF/SF actually assumes about C++)

| Suite | Clause | C++ mechanism | Verdict |
| ----- | ------ | ------------- | ------- |
| blocking-io-pool | WF(Dequeue), WF(Complete) | persistent workers + condvar; callable termination | justified + documented boundary |
| request-arena | WF(Enqueue), WF(Reap) | mandatory noexcept post-commit step; level-triggered wait/progress paths | justified |
| e9-park-wake | WF(worker leave/abandon/elect/drain) | worker loop + epoch cv predicate | justified; producers deliberately unfair |
| e9-wake-handle | WF(notifier), SF(destructor chain) | mutex holders proceed; SF stronger than `std::mutex` guarantees | SF = documented boundary assumption |
| e10-waitnode | WF(resolvers) | caller-thread wake/cancel — NOT scheduler-owned | conditional property (documented this audit) |
| e11-timer-wait | WF(Tick), WF(ResolveTimer) | steady_clock + loop-top pump call sites | strongest grounding in the tree |
| e12-event | WF(DrainOne/FinishSet) | setter's own drain loop under `global_mtx_` | justified |
| e16 | WF(driver/owner chains), env: task termination, backend completion | dedicated driver thread; user task bodies are environment | environment assumptions labeled in-model |
| e12 sem/mutex/cond/queue/rwlock, e13, d1 | none (safety-only) | — | justified per-suite (see matrix notes) |

No unrealistic fairness assumption was found proving a false no-deadlock
claim. The two conditional cases (e10 resolver fairness, e9-wake-handle SF)
are now labeled as boundary assumptions in their suite READMEs by this audit.

## Harness repairs made by this audit

- `scripts/formal/verify.py` `check`/`doctor` now validate every suite's
  `implementation_bindings` / `owner_docs` paths and require bindings or an
  explicit `binding_rationale` (caught: the blocking-io-pool binding that
  pointed at a non-existent `src/` prefix — the real file is
  `src/blocking_io_pool.cpp` — and the e7-publication owner doc that pointed
  at a non-existent `docs/history/` closeout file; both dangling references
  were removed together with the manifest repair).
- `verify-e8-ownership-transfer.sh` and `verify-e9-wake-handle-lifetime.sh`
  upgraded from any-counterexample to **named-invariant** assertions
  (matching what the manifest always claimed).

## Debt register (with revisit triggers)

| Debt | Trigger |
| ---- | ------- |
| request-arena multi-slot ready-ring ORDER (capacity-2 variant) | any change to the Decision-9 ordering rule |
| e9 reference-config (SplitWait=FALSE) gates are weaker than they look (vacuity note) | next e9 model edit; fold the reference exit conditions into real wake-source predicates |
| e7 into e8 fold-in via `StealEnabled` | next e7/e8 protocol change |
| e9 pre-Phase-G negative snapshots as one-line mutants | next e9 negative addition |
| uring full submission path (RequestArena interplay) under real liburing | already partially covered via request-arena + d1; revisit on Uring Phase F restart |
| e12-event deadline-precedence admission (SET beats due) unmodeled | next event admission change |
