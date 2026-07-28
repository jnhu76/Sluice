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
| E10 WaitNode | `docs/spec/e10_waitnode/` | — |
| E11 Timer Wait | `docs/spec/e11_timer_wait/` | — |
| E12 Event | `docs/spec/e12_event/` | — |
| E12 Semaphore | `docs/spec/e12_semaphore/` | `scripts/verify-async-semaphore-formal.sh` |
| E12 AsyncMutex | `docs/spec/e12_async_mutex/` | — |
| E12 AsyncCondition | `docs/spec/e12_async_condition/` | — |
| E12 Queue | `docs/spec/e12_queue/` | `scripts/verify-async-queue-formal.sh` |
| E12 RwLock | `docs/spec/e12_rwlock/` | — |
| E13 Select | `docs/spec/e13_select/` | — |
| E7 MultiWorker Progress | `docs/spec/e7_multiworker_progress/` | — |
| E7 Publication | `docs/spec/e7_publication/` | — |
| E8 Ownership Transfer | `docs/spec/e8_ownership_transfer/` | — |
| E9 Park/Wake | `docs/spec/e9_park_wake/` | — |
| E9 Wake Handle Lifetime | `docs/spec/e9_wake_handle_lifetime/` | — |
| BlockingIoPool | `spec/tla/BlockingIoPool.tla` | — |

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
| BlockingIoPool TLA+ spec | `spec/tla/BlockingIoPool.tla` |
| E13 formal design | `docs/history/formal-design/e13-select-formal-core-design.md` *(historical)* |
| E13 formal safety | `docs/history/formal-design/e13-select-formal-safety-design.md` *(historical)* |
| TLA+ spec guide | `docs/spec/blocking-io-pool-tla-spec.md` |