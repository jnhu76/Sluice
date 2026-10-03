# ADR-0003: Narrow Stackful File-I/O Host (v1 optional profile, D2)

- **Status**: PROPOSED (Issue #399, D2 slice)
- **Parent requirements**: PROD-02 (optional single-owner stackful adapter), PROD-03,
  ARCH-01/ARCH-02, INV-01 (host completed-return row), HOST-01, HOST-02, HOST-03,
  OBS-01/OBS-04, PROG-01/PROG-02/PROG-04, CANCEL-01/CANCEL-02, THREAD-01/THREAD-02,
  BOUND-01/BOUND-02, W-04, MIG-02 (ApplicationRuntime/Scheduler/Fiber disposition)
- **Baseline**: `79153cac28acde0b29af250850cd8169899e3a9e` (live master at slice start)
- **Decision boundary**: this ADR freezes only choices the root delegates under ADR-01
  ("Narrow host scheduler representation — optional dependency, bounded lifetime,
  supported cleanup paths") and HOST-01 ("names/representation belong in a derived
  ADR"). It creates no new public semantic beyond the root.

## 1. Decision: SUPPORT_W04

The D2-0 gate examined whether a conforming host can be retained without general
task-runtime semantics, a second progress authority, Request ownership changes,
unbounded resources, #400/#401 semantics, or a root amendment. All blocking
conditions are absent because every contract the host needs is already frozen:

- public `Request<T>` (B2): unique settlement responsibility, `ready()`/`try_result()`/
  `take_result()`/`cancel()`/`discard()`, always-on nonterminal-release fail-fast;
- context-owned `ProgressSource` with `claim_progress_owner()` / `poll_progress()` /
  `wait_one()` / `acknowledge_progress_control()` / `interrupt_progress_waiters()`
  (C2), one fixed owner, no live transfer, no nested polling (PROG-01);
- the host-neutral fiber substrate (`Fiber` + `fiber_ctx`), which has no IoContext
  dependency and carries ASan/TSan fiber annotations.

Therefore v1 ships the optional stackful host as a **supported** profile passing
W-04, implemented as a new narrow component. The legacy
`ApplicationRuntime`/`Scheduler`/`Group`/`Future`/`WaitPolicy` stack is not the
supported host and is dispositioned in §8.

## 2. Supported build profile and single-owner model

- The host is `sluice::async::StackfulIoHost` over an application-owned
  `AsyncIoContext`. It exists only in `sluice_async`; nothing in
  `sluice_core`, `RequestCore`, `ProgressSource`, or either backend includes or
  references it (ARCH-01 firewall; core-only builds stay independent).
- **Caller-driven, no hidden thread.** `StackfulIoHost::run()` is invoked on the
  host thread. It claims the context's `ProgressOwner` for the duration of the
  call and releases it on every exit. `run()` returns when every spawned task has
  retired. The host never spawns a thread, never falls back to thread-per-task,
  and has no second driver under any condition (PROG-01). A `run()` call on a
  context whose owner is already claimed (external loop, `RequestScope`, another
  host/drive) fails with `invalid_state` before any task executes.
- All host mutation APIs other than `request_stop()` (`spawn`, `run`) are
  host-thread-only: a caller obligation, not a policed precondition (the only
  runtime guard rejects cross-host reentry from inside a task).
  `request_stop()` is callable from any thread: it publishes a
  stop flag, requests the single host-level stop token that every task's
  `IoTaskContext` observes, and wakes a parked driver through
  `interrupt_progress_waiters()` (THREAD-01: other threads may request stop).
  The host-owned latched flag (`stop_requested_`) is the stop authority the
  await precheck and spawn admission read; the token is the task-visible
  projection (exposed read-only, because a public `clear()`/`rearm()` would
  let one task erase the stop observation for the others). A reentrant
  `run()` (from inside a task or on the same thread while a `run()` is
  active) is rejected with `invalid_state` by the host's drive guard before
  any task executes.

## 3. Task capacity, stacks, and bounds

| Resource | Bound | Exhaustion behavior |
|---|---|---|
| Task slots | `task_capacity` (config, ≥ 1), fixed array allocated at construction | `spawn` returns `no_space`; no growth |
| Stacks | `stack_bytes` per task (config, ≥ 64 KiB), allocated at construction, one per slot for the host's lifetime | setup error below the minimum |
| Ready queue | ring of `task_capacity` task indices; a task occupies at most one position (fiber state machine rejects double-ready) | structurally bounded |
| Wake/await state | one await link per suspension, at most one live per task slot, task-lifetime storage; no allocation after `run()` starts | none possible |
| Timers | none | n/a |
| Host-local request state | the awaited `Request<T>` lives in the task's await frame (task stack); at most one per task at any instant | n/a |

Construction (`StackfulIoHost::create`) validates `task_capacity ≥ 1`,
`stack_bytes ≥ 65536`, architecture support (`fiber_ctx::supported`), the
context's split-wait capability (a backend that cannot signal physical
progress cannot drive the park protocol; it is rejected at construction with
`not_supported` rather than dead-ending `run()` after the first suspension),
and `fiber_ctx::init_context` success for every slot, rolling back partial
setup on failure (BOUND-01). No allocation happens inside `run()` or inside
the await helpers after task admission: the only post-`spawn()` dynamic work
is the caller's own code.

## 4. Task launch, join, error propagation, stop

- `spawn(std::function<void(IoTaskContext&)>)` admits one task into a free slot.
  The callable is fully constructed before the slot commits (admission is
  transactional: a throwing callable construction leaves no slot taken). Host
  stop closes spawn admission (`canceled`). Tasks may spawn successor tasks from
  inside a running task (same thread), bounded by the same capacity.
- `run()` drives until all tasks have retired and returns the mapped first task
  error, or success. A task exception is captured by the task wrapper at the
  host boundary only after the task owns no accepted request (HOST-03); it is
  translated through the shared error mapping (`std::system_error` errno mapping,
  `bad_alloc` → `no_space`, other → `backend_error`, matching the existing
  `translate_task_exception` table). Stop-initiated early task exits are not
  errors.
- `request_stop()`: idempotent. Requests the single host-level stop token, so
  cooperative tasks observe stop at await boundaries and in their own loops,
  closes host spawn admission, and wakes the driver. Stop never destroys a
  suspended stack, never settles a request by fiat, and never implies physical
  cancellation (CANCEL-01 `requested` at most).

## 5. The await protocol (host completed-return helper)

`IoTaskContext` exposes completed-return operations over the four admitted
request kinds — `read` / `write` / `sync_data` / `sync_all`, each with an
optional-deadline form. One call owns at most one `Request<T>`:

```text
precheck host stop                     → return canceled (no acceptance)
r = ctx.submit_<op>(op)                → rejection returns verbatim (no acceptance)
link.request = &r; link.deadline=…; task.await_link = &link   (no-throw, stack storage)
suspend fiber
   [driver: poll_progress → wake when r.ready(); stop → r.cancel() once;
    deadline expiry → r.cancel() once]
resume: task.await_link = nullptr; obs = r.take_result()
return obs result                      (borrow ended at acquired publication)
```

- The wake link is written before the fiber suspends and is task-lifetime
  storage, so **no fallible step exists between acceptance and suspension**
  (OBS-04 reserve-before-acceptance arm; this structurally removes the
  post-acceptance observation-failure mode). The driver's `cancel()` on a
  suspended task's request is a THREAD-01-legal cross-thread cancel of a
  quiescent handle.
- The deadline bounds initial waiting only (CANCEL-02). On expiry the helper
  initiates best-effort cancel and keeps the task suspended until publication;
  it returns the operation's honest terminal result (data or `canceled`) — never
  a fictitious timeout success, and never a returned Request.
- Wake routing uses **no observer registration**: the driver polls each
  suspended task's `Request::ready()` after every progress pass (OBS-01 polling
  needs no allocation and no core state). One accepted request has one wake
  link; the fiber state machine admits exactly one runnable transition per
  suspension, so one obligation yields at most one logical resume. No-lost-wake
  follows from the C2-verified `wait_one` protocol plus the post-pass
  readiness sweep: any publication produces a progress signal, `wait_one`
  returns, and the sweep observes readiness.

