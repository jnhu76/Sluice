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
| RequestArena/RequestSlot explicit-I/O accepted-request lifecycle (five-stage admission, Scheme-B arbitration, terminal exactly-once, generation reuse, pin/reap, borrow-through-reap, Decision-11 verbatim) | `include/sluice/async/detail/request_arena.hpp`, `request_slot.hpp` (Layer-A leaf authority; Layer-B progress owners bound in the manifest) | **request-arena (NEW, closes the formal-debt gap)** | 18 inv (accounting, borrow window, winner/pub exactly-once, pin phase/eligibility, gen advance, destroy quiescence, canceled-source — the last an **environment-contract** law, see the obligation register below) | CONDITIONAL on Layer-B external progress: WF(Enqueue)=backend submit-path obligation, WF(Reap)=backend/runtime progress-loop obligation — NOT guaranteed by the leaf; every cfg CHECK_DEADLOCK FALSE (no deadlock-freedom claim) | NEG-RA-1..6 (double terminal, **causal** stale cancel — issued→released→reused→old key, direct publish, no-gen-increment, reap-ignores-pin, running-cancel-stores) + 2 wrong-prop controls + W1–W5 witnesses | `request_arena_test`, `request_lifecycle_scheme_b_test`, `request_waiter_borrow_lease_test`, `request_arena_cancel_intent_test`, `request_arena_death_test`, `threadpool_backend_scheme_b_race_test` | REPAIRED (debt → modeled; PR #125 review corrective: Layer-A/B authority separation, causal NEG-RA-2, Destroy-terminal CloseAdmission guard; residual multi-slot ready-ring ORDER recorded as PARTIAL in the gap entry) |
| Phase-F1 identity-bearing WaitRecord registry (generation-bump-before-visibility reuse, arena-leaf cancel/reap single-authority Race B, stale-token isolation after reuse Race C, lease pinning, P1-2 live accounting, P1-1 outcome coupling) | `include/sluice/async/scheduler.hpp` (WaitRecord/ReadyRoutingSink), `src/async/scheduler_park_wake.cpp` (acquire/retire, await_completion, cancel_waiter), `src/async/scheduler.cpp` (drain, `on_ready`), `include/sluice/async/detail/request_arena.hpp` (waiter registration leaf), `include/sluice/async/detail/ready_sink.hpp` (WaiterToken/RoutingLease) | **f1-wait-record (NEW, closes MODEL-007(b) from audit #162 / umbrella #171; child #174)** | 7 inv (generation isolation, arena single-authority, E7-T2 single publication, slot-lease pin, authority pin, P1-2 live accounting, P1-1 outcome coupling) | none (safety-only; the NEG-WR2 stranded-delivery liveness hole is noted, not claimed) | NEG-WR1/2/3 one-rule cfg flips, name-asserted (drop sink generation check / acquire delivered-pinned record / cancel keeps `waiter_delivery_present_`) + 3 specificity cfgs with documented entailed co-victim exclusions + 5 reachability witnesses | `tests/scheduler_identity_wake_test.cpp` T5 `f1_cancel_waiter_vs_reap_race` (real-thread Race B exactly-once), T6 `f1_stale_record_generation_no_wake` (forged stale token inertly dropped) | NEW (layering finding: double publication unreachable from any single-rule break — registry L2/L3 state checks + the E7-T2 CAS independently suppress it; SC protocol abstraction only) |
| Runnable-ticket publication (one ticket per runnable fiber) | `src/async/scheduler.cpp` (spawn/wake/route, `fiber.hpp` state machine) | e7-publication | 9 inv (ticket multiplicity, waiting/runnable registration coupling) | none (safety scope; progress is e7-multiworker/e9) | BuggyDuplicatePublish → `InvDoneNoTicket` (name-asserted) | `fiber.hpp` make_runnable CAS + scheduler wake tests | REPAIRED (dangling owner_doc removed; `W*Inbox`/`MoveInboxToLocal` are UNREACHABLE compatibility states — no producer action assigns an Inbox location, so the checked graph never enters that tier and the hop is NOT exercised by the gate; C++ `WorkerState::inbox` unused-storage + notify-only `inbox_cv` removal tracked in issue #170, live `inbox_mtx`/`local_runnable` stay; model-tier retirement rides the e8 fold-in debt) |
| Multi-worker admission classification + blocking-park commit (MW-S1/S2/S3, re-drain before commit) | `src/async/scheduler.cpp` (`classify_locked_impl`, Phase A/B elect-commit) | e7-multiworker-progress | MWInv2/3/6/7 | none (liveness carried by e9) | BuggyAdmission (commit skips re-drain), BuggyOutstanding (classifier from registrations) — both name-asserted | scheduler classification/park-commit tests | GOOD (~70% overlaps e9 for admission safety; retained for MWInv7 + the two negatives) |
| Runnable-ownership transfer / work stealing (owner/exec/wait triple authority) | `src/async/scheduler.cpp` (`try_steal`, `fiber_owner_`, wake routing), `scheduler_park_wake.cpp` | e8-ownership-transfer | 11 inv (`InvLocalMatchesOwner` etc.) | none | BuggyOwner (steal without owner transfer) → `InvLocalMatchesOwner` — **now name-asserted by the audit** | scheduler steal/routing tests | REPAIRED (empty binding filled; verifier upgraded from any-cex to named) |
| Suspend-switch steal exclusion (I47-F2 `suspend_switch_pending`: wake-before-physical-context-save window vs work stealing) | `src/async/scheduler.cpp` (`commit_suspend_locked`, `run_next_on`, `try_steal`), `include/sluice/async/scheduler.hpp` (`WorkerState::suspend_switch_pending`), `src/async/fiber_ctx.cpp` (physical save) | **e8-suspend-switch (NEW, closes MODEL-007(a) from audit #162 / umbrella #171)** | 4 inv (`InvNoResumeBeforeContextSaved` core — only unsafe RESUME forbidden, deliberately not pre-save ticket movement; `InvUnsavedSuspensionProtected` protocol authority; E7-T2 ticket structure; pending-committed binding) | none (safety-only by design; 2 workers / 1 fiber / 1 resolver) | NEG-SS1/2/3 cfg-flip defects (ignore-pending steal / old P1-1 late raise / old Select early clear) each name-asserted + NEG-SS2 chain gate + 3 specificity cfgs (co-victim exclusions documented per-defect) | `tests/select_multi_worker_test.cpp` `suspend_lw_mw_steal_before_switch_excluded` (seam-parked window + steal-refusal + mechanical counts), `tests/event_primitive_test.cpp` I47-F1 snapshot (asserts the R1 transient itself) | NEW (SC protocol abstraction only — no C++ weak-memory claim; comment-drift at the `suspend_switch_pending` field docs corrected to the as-built raise-under-G ordering in the same change) |
| Scheduler/backend wake convergence (park domains, epoch, split-wait bridge, R1–R4) | `src/async/scheduler_park_wake.cpp`, `scheduler.cpp` (park commit, `signal_wake_locked`, bridge) | e9-park-wake | Inv2/4/6/8/9/10 + Life1/3/5 | Life2/4/7/8 under worker/self fairness (producers deliberately unfair — justified) | DrainParks (temporal), MixedSource, PrePark (pre-Phase-G snapshots), NoBridge (one-line Phase-G mutant) | scheduler park/wake deterministic tests, `threadpool_wait_drain_deadlock_test` | REPAIRED (reference-config vacuity: the unconditional `~SplitWait` LeavePark disjunct made Inv2/Inv4/InvLife1 and Life2/Life4 near-trivial under SplitWait=FALSE — see suite README note; empty binding filled) |
| Wake-handle notify-vs-destructor lease (Control block lifetime) | `src/async/scheduler_park_wake.cpp` (`notify`), `scheduler.cpp` (dtor invalidation) | e9-wake-handle-lifetime | LifeInv1–6 | Life7/7b (WF notifier chain, SF destructor — SF documented as stronger than `std::mutex` guarantees, boundary assumption) | BuggySnapshot (release-before-callback) → `LifeInv4DestroyedNoCallback` — **now name-asserted** | wake-handle lifetime seam tests | REPAIRED (verifier named assertion; binding filled) |
| WaitNode terminal resolution (exactly-once resolve, single publication) | `include/sluice/async/wait_node.hpp`, `wait_queue.hpp`, `scheduler_park_wake.cpp` | e10-waitnode | 5 inv (**tautological `InvNoTerminalResurrection` and redundant `InvTerminalNotLinked` removed from the gate by this audit**) | EventualResolution under WF(resolvers) — caller-action fairness, documented as conditional | BuggyNoWinner → `InvNoDoubleCompletion` (name-asserted) | wait-node/wake-one tests | REPAIRED (cfg pruned; README stale "NOT executed" claim fixed; binding filled) |
| Deadline timer wait (admission expiry, epoch isolation, lifetime closure, bounded park) | `src/async/scheduler_timer.cpp` | e11-timer-wait | I1–I7 | I6 DeadlineParkLiveness under WF(Tick, ResolveTimer) — best-grounded fairness in the tree (steady_clock + pump call sites) | NEG-1..6 all real defect classes (name-asserted) | timer tests (`advance_clock` deterministic driver) | GOOD (strongest negative set of the primitive suites) |
| Manual-reset event set-drain / reset epoch isolation | `src/async/scheduler_event.cpp` | e12-event | E1/E2/E3/E5/E6 (**dead variable `wokenBySetDrain` removed by this audit**) | E4 drain liveness under WF(drain loop) — justified (setter's own call frame) | NEG-1/2/3 real; NEG-4 labeled ceremonial ghost-coupling probe (retained to pin E5) | event tests | REPAIRED (dead variable; NEG-4 relabeled; binding filled) |
| Semaphore permit conservation / FIFO transfer / admission recheck / overflow | `src/async/scheduler_semaphore.cpp` | e12-semaphore | 12 inv (P1–P10) | none — justified: `sem_release` transfers synchronously under `global_mtx_`; revisit if release ever splits | NEG-SEM-1..7 (Neg3 `+2` store is the one ceremonial mutant; wrong-prop gate proves specificity) | semaphore tests | GOOD (binding filled) |
| Async mutex FIFO handoff (owner commit before publication, no-barging) | `src/async/scheduler_mutex.cpp` | e12-async-mutex | 21 inv | none — FIFO order is safety (`InvFIFOGrant`); grant liveness deferred | NEG-M1..M11 (M4≈M5 near-duplicate pair, both retained — distinct laws) | async mutex tests | GOOD (binding filled) |
| Condition-variable lost-notification closure + unified reacquire/ordinary FIFO | `src/async/scheduler_condition.cpp` | e12-async-condition | 26 inv + 2 reachability (mixed-FIFO orders) | none — no-lost-notify encoded as safety | NEG-C1..C10 (C6 ghost-mutation is the one ceremonial mutant) | condition tests | GOOD (binding filled) |
| Bounded MPMC queue lease move + close/drain topology (B1–B7) | `src/async/queue_port.cpp` | e12-queue | A1–A12 + B1–B7 | none — close drains synchronously in one CS (revisit if async drain) | 7 real negatives + scenes R1–R8 (B3/B6 end-to-end) | queue tests | GOOD (binding filled) |
| Rwlock writer-fair FIFO reconcile (unlock/cancel/expire head-prefix grant) + timed admission (`*_lock_until` resource-first vs already-due deadline) | `src/async/scheduler_rwlock.cpp` (`rwlock_grant_from_head_locked`, `rwlock_{read,write}_lock_until`) | e12-rwlock | RW1–RW10 **+ `InvNoStrandedGrantableHead` + `ReaderRevocationFree` + `InvResourceFirstDeadline` (added by this audit; RW4 reconcile was vacuous for cancel/expire — MODEL-001 single-assignment repair, MODEL-002 `activeReaders = 0` guards; RW11 `deadlineDue` ghost + evidence latches at parity with P7/M7 — MODEL-003)** | none (safety-only) | ReaderBypass **+ NoReconcile + WriterRevoke (MODEL-002 control) + DeadlinePrecedence (MODEL-003 sensitivity control: ONLY the due=TRUE successor of the timed-admission actions resolves Expired and commits NO ownership — due=FALSE is exactly the positive behavior; parity with NEG-SEM-7, with a specificity cfg proving the SAME mutant passes the remaining 12 positive invariants, so the negative is exact; WriterRevoke/DeadlinePrecedence generated by `scripts/formal/gen-rwlock-neg-{writer-revoke,deadline-precedence}.py`, whose `--check` freshness modes run inside the verifier BEFORE TLC and fail closed on a stale committed negative)** + 7 reachability witnesses (cancel/expire reader-prefix merge, cancel/expire writer-refused, writer-blocked contrast, until resource-beat-due, until admission-expire) | `tests/async_rwlock_test.cpp`: `rwlock_t6_cancel_and_head_reconcile`, `rwlock_t12_last_reader_grants_writer`, `rwlock_head_writer_cancel_grants_reader_prefix_immediately`, `rwlock_cancel_reconcile_preserves_fifo`, `rwlock_mw_concurrent_last_reader_unblock_grants_writer`, + R1–R5 audit cases (`rwlock_audit_r1_cancel_head_writer_wall` … `rwlock_audit_r5_cancel_wins_grant_is_noop`) + M3 timed cases (`rwlock_audit_m3_write_lock_until_resource_first`, `rwlock_audit_m3_write_lock_until_due_blocked_expires`) | REPAIRED (audit-closed negative + timed-admission gaps; MODEL-001/002/003 repaired, non-vacuity witnessed, MODEL-003 negative-control sensitive, generated negatives freshness-gated) |
| Scheduler idle-dance termination convergence (contribution identity vs unlocked idle-count erases; the #161 T22 hang) | `src/async/scheduler.cpp` (worker loop dance, the two unlocked erases), `scheduler_park_wake.cpp` (park commit) | e12-rwlock-scheduler-liveness | NoReaderWriterOverlap/TerminalUniqueness/PublicationUniqueness/NoLinkedTerminal/NoStrandedRunnable **+ `DrainStuckState` (the #161 stall shape, added by this round)** | 4 temporal properties (`~>`) under WF on scheduler-controlled actions + SF(TryPop) (steal-war justification documented) | M1–M5 defect toggles + B4 per-site bump toggles (M4 = the as-built orphaning, name-asserted via DrainStuckState; M1–M3 close in-scenario, carried by e9 at their abstraction — documented) | `tests/issue161_idle_dance_orphan_test.cpp` (per-worker seams, pre-fix deterministic FAIL / post-fix PASS) + the full mw/park matrix | NEW (2026-08-21, issue #161: root cause PROVEN by TLC, C++ refined with the three ordering rules recorded in the compliance gate) |
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
| request-arena | WF(Enqueue) = backend submit-path obligation; WF(Reap) = backend/runtime progress-loop obligation | `ThreadPoolBackend::enqueue_after_commit` (noexcept post-commit step); `poll`/`wait_one`/uring reaper call sites of `arena_.reap` | **external Layer-B assumption — NOT guaranteed by the arena leaf** (conditional properties; CHECK_DEADLOCK FALSE, no deadlock-freedom claim) |
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

### Non-fairness environment obligations (request-arena, PR #125 review P1-2)

Beyond WF/SF clauses, the request-arena suite carries one caller-discipline
obligation that must not be misread as a leaf guarantee:

- `RecordCanceledConfirmed` / `InvCanceledTerminalSource` encode the
  Decision-11 **backend obligation**: only a backend that has CONFIRMED a
  running interruption took effect may store the canceled terminal. The C++ leaf
  `record_canceled(h)` is `record_terminal(err(canceled))` — it validates
  handle generation, slot state, and exactly-once, but performs **no
  cancel-intent or provenance check**, and **no production backend currently
  calls it** (tests simulate the confirming backend; NEG-RA-6 pins the
  ill-behaved caller). The invariant proves "IF callers honor the obligation
  THEN no intent-only running cancel yields a canceled terminal" — it does
  not prove the leaf enforces the discipline. A future backend that starts
  calling `record_canceled` owns this obligation at its call sites.

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
| RequestArena + backend-progress compositional refinement (discharging the Layer-B WF obligations against the real submit/progress loops instead of assuming them) | a production backend starts driving reap/enqueue liveness compositionally, or any suspected lost-progress defect between submit path and progress loop |
| e9 reference-config (SplitWait=FALSE) gates are weaker than they look (vacuity note) | next e9 model edit; fold the reference exit conditions into real wake-source predicates |
| e7 into e8 fold-in via `StealEnabled` | next e7/e8 protocol change |
| e9 pre-Phase-G negative snapshots as one-line mutants | next e9 negative addition |
| uring full submission path (RequestArena interplay) under real liburing | already partially covered via request-arena + d1; revisit on Uring Phase F restart |
| e12-event deadline-precedence admission (SET beats due) unmodeled | next event admission change |
| audit #162 MODEL-007(c)–(e) unmodeled mechanisms (items **(a)** and **(b)** are now modeled: (a) the I47-F2 `suspend_switch_pending` steal-refusal window — `spec/tla/e8_suspend_switch/`, issue #172, merged via PR #173; (b) the Phase-F1 WaitRecord registry generation/lease/delivery races — `spec/tla/f1_wait_record/`, issue #174, safety PASS + 3 exact negatives + 5 witnesses): CancelToken task-cancel epoch protocol (`cancel.cpp`); #115 spawn-to-busy-worker wake-epoch obligation; G1 worker retire-ring ticket rescue (local_runnable→pending_spawn_) — umbrella tracker: issue #171 | any change to the corresponding mechanism |
| e12-async-mutex generated negatives have no freshness gate: `E12AsyncMutexNegM4.tla` is stale vs `_gen_neg.py`'s current output (pre-existing; `verify-async-mutex.sh` never regenerates/compares) — tracker: issue #169 | taking up issue #169 (add `--check` freshness + wire into the verifier, regenerate M4); any `E12AsyncMutex.tla` positive edit must then regenerate the negatives |
| e7 C++ dead/inert fields: `WorkerState::inbox` unused storage, `inbox_cv` notify-only with no production waiter (live `inbox_mtx` + `local_runnable` stay) — tracker: issue #170 | issue #170 (wake-path removal with its own review + §16.3 TSan); the unreachable `W*Inbox` model tier retires with the e8 fold-in |
