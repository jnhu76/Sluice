# Formal Models at Sluice

## Purpose

TLA+ formal models are used to verify abstract protocol properties of Sluice's synchronization primitives and runtime substrate. They supplement — but do not replace — C++ implementation tests.

## What TLA+ does and does not prove

- **TLA+ proves abstract protocol properties** under the model's assumptions (bounded state space, weak fairness, etc.).
- **TLA+ does NOT prove C++ implementation correctness.** The model is an abstraction; the C++ code may have bugs that the model does not capture.
- **A negative model** (a deliberately broken spec that TLC finds a counterexample for) demonstrates that the model can actually fail — proving the invariants are not vacuously true.

## Model inventory

| Subsystem | Model location | Verification script |
|-----------|---------------|-------------------|
| BlockingIoPool | `spec/tla/blocking_io_pool/` | `scripts/formal/verify-blocking-io-pool.sh` |
| E7 Publication | `spec/tla/e7_publication/` | `scripts/formal/verify-e7-publication.sh` |
| E7 MultiWorker Progress | `spec/tla/e7_multiworker_progress/` | `scripts/formal/verify-e7-multiworker-progress.sh` |
| E8 Ownership Transfer | `spec/tla/e8_ownership_transfer/` | `scripts/formal/verify-e8-ownership-transfer.sh` |
| E9 Park/Wake | `spec/tla/e9_park_wake/` | `scripts/formal/verify-e9-park-wake.sh` |
| E9 Wake Handle Lifetime | `spec/tla/e9_wake_handle_lifetime/` | `scripts/formal/verify-e9-wake-handle-lifetime.sh` |
| E10 WaitNode | `spec/tla/e10_waitnode/` | `scripts/formal/verify-e10-waitnode.sh` |
| E11 Timer Wait | `spec/tla/e11_timer_wait/` | `scripts/formal/verify-timer-wait.sh` |
| E12 Event | `spec/tla/e12_event/` | `scripts/formal/verify-event.sh` |
| E12 Semaphore | `spec/tla/e12_semaphore/` | `scripts/formal/verify-async-semaphore.sh` |
| E12 AsyncMutex | `spec/tla/e12_async_mutex/` | `scripts/formal/verify-async-mutex.sh` |
| E12 AsyncCondition | `spec/tla/e12_async_condition/` | `scripts/formal/verify-async-condition.sh` |
| E12 Queue | `spec/tla/e12_queue/` | `scripts/formal/verify-async-queue.sh` |
| E12 RwLock | `spec/tla/e12_rwlock/` | `scripts/formal/verify-async-rwlock.sh` |
| E13 Select Core | `spec/tla/e13_select/` | `scripts/formal/verify-e13-select-core.sh` |
| E13 Select Safety | `spec/tla/e13_select/` | `scripts/formal/verify-e13-select-safety.sh` |

## Requirements

When a code change alters a modeled state transition, admission rule, winner rule, queue bound, lifecycle, or shutdown behavior:

1. Update the matching model or explicitly explain why the model is unaffected.
2. Run the repository's existing verification script or documented checker command.
3. Preserve a negative/broken-model check when the subsystem uses one.
4. Add or retain a C++ regression test connecting the modeled property to implementation behavior.

**Never report "formally verified implementation" when only the abstract protocol model was checked.**

## Navigation

| Topic | Document |
|-------|----------|
| BlockingIoPool TLA+ spec | `docs/verification/formal/blocking-io-pool-tla-spec.md` |
| E13 formal design | `docs/history/formal-design/e13-select-formal-core-design.md` *(historical)* |
| E13 formal safety | `docs/history/formal-design/e13-select-formal-safety-design.md` *(historical)* |
| TLA+ spec guide | `docs/verification/formal/blocking-io-pool-tla-spec.md` |