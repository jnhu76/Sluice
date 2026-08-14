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
| e7-publication | E7 Publication | `spec/tla/e7_publication/` | `scripts/formal/verify-e7-publication.sh` |
| e7-multiworker-progress | E7 MultiWorker Progress | `spec/tla/e7_multiworker_progress/` | `scripts/formal/verify-e7-multiworker-progress.sh` |
| e8-ownership-transfer | E8 Ownership Transfer | `spec/tla/e8_ownership_transfer/` | `scripts/formal/verify-e8-ownership-transfer.sh` |
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

### `request-arena-lifecycle` — RequestArena / RequestSlot explicit-I/O lifecycle

No TLA+ suite binds the RequestArena / RequestSlot lifecycle
(`include/sluice/async/detail/request_arena.hpp`, `request_slot.hpp`) — the
load-bearing accepted-request protocol since Phase B. A future model would
encode the five-stage admission (reserve → prepare → commit → enqueue →
dispatch), Scheme-B enqueue/cancel arbitration, terminal-winner exactly-once,
generation increment before reuse, the ready-ring FIFO, reap-only Completion
publication, borrow-through-reap, the enqueue-in-flight pin, and the quiescent-
destruction conditions.

**Not covered by adjacent suites.** The e7-publication / e8-ownership-transfer
suites model the Completion claim/publish CAS and ownership transfer (the
*publication object*, a different protocol from the slot lifecycle);
e9-park-wake / e10-waitnode model Scheduler wake / wait-queue primitives;
e16-application-runtime models the runtime epoch. None models the arena slot
state machine, Scheme-B arbitration, generation/reuse, ready-ring, or
reap-only publication — do not read those suites as RequestArena coverage.

**Current executable evidence instead of a model:**
`tests/request_arena_test.cpp` (state-machine matrix),
`tests/request_lifecycle_scheme_b_test.cpp` (Scheme-B arbitration, terminal
winner, generation/reuse),
`tests/request_waiter_borrow_lease_test.cpp` (register-vs-reap,
cancel_waiter-vs-reap, borrow-through-reap), the per-backend C2b/C2c/C2d/C2e
integration rows on Fake + ThreadPool, 8–13 single-point production mutation
executions per C2 class, and the arena death tests. Per-slice gap notes also
appear in the Phase C2c / C2d / E compliance gates.

**Revisit triggers:** before materially changing RequestArena / RequestSlot
lifecycle authority; before changing generation/reuse rules; before introducing
a second waiter or multi-wait abstraction into the arena; before splitting the
terminal-winner or reap-only-publication authority across a second domain; or
when a future concurrency defect demonstrates the existing executable evidence
does not distinguish the faulty protocol.

## Navigation

| Topic | Document |
|-------|----------|
| BlockingIoPool TLA+ spec | `docs/verification/formal/blocking-io-pool-tla-spec.md` |
| E13 formal design | `docs/history/formal-design/e13-select-formal-core-design.md` *(historical)* |
| E13 formal safety | `docs/history/formal-design/e13-select-formal-safety-design.md` *(historical)* |
| TLA+ spec guide | `docs/verification/formal/blocking-io-pool-tla-spec.md` |
