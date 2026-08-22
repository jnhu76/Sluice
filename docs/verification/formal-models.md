# Formal Models at Sluice

## Purpose

TLA+ formal models are used to verify abstract protocol properties of Sluice's synchronization primitives and runtime substrate. They supplement — but do not replace — C++ implementation tests.

## What TLA+ does and does not prove

- **TLA+ proves abstract protocol properties** under the model's assumptions (bounded state space, weak fairness, etc.).
- **TLA+ does NOT prove C++ implementation correctness.** The model is an abstraction; the C++ code may have bugs that the model does not capture.
- **A negative model** (a deliberately broken spec that TLC finds a counterexample for) demonstrates that the model can actually fail — proving the invariants are not vacuously true.

## Model inventory

| Suite ID | Subsystem | Model location | Verification script |
|----------|-----------|---------------|-------------------|
| blocking-io-pool | BlockingIoPool | `spec/tla/blocking_io_pool/` | `scripts/formal/verify-blocking-io-pool.sh` |
| d1-uring-poison | D1 io_uring Poison/Recovery | `spec/tla/d1_uring_poison/` | `scripts/formal/verify-d1-uring-poison.sh` |
| request-arena | RequestArena/RequestSlot explicit-I/O lifecycle | `spec/tla/request_arena/` | `scripts/formal/verify-request-arena.sh` |
| spawn-wake-epoch | Spawn Wake-Epoch Obligation (MODEL-007d) | `spec/tla/spawn_wake_epoch/` | `scripts/formal/verify-spawn-wake-epoch.sh` |
| worker-retire-rescue | Worker Retire Ticket Rescue (MODEL-007e) | `spec/tla/worker_retire_rescue/` | `scripts/formal/verify-worker-retire-rescue.sh` |
| cancel-token-epoch | CancelToken Request-Epoch Isolation (MODEL-007c) | `spec/tla/cancel_token_epoch/` | `scripts/formal/verify-cancel-token-epoch.sh` |
| e7-publication | E7 Publication | `spec/tla/e7_publication/` | `scripts/formal/verify-e7-publication.sh` |
| f1-wait-record | F1 WaitRecord Registry (MODEL-007b) | `spec/tla/f1_wait_record/` | `scripts/formal/verify-f1-wait-record.sh` |
| e7-multiworker-progress | E7 MultiWorker Progress | `spec/tla/e7_multiworker_progress/` | `scripts/formal/verify-e7-multiworker-progress.sh` |
| e8-ownership-transfer | E8 Ownership Transfer | `spec/tla/e8_ownership_transfer/` | `scripts/formal/verify-e8-ownership-transfer.sh` |
| e8-suspend-switch | E8 Suspend-Switch Steal Exclusion (MODEL-007a) | `spec/tla/e8_suspend_switch/` | `scripts/formal/verify-e8-suspend-switch.sh` |
| e9-park-wake | E9 Park/Wake | `spec/tla/e9_park_wake/` | `scripts/formal/verify-e9-park-wake.sh` |
| e9-wake-handle-lifetime | E9 Wake Handle Lifetime | `spec/tla/e9_wake_handle_lifetime/` | `scripts/formal/verify-e9-wake-handle-lifetime.sh` |
| e10-waitnode | E10 WaitNode | `spec/tla/e10_waitnode/` | `scripts/formal/verify-e10-waitnode.sh` |
| e11-timer-wait | E11 Timer Wait | `spec/tla/e11_timer_wait/` | `scripts/formal/verify-timer-wait.sh` |
| e12-event | E12 Event | `spec/tla/e12_event/` | `scripts/formal/verify-event.sh` |
| e12-semaphore | E12 Semaphore | `spec/tla/e12_semaphore/` | `scripts/formal/verify-async-semaphore.sh` |
| e12-async-mutex | E12 AsyncMutex | `spec/tla/e12_async_mutex/` | `scripts/formal/verify-async-mutex.sh` |
| e12-async-condition | E12 AsyncCondition | `spec/tla/e12_async_condition/` | `scripts/formal/verify-async-condition.sh` |
| e12-queue | E12 Queue | `spec/tla/e12_queue/` | `scripts/formal/verify-async-queue.sh` |
| e12-rwlock | E12 RwLock | `spec/tla/e12_rwlock/` | `scripts/formal/verify-async-rwlock.sh` |
| e12-rwlock-scheduler-liveness | E12 RwLock + Scheduler Liveness (issue #161) | `spec/tla/e12_rwlock_scheduler_liveness/` | `scripts/formal/verify-e12-sched-liveness.sh` |
| e13-select-core | E13 Select Core | `spec/tla/e13_select/` | `scripts/formal/verify-e13-select-core.sh` |
| e13-select-safety | E13 Select Safety | `spec/tla/e13_select/` | `scripts/formal/verify-e13-select-safety.sh` |
| e16-application-runtime | E16 Application Runtime | `spec/tla/e16_application_runtime/` | `scripts/formal/verify-e16-application-runtime.sh` |

## Requirements

When a code change alters a modeled state transition, admission rule, winner rule, queue bound, lifecycle, or shutdown behavior:

1. Update the matching model or explicitly explain why the model is unaffected.
2. Run the repository's existing verification script or documented checker command.
3. Preserve a negative/broken-model check when the subsystem uses one.
4. Add or retain a C++ regression test connecting the modeled property to implementation behavior.

**Never report "formally verified implementation" when only the abstract protocol model was checked.**

## Coverage gaps (AGENTS.md §17 — recorded, not invented)

Not every load-bearing protocol has a TLA+ model. AGENTS.md §17 permits, for a
high-risk protocol, *either* a focused model *or* a recorded justified gap with
a follow-up trigger. Each accepted gap is recorded as a `coverage_gaps` entry in
`spec/tla/manifest.json` and summarized here. An accepted gap is formal debt, not
a claim that the protocol is verified, and not permission to skip the model when
a revisit trigger fires.

The `coverage_gaps` structure is authoritative long-lived manifest structure:
`scripts/formal/verify.py check` (and `doctor`) validate every entry — unique
`id`, required fields, non-empty `revisit_triggers`, and existence of every
referenced `implementation_bindings` / `regression_test_cross_links` /
`owner_docs` file — so a malformed entry fails the structural gate instead of
silently passing (audit #94/#100 review hardening, 2026-08-14).

### `request-arena-lifecycle` — RequestArena / RequestSlot explicit-I/O lifecycle

**Status (2026-08-18 formal audit): PARTIALLY MODELED.** The `request-arena`
suite now binds the slot lifecycle
(`include/sluice/async/detail/request_arena.hpp`, `request_slot.hpp`) with the
smallest single-slot model: five-stage admission (reserve → prepare →
install binding → commit → enqueue → dispatch), Scheme-B enqueue/cancel arbitration, terminal-winner
exactly-once, generation increment before reuse, reap-only Completion
publication gated on the acknowledged enqueue pin, borrow-through-reap, the
running-cancel intent verbatim law (ADR Decision 11), and the quiescent-
destruction conditions. Negative models NEG-RA-1..6, wrong-property controls,
and reachability witnesses W1–W5 gate the abstraction (see the suite README
for the property → C++ regression bridge). Authority layering (PR #125
review P1): the liveness properties are CONDITIONAL on Layer-B external
progress obligations (`WF(Enqueue)` = backend submit path,
`WF(Reap)` = backend/runtime progress loop; all cfgs `CHECK_DEADLOCK FALSE`),
and the Decision-11 confirmed-interruption provenance is a **backend
obligation, not leaf-enforced** — the C++ `record_canceled` checks only slot
state and exactly-once and has no production caller today.

**Residual (still unmodeled, executable-evidence scope):** ready-ring reap
ORDER across two or more simultaneously backend_ready slots (ADR Decision 9
backend-known order), multi-slot free-list/accounting interference, and the
backend admission transaction around commit (Completion
`idle → binding → outstanding`, AGENTS.md §4.3). A capacity-2 suite variant
is the recorded follow-up; trigger: any change to the ready-ring ordering
rule.

**Not covered by adjacent suites.** The e7-publication / e8-ownership-transfer
suites model runnable tickets and ownership transfer; e9-park-wake /
e10-waitnode model Scheduler wake / wait-queue primitives; e16-application-
runtime models the runtime epoch. Do not read those suites as RequestArena
coverage.

**Current executable evidence:** `tests/request_lifecycle_scheme_b_test.cpp`
(Scheme-B arbitration, terminal winner, generation/reuse, the
`arena_mainline_state_transition_matrix` / `arena_illegal_transition_contract_errors`
state-machine matrix, waiter exactly-once delivery, acquire-observer
ordering), `tests/request_arena_test.cpp` (supporting state/accounting/
borrow-lifecycle cases), `tests/request_waiter_borrow_lease_test.cpp`
(register-vs-reap, cancel_waiter-vs-reap, borrow-through-reap), the per-backend C2b/C2c/C2d/C2e
integration rows on Fake + ThreadPool, 8–13 single-point production mutation
executions per C2 class, and the arena death tests. Per-slice gap notes also
appear in the Phase C2c / C2d / E compliance gates.

**Revisit triggers:** before materially changing RequestArena / RequestSlot
lifecycle authority; before changing the ready-ring FIFO ordering rule
(Decision 9) — the trigger for the capacity-2 order model; before changing
generation/reuse rules; before introducing a second waiter or multi-wait
abstraction into the arena; before splitting the terminal-winner or
reap-only-publication authority across a second domain; or when a future
concurrency defect demonstrates the existing executable evidence does not
distinguish the faulty protocol.

## Navigation

| Topic | Document |
|-------|----------|
| BlockingIoPool TLA+ spec | `docs/verification/formal/blocking-io-pool-tla-spec.md` |
| E13 formal design | `docs/history/formal-design/e13-select-formal-core-design.md` *(historical)* |
| E13 formal safety | `docs/history/formal-design/e13-select-formal-safety-design.md` *(historical)* |
| TLA+ spec guide | `docs/verification/formal/blocking-io-pool-tla-spec.md` |