## 6. Settlement, unwind, and teardown ordering (HOST-03)

Task retirement requires: no live await link, the awaited request (if any)
consumed at acquired publication. Because a task can hold an accepted request
only inside one helper call, and every helper exit path consumes or settles it:

```text
task throws (between awaits — owns nothing)
    ↓
wrapper captures exception → slot retired → stack reusable
```

```text
stop/deadline while suspended
    ↓
driver best-effort cancels → publication follows (L1) → task resumes
    ↓
helper takes/discards terminal result (settlement; borrow ends)
    ↓
task observes token, unwinds/returns → wrapper retires slot
    ↓
last: stack storage reused only via a later spawn
```

The host destructor fails fast if any task has not retired (spawned but never
driven to retirement) — mirroring the D1/legacy teardown diagnostics. Driving
settlement inside a destructor would require a driver contract the destructor
cannot assume (SHUT obligations belong to the application's driving sequence).

## 7. Progress ownership composition (RequestScope adjudication)

The D1 `RequestScope` is owned-driver only and keeps that meaning. A
`RequestScope` constructed on a host-driven context fails its `claim_progress_
owner()` with `invalid_state` (the host thread holds the owner during `run()`;
outside `run()` the scope works normally — the two never overlap). Inside host
tasks, task-local settlement is the host helper itself (§5): a **host-local
scoped responsibility mechanism built directly over `Request<T>`**, which is
what HOST-03/OBS-04 already specify for completed-return adapters. No
delegated-driver mode, borrowed-owner mode, or `RequestScope` semantics change
is introduced. This resolves the D2 composition question as permitted by the
root without amendment.

## 8. Legacy runtime disposition (survival audit)

| Mechanism | v1 disposition after this slice |
|---|---|
| `Fiber`, `fiber_ctx` | RETAINED as the shared stackful substrate (also used by the legacy scheduler) |
| `StackfulIoHost` / `IoTaskContext` (new) | SUPPORTED optional host (this ADR) |
| `ApplicationRuntime` / `RuntimeBuilder` / `RuntimeTaskContext` | COMPATIBILITY_ONLY: still compiles and serves existing apps/tests; no v1 support claim; retirement with #402 after consumer migration |
| `Scheduler` (multi-worker run, work stealing, primitive-suite methods, timer/select machinery, WaitRecord registry) | COMPATIBILITY_ONLY, same boundary; the supported host uses none of it |
| `Group` (thread-per-task fallback, unbounded admission), `Future`, `WaitPolicy`/`EventedWaitPolicy` | COMPATIBILITY_ONLY; forbidden patterns for the supported host; retirement with #402 |
| public `Event`/`Semaphore`/`Mutex`/`Condition`/`RwLock`/`Queue`/`select` | unchanged code, outside v1 (HOST-01); classified for #402 |
| `await_take`/`await_drain`/`await_read_*`/`await_write_exact` (Completion-based) | COMPATIBILITY_ONLY (D1 classification); the supported forms are the `IoTaskContext` helpers |

Nothing is deleted in this slice: existing consumers (four apps, the
`run_task_to_result` test family, `runtime_waiter_observer_test`) keep
compiling unchanged. Physical retirement is #402's consumer-audit work; this
ADR records that the legacy stack carries no v1 host authority once the narrow
host lands.

## 9. Explicitly rejected for the supported host

General async runtime semantics; multi-worker execution (the host is
single-driver); the public runtime-aware synchronization suite; transparent
async open/close/resize; a second progress owner or driver handoff; backend or
core knowledge of host/fiber identity; timeout-as-cancellation; bounded-stop
guarantees (stop converges under the L1/L4 environment assumptions; a stalled
filesystem is outside REQ-06); recoverable poison handling during host driving
(health failure during `run()` fails fast — the documented boundary until #401
owns a recoverable form, same as D1).

## 10. Blocking operation boundary

`File::open`, explicit close and `resize` are **outside the supported task
region**: the host offers no task-facing form of them and documents that they
must be performed by the application on the host thread outside `run()` or
before/after task execution (W-04 second arm). The task surface is exactly the
four admitted request operations. `file_info`/`size` request forms stay with
the #400 operation matrix.

## 11. Validation obligations

W-04 tracer (sequential read-fill → write → sync chain, ThreadPool and a
successfully constructed io_uring); the HOST-03 matrix H1–H6 (task failure while
another task's I/O is outstanding; stop during suspension; post-acceptance
observation-failure structural elimination with mutation discrimination; wait
timeout with settlement; exception during resumed processing; delivery/stop
races with retirement); progress-owner composition evidence (second claim,
`RequestScope` on host context, re-claim after `run()`); bound evidence
(capacity rejection, fixed stacks); mutation kills M1–M6 (recorded in the
v1 conformance ledger D2 entry); ASan/TSan including fiber switches; core-only
probes unchanged.

## 12. Known limitations

Plain stacks without guard pages (same as the legacy fiber substrate); x86_64
Linux only (`fiber_ctx::supported`, explicit setup failure elsewhere); stop
latency is bounded by cooperative tasks plus physical completion (no operation
preemption); a task that ignores its token and never awaits can defer host
exit indefinitely (documented cooperative rule, HOST-01); deadline forms are
cancel-hinted waits, not timeout returns (CANCEL-02); health failure during
`run()` fails fast rather than returning an error result.
