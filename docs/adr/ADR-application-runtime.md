# ADR: Application Runtime Architecture

```text
Status: Accepted
Date: 2026-07-29
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
contract. See `docs/history/implementation-plans/e16-application-runtime.md` §3 for the full problem
statement.

The load-bearing production constraints that shape any solution:

- `Scheduler::run()` / `run_live()` **block the caller**; worker threads are
  created AND joined *inside* the blocking call
  (`scheduler.hpp:241,249`; `scheduler.cpp:495-584, 565-579`). There is **no
  separate worker-start / worker-join API**.
- `Group::await()` drives `run_live` inline on the caller's OS thread
  (`group.cpp:57-72`); `await` and "drive the scheduler" are the same blocking
  call.
- `run_live` may return on QUIESCENT, on MW-S3 without an effective wake source,
  or when the invocation stop predicate returns true
  (`scheduler.cpp:856-857, 891-897`). A single `run_live` invocation cannot be
  assumed to remain resident for a Runtime's lifetime.
- Backend `outstanding_` decrements at **poll reap, not syscall completion**
  (`threadpool_backend.cpp:160-163`). So `outstanding()==0` requires that someone
  has polled/reaped every op — which only a live Scheduler invocation does.

The foundation components are non-movable and have strict lifetime contracts:
- `Scheduler` borrows `AsyncIoContext` (`scheduler.hpp:213`); non-movable
  (`scheduler.hpp:216-219`); destructor asserts quiescence
  (`scheduler.cpp:165-196`).
- `AsyncIoContext` owns its backend (`async_io_context.hpp:121`); move-only
  (`async_io_context.hpp:125-133`); destructor fail-fast if outstanding
  (`async_io_context.cpp:30-41`); public `outstanding()` query
  (`async_io_context.hpp:149`).
- `Group(Scheduler&)` borrows the scheduler (`group.hpp:78`); its `group_token()`
  (`group.hpp:114`) is the cancel authority; destructor fail-fast on pending
  evented tasks (`group.cpp:117-122`).

This ADR decides the architecture of the E16 Application Runtime layer that
closes these gaps.

## 2. Decision

Adopt a **builder-constructed, one-shot, injected-backend Application Runtime
driven by a single dedicated driver thread** that owns the `AsyncIoContext`,
`Scheduler`, root task domain (a root `Group`), root cancellation, and the
driver-thread lifecycle, and exposes a unified
`start / submit / request_stop / drain / join / shutdown` contract.

The Runtime is a layer above the existing foundation. The driver thread is the
only thread the Runtime spawns; it is the only caller of
`Scheduler::run_live(worker_count, stop_predicate, this)` (`scheduler.hpp:264`),
re-entering on a loop because `run_live` returns for reasons other than all-work-done.

This is **not** a "zero new seam" layer. The design requires five private
foundation seams/prerequisites in total (§9) — none public, none authorizing
implementation, none a change to Scheduler *drive semantics*. One (the Group
transactional admission seam, P2-01) was a foundation prerequisite for E16
admission rollback; it is now **SATISFIED** (item 5, §9), so it no longer blocks
E16 implementation. The remaining four are Runtime-side and unauthorized.

## 3. Ownership decision

The Runtime **owns**:
- `AsyncIoContext` (which owns the injected `AsyncBackend`)
- `Scheduler` (borrows the `AsyncIoContext`)
- root `Group` (borrows the `Scheduler`); its `group_token()` IS the Runtime root
  cancel authority — the Runtime does **not** create a second independent token
- a `SchedulerWakeHandle` (Runtime-owned, via `Scheduler::make_wake_handle`,
  `scheduler.hpp:909`)
- `admitted_count` / `terminal_count` (mutex-protected, Runtime-owned — NOT
  `Group::size()`)
- a **single dedicated driver thread** (the ONLY thread the Runtime spawns/joins)
- `lifecycle_mutex` / `runtime_cv` / monotonic `control_epoch`
- `startup_abort_requested` and `root_cancel_published` (lifecycle-protected
  booleans; see §6/§7 — P1-01, P1-03)
- `close_state ∈ {Open, InProgress, Closed}` — the unified terminal-close
  ownership (§7), replacing the prior `join_state`
- three lock-free atomic stop-predicate snapshots
  (`driver_exit_requested`, `task_set_terminal_snapshot`, `fatal_snapshot`)
- a per-Fiber execution-identity tag (set/cleared by the Runtime task wrapper)

The backend is **injected** (not created internally), enabling deterministic test
injection via the existing `AsyncIoContext(unique_ptr<AsyncBackend>)` seam
(`async_io_context.hpp:121`).

The Runtime is non-copyable and non-movable. **Because it is non-movable, it
cannot be returned by value through `Result<T>`** (which moves its value into
storage, `result.hpp:155-189`, and has no in-place immovable-value facility).
`RuntimeBuilder::build()` therefore returns **owned indirection**:
`Result<std::unique_ptr<ApplicationRuntime>>` (P1-02). The stable heap address
also anchors driver-thread captures, the Fiber-local Runtime identity tag, the
lifecycle mutex/CV, and private component pointers.

**Scheduler worker threads are NOT owned by the Runtime.** They are transient
inside each `run_live` invocation (`scheduler.cpp:565-579`). The Runtime joins
only the driver; that transitively proves Scheduler worker threads were joined
inside the last `run_live`. **Backend worker threads** are joined by the backend
destructor (`threadpool_backend.cpp:23-32`), which the join owner runs during
resource close.

The Runtime **never calls `Group::await()` while the driver exists** — `await()`
would call `run_live(1,...)` itself (`group.cpp:57-72`), violating "only the
driver drives the Scheduler" and forcing single-worker.

Evidence: `docs/history/implementation-plans/e16-application-runtime.md` §8.

## 4. Lifecycle decision

The lifecycle is a one-shot state machine:

```text
Constructed ──start()──→ Starting ──commit (if !stop_requested)──→ Running ──request_stop()──→ Stopping
   │                        │                                                       │
   │ remembers stop         ├──driver spawn throw──→ StartFailed               drain()──→ Draining
   │ (stays Constructed)    │                                                       │
   │                        └──stop wins pre-commit (abort path)──→ Stopped   join()/shutdown()──→ Stopped
   │                                                       (close owner: join + close)
   │   shutdown() in Constructed/StartFailed → Stopped (direct close, §7)
   │   shutdown() in any safe state → Stopped (state-dispatched, §7)
   Any state ──invariant violation──→ Fatal (std::terminate)
