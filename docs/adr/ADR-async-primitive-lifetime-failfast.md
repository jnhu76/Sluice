# ADR: Async Synchronization Primitive Destruction Fail-Fast

```text
Status: Accepted
Date: 2026-08-19
Baseline SHA: d7ee077 (master, PR #130 merge)
Supersedes: the "Release trusts the caller" half of the E12 destruction
            contract for AsyncMutex / AsyncRwLock / AsyncCondition /
            WaitQueue (the Debug-only assertion behavior). Everything else
            of the E12 destruction contract (no cancel-all, no wake-all,
            no force-release, no synthesized results) is unchanged.
Superseded by: none
```

## 1. Context

The async synchronization primitives (`AsyncMutex`, `AsyncRwLock`,
`AsyncCondition`, and their shared `WaitQueue`; `Event` separately) document
a destruction contract: the caller must end the object's lifetime only in a
quiescent state — no owner, no active readers/writer, no in-flight `wait()`,
no registered waiters. Until this ADR the enforcement was inconsistent:

- `AsyncMutex`, `AsyncRwLock`, `AsyncCondition`, `WaitQueue`: a bare
  `assert(...)` in the destructor — Debug abort, **Release silent pass**
  ("Release trusts the caller", recorded in the E12 design notes and in the
  `async_rwlock_death_test` Category A gating).
- `Event`: `assert(...)` **plus** a named Release fail-fast
  (`detail::select_invariant_fail_fast()`) for live Select arms.
- `AsyncIoContext`, `Scheduler`, `ThreadPoolBackend`, `UringAsyncBackend`,
  `RequestArena`, `Group`: named per-authority fail-fast entries in
  `include/sluice/async/detail/fail_fast.hpp`, active in Debug AND Release, death-tested
  (`async_context_outstanding_fail_fast`,
  `scheduler_wait_registry_nonempty_fail_fast`,
  `threadpool_non_quiescent_destruction_fail_fast`,
  `uring_non_quiescent_destruction_fail_fast`,
  `request_arena_destruction_fail_fast`, `group_lifetime_fail_fast`).

