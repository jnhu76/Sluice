# ADR: Application Runtime Architecture

```text
Status: Proposed
Date: 2026-07-27
Baseline SHA: ba7eb62563ca7c8af19e264ddb05a5a88a2fd7a7
Supersedes: none
Superseded by: none
```

## 1. Context

Sluice's async runtime foundation (E0–E15) is complete: `Scheduler`, `Fiber`,
`Group`, `Future`, `Completion<T>`, `AsyncIoContext`, `AsyncBackend`, cooperative
cancellation, and the E10–E13 synchronization primitives. What does not yet exist
is the application layer that ties these components into a coherent, startable,
stoppable, drainable, joinable unit with lifecycle authority.

Today a consumer must manually compose `AsyncIoContext` → `Scheduler` →
`Group(Scheduler&)` → `Group::await()` → `Scheduler::run_live()`. This manual
composition has gaps: no unified start, no admission control, no runtime-level
cancellation, no startup rollback, no drain/join separation, and no destructor
contract. See `docs/design/e16-application-runtime.md` §3 for the full problem
statement.

The foundation components are non-movable and have strict lifetime contracts:
- `Scheduler` borrows `AsyncIoContext` (`scheduler.hpp:213`); non-movable
  (`scheduler.hpp:216-219`); destructor asserts quiescence
  (`scheduler.cpp:169-196`).
- `AsyncIoContext` owns its backend (`async_io_context.hpp:121`); move-only
  (`async_io_context.hpp:125-133`); destructor fail-fast if outstanding
  (`async_io_context.cpp:30-41`).
- `Group(Scheduler&)` borrows the scheduler (`group.hpp:78`); destructor
  fail-fast on pending evented tasks (`group.cpp:117-122`).

This ADR decides the architecture of the E16 Application Runtime layer that
closes these gaps.

## 2. Decision

Adopt a **builder-constructed, one-shot, injected-backend Application Runtime**
that owns the `AsyncIoContext`, `Scheduler`, root task domain, root cancellation,
and worker-thread lifecycle, and exposes a unified
`start / submit / request_stop / drain / join / shutdown` contract.

The Runtime is a thin layer above the existing foundation. It introduces no new
primitives, no scheduler changes, and no public API re-semanticization.

## 3. Ownership decision

The Runtime **owns**:
- `AsyncIoContext` (which owns the injected `AsyncBackend`)
- `Scheduler` (borrows the `AsyncIoContext`)
- root `Group` (borrows the `Scheduler`)
- root `CancelToken` (runtime-level cancellation)
- worker/control state (admission gate, lifecycle state, worker threads)

The backend is **injected** (not created internally), enabling deterministic test
injection via the existing `AsyncIoContext(unique_ptr<AsyncBackend>)` seam
(`async_io_context.hpp:121`).

The Runtime is non-copyable and non-movable.

Evidence: `docs/design/e16-application-runtime.md` §8.

## 4. Lifecycle decision

The lifecycle is a one-shot state machine:

```text
Constructed → Starting → Running → Stopping → Draining → Stopped
                      ↘ StartFailed (rollback)
Any state ↗ Fatal (invariant violation → std::terminate)
```

| State | Meaning |
| --- | --- |
| `Constructed` | Built but not started. Admission closed. No workers. |
| `Starting` | `start()` in progress. |
| `Running` | Started. Admission open. Workers active. |
| `Stopping` | `request_stop()` called. Admission closed. Root cancel published. |
| `Draining` | `drain()` in progress. |
| `Stopped` | Drained + joined. Safe to destroy. |
| `StartFailed` | `start()` failed. Rolled back. Safe to destroy. |
| `Fatal` | Invariant violation. Process terminates. |

Restart is **not** supported. A Stopped Runtime may not be restarted.

Evidence: `docs/design/e16-application-runtime.md` §11.

## 5. Admission decision

**A task is successfully admitted when the admission gate is open at the instant
the task is enqueued into the root Group via `Group::async()`.**

The admission gate is an `std::atomic<bool>` owned by the Runtime. `submit()`
checks the gate before `Group::async()`. `request_stop()` atomically closes the
gate. The gate check is the linearization point for stop-vs-submit races:
- Winner (gate open) → admitted.
- Loser (gate closed) → rejected with `IoError::invalid_state`; body never runs.

Evidence: `docs/design/e16-application-runtime.md` §13.

## 6. Cancellation decision