```

| State | Meaning |
| --- | --- |
| `Constructed` | Built but not started. Admission closed. No driver. `stop_requested` may be remembered. |
| `Starting` | `start()` in progress. Driver spawned but waiting at the startup barrier (predicate: `state != Starting OR startup_abort_requested OR fatal`, P1-03). |
| `Running` | Started. Admission open (only if `!stop_requested` at commit). Driver active. |
| `Stopping` | `request_stop()` committed. Admission closed. Root cancel published. |
| `Draining` | `drain()` in progress. |
| `Stopped` | Driver joined AND all execution resources destroyed. |
| `StartFailed` | `start()` failed. Rolled back. Components may exist; safe to destroy. |
| `Fatal` | Invariant violation. Process terminates. |

**`Stopped` is a resource end-state, not just a state enum.** `Stopped` implies:
driver joined AND all execution resources destroyed (Group/Scheduler/AsyncIoContext/
backend gone; backend workers joined via backend destruction). Every transition
INTO `Stopped` performs resource close first. `request_stop()` in `Constructed`
does NOT transition to `Stopped` — it remembers `stop_requested` and stays
`Constructed`; a later `start()` returns `canceled`. Diagnostics must be
snapshotted before resource close (components do not exist at `Stopped`).

Restart is **not** supported. A Stopped Runtime may not be restarted.

Evidence: `docs/history/implementation-plans/e16-application-runtime.md` §10, §11, §19.

## 5. Admission decision

**A task is successfully admitted when its reservation succeeds under
`lifecycle_mutex`: `admission_open` is verified true AND `admitted_count` is
incremented.** The reservation is the linearization point of stop-vs-submit.

`submit()` reserves under `lifecycle_mutex` (admitted_count++,
recompute_task_set_terminal_locked), then calls `root_group.async(...)`. On
throw (all throwable steps precede `Scheduler::spawn`, `group.hpp:264-282`),
`admitted_count--` and recompute run under `lifecycle_mutex` before
`control_epoch++` and dual-wake. On exception the user task body has NOT executed.

**The Runtime reservation+rollback rolls back Runtime admission accounting
ONLY** (admitted_count, snapshot). **Group internal ownership** (Future/stack/
Fiber vectors) is now all-or-nothing: `Group::async_evented` reserves all three
vectors BEFORE the first `push_back` (`group.hpp:350-368`), so a reserve failure
propagates with no partial task record (P2-01 — SATISFIED; see §9 seam 5 and
design §13.5). The Runtime-level `admitted_count` rollback remains E16 production
work.

Admission opens **only after** the startup commit AND only if
`!stop_requested` at commit. Therefore "admission opens then commit fails / stop
wins" is impossible (commit and admission-open are the same locked compound
transition). An admitted task may continue submitting I/O through its
`RuntimeTaskContext` even after `request_stop()` closes admission, because
per-task I/O progress must be drainable; no new I/O may be submitted after the
task body exits (P1-04, §11).

**No structured child submission** in E16 v1. `RuntimeTaskContext` (§11) exposes
cancellation + I/O submission but **no `spawn`**, so a task cannot spawn
Runtime-owned child tasks. External capture of `ApplicationRuntime&` by a task
body is treated as ordinary concurrent external `submit()`. A restricted
`RuntimeTaskContext::spawn()` is a future extension.

Evidence: `docs/history/implementation-plans/e16-application-runtime.md` §13.

## 6. Cancellation decision

The authoritative Runtime root cancellation state is **`root_group.group_token()`**
(`group.hpp:114`) — the Runtime does NOT create an independent second token.
`request_stop()` publishes root cancellation via `root_group.group_token().request()`.
**In the Running state this publication occurs while `lifecycle_mutex` is held**
(P1-01), so root cancellation cannot race with the resource close that destroys
`root_group` (close owner acquires the same mutex). Calling `CancelToken::request()`
under the mutex is safe: it is `noexcept` (`cancel.hpp:59`), idempotent, performs a
single atomic `release` store (`cancel.hpp:78-80`), and does not acquire Scheduler
locks or invoke user code. Cancellation is cooperative (matching the existing
model, `cancel.hpp:14`): tasks observe the token at cancel points (`check_cancel`,
`cancel.hpp:147`). Cancellation is not an unconditional escape hatch — a task that
does not observe cancellation can prevent `drain()` from returning (matching
`group.hpp:69-76`).

Evidence: `docs/history/implementation-plans/e16-application-runtime.md` §14, §15.

## 7. Drain/join/terminal-close decision

`drain()`, `join()`, and `shutdown()` are **separate, non-conflated** operations
that all funnel terminal close through **one** ownership mechanism.

### Unified terminal-close ownership (P1-05)

There is a single lifecycle close/join ownership mechanism — `close_state ∈
{Open, InProgress, Closed}` (under `lifecycle_mutex`) — that covers **all**
paths that destroy Runtime execution resources: `join()`, `shutdown()`, the
startup-abort close, the StartFailed close, and the Constructed close. The
prior `join_state` (named for the driver join) is renamed to `close_state` so it
truthfully covers every close path, not only the driver join. One caller is
elected close owner (`Open → InProgress`); every other concurrent caller waits
on `runtime_cv` for `Closed`. No two callers ever destroy resources
simultaneously.

### Driver state machine and admission wake (P1-07)

The dedicated driver thread runs one state machine:
`not_started → barrier_wait → in_run_live ↔ between_invocations → drained_wait →
exiting → exited` (design §16.1b). Because `run_live` may return at
QUIESCENT / MW-S3-no-wake / stop-predicate-true boundaries
(`scheduler.cpp:823-918`) without all work done, the driver **parks at each
invocation boundary** (`between_invocations`, and after `drain_complete` in
`drained_wait`) on a **persistent CV predicate** checked under `lifecycle_mutex`:
`driver_exit_requested OR fatal_snapshot OR control_epoch != observed_epoch`.
`observed_epoch` is the value captured before entering the current
`run_live()` invocation. On return, the driver compares it with
`control_epoch` before updating it; a change that raced with Scheduler
termination causes immediate re-entry. Overwriting the entry value first would
lose the only wake for work deferred at the Scheduler's terminal topology
boundary.

The persistent predicate — not the notification — is the liveness authority.
Every control-changing operation (`request_stop()`, **successful `submit()`**,
admission rollback, terminal-guard `terminal_count++`, startup abort/commit,
close-owner `driver_exit_requested`) publishes `control_epoch++` and
`runtime_cv.notify_all()` after releasing `lifecycle_mutex`. **A successful
`submit()` MUST publish `control_epoch++` + dual-wake on its success path**
(design §13.1): `Scheduler::spawn()` only notifies a worker `inbox_cv`
(`scheduler.cpp:419-441`) and does not touch the Runtime CV or epoch, so without
the submit-side epoch++ + wake an admitted task could strand at the invocation
boundary. This closes the driver re-entry / boundary-liveness gap (acceptance
A20).

### drain()

`drain()` is legal **only in `Stopping` or `Draining`**; in `Running` it returns
`invalid_state` (caller must `request_stop()` first, which atomically closes
admission). It waits on `runtime_cv` until **`drain_complete`** is published.
`drain_complete` (published by the driver **between** `run_live` invocations) =
`task_set_terminal_snapshot && AsyncIoContext::outstanding()==0`. The Scheduler
stop predicate reads ONLY three lock-free atomic snapshots
(`fatal_snapshot`, `driver_exit_requested`, `task_set_terminal_snapshot`) — it
never reads `drain_complete` (circular) and never calls `outstanding()` (lock
hazard). At `drain()` return: all admitted task bodies exited AND no
outstanding backend op remains. After publishing `drain_complete`, the driver
**parks** on `runtime_cv` (under `lifecycle_mutex`) until `driver_exit_requested`
/ `fatal` / `control_epoch` change — it does not busy-loop and does not exit
before the close owner requests exit (P2-03).

### join()

`join()` is legal **only after `drain_complete`** and is the post-drain close
path. The elected close owner (`close_state Open → InProgress`) joins the driver
(transitively proving Scheduler workers joined inside the last `run_live`),
snapshots diagnostics, destroys Group→Scheduler→AsyncIoContext/backend (the
backend destructor joins backend workers, `threadpool_backend.cpp:23-32`), and
publishes `Stopped`; `close_state → Closed`. `join()` return ⇒ driver joined AND
Scheduler workers joined AND backend destroyed AND backend workers joined AND
Runtime == Stopped.

### shutdown() (P1-05)

`shutdown()` is **NOT** a simple `request_stop() + drain() + join()` composition
— that composition returns `invalid_state` in `Constructed`/`Starting`. It is a
**state-dispatched** lifecycle operation that elects the close owner and runs
the state-appropriate close path: Constructed/StartFailed → direct component
destruction; Starting → record startup_abort, let the start owner rollback+close;
Running → request_stop+drain+join; Stopping → drain+join; Draining → wait
drain_complete then join+close; Stopped → idempotent. This is why `shutdown()`
must be defined as state-dispatched, not as the three-way composition.

All three are idempotent via `close_state == Closed`.

`drain()`/`join()`/`shutdown()` return `invalid_state` when invoked from a task
owned by the same Runtime (detected via the Fiber-local execution tag, §9).
`request_stop()` is worker-safe.

Evidence: `docs/history/implementation-plans/e16-application-runtime.md` §16, §17.

## 8. Destructor decision

**Explicit shutdown required; destructor validates and fail-fast on misuse.**
Per-state safety:
- `Constructed` / `StartFailed`: destroy components normally (reverse order).
- `Stopped`: no-op (components already destroyed by the join owner).
- Any other state: **fail-fast** via `runtime_lifetime_fail_fast()` (PROPOSED).

The safe-state predicate is not state alone — it is the conjunction of state ∈
{Constructed, StartFailed, Stopped} AND (for Constructed/StartFailed) no joinable
driver AND no active `run_live` AND `admitted==terminal` AND `outstanding()==0`.

This matches existing contracts:
- `AsyncIoContext::~AsyncIoContext()` fail-fast (`async_io_context.cpp:30-41`).
- `Group::~Group()` fail-fast (`group.cpp:117-122`).
- `Scheduler::~Scheduler()` asserts quiescence (`scheduler.cpp:165-196`).

Rationale: hidden blocking in a destructor is an anti-pattern (AGENTS.md §7).
`shutdown()` returns `Result`, enabling error reporting.

Evidence: `docs/history/implementation-plans/e16-application-runtime.md` §10, §18.

## 9. Consequences — required private PROPOSED seams/prerequisites

To close the lifecycle authority gaps honestly, the design requires **five
private foundation seams/prerequisites** in total. **None is a public API; none
authorizes implementation; none is a change to Scheduler *drive semantics*.**
Of these, **one (P2-01, the Group transactional admission seam — item 5 below)
is SATISFIED**; the remaining four (items 1–4) are Runtime-side and remain
PROPOSED/unimplemented.

1. **`sluice::app::detail::runtime_lifetime_fail_fast`** — a private Runtime
   fail-fast entry for the destructor. Production currently has NO generic
   fail-fast; all 7 existing entries are subsystem-bound (`fail_fast.hpp:31-129`).
   Contract: `[[noreturn]] noexcept`, no alloc/lock/IO/format, deterministic
   Debug+Release, → `std::terminate` (matching `fail_fast.cpp:16-62`).

2. **Fiber-local execution-identity seam** — an opaque tag stored **in Fiber
   state** (NOT `thread_local`, which is unsound under Fiber multiplexing: one
   OS worker runs many Fibers and a TLS guard does not follow Fiber
   suspend/resume; `Fiber` has NO existing tag field, `fiber.hpp:60-127`). Read
   via a private `Scheduler/detail::current_execution_tag()` accessor; the
   Runtime task wrapper sets/clears it around user code. Because the tag belongs
   to the Fiber, it survives suspension, resumption, and migration.
   **The ADR explicitly states: NO change to Scheduler drive semantics; ONE
   private Fiber-local identity seam is required for deterministic worker-call
   detection.**

3. **`RuntimeTaskTerminalGuard`** — a `noexcept` RAII guard that publishes
   `terminal_count++` exactly once even if the user `RuntimeTaskFn` throws (RAII
   runs on unwind).

4. **`recompute_task_set_terminal_locked()`** — invoked under `lifecycle_mutex`
   on every mutation of `admission_open` / `admitted_count` / `terminal_count`.

5. **Group transactional admission seam (P2-01) — SATISFIED.** `Group::async_evented`
   now reserves all three vectors (`evented_fibers_`, `evented_stacks_`,
   `futures_`) BEFORE the first `push_back` inside one `mtx_` critical section
   (`group.hpp:350-368`), making one Evented task admission an all-or-nothing
   transaction (selected option B: reserve-all-before-first-push). The three
   `push_back`s are guaranteed not to allocate, and their moved types are
   noexcept-movable (pinned by `static_assert`s, `group.hpp:329-334`). A reserve
   failure propagates `std::bad_alloc` with no partial task record, no
   `Scheduler::spawn`, and the user task does not run. Deterministic failure
   injection at each reserve boundary is covered by
   `tests/group_evented_admission_exception_safety_test.cpp` (confirmed to fail
   on the pre-fix defective code). This foundation prerequisite no longer blocks
   E16 implementation; the remaining four foundation seams (1–4) and the full
   E16 production surface remain unauthorized (design §13.5, §28).

Evidence: `docs/history/implementation-plans/e16-application-runtime.md` §8, §13.5, §16, §18, §21.

## 10. Error-model decision

The Runtime uses a **mixed model** (P2-02): public methods return `Result<void>`
for ordinary/expected failures; invariant violations fail-fast. The mapping is
**deterministic and fixed** (no "implementation-defined"):

| Failure | Result / behavior |
| --- | --- |
| Invalid configuration (`build()`) | `Result` with `IoError::invalid_state` (`error.hpp:20`) |
| Wrong lifecycle state / lifecycle op from a Runtime task | `Result` with `IoError::invalid_state` |
| `request_stop()` interrupts `start()` during Starting | `start()` returns `Result` with `IoError::canceled` (`error.hpp:15`) |
| `std::thread` construction throws `std::system_error` (driver spawn) | `start()` returns `Result` with `IoError::backend_error` (`error.hpp:21`), native code preserved in `os_errno` when representable |
| `Scheduler::init_fiber()` returns false | `submit()` returns `Result` with `IoError::backend_error` |
| Allocation failure (`std::bad_alloc`) | **propagated as `std::bad_alloc`** — NOT mapped to `invalid_state` (no accepted global no-throw allocation policy; existing code lets `bad_alloc` propagate) |
| User `RuntimeTaskFn` throws | Swallowed at the Group boundary (`group.hpp:251-256`); terminal guard still fires; NOT returned by `submit()` |
| Backend I/O result | Existing `Completion<T>`/backend result contract, surfaced through the task-owned Completion |
| Invariant violation (destructor misuse, quiescence, double-close) | fail-fast (`std::terminate`) — NOT a `Result` |

`start()`, `submit()`, `drain()`, `join()`, `shutdown()` return `Result<void>`.
`request_stop()` is `noexcept` (idempotent, legal in all non-Fatal states,
worker-safe). `start()` on an already-Stopped one-shot Runtime → `invalid_state`.

**`std::bad_alloc` is NOT silently mapped to `invalid_state`.** If the project
later adopts a global no-throw allocation policy, that row would change — but
that would be an **OPEN HUMAN DECISION that blocks ADR acceptance** until
resolved, not a silent implementation choice.

**No new error code is invented.** `IoError::Code` has exactly 8 enumerators
(`error.hpp:14-21`); `canceled` (:15) and `invalid_state` (:20) both exist.

## 11. Public-surface direction

**PROPOSED — NOT AN EXISTING API.**

```cpp
// PROPOSED — NOT AN EXISTING API
namespace sluice::async {

// Restricted, non-owning task execution context (P1-04). Valid only during one
// RuntimeTaskFn invocation; delegates I/O to the Runtime-owned AsyncIoContext.
class RuntimeTaskContext {
public:
    CancelToken& cancel_token() noexcept;
    Result<void> submit_read     (ReadOp op, Completion<std::size_t>& completion);
    Result<void> submit_write    (WriteOp op, Completion<std::size_t>& completion);
    Result<void> submit_sync_data(SyncDataOp op, Completion<void>& completion);
    Result<void> submit_sync_all (SyncAllOp op, Completion<void>& completion);
    RuntimeTaskContext(const RuntimeTaskContext&) = delete;
    RuntimeTaskContext& operator=(const RuntimeTaskContext&) = delete;
};
using RuntimeTaskFn = std::function<void(RuntimeTaskContext&)>;

class RuntimeBuilder {
public:
    RuntimeBuilder& backend(std::unique_ptr<AsyncBackend> b);
    RuntimeBuilder& workers(unsigned n);
    // P1-02: owned indirection. ApplicationRuntime is non-movable, so it cannot
    // be returned by value through Result<T> (result.hpp:155-189).
    Result<std::unique_ptr<ApplicationRuntime>> build();
};

class ApplicationRuntime {
public:
    Result<void> start();                    // may return canceled / invalid_state
    Result<void> submit(RuntimeTaskFn task); // admission-gated; invalid_state if closed
    void request_stop() noexcept;
    Result<void> drain();                    // invalid_state if from a Runtime task or in Running
    Result<void> join();                     // post-drain close; invalid_state if from a Runtime task
    Result<void> shutdown();                 // state-dispatched lifecycle op (§7)
    ~ApplicationRuntime();
};

}  // namespace sluice::async
```

Decisions:
- Builder collects config; `build()` validates, constructs on the heap, and
  returns **`Result<std::unique_ptr<ApplicationRuntime>>`** (P1-02).
- `start()` is a separate, fallible transaction (spawns driver; startup barrier
  with abort path, P1-03).
- `submit()` takes a `RuntimeTaskFn(RuntimeTaskContext&)` (P1-04) and returns
  `Result<void>` (admission rejection via reservation+rollback).
- `request_stop()` is `noexcept`, worker-safe, legal in all non-Fatal states;
  publishes root cancellation under `lifecycle_mutex` in Running (P1-01).
- `drain()`/`join()`/`shutdown()` return `Result<void>`; return `invalid_state`
  when invoked from a task owned by the same Runtime (Fiber-local tag).
  `shutdown()` is **state-dispatched**, correct in every safe state (P1-05).
- Non-copyable, non-movable.
- No direct accessors to `Scheduler`/`Group`/`AsyncIoContext`/`Backend`
  (direct access weakens lifecycle authority); tasks reach I/O only through
  `RuntimeTaskContext`.
- Diagnostics via snapshot (not reference) — components are destroyed at
  `Stopped`.
- Task function receives `RuntimeTaskContext&` (exposes `cancel_token()` + I/O
  submission; no `spawn`) — resolves Q6.

Evidence: `docs/history/implementation-plans/e16-application-runtime.md` §21.

## 12. Consequences

### Positive

- Unified lifecycle closes the gaps in manual composition (§3 of the design).
- Backend injection enables deterministic testing via `FakeAsyncBackend`.
- Construct/start separation prevents use-before-start.
- Transactional `start()` with rollback prevents partial-start leaks; the driver
  thread is the real background resource whose partial-spawn rolls back.
- Dedicated driver + re-entry loop makes `drain()`/`join()` genuinely separate
  and independently testable.
- `drain_complete = task terminal AND outstanding()==0` closes the
  "looks-stopped-but-I/O-still-outstanding" trap.
- Unified `close_state` ownership (P1-05) gives a clean concurrent-shutdown
  contract across join, shutdown, startup-abort, StartFailed, and Constructed.
- Per-state destructor safety + fail-fast prevents silent resource leaks.
- Throw-safe terminal guard prevents a thrown task from blocking drain forever.
- Root cancellation published under `lifecycle_mutex` (P1-01) closes the
  request_stop-vs-close use-after-free.
- `RuntimeTaskContext` (P1-04) gives tasks a real I/O capability without
  exposing internals.
- The unified driver FSM with a persistent CV predicate + success-admission
  `control_epoch++`/dual-wake (P1-07) guarantees invocation-boundary liveness:
  an admitted task is never stranded and the post-drain driver neither spins nor
  exits prematurely.

### Negative

- One-shot lifecycle means a new Runtime must be built for each application run.
- Builder pattern adds API surface.
- **Five** new private PROPOSED foundation seams/prerequisites (§9) — honest cost
  of closing the lifecycle authority gaps; not zero-seam. One of them (the Group
  transactional admission seam, P2-01) is a foundation prerequisite that has now
  been SATISFIED (§9 seam 5); the remaining four are Runtime-side and remain
  E16 production work.
- Owned-indirection return type (`unique_ptr`, P1-02) adds one heap allocation
  and an indirection; accepted because the Runtime is non-movable and the stable
  address anchors driver captures and the Fiber-local tag.
- Components are destroyed at `Stopped`, so no post-Stop diagnostics by reference
  (must snapshot before close).
- No direct access to `Scheduler`/`Group` may frustrate advanced users who want
  to drive the scheduler manually (they can still do so without the Runtime).

### Risks

- The dual-wake protocol (release-then-notify) and driver re-entry loop have a
  lost-wake surface at the invocation boundary. Mitigation: correctness comes
  from the persistent state/control-epoch predicate (not atomic notifications);
  atomic snapshots for the stop predicate; covered by a TLA+ model with a
  negative counterexample (§15).
- The Fiber-local execution tag must be saved/restored correctly across Fiber
  context switches. Mitigation: the tag is stored IN Fiber state (not
  `thread_local`), so it is inherently correct under multiplexing.
- `drain()` may block indefinitely if a task never observes cancellation. This
  matches existing `Group::await()` semantics (`group.hpp:69-76`); documented as
  a cooperative-cancellation property.

## 13. Rejected alternatives

### Alternative A — Runtime-owned backend

The Runtime creates the backend internally. **Rejected**: cannot inject
deterministic backends for testing; requires internal backend creation hooks
that weaken production guarantees.

Evidence: `docs/history/implementation-plans/e16-application-runtime.md` §7.

### Alternative B — Backend injected into Runtime (without builder)

Backend injected at construction. **Rejected in favor of C**: lacks
construct/start separation and config validation point.

Evidence: `docs/history/implementation-plans/e16-application-runtime.md` §7.

### Alternative D — Caller-driven single-worker Runtime

The caller's thread drives `run_live(1, ...)` inline (like `Group::await` today),
no background driver. **Rejected as the default**: makes `start()`
non-operational, removes parallelism, collapses `drain()` into execution. May be
documented as a future deterministic/manual variant.

Evidence: `docs/history/implementation-plans/e16-application-runtime.md` §7.6.

### Structured child submission

A `RuntimeTaskContext::spawn()`. **Rejected for E16 v1**: adds API surface and a
structured-concurrency model out of scope for A0. `RuntimeTaskContext` exposes
cancellation + I/O submission but no `spawn` (P1-04); the child-admission
contract is removed. A future `spawn()` is a possible extension.

Evidence: `docs/history/implementation-plans/e16-application-runtime.md` §13.4, §21.5.

### Ordinary `thread_local` for worker-call detection

**Rejected**: unsound under Fiber multiplexing (one OS worker runs many Fibers;
a TLS guard bound to the C++ call stack does not follow Fiber context switch).
Replaced by the Fiber-local execution-identity seam (§9).

## 14. Compatibility impact

- **No impact on existing users.** The Runtime is a new, additive layer above the
  existing foundation. No existing headers, implementations, or public APIs are
  modified.
- The existing `AsyncIoContext`, `Scheduler`, `Group`, `Future`, `Completion`,
  and E10–E13 primitives are unchanged in their public contracts.
- The Runtime depends on the existing `AsyncIoContext(unique_ptr<AsyncBackend>)`
  injection seam (`async_io_context.hpp:121`), the existing
  `AsyncIoContext::outstanding()` query (`async_io_context.hpp:149`), and the
  existing `Scheduler::make_wake_handle()` / `run_live(worker_count, stop_fn,
  stop_ctx)` API (`scheduler.hpp:264,909`).
- The five private foundation seams/prerequisites (§9) are implementation
  details / prerequisites; none is a public API change. The Group transactional
  admission seam (P2-01) is a private `Group::async_evented` change, not a
  public API change, and is **SATISFIED** (item 5, §9); the other four remain
  PROPOSED.

## 15. Verification obligations

### Acceptance testing

A real public consumer target covering: construct, start, submit, request_stop,
drain, join, safe destruction. Must not be a unit-test binary renamed "acceptance."
Acceptance contracts A1–A20 in `docs/history/implementation-plans/e16-application-runtime.md` §22,
including: normal lifecycle; submit-after-stop; stop-wins-pre-commit / driver
spawn failure; outstanding-I/O drain; concurrent join/shutdown owner election;
destructor misuse; task-throws (terminal guard bridge); outstanding-I/O keeps
driver alive; **stop publication vs. close (A13, P1-01); non-movable ownership
(A14, P1-02); stop-before-commit barrier (A15, P1-03); Runtime task performs I/O
(A16, P1-04); shutdown in every safe state (A17, P1-05); Group task-record
insertion failure (A18, P2-01); post-drain driver park (A19, P2-03);
invocation-boundary liveness on successful admission (A20, P1-07)**.

### Unit / component testing

Deterministic tests (driven by the private phase seams of design §23.11 — no
wall-clock sleeps) for: every state transition; every illegal operation;
admission race ordering (incl. `Group::async` throw after reservation);
startup rollback at every fallible step (incl. stop-before-commit barrier abort,
P1-03); stop/drain/join/shutdown idempotence; task exception containment +
terminal guard; outstanding-I/O shutdown; driver re-entry loop;
**post-drain driver park**; **invocation-boundary liveness on successful
admission (epoch++ + dual-wake, P1-07)**; dual-wake lost-wake race (both
directions); concurrent join/shutdown owner election (**one close owner across
all close paths**); destructor misuse; worker-blocking-call returns
`invalid_state` via Fiber-local tag; **shutdown state-dispatched close in
Constructed/Starting/StartFailed**; **`unique_ptr` ownership return**.

### Mutation testing

Mutations (killing tests in `docs/history/implementation-plans/e16-application-runtime.md` §24),
including: allow submit after admission closes; omit root cancellation
publication; **publish state=Stopping before root cancellation then allow close
(P1-01)**; return from drain with one admitted task alive; forget to join the
driver; publish Running before startup committed; omit rollback for stop-pre-commit;
**omit startup-barrier abort wake / driver enters run_live after stop won
pre-commit (P1-03)**; allow destructor with live work; misclassify a losing
concurrent submit as admitted; omit reservation rollback on `Group::async` throw;
terminal guard not noexcept/not exactly-once; omit recompute on rollback; call
`outstanding()` in the stop predicate; `drain()` legal in Running; no close owner
election; resource close omitted in join/close; omit CV notify only; omit
WakeHandle notify only; admission open before commit; `join()` returns before
driver joined; re-entry loop omitted; `stop_requested` not checked at commit;
`Group::size()` used instead of Runtime counts; outstanding-I/O check omitted;
Fiber tag stored in `thread_local`; **return ApplicationRuntime by value despite
non-movable type (P1-02)**; **RuntimeTaskContext lacks I/O submission (P1-04)**;
**shutdown in Constructed calls drain()+join() and returns invalid_state (P1-05)**;
**two shutdown() callers both destroy components (P1-05)**; **Group
second/third task-record insertion throws after earlier insertion (P2-01)**;
**map `std::bad_alloc` to `invalid_state` (P2-02, forbidden)**; **driver
busy-loops after drain_complete / exits immediately at drain_complete (P2-03)**;
**successful `submit()` omits `control_epoch++` / Runtime-CV notify (P1-07)**;
**driver has no `between_invocations` park, or parks on notification timing
instead of the persistent epoch predicate (P1-07)**.

### Code quality analysis

Clang Debug, Clang Release, GCC Debug, Hardened Release, ASan + UBSan, TSan for
concurrency changes, warnings-as-errors, clang-tidy or equivalent focused analysis.

### AI workflow discipline

Contract before implementation; acceptance scenario before implementation; failing
test before fix; mutation proof; independent adversarial review; no silent API
invention; no broad unrelated refactor.

### Formal model

**MODEL_REQUIRED.** The lifecycle state machine (8 states, concurrent
operations, admission reservation, driver re-entry, dual wake, unified
terminal-close owner election) requires a small TLA+ state model with a
deliberate negative/broken model reproducing a known defect (e.g.
invocation-boundary lost-wake, or stop-vs-close use-after-free). A formal model
is a **required ADR-acceptance prerequisite**, not a recommendation (P2-03).

**Required before ADR acceptance:**
```text
1. A passing E16 Runtime lifecycle TLA+ model.
2. At least one deliberate buggy/negative model.
3. A TLC counterexample for a known broken transition.
4. Final model transitions match the design and ADR.
```

Variables: `runtime_state`, `admission_open`, `stop_requested`,
`root_cancel_published`, `startup_abort_requested`, `admitted_count`,
`terminal_count`, `control_epoch`, `driver_state` ∈ {not_started, barrier_wait,
in_run_live, between_invocations, drained_wait, exiting, exited}, `close_state`
∈ {Open, InProgress, Closed}, `resources_alive`, `runtime_task_io_outstanding`,
`outstanding_io`, `successful_submit_published`, `execution_tag_per_fiber`. Key
invariants:
`resources_alive == false ⇒ root_cancel publication cannot access Group`;
`Stopped ⇒ resources_alive == false ∧ close_state == Closed`;
`startup_abort_requested ⇒ driver never enters run_live`;
`successful submit() ⇒ control_epoch++ ∧ dual-wake` (P1-07);
`driver_state ∈ {between_invocations, drained_wait} ⇒ driver parks on the
persistent predicate (driver_exit_requested ∨ fatal_snapshot ∨
control_epoch != observed_epoch) and never busy-re-enters without an epoch
change` (P1-07/P2-03).
The repository has demonstrated capacity for this
(`spec/tla/e7_publication/E7Buggy.tla`, `spec/tla/e9_wake_handle_lifetime/`,
`spec/tla/e13_select/E13SelectContract.tla`; TLC via `tla2tools.jar`,
`scripts/formal/verify-timer-wait.sh` et al. with deliberate Buggy/Neg counterexample
discipline). **ADR acceptance remains blocked until the required model exists and
passes.**

Evidence: `docs/history/implementation-plans/e16-application-runtime.md` §25.

## 16. Open human decisions

| ID | Question | Status |
| --- | --- | --- |
| Q1 | Does internal cleanup work bypass the admission gate? | **OPEN HUMAN DECISION** |
| Q2 | Should `submit()` return a handle/Future for the admitted task? | **OPEN HUMAN DECISION** |
| Q3 | Should the Runtime expose a diagnostics snapshot? | **OPEN HUMAN DECISION** |
| Q4 | Should `drain()` have a deadline? | **OPEN HUMAN DECISION** |
| Q5 | Should the Runtime support Threaded mode in addition to Evented? | **OPEN HUMAN DECISION** |

Q6 (task I/O capability) is **resolved** in this ADR (§11 `RuntimeTaskContext`;
`RuntimeTaskFn = void(RuntimeTaskContext&)`). Q7 (wake-epoch design) and Q8
(cancellation error code) are **resolved** in this ADR (§7 control_epoch; §4/§10
`canceled` vs `invalid_state`).

**No implementation-blocking contract gap may remain open.** The remaining Q1–Q5
are optional product decisions. Resolved (no longer open):
- Runtime ownership return type → `Result<unique_ptr<ApplicationRuntime>>` (P1-02)
- Task I/O capability → `RuntimeTaskContext` (P1-04)
- stop/start barrier behavior → startup barrier + abort path (P1-03)
- shutdown behavior by state → state-dispatched (P1-05)
- terminal-close ownership → unified `close_state` (P1-05)
- successful-submit wake → epoch++ + dual-wake (P1-07)
- error mapping for documented failures → fixed table (§10) (P2-02)
- Group transactional admission prerequisite → SATISFIED (§9 seam 5) (P2-01)
- formal-model requirement → MODEL_REQUIRED (§15) (P2-03)
- deterministic test seams → design §23.11 (P2-02)

## 17. Implementation authorization

```text
E16 production implementation is authorized.
Prerequisites satisfied:
  - ADR accepted (2026-07-29);
  - TLA+ lifecycle model passing (spec/tla/e16_application_runtime/):
    safety (Inv1-Inv23), liveness (Live5), wide-domain, 4 negative CEX,
    16 reachability witnesses;
  - verifier: scripts/formal/verify-e16-application-runtime.sh;
  - manifest entry: spec/tla/manifest.json (e16-application-runtime).
```

## 18. References

- Design document: `docs/history/implementation-plans/e16-application-runtime.md`
- Execution model ADR: `docs/adr/ADR-execution-model.md`
- Async I/O model ADR: `docs/adr/ADR-async-io-model.md`
- Sync runtime contract ADR: `docs/adr/ADR-024S-sync-runtime-contract.md`
- API reference: `docs/reference/api.md`
- Repository instructions: `AGENTS.md`
