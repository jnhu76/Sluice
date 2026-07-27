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

This is **not** a "zero new seam" layer. The design requires four private PROPOSED
seams (§8) — none public, none authorizing implementation, none a change to
Scheduler *drive semantics*.

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
- three lock-free atomic stop-predicate snapshots
  (`driver_exit_requested`, `task_set_terminal_snapshot`, `fatal_snapshot`)
- a per-Fiber execution-identity tag (set/cleared by the Runtime task wrapper)

The backend is **injected** (not created internally), enabling deterministic test
injection via the existing `AsyncIoContext(unique_ptr<AsyncBackend>)` seam
(`async_io_context.hpp:121`).

The Runtime is non-copyable and non-movable.

**Scheduler worker threads are NOT owned by the Runtime.** They are transient
inside each `run_live` invocation (`scheduler.cpp:565-579`). The Runtime joins
only the driver; that transitively proves Scheduler worker threads were joined
inside the last `run_live`. **Backend worker threads** are joined by the backend
destructor (`threadpool_backend.cpp:23-32`), which the join owner runs during
resource close.

The Runtime **never calls `Group::await()` while the driver exists** — `await()`
would call `run_live(1,...)` itself (`group.cpp:57-72`), violating "only the
driver drives the Scheduler" and forcing single-worker.

Evidence: `docs/design/e16-application-runtime.md` §8.

## 4. Lifecycle decision

The lifecycle is a one-shot state machine:

```text
Constructed ──start()──→ Starting ──commit──→ Running ──request_stop()──→ Stopping
   │                        │                                                    │
   │ remembers stop         ├──construct throw──→ StartFailed              drain()──→ Draining
   │ (stays Constructed)    │                                                    │
   │                        └──stop wins pre-commit──→ Stopped            join()──→ Stopped
   │                                                                           (join + close)
   Any state ──invariant violation──→ Fatal (std::terminate)
```

| State | Meaning |
| --- | --- |
| `Constructed` | Built but not started. Admission closed. No driver. `stop_requested` may be remembered. |
| `Starting` | `start()` in progress. Driver spawned but waiting at startup barrier. |
| `Running` | Started. Admission open (unless stop won at commit). Driver active. |
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

Evidence: `docs/design/e16-application-runtime.md` §10, §11, §19.

## 5. Admission decision

**A task is successfully admitted when its reservation succeeds under
`lifecycle_mutex`: `admission_open` is verified true AND `admitted_count` is
incremented.** The reservation is the linearization point of stop-vs-submit.

`submit()` reserves under `lifecycle_mutex` (admitted_count++,
recompute_task_set_terminal_locked), then calls `root_group.async(...)`. On
throw (all throwable steps precede `Scheduler::spawn`, `group.hpp:264-282`),
`admitted_count--` and recompute run under `lifecycle_mutex` before
`control_epoch++` and dual-wake. On exception the user task body has NOT executed.

Admission opens **only after** the startup commit AND only if
`!stop_requested` at commit. Therefore "admission opens then commit fails / stop
wins" is impossible (commit and admission-open are the same locked compound
transition).

**No structured child submission** in E16 v1. `TaskFn = void(CancelToken&)`
gives no child-spawn capability. External capture of `ApplicationRuntime&` by a
task body is treated as ordinary concurrent external `submit()`. A restricted
`TaskContext` is a future extension.

Evidence: `docs/design/e16-application-runtime.md` §13.

## 6. Cancellation decision

The authoritative Runtime root cancellation state is **`root_group.group_token()`**
(`group.hpp:114`) — the Runtime does NOT create an independent second token.
`request_stop()` publishes root cancellation via `root_group.group_token().request()`.
Cancellation is cooperative (matching the existing model, `cancel.hpp:14`): tasks
observe the token at cancel points (`check_cancel`, `cancel.hpp:147`). Cancellation
is not an unconditional escape hatch — a task that does not observe cancellation
can prevent `drain()` from returning (matching `group.hpp:69-76`).

Evidence: `docs/design/e16-application-runtime.md` §14.

## 7. Drain/join decision

`drain()` and `join()` are **separate, non-conflated** operations.

- `drain()` is legal **only in `Stopping` or `Draining`**; in `Running` it returns
  `invalid_state` (caller must `request_stop()` first, which atomically closes
  admission). It waits on `runtime_cv` until **`drain_complete`** is published.
  `drain_complete` (published by the driver **between** `run_live` invocations) =
  `task_set_terminal_snapshot && AsyncIoContext::outstanding()==0`. The Scheduler
  stop predicate reads ONLY three lock-free atomic snapshots
  (`fatal_snapshot`, `driver_exit_requested`, `task_set_terminal_snapshot`) — it
  never reads `drain_complete` (circular) and never calls `outstanding()` (lock
  hazard). At `drain()` return: all admitted task bodies exited AND no
  outstanding backend op remains.