The Runtime owns a root `CancelToken` (`cancel.hpp:47`). `request_stop()`
publishes root cancellation via `root_token_.request()`. Cancellation is
cooperative (matching the existing model, `cancel.hpp:14`): tasks observe the
token at cancel points (`check_cancel`, `cancel.hpp:147`). Cancellation is not an
unconditional escape hatch — a task that does not observe cancellation can
prevent `drain()` from returning (matching `group.hpp:69-76`).

Evidence: `docs/design/e16-application-runtime.md` §14.

## 7. Drain/join decision

`drain()` and `join()` are **separate, non-conflated** operations:

- `drain()` waits until all admitted tasks are terminal (their `Future<void>` is
  ready). Implemented by driving the scheduler via the existing `Group::await()`
  → `Scheduler::run_live(1, stop_predicate)` path (`group.cpp:41-91`).
- `join()` joins all worker threads. Precondition: `drain()` must have completed.
- Both are idempotent.
- `shutdown()` is the composition: `request_stop()` → `drain()` → `join()`.

Evidence: `docs/design/e16-application-runtime.md` §15-17.

## 8. Destructor decision

**Explicit shutdown required; destructor validates and fail-fast on misuse.**

The destructor checks `state_ ∈ {Stopped, StartFailed, Constructed}`. Any other
state → `std::terminate` (via `detail::runtime_lifetime_fail_fast`).

This matches existing contracts:
- `AsyncIoContext::~AsyncIoContext()` fail-fast (`async_io_context.cpp:30-41`).
- `Group::~Group()` fail-fast (`group.cpp:117-122`).
- `Scheduler::~Scheduler()` asserts quiescence (`scheduler.cpp:169-196`).

Rationale: hidden blocking in a destructor is an anti-pattern (AGENTS.md §7:
"Destructors must not invent unreportable I/O success"). `shutdown()` returns
`Result`, enabling error reporting.

Evidence: `docs/design/e16-application-runtime.md` §18.

## 9. Restartability decision

**One-shot lifecycle.** A Stopped Runtime may not be restarted. Restart requires
backend/Scheduler reconstruction, worker generation, cancellation generations,
stale references — all out of scope for E16.

Evidence: `Scheduler` is non-movable (`scheduler.hpp:216-219`).

## 10. Error-model decision

- Ordinary errors (rejected submit, misuse) → `Result<void>` with
  `IoError::Code::invalid_state`.
- Invariant violations (destructor misuse, quiescence failure) → fail-fast
  (`std::terminate`), matching the existing fail-fast authority
  (`fail_fast.cpp:16-62`).
- `request_stop()` is `noexcept` (idempotent, always succeeds).
- `start()`, `submit()`, `drain()`, `join()`, `shutdown()` return `Result<void>`.

## 11. Public-surface direction

**PROPOSED — NOT AN EXISTING API.**

```cpp
// PROPOSED — NOT AN EXISTING API
namespace sluice::async {

class RuntimeBuilder {
public:
    RuntimeBuilder& backend(std::unique_ptr<AsyncBackend> b);
    RuntimeBuilder& workers(unsigned n);
    Result<ApplicationRuntime> build();
};

class ApplicationRuntime {
public:
    Result<void> start();
    Result<void> submit(TaskFn task);
    void request_stop() noexcept;
    Result<void> drain();
    Result<void> join();
    Result<void> shutdown();
    ~ApplicationRuntime();
};

}  // namespace sluice::async
```

Decisions:
- Builder collects config; `build()` validates and constructs.
- `start()` is a separate, fallible transaction.
- `submit()` returns `Result<void>` (admission rejection).
- `request_stop()` is `noexcept`.
- `drain()`/`join()`/`shutdown()` return `Result<void>`.
- Non-copyable, non-movable.
- No direct accessors to `Scheduler`/`Group`/`AsyncIoContext`/`Backend`
  (default assumption: direct access weakens lifecycle authority).
- Diagnostics via snapshot (not reference).
- Task function receives `CancelToken&` (matching `Group::async` signature,
  `group.hpp:89`).

Evidence: `docs/design/e16-application-runtime.md` §21.

## 12. Consequences

### Positive

- Unified lifecycle closes the gaps in manual composition (§3 of the design).
- Backend injection enables deterministic testing via `FakeAsyncBackend`.
- Construct/start separation prevents use-before-start.
- Transactional `start()` with rollback prevents partial-start leaks.
- Separate drain/join enables independent reasoning about work completion vs.
  thread lifetime.