The failure-model remediation (issue #135; `AGENTS.md` §9.2; the T6
destruction-violation class of the failure-response taxonomy under review in
PR #147) classifies destruction-in-a-forbidden-state uniformly: a lifetime contract violation with no recovery
semantics. `NDEBUG` is not semantic authority; a contract that only a Debug
assertion enforces is not enforced for Release binaries or downstream
consumers. Two primitives families already follow the unified rule; four did
not.

## 2. Decision

1. Destruction of `AsyncMutex`, `AsyncRwLock`, `AsyncCondition`, and
   `WaitQueue` in a non-quiescent state is a caller contract violation that
   **fails fast via a named per-authority entry in `include/sluice/async/detail/fail_fast.hpp`,
   active in Debug AND Release**:
   - `async_mutex_lifetime_fail_fast()` — destroyed while owned;
   - `async_rwlock_lifetime_fail_fast()` — destroyed with active readers or
     an active writer;
   - `async_condition_lifetime_fail_fast()` — destroyed while a `wait()` is
     in flight (condition epoch or reacquire epoch);
   - `wait_queue_lifetime_fail_fast()` — destroyed with registered waiters.
   Naming is per authority (never a generic `lifetime_fail_fast`) so the
   terminating entry names the violated authority in the stack trace.
2. The existing Debug `assert(...)` messages are kept as Debug-only
   diagnostics ahead of the fail-fast call (the `Event` destructor's
   belt-and-braces shape). The fail-fast is the enforcement; the assert is
   the tripwire.
3. Quiescent destruction is unchanged and MUST remain side-effect-free.
4. No recovery, no cleanup, no synthesized outcomes: the destructor MUST NOT
   cancel waiters, wake waiters, unlink Select arms, force-release ownership,
   implicitly unlock, drain, or fabricate results before failing fast. The
   violation is reported, never repaired (AGENTS.md §14).
5. Same `[[noreturn]] noexcept` contract as every other `fail_fast.hpp`
   entry: no allocation, no locking, no I/O, no dynamic strings, ultimately
   `std::terminate()`.
6. Non-destruction misuse assertions (e.g. `unlock_read` underflow,
   non-owner `unlock_write`, recursive `lock`) are OUT OF SCOPE: they remain
   Debug-only diagnostics under this ADR. Widening them is a separate
   decision with its own analysis.
7. `Event` is unchanged: it already follows this rule
   (`select_invariant_fail_fast` for live Select arms; its `WaitQueue`
   member inherits `wait_queue_lifetime_fail_fast`).

## 3. Why fail-fast instead of Release "trust the caller"

- A non-quiescent destruction leaves REGISTERED WAITERS and Scheduler
  routing records referring to freed memory — continuing is silent
  use-after-free, not graceful degradation. There is no sound "trust" to
  extend.
- A destructor has no `Result` channel; the truthful options are silent
  abandonment (rejected: it strands wake obligations, exactly what
  `scheduler_wait_registry_nonempty_fail_fast` exists to prevent) or
  deterministic termination.
- Consistency: the rest of the async lifetime surface (context, scheduler,
  backends, arena, group) already fails fast in both modes. The split was
  historical, not principled.

## 4. Death-test requirements

Per primitive authority, in BOTH Debug and Release (POSIX fork/exec harness,
`tests/death_test_runner_posix.hpp`):

- violation case: destruction in the forbidden state terminates via the
  named fail-fast boundary (exit 86 protocol);
- control case: valid quiescent usage (lock/unlock, read/write lock
  cycles, wait/notify cycles drained) destroys normally (exit 0).

Actual test layout (this ADR's change):

- **`async_sync_lifetime_death_test` (new binary)** carries the
  `AsyncMutex` and `AsyncCondition` authorities:
  - M1 — destroy `AsyncMutex` while a parked fiber owns it
    (`async_mutex_lifetime_fail_fast`);
  - C1 — destroy `AsyncCondition` while a `wait()` is in flight — the
    fiber is parked inside the condition wait and `active_waits_ != 0`
    (`async_condition_lifetime_fail_fast`);
  - CTL — control: full lock/wait/notify/reacquire/unlock cycle, then
    quiescent destruction of both objects, exit 0.
- **`async_rwlock_death_test` (existing binary, gate change)**: cases A4
  (active reader), A5 (active writer), and A6 (queued waiter) move from the
  Debug-only gate to the both-mode gate; A4/A5 exercise
  `async_rwlock_lifetime_fail_fast`, A6 exercises
  `wait_queue_lifetime_fail_fast` directly (an `AsyncRwLock` destroyed with
  a queued waiter reaches its `WaitQueue` member's destructor).
- `async_mutex_death_test` is NOT involved: it tests the low-level
  non-async `Mutex` acquisition boundary (a different authority,
  `async_mutex_lock_fail_fast`) and is unchanged by this ADR.
- C1 does NOT additionally prove `wait_queue_lifetime_fail_fast`: in C1 the
  outer `~AsyncCondition` authority fires before its `WaitQueue` member's
  destructor runs. The `WaitQueue` authority has its own both-mode case
  (A6 above).

## 5. Consequences

- Release binaries and downstream consumers get deterministic, named
  termination instead of silent use-after-free on this misuse class.
- `docs/reference/api.md` and the primitive headers' destruction-contract
  comments are updated in the same change; the historical E12 design notes
  are not rewritten (this ADR supersedes the Release half explicitly).
- The failure-response taxonomy's T6 class (policy PR #147) gains these
  four named entries as canonical instances once both land (no text
  dependency in either direction).

## 6. AGENTS.md §8 design-compliance record

This ADR changes synchronization-primitive lifetime behavior, which triggers
the §8 architecture compliance gate. Compact record (all other §8 fields are
no-change):

- **Gate 0 (state machine)**: every normal-path state transition of
  AsyncMutex / AsyncRwLock / AsyncCondition / WaitQueue is unchanged. The
  only state-machine change is the response to an INVALID lifetime terminal
  state in a destructor: Debug-only assert → named fail-fast active in
  Debug AND Release. No new states, no new legal transitions.
- **Gate 1 (lock / atomic authority)**: no new lock-order edges and no
  blocking cleanup. Each destructor check reads existing members under the
  destruction context it already had; the fail-fast entry itself takes no
  locks, allocates nothing, performs no I/O (`[[noreturn]] noexcept`,
  terminates).
- **Gate 2 (wake / progress)**: no new wake, publication, or progress
  semantics. The fail-fast path never notifies, never publishes, never
  routes — it terminates.
- **Gate 3 (resource capacity)**: N/A — no queue, arena, worker, or
  capacity model is touched; no allocation anywhere on the new path.
- **Gate 4 (shutdown semantics)**: invalid destruction is a T6 named
  fail-fast in Debug AND Release (AGENTS.md §14): **no drain, no cancel,
  no wait, no allocation, no recovery — a destructor violation is
  terminate-only.** Quiescent destruction remains side-effect-free and
  silent.
