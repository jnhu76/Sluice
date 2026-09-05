# C++ ↔ TLA+ Model Coverage Matrix

> **Claim scope** (#163 §12 vocabulary) — **C++ revision**: rebaselined per
> audit (the governing audit and date are named below); each suite's as-built
> C++ revision binding lives in `spec/tla/manifest.json`. **Model scope**:
> abstraction faithfulness and protocol-model coverage of the C++ design
> (what each suite abstracts, which C++ code owns the protocol, where the
> executable regression bridge lives, what remains debt) — not implementation
> verification. **Fairness / bounds**: per-suite (manifest fields).
> **Unsupported regions**: trace conformance, weak-memory kernels, and fault
> envelopes are separate evidence technologies (the "Not TLA debt" section
> below) and are never paid — or claimed — as TLA+ debt or TLA+ credit.

> Current developer document (2026-08-18 formal realignment audit
> `audit/formal-cpp-tla-realignment`; debt register rebaselined 2026-08-23,
> issue #186). This is the authoritative map of what each TLA+ suite
> abstracts, which C++ code owns the protocol, where the executable
> regression bridge lives, and what remains debt. The suite IDs and gate
> counts match `spec/tla/manifest.json`; per-suite details live in each
> `spec/tla/*/README.md`.
>
> Governing principle: **TLA+ serves the C++ design** (AGENTS.md §7). The
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
| RequestArena/RequestSlot explicit-I/O accepted-request lifecycle (five-stage admission, Scheme-B arbitration, terminal exactly-once, generation reuse, pin/reap, borrow-through-reap, Decision-11 verbatim, kernel-canceled verbatim) | `include/sluice/async/detail/request_arena.hpp`, `request_slot.hpp` (Layer-A leaf authority; Layer-B progress owners bound in the manifest) | **request-arena (NEW, closes the formal-debt gap)** | 18 inv (accounting, borrow window, winner/pub exactly-once, pin phase/eligibility, gen advance, destroy quiescence, canceled-source — the last an **environment-contract** law, see the obligation register below) | CONDITIONAL on Layer-B external progress: WF(Enqueue)=backend submit-path obligation, WF(Reap)=backend/runtime progress-loop obligation — NOT guaranteed by the leaf; every cfg CHECK_DEADLOCK FALSE (no deadlock-freedom claim) | NEG-RA-1..6 (double terminal, **causal** stale cancel — issued→released→reused→old key, direct publish, no-gen-increment, reap-ignores-pin, running-cancel-stores) + 2 wrong-prop controls + W1–W6 witnesses (W6 = the #262 kernel-canceled-verbatim shape, S1A #294) | `request_arena_test`, `request_lifecycle_scheme_b_test`, `request_waiter_borrow_lease_test`, `request_arena_cancel_intent_test`, `request_arena_death_test`, `threadpool_backend_scheme_b_race_test`, `reference_backend_arena_lifecycle_test::fake_kernel_canceled_completion_verbatim_without_caller_cancel` (S1A #294) | REPAIRED (debt → modeled; PR #125 review corrective: Layer-A/B authority separation, causal NEG-RA-2, Destroy-terminal CloseAdmission guard; residual multi-slot ready-ring ORDER recorded as PARTIAL in the gap entry; S1A #294: `RecordTerminalKernelCanceled` added — canceled real results are modeled through the real-result recording path (the `kernel_canceled` stamp is the recording source, not causal attribution or caller-intent absence; W6 existentially witnesses the no-caller-intent #262 shape), closing the S0B #262 handoff) |
| Phase-F1 identity-bearing WaitRecord registry (generation-bump-before-visibility reuse, arena-leaf cancel/reap single-authority Race B, stale-token isolation after reuse Race C, lease pinning, P1-2 live accounting, P1-1 outcome coupling) | `include/sluice/async/scheduler.hpp` (WaitRecord/ReadyRoutingSink), `src/async/scheduler_park_wake.cpp` (acquire/retire, await_completion, cancel_waiter), `src/async/scheduler.cpp` (drain, `on_ready`), `include/sluice/async/detail/request_arena.hpp` (waiter registration leaf), `include/sluice/async/detail/ready_sink.hpp` (WaiterToken/RoutingLease) | **f1-wait-record (NEW, closes MODEL-007(b) from audit #162 / umbrella #171; child #174)** | 7 inv (generation isolation, HISTORICAL arena single-authority XOR per registration epoch, E7-T2 single publication, slot-lease pin, authority pin, P1-2 live accounting, P1-1 one-directional outcome-frozen-on-publication) | none (safety-only; the NEG-WR2 stranded-delivery liveness hole is noted, not claimed) | NEG-WR1/2/3 one-rule cfg flips, name-asserted (drop sink generation check / acquire delivered-pinned record / cancel keeps `waiter_delivery_present_`) + 3 specificity cfgs with documented entailed co-victim exclusions + 6 reachability witnesses (incl. the sequential double-grant shape: cancel granted -> cancel consumed -> late delivery grant) | `tests/scheduler_identity_wake_test.cpp` T5 `f1_cancel_waiter_vs_reap_race` (real-thread Race B exactly-once), T6 `f1_stale_record_generation_no_wake` (forged stale token inertly dropped) | NEW (layering finding: double publication unreachable from any single-rule break — registry L2/L3 state checks + the E7-T2 CAS independently suppress it; SC protocol abstraction only) |
| Runnable-ticket publication (one ticket per runnable fiber) | `src/async/scheduler.cpp` (spawn/wake/route, `fiber.hpp` state machine) | e7-publication | 9 inv (ticket multiplicity, waiting/runnable registration coupling) | none (safety scope; progress is e7-multiworker/e9) | BuggyDuplicatePublish → `InvDoneNoTicket` (name-asserted) | `fiber.hpp` make_runnable CAS + scheduler wake tests | REPAIRED (dangling owner_doc removed; the former unreachable `W*Inbox`/`MoveInboxToLocal` compatibility tier RETIRED by issue #170, which deleted the dead C++ `WorkerState::inbox` storage and the notify-only `inbox_cv` — the model now matches the as-built single `local_runnable` queue under the kept `inbox_mtx`; checked state graph unchanged, the tier was never reachable) |
| Multi-worker admission classification + blocking-park commit (MW-S1/S2/S3, re-drain before commit) | `src/async/scheduler.cpp` (`classify_locked_impl`, Phase A/B elect-commit) | e7-multiworker-progress | MWInv2/3/6/7 | none (liveness carried by e9) | BuggyAdmission (commit skips re-drain), BuggyOutstanding (classifier from registrations) — both name-asserted | scheduler classification/park-commit tests | GOOD (~70% overlaps e9 for admission safety; retained for MWInv7 + the two negatives) |
| Runnable-ownership transfer / work stealing (owner/exec/wait triple authority) | `src/async/scheduler.cpp` (`try_steal`, `fiber_owner_`, wake routing), `scheduler_park_wake.cpp` | e8-ownership-transfer | 10 inv (`InvLocalMatchesOwner` etc.) | none | BuggyOwner (steal without owner transfer) → `InvLocalMatchesOwner` — **now name-asserted by the audit** | scheduler steal/routing tests | REPAIRED (empty binding filled; verifier upgraded from any-cex to named; issue #184 retired the unreachable `W*Inbox`/`MoveInboxToLocal`/`InvInboxMatchesOwner` compatibility tier left after #170 — pre-change `InvNoInboxTicket` probe PASSed 341/100/11, producer-injection probe FAILed; checked graph unchanged) |
| Suspend-switch steal exclusion (I47-F2 `suspend_switch_pending`: wake-before-physical-context-save window vs work stealing) | `src/async/scheduler.cpp` (`commit_suspend_locked`, `run_next_on`, `try_steal`), `include/sluice/async/scheduler.hpp` (`WorkerState::suspend_switch_pending`), `src/async/fiber_ctx.cpp` (physical save) | **e8-suspend-switch (NEW, closes MODEL-007(a) from audit #162 / umbrella #171)** | 4 inv (`InvNoResumeBeforeContextSaved` core — only unsafe RESUME forbidden, deliberately not pre-save ticket movement; `InvUnsavedSuspensionProtected` protocol authority; E7-T2 ticket structure; pending-committed binding) | none (safety-only by design; 2 workers / 1 fiber / 1 resolver) | NEG-SS1/2/3 cfg-flip defects (ignore-pending steal / old P1-1 late raise / old Select early clear) each name-asserted + NEG-SS2 chain gate + 3 specificity cfgs (co-victim exclusions documented per-defect) | `tests/select_multi_worker_test.cpp` `suspend_lw_mw_steal_before_switch_excluded` (seam-parked window + steal-refusal + mechanical counts), `tests/event_primitive_test.cpp` I47-F1 snapshot (asserts the R1 transient itself) | NEW (SC protocol abstraction only — no C++ weak-memory claim; comment-drift at the `suspend_switch_pending` field docs corrected to the as-built raise-under-G ordering in the same change) |
| **E9 trace conformance (#196, V2)**: ObservedBehaviors of a deterministic C++ corpus ⊆ Behaviors of e9-park-wake at the same revision — semantic traces (ParkCommitted/ParkEntered/ParkRefused/WakePublished/ParkReturned) captured from the SLUICE_ASYNC_INTERNAL_TESTING controller recorder, each validated by a GENERATED TLC replay wrapper over the PRISTINE model (existential realizability; the repository model is the authority — no second protocol implementation) | `tests/e9_trace_conformance_test.cpp` (capture + shape assertions), `src/async/scheduler_park_wake.cpp` + `scheduler.cpp` (macro-guarded record sites — production compiles none of them) | **e9-trace-conformance (NEW, #196)** | none of its own — the wrapper reuses the pristine model's transition relation; acceptance IS the property (TraceIncomplete violated) | n/a | neg_a (causeless return — `LeavePark` has no enabled disjunct under SplitWait=TRUE) + neg_b (pre-#185-style unconditional-escape claim on an UN-ARMED reference park — the faithful escape requires `observationArmed`) both rejected BY MODEL SEMANTICS; malformed/revision fixtures fail closed; the validator self-test carries one ACCEPT + one REJECT TLC leg (verdict non-vacuity) | the same corpus test's in-test shape assertions | NEW — claim: **TRACE-CONFORMANT (TESTED EXECUTIONS)**, corpus only; documented model-scope boundaries (owner doc): the E5-A2 ready-flag observation return under SplitWait=TRUE has no model action (follow-up candidate — the model under-covers a real C++ behavior); delegation/quiescent/drain-dance park classes and backend-domain parks are outside the pilot vocabulary; the model's terminal semantics collapses post-terminal physical returns/epilogue wakes (documented terminal-collapse rules) |
| Scheduler/backend wake convergence (park domains, epoch, split-wait bridge, R1–R4, worker-startup population R-F1) | `src/async/scheduler_park_wake.cpp`, `scheduler.cpp` (park commit, `signal_wake_locked`, bridge, run_impl startup publication) | e9-park-wake | Inv2/4/6/8/9/10 + Life1/3/5 + `InvNoCauselessReturn` (#185, in `Inv`, both configs) + `InvStartupWellFormed`/`InvPopulationTerminal` (R-F1, in `Inv`, both configs) | Life2/4/7/8 under worker/self fairness + `FairStartWorker` (R-F1; a real `std::thread` guarantee) (producers deliberately unfair — justified) | DrainParks (temporal), MixedSource, PrePark (pre-Phase-G snapshots), NoBridge (one-line Phase-G mutant), WitnessRetire/WitnessTerminate/WitnessPnpExit/WitnessPnpEndedRun (non-vacuity #189/#191), NegRetireDead/NegParticipantDead (fail-closed), WitnessRefObservation/WitnessRefUnbounded/WitnessRefDrainMWS3 (non-vacuity #185), NegOldEscape/NegNaiveEscape (fail-closed #185), WitnessStart1/WitnessStart2/WitnessStartEstablished (non-vacuity R-F1) + NegStartUnrefinedElection/NegStartUnsettledTerminal (fail-closed R-F1) | scheduler park/wake deterministic tests, `threadpool_wait_drain_deadlock_test`, `tests/issue223_startup_skew_election_test.cpp` (R-F1: worker 0 held pre-publication while worker 1 elects — the #210 skew shape, schedule-pinned; the run returns only after both configured threads have run) | REPAIRED, then **vacuity characterized 2026-08-23 (issue #185, B6 STOP) and REPAIRED at #185** (reference escape scoped to entry-armed parks; `InvNoCauselessReturn` detector; 19/19 gates); **#189 (merged 921dd86)**: `RetireWorkerQuiescent` revived to the as-built retire epilogue; **#191 (merged 8c08e46)**: `ParticipantNoProgressExit` revived; then **EXPANDED 2026-09-05 (R-F1, issue #223)**: the S1A `MODEL_SCOPE_EXCLUDED` worker-startup boundary is now MODELED — `workerStarted` (Init FALSE, `StartWorker` = the per-thread `active.store(true)`, scheduler.cpp:460), `Eligible = alive ∧ started` as the single election/observer authority (Inv6/Inv9/Inv10/`LowestAlive`/`SomeActiveWorker` refined in place; post-settlement `Eligible == workerAlive` so steady-state guards coincide point-for-point), `Settled` gating the run-ending classifications (the `run_impl` join boundary, scheduler.cpp:472-475; `ParticipantNoProgressExit` deliberately ungated per C++ :1000), and the two naive-extension mutants as generated fail-closed negatives. Production C++ semantics unchanged (test-only seam). See `docs/investigations/rf1-e9-startup-refinement.md` and the suite README |
| Spawn-to-busy-worker wake obligation (#115: a runnable ticket on a busy worker's queue must advance the wake epoch; committed-parked peer + commit-recheck refusal) | `src/async/scheduler.cpp` (`spawn`/`spawn_on`/`route_runnable_locked`, `unguarded_progress_pending_locked`), `src/async/scheduler_park_wake.cpp` (`signal_wake_locked`, park commit + cv predicate), `include/sluice/async/scheduler.hpp` | **spawn-wake-epoch (NEW, closes MODEL-007(d) from audit #162 / umbrella #171; child #176; historical defect #115)** | 4 inv (wake obligation — the violating state IS the persistent stranded shape; baseline soundness; steal-requires-awake; consumed-requires-publication) | none (safety/accounting only — the strand is the violated state predicate, no temporal formula; no fairness) | NegNoSignal (#115 pre-fix publication: push + inert inbox notify, no epoch advance) + NegNoRecheck (pre-G1 consumed-baseline commit) — both EXACT historical defect behaviors, name-asserted on `InvWakeObligation`; NegNoRecheck excludes entailed co-victim `InvStealRequiresAwake` | `tests/issue115_runnable_publication_wake_test.cpp` (seam-parked W0 + pinned-busy W1 + strictly post-commit production spawn/spawn_on) | NEW (SC abstraction; two-layer protection — publication signals + commit recheck — each with its exact historical mutant) |
| Worker-retirement runnable-ticket rescue (G1 epilogue: a worker leaving the loop must move local_runnable tickets to the pre-run domain; survivor loop-top redispatch re-records the dead owner) | `src/async/scheduler.cpp` (worker_loop retire epilogue :1230-1270, loop-top pending_spawn_ pop + fiber_owner_ re-record :485-545, run() setup redistribution :383-393), `include/sluice/async/scheduler.hpp` (WorkerState::active/loop_exited) | **worker-retire-rescue (NEW, closes MODEL-007(e) from audit #162 / umbrella #171; child #178)** | 4 inv (no ticket on a retired worker's queue; single live ticket per fiber — transport never publishes; runnable fiber's ticket recoverable from an ACTIVE queue or the pre-run domain; owner/location consistency with the dead-owner-rides-the-ticket semantics) | none (safety only; retire-with-empty-queue and the #161 idle-dance terms are outside — e12's domain) | NegNoRescue (pre-G1 strand) / NegRescueCopies (copy-not-move) / NegNoRerecord (dispatch drops the owner re-record) — one-rule cfg flips, all name-asserted + 3 specificity cfgs | C++ bridge classified as CAUSAL SEAM + ADJACENT BRIDGES (`WorkerState::loop_exited`, phase_g no-progress-exit stranded-inbox shape, the D2 setup redistribution in issue161_idle_dance) — no exact deterministic topology assertion exists (coverage note, review-fix) | NEW (review-fix: retire guarded to the unconsumed-ticket epilogue state; RunFiber carries make_running's Runnable precondition; 5 strengthened witnesses incl. the full survivor-resume chain) |
| CancelToken request-epoch isolation (reuse = `clear()`+`request()` + per-consumer `acked_epoch`; single-shot delivery per epoch per consumer **between the two explicit re-arm authorities** — token `rearm()` and per-consumer `reset_acknowledgement()`; protection (protected cancel-point) blocks delivery; the ADR `cancel_rearm_re_enables_delivery` / `cancel_clear_then_request_is_a_fresh_request` sticky-ack semantics) | `include/sluice/async/cancel.hpp`, `src/async/cancel.cpp` (`request`/`rearm`/`clear`/`reset_acknowledgement`/`check_cancel`, atomic `state_` bit 0 pending + request-epoch bits 1..63; ADR-cancel-request-epoch), consumers `future.hpp` / `group.hpp` / `application_runtime.cpp` (Group shares one token across tasks) | **cancel-token-epoch (NEW, closes MODEL-007(c) from audit #162 / umbrella #171; child #180)** | 7 inv (`InvSingleShotPerEpoch` — review-fix restated law: no duplicate delivery WITHOUT an explicit re-arm authority, `dupDelivered` = a delivery with `acked[c]=epoch`; `InvNoDeliveryWhenIdle`; `InvProtectionBlocksDelivery`; `InvClearRemovesIntent`; `InvNoStaleAckStarvesDelivery`; `InvDeliveredWasRequested`; `InvAckIsRealEpoch`) | none (safety-only; rearm() is the alternative generation-advance path, no fairness clause) | NEG-CT1..5 one-rule cfg flips, name-asserted (sticky-bool ack / clear-keeps-pending / drop single-shot / drop pending gate / drop protection) + 5 specificity cfgs (documented entailed co-victim exclusions, e.g. NegStickyAck ⊂ InvNoStaleAckStarvesDelivery) + 9 reachability witnesses (clear+request SAME-consumer reuse pinned on the fresh request's exact epoch `lastClearedEpoch + 1` — round-3 tightening, so a clear→request→rearm→deliver-later-epoch chain cannot impersonate it; shared-consumer redelivery; protected-request SAME-consumer blocked-not-delivered on per-consumer `blockedCheckedEpochs[c]`; rearm SAME-consumer redelivery pinned on `RearmedFromEpoch`; reset_acknowledgement per-consumer re-arm; cleared-idle no delivery) | `tests/cancel_token_test.cpp` (`cancel_rearm_re_enables_delivery`, `cancel_clear_then_request_is_a_fresh_request` — ADR pre-fix RED / post-fix GREEN; `cancel_shared_token_two_consumers_deliver_and_rearm` executes `b.reset_acknowledgement()` — the per-consumer re-arm bridge) | NEW (AS-BUILT MODELED — no C++ defect candidate, zero production edits; request-epoch not task-incarnation; SC protocol abstraction only; review-fix round: `reset_acknowledgement` modeled, single-shot restated with its explicit re-arm authorities, the four key witnesses same-consumer-pinned with impersonation continuations proven excluded; round-3: fresh-request witness tightened to `lastClearedEpoch + 1`, the fake rearm chain proven excluded by an INIT probe) |
| Wake-handle notify-vs-destructor lease (Control block lifetime) | `src/async/scheduler_park_wake.cpp` (`notify`), `scheduler.cpp` (dtor invalidation) | e9-wake-handle-lifetime | LifeInv1–6 | Life7/7b (WF notifier chain, SF destructor — SF documented as stronger than `std::mutex` guarantees, boundary assumption) | BuggySnapshot (release-before-callback) → `LifeInv4DestroyedNoCallback` — **now name-asserted** | wake-handle lifetime seam tests | REPAIRED (verifier named assertion; binding filled) |
| WaitNode terminal resolution (exactly-once resolve, single publication) | `include/sluice/async/wait_node.hpp`, `wait_queue.hpp`, `scheduler_park_wake.cpp` | e10-waitnode | 5 inv (**tautological `InvNoTerminalResurrection` and redundant `InvTerminalNotLinked` removed from the gate by this audit**) | EventualResolution under WF(resolvers) — caller-action fairness, documented as conditional | BuggyNoWinner → `InvNoDoubleCompletion` (name-asserted) | wait-node/wake-one tests | REPAIRED (cfg pruned; README stale "NOT executed" claim fixed; binding filled) |
| Deadline timer wait (admission expiry, epoch isolation, lifetime closure, bounded park) | `src/async/scheduler_timer.cpp` | e11-timer-wait | I1–I7 | I6 DeadlineParkLiveness under WF(Tick, ResolveTimer) — best-grounded fairness in the tree (steady_clock + pump call sites) | NEG-1..6 all real defect classes (name-asserted) | timer tests (`advance_clock` deterministic driver) | GOOD (strongest negative set of the primitive suites) |
| Manual-reset event set-drain / reset epoch isolation | `src/async/scheduler_event.cpp` | e12-event | E1/E2/E3/E5/E6 (**dead variable `wokenBySetDrain` removed by this audit**) | E4 drain liveness under WF(drain loop) — justified (setter's own call frame) | NEG-1/2/3 real; NEG-4 labeled ceremonial ghost-coupling probe (retained to pin E5) | event tests | REPAIRED (dead variable; NEG-4 relabeled; binding filled) |
| Semaphore permit conservation / FIFO transfer / admission recheck / overflow | `src/async/scheduler_semaphore.cpp` | e12-semaphore | 12 inv (P1–P10) | none — justified: `sem_release` transfers synchronously under `global_mtx_`; revisit if release ever splits | NEG-SEM-1..7 (Neg3 `+2` store is the one ceremonial mutant; wrong-prop gate proves specificity) | semaphore tests | GOOD (binding filled) |
| Async mutex FIFO handoff (owner commit before publication, no-barging) | `src/async/scheduler_mutex.cpp` | e12-async-mutex | 21 inv | none — FIFO order is safety (`InvFIFOGrant`); grant liveness deferred | NEG-M1..M11, all generated + freshness-gated (`_gen_neg.py --check` before TLC; M4/M5 = ONE mutation with two designated detector roles — transition-identical defect bodies, each designated invariant an entailed co-victim of the same defect, proven by the verifier's cross-detector probes) | async mutex tests | GOOD (binding filled) |
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

## Debt register (rebaselined 2026-08-23, issue #186)

Every entry was re-audited against current `master` (5227eb2, post-#187): the
register format now records, per debt, its classification, the exact evidence
boundary today, why it is not being paid now, the revisit trigger, and the done
condition. Classes: `ACTIVE-NOW` (known current defect; repair due — a row may
route the payment to a specific reviewed change) · `TRIGGERED-DEFERRED`
(trigger fired; repair scoped and evidenced but blocked on a reviewed change)
· `ON-TOUCH` (pay when the trigger fires; idle otherwise) · `PHASE-BOUND`
(blocked on a scheduled phase) · `PLATFORM-BOUND` (blocked on an environment)
· `COVERAGE-BOUNDARY` (a deliberate authority split, not an omission) ·
`RESOLVED` / `RETIRED` / `UNKNOWN`.

| Debt | Class | Current evidence boundary | Why not now | Revisit trigger | Done condition |
| ---- | ----- | ------------------------- | ----------- | --------------- | -------------- |
| request-arena multi-slot ready-ring ORDER (capacity-2 variant) | ON-TOUCH | single-slot suite: 18 inv, NEG-RA-1..6, W1–W6, C++ C2b–C2e mutation matrix; post-audit churn in `request_arena.hpp` (b50cc41 docs, 521c081 phase-g tests) contains **zero** ready-ring hunks — the Decision-9 ordering rule is unchanged since the audit; the S0B W1 waiter-provenance fix and the S1A kernel-canceled action (#294) are terminal-source/provenance changes, not ring-ordering changes | no ordering-rule change; a capacity-2 model without a driving defect is ceremonial (AGENTS.md §7) | any change to the Decision-9 ready-ring FIFO ordering rule | capacity-2 suite variant in `verify-request-arena.sh` with its own negative proving the ORDER law can fail |
| RequestArena + backend-progress compositional refinement (discharge the Layer-B WF obligations against the real submit/progress loops instead of assuming them) | COVERAGE-BOUNDARY | leaf suite CONDITIONAL on WF(Enqueue)/WF(Reap) by design; `record_canceled` still has no production caller; no backend composes reap/enqueue liveness today | the Layer-A/Layer-B authority split is the reviewed design (PR #125 P1); discharging Layer-B obligations requires a backend that actually composes — none exists yet | a production backend starts driving reap/enqueue liveness compositionally, or a suspected lost-progress defect between submit path and progress loop | a compositional (or focused-loop) model discharging both WF obligations against the real call sites, with a negative control |
| e9 reference-config (SplitWait=FALSE) gate vacuity | **RESOLVED / REPAIRED** (this branch revision, issue #185) | B6 classification CONFIRMED and repaired: the as-shipped unconditional `~SplitWait` LeavePark escape modeled ALL reference parks as timeout-bounded, but the C++ bounds a park only when `bounded_backend_observation` was armed at entry or a deadline is active (`scheduler_park_wake.cpp:400/455/468` — "No deadline and no backend observation: unbounded park"; only the reference MIXED-WAKE park arms it, `scheduler.cpp:818`; the MW-S3 idle park is unbounded, `scheduler.cpp:1173-1187`). Repair (#185, branch formal/e9-reference-nonvacuity): entry-captured ghost `observationArmed[w]` (set at scheduler-park entry iff `ExternalWakePossible`, mirroring the C++ entry arming); escape scoped to `(~SplitWait /\ observationArmed[w])`; `InvLife1DrainNoMW3Park` scoped to armed parks; `Inv10` comment corrected; state-predicate detector `InvNoCauselessReturn` added to `Inv` in BOTH configs. Non-vacuity witnesses: `WitnessRefObservation` (armed park reachable), `WitnessRefUnbounded` (un-armed park reachable — the unbounded class was not retired from the graph), `WitnessRefDrainMWS3` (scoped InvLife1 antecedent reachable). Fail-closed negatives (GENERATED, SplitWait=FALSE, both EXACT — only the detector fires): `NegOldEscape` (the unconditional escape = causeless return), `NegNaiveEscape` (live `ExternalWakePossible` at leave instead of entry capture). Adversarial probes: U (unscoped InvLife1 vs the faithful escape) VIOLATES in reference config — the scoping is forced by C++ truth, not convenience; G (capture removed) makes both capture-dependent witnesses HOLD — they genuinely test the capture. E9 verifier 19/19 PASS (4 positive + 7 witnesses + 2 dead-action fail-closed + 2 escape fail-closed + 4 buggy controls) | the B6 classification routed the repair to this separate reviewed change (#185 follow-ups 1–3: faithful predicates, witnesses, negatives) rather than weakening invariants in the rebaseline (#186) | done condition met on this branch | e9 model carries the as-built escape (entry-armed observation); InvLife1 scoped to armed parks with negative controls; reference-config safety gates now carry the causeless-return detector (`InvNoCauselessReturn` in `Inv`) — the load-bearing no-lost-wake evidence hierarchy is unchanged (SplitWait=TRUE gates remain primary; see the suite README's superseded 2026-08-18 audit note) |
| e9 `RetireWorkerQuiescent` dead transition → `FairRetire` vacuous | **RESOLVED / REPAIRED** (this branch revision, issue #189) | static + mechanical: the action (`E9ParkWake.tla:594-610`) conjoined `BridgeEffect(1 - wakeEpoch)` — which primes `wakeEpoch'`/`bridgePending'` (lines 266-269) — while its own `UNCHANGED` list pinned both variables; unsatisfiable for `wakeEpoch ∈ {0,1}` in **every** config; TLC warning at line 609 + action coverage `0:0` + guard-reachable probe (guard satisfiable at Init). Repair (Draft PR #190, formal/e9-retire-worker-quiescent, human review-fixes applied): keep `BridgeEffect` (the C++ departure signal is unconditional), drop `wakeEpoch`/`bridgePending` from the action's `UNCHANGED`; faithful terminal classification (`ReturnedQuiescent` only at true quiescence, else `ReturnedStalled` — revived action exposed an `InvLife5` dormant defect); `InvLife3` scoped to `~terminateFlag` (survivor-runs-after-terminate family is legal C++); `vars` duplicate `terminateFlag` removed; causal witness ghost `retireFired` + `WitnessRetire` cfg (`NoReach*` VIOLATED) + exact-pre-fix negative `NegRetireDead` (witness fail-closed); `FairRetire`'s `RetireWorkerQuiescent` conjunct coverage 904:1720 under `LivenessSpec` (was 0:0) | distinct root cause from the SplitWait row above; repair decided the as-built retire shape against the C++ retire epilogue (`scheduler.cpp:1216-1250`) — see the PR and issue comments | done condition met on this branch (see the two lines above); awaiting human review/merge of the Draft PR | contradiction repaired to the as-built shape; reachability witness proving the retire action fires; fairness gates rerun with the `RetireWorkerQuiescent` conjunct of `FairRetire` non-vacuous (coverage 904:1720 under `LivenessSpec`, was 0:0); negative/mutation proof — all implemented and TLC-verified (see PR); the sibling `ParticipantNoProgressExit` conjunct is a separate registered defect (next row, issue #191) |
| e9 `ParticipantNoProgressExit` dead transition (same `FairRetire` family, distinct root cause) | **RESOLVED / REPAIRED** (this branch revision, issue #191) | guard reachable (probe: 1055 states), but the action was unsatisfiable in every config: its body conjoined `BridgeEffect(1 - wakeEpoch)` — whose bridge branch sets `bridgePending' = TRUE` (a participant exists) — AND an explicit `bridgePending' = FALSE` (one-shot consume): a double-prime contradiction on `bridgePending'`. Coverage `0:0` pre-#191. Repair (this branch, #191): fuse the two C++ wake publications (`scheduler.cpp:966` terminate + `:1249` retire epilogue) into a single action (matching #189's retire precedent); replace `BridgeEffect` with direct `wakeEpoch' = 1 - wakeEpoch` (the participant slot is cleared BEFORE the signal — `backendWaitParticipant' = NONE`, `bridgePending' = FALSE` — so neither signal re-arms the bridge); add `~ExecutableWork` guard (matching C++ `:942` reclassify to `mw_s1` + continue); last-alive branch classifies `ReturnedStalled` (never `ReturnedQuiescent`, because a participant exit is beside outstanding backend work or bridge obligation); causal witness ghosts `participantExitFired` / `participantExitEndedRun` + `WitnessPnpExit` / `WitnessPnpEndedRun` cfgs (`NoReach*` VIOLATED) + exact-pre-fix negative `NegParticipantDead` (witness fail-closed, GENERATED by `_gen_neg.py` #191). `Inv8` refined: D4-RM14 arm baseline (during-residency publications only owe bridge; `terminateFlag` exempted as a past event). `Inv10` refined: R2 transferable election (post-terminate participants are legal C++, E4/E5 owns the outstanding work; `terminateFlag` exempted). Coverage `162:176` under `LivenessSpec` (was 0:0) | distinct root cause from #189 (double-prime on `bridgePending'`, not the `wakeEpoch` UNCHANGED contradiction); a separate focused change, deliberately NOT folded into #189; tracked independently in issue #191 | done condition met on this branch (see the two lines above); awaiting human review/merge of the Draft PR | contradiction repaired to the as-built shape (two wake publications fused, participant slot cleared before signal); reachability witnesses proving the action fires; fairness gates rerun with the `ParticipantNoProgressExit` conjunct of `FairRetire` non-vacuous (coverage 162:176 under `LivenessSpec`, was 0:0); negative/mutation proof — all implemented and TLC-verified |
| e7 into e8 fold-in via `StealEnabled` | ON-TOUCH | both suites green with name-asserted negatives; e7 ⊂ e8 recorded in the duplication ledger; no e7/e8 protocol change since the audit (post-audit churn is vocabulary only: #170 comment alignment and #187's E8-suite vocabulary retirement, both merged — neither touched the steal/publication protocol) | churning a passing gate without a driving defect; the fold-in is a simplification, not a correctness need | next e7/e8 protocol change | one suite with a `StealEnabled` CONSTANT flipping the steal law; e7's historical duplicate-publication negative preserved as a cfg constant assignment |
| e9 duplicated negative snapshots -> generator + freshness gate | **RESOLVED / REPAIRED** (this branch revision, issue #192) | #192's 1A inventory split the three snapshots by lineage: `NegRetireDead` (1029 lines) and `BuggyNoBridge` (994 lines, already STALE at the pre-#190 model — the concrete drift this debt predicted) are snapshots of the CURRENT main module and are now GENERATED by `spec/tla/e9_park_wake/_gen_neg.py` (exact-one-match fragment mutations; `--check` byte-gate runs inside `verify-e9-park-wake.sh` BEFORE any TLC run, fail-closed on stale/missing/unexpected artifacts). NegRetireDead regen preserved the exact reachable graph (46456/14472, both NoReach* witnesses HOLDING); BuggyNoBridge regen from the post-#190 base keeps the named `Inv8BridgeReachesBackendPark` detector (Life7 remains the documented liveness co-victim). Adversarial probes: stale/corrupt/delete/unexpected artifacts and zero-/multi-match anchors each fail closed. `BuggyPrePark`/`BuggyMixedSource` are NOT generated — the inventory proved they are snapshots of the RETIRED pre-Phase-G E9-A protocol (their source-of-truth model left the tree at Phase G; their cfgs carry no SplitWait constant; their exact historical mutations — lost-wake publish + signal-only LeavePark, blind mixed-source entry — are defined against era guard/invariant shapes that no longer exist), so generation from the current module would not be representation-preserving; they stay frozen historical controls per the suite README policy that predates #192. `BuggyDrainParks` is the EXTENDS-based minimal mutant (auto-tracks the positive model; no drift, nothing to gate) | the done condition's literal "all negatives generated" was authored on the belief that all three snapshots duplicated the current module; the inventory disproved that premise for two of them — recorded here rather than silently meeting the letter by regenerating frozen history (or by adding a frozen in-tree era source whose freshness gate over a never-changing model would be ceremony) | next e9 negative addition (a new single-rule mutant of the current model joins the generator's CASES — mutation spec + cfg + verifier expectation, no new snapshot; #191's `ParticipantNoProgressExit` exact-old mutant is the first consumer) | no duplicated snapshot of the current main module is hand-maintained; future negatives are generator cases; frozen pre-Phase-G controls stay frozen with documented rationale |
| uring full submission path (RequestArena interplay) under real liburing | PLATFORM-BOUND | request-arena + d1-poison cover the abstracted arena/ledger paths with stub-safe evidence; **note**: the production submission transaction churned post-audit (7f74a65 centralization, 1a6de37 Stage-0 ring-gate alignment) — the C++ shape a future model must abstract has changed, though no abstracted C++ bridge was affected | no real-liburing environment in the default gate; Uring Phase F is not scheduled | Uring Phase F restart, or a real-liburing defect report | submission-path model (or documented compositional argument) validated with real liburing runs, reported separately from stub evidence (AGENTS.md §6) |
| e12-event deadline-precedence admission (SET beats due) unmodeled | ON-TOUCH | e12-event E1/E2/E3/E5/E6 + NEG-1..3 green; the timed-admission precedence pattern IS modeled one suite over (e12-rwlock `InvResourceFirstDeadline` + the DeadlinePrecedence negative, MODEL-003); post-audit `scheduler_event.cpp` churn (7e9dbb6) was dead-call removal, not admission | no event admission change since the audit; the rwlock model pins the pattern if the event side ever adopts it | next event admission change | e12-event models the set-vs-due admission order with a precedence negative mirroring the rwlock control |

**Retirement check (2026-08-23, post-#187; updated by the #189 branch):** the
e9 `RetireWorkerQuiescent` row is marked **RESOLVED/REPAIRED on the #189
branch revision** (repair + witnesses + negative implemented and
TLC-verified there; human review-fixes applied — live comment, debt
trackers, wording — the row returns to ACTIVE-NOW only if the Draft PR is
amended). One NEW row was registered during #189: e9
`ParticipantNoProgressExit` is a second dead action in the same
`FairRetire` family, with a DISTINCT root cause (double-prime on
`bridgePending'`), guard reachable — deliberately NOT folded into #189 and
now tracked independently in issue #191. The e9 negative-snapshot row's
"next e9 negative addition" trigger FIRED (the #189 repair added
`NegRetireDead`); it moved ON-TOUCH -> **TRIGGERED-DEFERRED** and is
tracked in issue #192 (generatorization, explicitly out of #190 scope) —
that row is now **RESOLVED on the #192 branch** (NegRetireDead/BuggyNoBridge
generated + freshness-gated; PrePark/MixedSource proven frozen-era, kept).
The remaining rows were each re-verified live against `master`: the
request-arena multi-slot scope note still matches `spec/tla/manifest.json`
(`coverage_gaps` entry `request-arena-lifecycle`, `PARTIALLY MODELED`); the
e9 vacuity debt (SplitWait) stays ACTIVE-NOW (#185); #187's E8 vocabulary
retirement retired model vocabulary only — no registered debt; the
remaining rows have un-fired triggers and accurate boundaries.

**Retirement check (2026-08-23, #191 branch):** the e9
`ParticipantNoProgressExit` row is now **RESOLVED/REPAIRED on the #191
branch revision** (repair + two witnesses + fail-closed negative
implemented and TLC-verified; 14/14 gates PASS on the full E9 verifier;
`Inv8`/`Inv10` refined to match as-built C++ semantics — D4-RM14 arm
baseline + R2 transferable election; `FairRetire`'s participant-side
conjunct coverage 162:176 under `LivenessSpec`, was 0:0). The e9
negative-snapshot row's "next e9 negative addition" trigger FIRED AGAIN
(the #191 repair added `NegParticipantDead` to `_gen_neg.py`); it was
already resolved by #192's generatorization, so this is a CONSUMED
trigger (the new negative joined the generator's CASES as designed).
`Inv8BridgeReachesBackendPark` and `Inv10BackendProgressHasObserver`
refinements are faithful encodings of the as-built C++ semantics (D4-RM14
arm baseline: during-residency publications only owe bridge; R2
transferable election: post-terminate participants are legal, E4/E5 owns
the outstanding work) — recorded in the suite README and the manifest
notes, not silent drift.

**Retirement check (2026-08-23, #185 branch):** the e9 reference-config
vacuity row is now **RESOLVED/REPAIRED on the #185 branch revision**
(faithful entry-armed escape + `InvNoCauselessReturn` detector in `Inv`
+ three reachability witnesses + two EXACT fail-closed GENERATED
negatives, all TLC-verified; the E9 verifier grew 14 -> 19 gates and the
manifest counts to 8 negative / 7 reachability; 19/19 PASS). The e9
negative-snapshot row's "next e9 negative addition" trigger FIRED AGAIN
(the #185 repair added `NegOldEscape`/`NegNaiveEscape` to `_gen_neg.py`,
the first generator cases carrying a per-case CONSTANTS override —
SplitWait=FALSE); CONSUMED as designed. The marker audit's one pending
correction landed with this change: the e9 README's 2026-08-18 audit note
now carries a supersede banner, the `Inv10` comment states the 2 ms bound
belongs only to the entry-armed reference park, and the manifest AUDIT
NOTE records the repair. The B6 evidence hierarchy is preserved: the
SplitWait=TRUE gates remain the load-bearing no-lost-wake evidence; the
reference gates additionally carry the causeless-return detector.

**Marker audit (2026-08-23):** debt/vacuity markers are consolidated in
three authoritative places — this register and matrix, the
`formal-models.md` coverage-gap entry, and `spec/tla/manifest.json`
(`coverage_gaps`). Everything else carrying `vacuity`/`unmodeled`/`DEBT`
wording lives under `docs/history/` (immutable audit/review records) and is
correctly scoped there. No stray `TODO`-formal marker exists outside
history. No marker contradicts the register; the one wording that the #185
evidence now qualifies — the e9 README's "reference topology's parks are
timeout-bounded" rationale for the unconditional escape (true only for the
MW-S2 MIXED-WAKE participant park, `scheduler.cpp:818`) — is recorded in the
e9 ACTIVE-NOW row and will be corrected by the #185 follow-up change,
not by this governance-only rebaseline.

## Not TLA debt: next evidence layers (issue #163 V2+)

The register above is **TLA+ debt only**: abstraction faithfulness and
protocol-model coverage of the C++ design. The following #163 layers are
separate evidence technologies, not missing models, and must not be recorded
(or paid) as TLA+ debt — nor may a TLA+ debt be claimed paid by pointing at
them:

- **V2 — trace conformance** (`ObservedBehaviors(C++ tests at R) ⊆
  Behaviors(as-built model at R)`, with a mandatory broken-trace negative
  control): **DONE for the E9 pilot** (issue #196; manifest suite
  `e9-trace-conformance`, claim `TRACE-CONFORMANT (TESTED EXECUTIONS)`
  at `d98d70dd`, corpus only; see
  `docs/verification/formal/e9-trace-conformance.md`). Generalizing the
  recorder/vocabulary beyond E9 remains unbuilt until a second protocol
  needs it.
- **V3 — weak-memory kernels** (extract the real production atomic order
  into bounded kernels; GenMC-class checker; broken-order negative
  control): **first kernel DONE** (issue #197: Completion publication/reset,
  `MEMORY-MODEL-CHECKED (BOUNDED KERNEL)`, RC11+RA+RLX with five rejected
  negative controls; `docs/verification/weak-memory/
  completion-publication-kernel.md`). S1A (#294) evaluated the remaining
  candidates: the CancelToken single-word protocol needs NO kernel (the
  only shared data is one `std::atomic<uint64_t>`; per-consumer
  `CancelState` fields are plain but thread-confined by fiber ownership —
  the TLA SC step maps to one atomic word transition; boundary: the
  best-effort `CancelState::acknowledged()` introspection reads a plain
  field and carries no cross-thread ordering claim; argument-based
  evaluation, not GenMC-checked — reopen on any one of: CancelState
  becomes cross-thread visible/read, the token protocol becomes
  multi-word or multi-atomic, correctness starts depending on ordering
  between the token atomic and another shared atomic/state, or
  epoch/reuse semantics introduce an ABA-sensitive cross-thread
  relation), and the RequestSlot
  generation/reuse face needs no kernel (the arena leaf is a pure-mutex
  domain with zero atomics). The park-commit vs wake-epoch face remains an
  unextracted candidate, only worth paying alongside a model change in the
  scheduler domain. TLA+ does not prove C++20 acquire/release/RMW behavior —
  this layer does.
- **V4 — deterministic-schedule corpus**: partially served by the
  `SLUICE_ASYNC_INTERNAL_TESTING` phase seams and the issue-#115/#161
  deterministic regressions; the #163 acceptance criterion (≥1 historical
  intermittent failure as a deterministic schedule) is MET. The
  consolidated corpus stays deliberately unbuilt until a second
  intermittent bug shows which seams to generalize.
- **V5 — failure-envelope matrix** (machine-checkable phase × fault ×
  required-outcome): **owned by child issue #198** — consolidates
  failure-model.md and the C2b–C2e / D2–D4 mutation matrices without
  manufacturing evidence.
- **V6 — guarantee-cost accounting**: partially served by the
  performance-attribution methodology; the explicit per-mechanism cost
  vectors and sustained-overload backpressure measurement are **owned by
  child issue #199**.
- **V7 — claim-policy enforcement**: partially served by manifest
  notes / AGENTS.md §8 / formal-models.md (a 2026-08-23 grep found no
  overclaim); revision-naming discipline and the mechanical changed-lines
  overclaim guard are **owned by child issue #200**.

A gap in V2/V3 is recorded on the #163 roadmap (children #196/#197),
never in this register.
