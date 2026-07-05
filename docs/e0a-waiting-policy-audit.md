# E0-A: Task waiting-policy audit

**Status: sluice-CORE-E0A.** Audit + refactor of the task-layer wait policy,
required by the E0 ADR (`docs/adr/ADR-execution-model.md` §3) before any
context-switching code lands. The ADR's required boundary:

```text
task state / result / cancel state
              |
              v
       logical wait request      <- Future/Group/Batch ask "wait until terminal"
              |
              v
       Io execution strategy     <- Threaded (cv/park) vs Evented (fiber yield)
          /             \
   Threaded            Evented
```

The directive (task §E0-A): inspect `Future<T>`, `Group`, `Batch`,
cancellation state, and all await/wait paths. Determine whether any task
abstraction directly embeds Threaded waiting policy through
`std::condition_variable`, `atomic::wait`, `Completion::wait`, sleeps,
polling, or equivalent. Refactor behind the Io execution boundary IF so,
preserving the result/idempotency/lifetime/cancellation contracts.

## 1. Findings (exhaustive grep)

Wait primitives found in `include/sluice/async/` and `src/async/`:

### 1a. TASK LAYER — leaks (must refactor)

| Location | Primitive | Verdict |
| --- | --- | --- |
| `future.hpp:62-83` `Future<T>::complete_with`/`await` | `std::mutex mtx_` + `std::condition_variable cv_`; `await()` blocks on `cv_.wait(lk, pred)` | **LEAK.** The task abstraction embeds the Threaded physical mechanism. This is the primary refactor target. |
| `group.hpp` `Group::mtx_` | `std::mutex` guarding the task/future vectors | **NOT a leak.** This protects the Group's own internal state (the task list), not a wait. Legitimate. |
| `group.cpp` `Group::await` | calls `f->await()` (Future) | **INHERITS the leak.** Once Future's await is refactored, Group composes the strategy-provided wait. No direct change needed beyond passing the policy through. |

### 1b. BACKEND LAYER — NOT leaks (backends ARE the strategy implementation)

| Location | Primitive | Verdict |
| --- | --- | --- |
| `threadpool_backend.cpp:158-159` `wait_one` | `cv_.wait(lk, pred)` | **Correct.** `ThreadPoolBackend` IS the Threaded strategy. Backends are allowed to embed the physical mechanism — that is their role (ADR §3). |
| `uring_backend.cpp:414` `wait_one` | `io_uring_submit_and_wait` | **Correct.** `UringAsyncBackend` is the Evented op backend. |
| `threadpool_backend.cpp:36-50` `do_read/write/sync` | blocking syscalls (`pread`/`pwrite`/`fsync`/`fdatasync`) | **Correct.** ThreadPool's ops ARE blocking syscalls by design (ADR §4). |

### 1c. CANCELLATION LAYER — no wait primitives

`CancelToken` (`cancel.hpp`) uses `std::atomic<uint8_t>` with release/acquire
ordering. `CancelState` is plain state. `check_cancel` is non-blocking. **No
wait policy embedded.** No change needed.

### 1d. BATCH

`Batch::await_one` (`batch.cpp`) drives `ctx.wait_one()`. It does NOT embed a
cv directly — it delegates to the backend's wait. The backend wait is
strategy-correct (1b). **No leak.** (Batch's own `mtx_` — it has none; slots
are a vector of unique_ptr.) No change needed.

## 2. Refactor target

**Only `Future<T>` leaks.** The fix: extract the physical wait behind a
strategy-provided hook so `Future::await()` asks "wait until ready" without
knowing HOW. The default hook is the Threaded policy (the cv) — preserving
100% of current behavior and all existing tests.

### 2a. Design: `WaitPolicy` seam

Introduce a small abstract seam:

```cpp
namespace sluice::async {
// The physical-wait seam. A Future delegates "wait until terminal" here.
// The default (Threaded) policy blocks the calling thread on a cv; a future
// Evented policy (E5) will suspend the current fiber via the scheduler.
class WaitPolicy {
public:
    virtual ~WaitPolicy() = default;
    // Block the current execution context until `ready` flips true, then
    // return. The implementation owns the physical mechanism (cv / fiber yield).
    virtual void wait_until_ready(const std::atomic<bool>& ready,
                                  std::mutex& mtx,
                                  std::condition_variable& cv) = 0;
};
}
```

`Future<T>` keeps its `mtx_`/`cv_`/`ready_` (the STATE is strategy-independent)
but `await()` delegates the wait to an injectable `WaitPolicy*` (default =
Threaded). When E5 lands, an Evented policy replaces the wait with a fiber
yield; the Future's state, result, idempotency, and cancel contracts are
unchanged.

This is the narrowest refactor that satisfies the ADR §3 boundary: the task
abstraction no longer embeds the physical mechanism; the strategy does.

### 2b. Contracts preserved (verification)

The refactor MUST NOT change:
- `Future::complete_with` exactly-once terminal publish.
- `Future::await` idempotency (cached result on second call).
- `Future::cancel` idempotency + best-effort semantics.
- `Group::await/cancel` idempotency + cancel-propagation boundary.
- `Batch::await_one/next` semantics.
- All existing tests (028 future, 029 group, 030 batch) pass unchanged.

The Threaded default policy makes the existing behavior identical to today;
the existing tests are the GREEN proof that the refactor preserved contracts.

## 3. Non-leaks deliberately left alone

- `ThreadPoolBackend`/`UringAsyncBackend` wait primitives: backends are the
  strategy implementation; embedding the physical mechanism there is correct.
- `Group::mtx_`: internal-state mutex, not a wait.
- `CancelToken` atomics: non-blocking state, not a wait.

## 4. Out of scope (deferred to E5)

The Evented `WaitPolicy` implementation (fiber-yield-based) does not exist yet
— it depends on the Task/Fiber + context-switch + single-worker proof (E1-E4).
This audit only refactors the SEAM into place; E5 plugs the Evented policy in.

## 5. Cross-links

- ADR: `docs/adr/ADR-execution-model.md` (E0) §3.
- Job sequence: `docs/zig-stdio-migration-jobs.md` (023C) — PHASE E.
- Source graph: `docs/zig-stdio-async-port-map.md` (023A).