- `join()` is the **terminal close operation** and is legal only after
  `drain_complete`. An elected join owner (join_state NotStarted→InProgress→
  JoinedAndClosed) joins the driver (transitively proving Scheduler workers
  joined inside the last `run_live`), snapshots diagnostics, destroys
  Group→Scheduler→AsyncIoContext/backend (the backend destructor joins backend
  workers, `threadpool_backend.cpp:23-32`), and publishes `Stopped`. `join()`
  return ⇒ driver joined AND Scheduler workers joined AND backend destroyed AND
  backend workers joined AND Runtime == Stopped.
- Both are idempotent. `shutdown()` composes `request_stop() + drain() + join()`.
  Concurrent `join()`/`shutdown()` callers all route through one join owner;
  others wait on `runtime_cv` for `JoinedAndClosed`.

`drain()`/`join()`/`shutdown()` return `invalid_state` when invoked from a task
owned by the same Runtime (detected via the Fiber-local execution tag, §8).
`request_stop()` is worker-safe.

Evidence: `docs/design/e16-application-runtime.md` §16, §17.

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

Evidence: `docs/design/e16-application-runtime.md` §10, §18.

## 9. Consequences — required private PROPOSED seams

To close the lifecycle authority gaps honestly, the design requires **four
private PROPOSED seams**. **None is a public API; none authorizes implementation;
none is a change to Scheduler *drive semantics*.** Production currently has none
of them.

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
   `terminal_count++` exactly once even if the user `TaskFn` throws (RAII runs on
   unwind).

4. **`recompute_task_set_terminal_locked()`** — invoked under `lifecycle_mutex`
   on every mutation of `admission_open` / `admitted_count` / `terminal_count`.

Evidence: `docs/design/e16-application-runtime.md` §8, §16, §18, §21.4.

## 10. Error-model decision

- Ordinary errors (rejected submit, misuse, wrong-state) → `Result<void>` with
  `IoError::Code::invalid_state` (`error.hpp:20`, EXISTING).
- `start()` interrupted by `request_stop()` during Starting → `Result<void>`
  with `IoError::Code::canceled` (`error.hpp:15`, EXISTING).
- `start()` on an already-Stopped one-shot Runtime → `invalid_state`.
- Invariant violations (destructor misuse, quiescence failure) → fail-fast
  (`std::terminate`), matching the existing fail-fast authority
  (`fail_fast.cpp:16-62`).
- `request_stop()` is `noexcept` (idempotent, legal in all non-Fatal states,
  worker-safe).
- `start()`, `submit()`, `drain()`, `join()`, `shutdown()` return `Result<void>`.