- Destructor fail-fast prevents silent resource leaks.

### Negative

- One-shot lifecycle means a new Runtime must be built for each application run.
- Builder pattern adds API surface.
- No direct access to `Scheduler`/`Group` may frustrate advanced users who want
  to drive the scheduler manually (they can still do so without the Runtime).

### Risks

- The admission gate is a new atomic linearization point; its correctness depends
  on the memory model. Mitigation: deterministic phase-seam tests (E7 admission
  discipline).
- `drain()` may block indefinitely if a task never observes cancellation.
  Mitigation: this matches existing `Group::await()` semantics
  (`group.hpp:69-76`); documented as a cooperative-cancellation property.

## 13. Rejected alternatives

### Alternative A — Runtime-owned backend

The Runtime creates the backend internally. **Rejected**: cannot inject
deterministic backends for testing; requires internal backend creation hooks
that weaken production guarantees.

Evidence: `docs/design/e16-application-runtime.md` §7.1.

### Alternative B — Backend injected into Runtime (without builder)

Backend injected at construction. **Rejected in favor of C**: lacks
construct/start separation and config validation point.

Evidence: `docs/design/e16-application-runtime.md` §7.2.

## 14. Compatibility impact

- **No impact on existing users.** The Runtime is a new, additive layer above the
  existing foundation. No existing headers, implementations, or public APIs are
  modified.
- The existing `AsyncIoContext`, `Scheduler`, `Group`, `Future`, `Completion`,
  and E10–E13 primitives are unchanged.
- The Runtime depends on the existing `AsyncIoContext(unique_ptr<AsyncBackend>)`
  injection seam (`async_io_context.hpp:121`), which is already public and stable.

## 15. Verification obligations

### Acceptance testing

A real public consumer target covering: construct, start, submit, request_stop,
drain, join, safe destruction. Must not be a unit-test binary renamed "acceptance."

### Unit / component testing

Deterministic tests for: every state transition, every illegal operation,
admission race ordering, startup rollback at every fallible step,
stop/drain/join idempotence, task exception containment, outstanding-I/O shutdown,
destructor misuse.

### Mutation testing

Mutations: allow submit after admission closes; omit root cancellation
publication; return from drain with one admitted task alive; forget to join one
worker; publish Running before startup is committed; omit rollback for Nth failure;
allow destructor with live work; misclassify a losing concurrent submit as admitted.

### Code quality analysis

Clang Debug, Clang Release, GCC Debug, Hardened Release, ASan + UBSan, TSan for
concurrency changes, warnings-as-errors, clang-tidy or equivalent focused analysis.

### AI workflow discipline

Contract before implementation; acceptance scenario before implementation; failing
test before fix; mutation proof; independent adversarial review; no silent API
invention; no broad unrelated refactor.

### Formal model

**MODEL_RECOMMENDED.** The lifecycle state machine (8 states, concurrent
operations, admission linearization) merits a small TLA+ or similar state model.
Variables: `runtime_state`, `admission_open`, `stop_requested`, `admitted_count`,
`terminal_count`, `workers_started`, `workers_joined`, `outstanding_io`.

Evidence: `docs/design/e16-application-runtime.md` §25.

## 16. Open human decisions

| ID | Question | Status |
| --- | --- | --- |
| Q1 | Does internal cleanup work bypass the admission gate? | **OPEN HUMAN DECISION** |
| Q2 | Should `submit()` return a handle/Future for the admitted task? | **OPEN HUMAN DECISION** |
| Q3 | Should the Runtime expose a diagnostics snapshot? | **OPEN HUMAN DECISION** |
| Q4 | Should `drain()` have a deadline? | **OPEN HUMAN DECISION** |
| Q5 | Should the Runtime support Threaded mode in addition to Evented? | **OPEN HUMAN DECISION** |
| Q6 | What is the exact TaskFn signature? | **OPEN HUMAN DECISION** |

## 17. Implementation authorization

```text
E16 production implementation remains unauthorized.
Authorization requires an accepted ADR and an independent design review
with no open P0/P1 or mandatory-contract findings.
```

## 18. References

- Design document: `docs/design/e16-application-runtime.md`
- Execution model ADR: `docs/adr/ADR-execution-model.md`
- Async I/O model ADR: `docs/adr/ADR-async-io-model.md`
- Sync runtime contract ADR: `docs/adr/ADR-024S-sync-runtime-contract.md`
- API reference: `docs/api-reference.md`
- Repository instructions: `AGENTS.md`
