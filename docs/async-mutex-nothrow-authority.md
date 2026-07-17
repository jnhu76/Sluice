# ASYNC Mutex Nothrow Authority

> **Decision identity:** `ASYNC-MUTEX-NOTHROW-AUTHORITY-1`
>
> **Design status:** `PASS — INDEPENDENT REVIEW REQUIRED`
>
> **Production status:**
> `IMPLEMENTED — AUTHOR SELF-ASSESSMENT —`
> `INDEPENDENT IMPLEMENTATION REVIEW REQUIRED`
>
> This document is a substrate design authority only. It does not authorize a
> production change. E12-E Queue depends on this decision and remains
> implementation-denied until this decision has passed an independent review
> and its production realization has been separately authorized. The author's
> self-assessment that production implementation has landed is recorded in
> `docs/async-mutex-nothrow-implementation.md`; it is **not** a closed gate
> and does not authorize the Queue.

## 1. Current-source fact

The current `sluice::async::Mutex` delegates directly to `std::mutex`:

```cpp
void lock() { impl_.lock(); }
bool try_lock() { return impl_.try_lock(); }
void unlock() { impl_.unlock(); }
```

`lock()` and `try_lock()` are not declared `noexcept`. `std::mutex::lock()` may
throw `std::system_error`; therefore current Scheduler code cannot treat an
acquisition after an irreversible winner CAS as a recoverable exception edge.
Merely describing the current function as no-throw would be false.

## 2. Binding failure policy

An internal mutex acquisition that participates in an authoritative Scheduler
transition has this policy:

```text
lock success:
    continue the transition

underlying lock failure:
    terminate/fail-fast
    never propagate a recoverable exception
```

The runtime cannot resume user execution after a lock failure while preserving
winner, ownership, queue-membership, and publication invariants. Treating that
failure as process-fatal is therefore the selected semantic policy. There is no
persistent dirty-Queue or deferred-reconciliation recovery protocol.

## 3. Selected production shape

The selected future production change is the single `Mutex` interface below;
no separate `lock_or_terminate()` API is selected:

```cpp
class Mutex {
public:
    void lock() noexcept SLUICE_ACQUIRE() {
        try {
            impl_.lock();
        } catch (...) {
            std::terminate();
        }
    }

    bool try_lock() noexcept SLUICE_TRY_ACQUIRE(true) {
        try {
            return impl_.try_lock();
        } catch (...) {
            std::terminate();
        }
    }

    void unlock() noexcept SLUICE_RELEASE() {
        impl_.unlock();
    }
};
```

The explicit catch documents the policy at the boundary; `noexcept` is also a
backstop. A violated `std::mutex::unlock()` ownership precondition remains a
program invariant failure, not a recoverable error.

`std::unique_lock<Mutex>` and `std::condition_variable_any` reacquisition use
the same `Mutex::lock()` entry and therefore inherit the fail-fast policy.

## 4. Scope

The selected policy applies to every use of `sluice::async::Mutex`, including:

| Authority | Required acquisition |
| --- | --- |
| Scheduler coordination | `Scheduler::global_mtx_` |
| Queue structural state | `QueueCore::state_mtx_` |
| Wait membership and resolution | each `WaitQueue::mtx_` |
| Scheduler wake epoch | `Scheduler::wake_mtx_` |
| winner follow-up | any Queue reconciliation acquisition |
| callback lease | `SchedulerWakeHandle::Control::mtx` |

`Mutex` is installed substrate, not the public Fiber-aware `AsyncMutex`
primitive. A downstream caller that directly uses `Mutex` receives the same
fail-fast lock-failure policy. Maintaining two exception policies on the same
BasicLockable type would make `LockGuard`, `unique_lock`, and condition-variable
reacquisition ambiguous, so a second throwing `lock()` surface is rejected.

This decision does **not** silently cover `std::mutex` fields such as
`WorkerState::inbox_mtx`. An authoritative post-winner path must either avoid
such a lock or establish a separately reviewed fail-fast wrapper. E12-E selects
the former: Queue runnable tickets are linked under the already-held
`global_mtx_`, not under `inbox_mtx`.

## 5. Winner-region consequence

After a Queue `WaitNode::resolve_` CAS succeeds, every remaining operation in
that winner's CommitGap must be one of:

```text
pointer/index/enum/counter mutation
WaitQueue unlink by the same winner
TimerRegistration state CAS
Fiber atomic state transition
intrusive Queue ticket link under already-held global_mtx_
Mutex acquisition governed by this fail-fast authority
condition-variable notification
```

No allocation, user-defined operation, virtual/function-pointer callback, or
recoverable exception is permitted. Follow-up reconciliation may acquire the
other role mutex only after the current winner is fully published; its lock
failure is still fatal under this authority and cannot strand the published
winner.

## 6. Lock-order compatibility

This decision changes failure behavior, not ordering. E12-E retains:

```text
Scheduler::global_mtx_
    -> QueueCore::state_mtx_
        -> at most one role WaitQueue mutex

Scheduler::global_mtx_
    -> Scheduler::wake_mtx_
```

No code may acquire `global_mtx_` while holding `wake_mtx_`, a Queue state
mutex, or a WaitQueue mutex.

## 7. Verification obligations for the later substrate task

The production task must independently prove:

1. the exact signatures above compile with TSA and every current lock wrapper;
2. no current call site depends on catching `std::system_error` from `Mutex`;
3. condition-variable wait/reacquire remains well-formed;
4. an injected/controlled lock failure terminates rather than returns into an
   authoritative transition;
5. ASan, UBSan, TSan, and existing Scheduler tests remain green;
6. an independent adversarial review accepts the changed runtime-wide policy.

Until those obligations pass:

```text
ASYNC-MUTEX-NOTHROW-AUTHORITY-1 IMPLEMENTATION: UNAUTHORIZED
E12-E IMPLEMENTATION AUTHORIZATION: DENIED
```