**No new error code is invented.** `IoError::Code` has exactly 8 enumerators
(`error.hpp:14-21`); `canceled` (:15) and `invalid_state` (:20) both exist. The
term "operation_cancelled" does not appear.

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
    Result<void> start();                    // may return canceled / invalid_state
    Result<void> submit(TaskFn task);
    void request_stop() noexcept;
    Result<void> drain();                    // invalid_state if from a Runtime task or in Running
    Result<void> join();                     // terminal close owner; invalid_state if from a Runtime task
    Result<void> shutdown();
    ~ApplicationRuntime();
};

}  // namespace sluice::async
```

Decisions:
- Builder collects config; `build()` validates and constructs.
- `start()` is a separate, fallible transaction (spawns driver).
- `submit()` returns `Result<void>` (admission rejection via reservation+rollback).
- `request_stop()` is `noexcept`, worker-safe, legal in all non-Fatal states.
- `drain()`/`join()`/`shutdown()` return `Result<void>`; return `invalid_state`
  when invoked from a task owned by the same Runtime (Fiber-local tag).
- Non-copyable, non-movable.
- No direct accessors to `Scheduler`/`Group`/`AsyncIoContext`/`Backend`
  (direct access weakens lifecycle authority).
- Diagnostics via snapshot (not reference) — components are destroyed at
  `Stopped`.
- Task function receives `CancelToken&` (matching `Group::async` signature,
  `group.hpp:89`); no child-spawn capability.

Evidence: `docs/design/e16-application-runtime.md` §21.

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
- Join-owner election gives a clean concurrent-shutdown contract.
- Per-state destructor safety + fail-fast prevents silent resource leaks.
- Throw-safe terminal guard prevents a thrown task from blocking drain forever.

### Negative

- One-shot lifecycle means a new Runtime must be built for each application run.
- Builder pattern adds API surface.
- Four new private PROPOSED seams (§9) — honest cost of closing the lifecycle
  authority gaps; not zero-seam.
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

Evidence: `docs/design/e16-application-runtime.md` §7.

### Alternative B — Backend injected into Runtime (without builder)

Backend injected at construction. **Rejected in favor of C**: lacks
construct/start separation and config validation point.

Evidence: `docs/design/e16-application-runtime.md` §7.

### Alternative D — Caller-driven single-worker Runtime

The caller's thread drives `run_live(1, ...)` inline (like `Group::await` today),
no background driver. **Rejected as the default**: makes `start()`
non-operational, removes parallelism, collapses `drain()` into execution. May be
documented as a future deterministic/manual variant.

Evidence: `docs/design/e16-application-runtime.md` §7.6.

### Structured child submission

A restricted `TaskContext` with `spawn()`. **Rejected for E16 v1**: adds API
surface and a structured-concurrency model out of scope for A0. `TaskFn =
void(CancelToken&)` gives no child-spawn capability; the child-admission
contract is removed.

Evidence: `docs/design/e16-application-runtime.md` §13.4.

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
- The four private PROPOSED seams (§9) are implementation details; none is a
  public API change.

## 15. Verification obligations

### Acceptance testing

A real public consumer target covering: construct, start, submit, request_stop,
drain, join, safe destruction. Must not be a unit-test binary renamed "acceptance."
Acceptance contracts A1–A12 in `docs/design/e16-application-runtime.md` §22,
including: normal lifecycle; submit-after-stop; stop-wins-pre-commit / driver
spawn failure; outstanding-I/O drain; concurrent join/shutdown owner election;
destructor misuse; task-throws (terminal guard bridge); outstanding-I/O keeps
driver alive.

### Unit / component testing

Deterministic tests for: every state transition; every illegal operation;
admission race ordering (incl. `Group::async` throw after reservation);
startup rollback at every fallible step; stop/drain/join idempotence; task
exception containment + terminal guard; outstanding-I/O shutdown; driver
re-entry loop; dual-wake lost-wake race (both directions); concurrent
join/shutdown owner election; destructor misuse; worker-blocking-call returns
`invalid_state` via Fiber-local tag.

### Mutation testing

Mutations (killing tests in `docs/design/e16-application-runtime.md` §24),
including: allow submit after admission closes; omit root cancellation
publication; return from drain with one admitted task alive; forget to join the
driver; publish Running before startup committed; omit rollback for stop-pre-commit;
allow destructor with live work; misclassify a losing concurrent submit as
admitted; omit reservation rollback on `Group::async` throw; terminal guard not
noexcept/not exactly-once; omit recompute on rollback; call `outstanding()` in
the stop predicate; `drain()` legal in Running; no join owner election; resource
close omitted in join; omit CV notify only; omit WakeHandle notify only;
admission open before commit; `join()` returns before driver joined; re-entry
loop omitted; `stop_requested` not checked at commit; `Group::size()` used
instead of Runtime counts; outstanding-I/O check omitted; Fiber tag stored in
`thread_local`.

### Code quality analysis

Clang Debug, Clang Release, GCC Debug, Hardened Release, ASan + UBSan, TSan for
concurrency changes, warnings-as-errors, clang-tidy or equivalent focused analysis.

### AI workflow discipline

Contract before implementation; acceptance scenario before implementation; failing
test before fix; mutation proof; independent adversarial review; no silent API
invention; no broad unrelated refactor.

### Formal model

**MODEL_RECOMMENDED.** The lifecycle state machine (8 states, concurrent
operations, admission reservation, driver re-entry, dual wake, join-owner
election) merits a small TLA+ state model with a deliberate negative/broken
model reproducing a known defect (e.g. invocation-boundary lost-wake).

Variables: `runtime_state`, `admission_open`, `stop_requested`, `admitted_count`,
`terminal_count`, `control_epoch`, `driver_state`, `join_state`, `outstanding_io`,
`execution_tag_per_fiber`. The repository has demonstrated capacity for this
(`docs/spec/e7_publication/E7Buggy.tla`, `docs/spec/e9_wake_handle_lifetime/`,
`docs/spec/e13_select/E13SelectContract.tla`; TLC via `tla2tools.jar`,
`scripts/verify-e11-formal.sh` et al. with deliberate Buggy/Neg counterexample
discipline).

Evidence: `docs/design/e16-application-runtime.md` §25.

## 16. Open human decisions

| ID | Question | Status |
| --- | --- | --- |
| Q1 | Does internal cleanup work bypass the admission gate? | **OPEN HUMAN DECISION** |
| Q2 | Should `submit()` return a handle/Future for the admitted task? | **OPEN HUMAN DECISION** |
| Q3 | Should the Runtime expose a diagnostics snapshot? | **OPEN HUMAN DECISION** |
| Q4 | Should `drain()` have a deadline? | **OPEN HUMAN DECISION** |
| Q5 | Should the Runtime support Threaded mode in addition to Evented? | **OPEN HUMAN DECISION** |
| Q6 | What is the exact TaskFn signature beyond `void(CancelToken&)`? | **OPEN HUMAN DECISION** |

Q7 (wake-epoch design) and Q8 (cancellation error code) are **resolved** in this
ADR (§7 control_epoch; §4/§10 `canceled` vs `invalid_state`).

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
