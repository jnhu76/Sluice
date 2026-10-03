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
  `request_stop()` is callable from any thread: it publishes a stop flag and
  requests the single host-level stop token that every task's
  `IoTaskContext` observes (THREAD-01: other threads may request stop). It
  writes nothing into the context's control plane — the host plants no
  progress-source interrupt, so a stop that lands while the driver is not in
  a wait leaves no unacknowledged control behind for the next owner.
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
| Task payload bytes | caller-dependent (`std::function` target size); the host bounds the retained instance count at `task_capacity`, not the byte size | count-bounded, not byte-bounded |

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
- `request_stop()`: idempotent. Requests the single host-level stop token,
  so cooperative tasks observe stop at await boundaries and in their own
  loops, and closes host spawn admission (`canceled`). Stop never destroys a
  suspended stack, never settles a request by fiat, and never requests
  cancellation of an accepted request — cancellation is a separate explicit
  action (CANCEL-02) that no host path performs. An accepted request keeps
  its responsibility; the host continues driving until its natural
  publication, after which the task observes the token and retires. Stop
  therefore does not wake a parked driver: convergence waits on the accepted
  operations' own terminal states, and a request that never reaches a
  terminal defers `run()` exit indefinitely (no bounded-stop promise; §9/§12).
- Stop-versus-spawn linearization: `spawn()`'s acquire-load of the stop flag
  is the task-admission decision point. A spawn that observes stop=false may
  complete its slot commitment even if `request_stop()` stores stop=true
  immediately afterwards; that admitted task is part of the finite host set
  and is driven to retirement normally. A spawn that observes stop=true
  admits no task. No synchronization beyond that load/store pair is added:
  the race is explicitly linearized at the admission decision, not closed.
- The await precheck's stop rejection returns `IoError::Code::canceled`
  before any acceptance. This is a host admission-stop rejection reported
  through the existing error code — no operation was accepted, so the code
  is admission-flavored rather than an operation result; the global error
  model is not redesigned here (out of #399 scope).

## 5. The await protocol (host completed-return helper)

`IoTaskContext` exposes completed-return operations over the four admitted
request kinds — `read` / `write` / `sync_data` / `sync_all`, each with an
optional-deadline form — plus the two composition conveniences of §10
(`read_exact` / `write_all`). One call owns at most one `Request<T>` at any
instant, the composition loops included: each convenience step acquires and
settles exactly one primitive request before the next step begins. The
`IoTaskContext&` is valid only inside the task body invocation that received
it; tasks must not retain it past return:

```text
precheck host stop                     → return canceled (no acceptance)
r = ctx.submit_<op>(op)                → rejection returns verbatim (no acceptance)
link.request = &r; link.deadline=…; task.await_link = &link   (no-throw, stack storage)
suspend fiber
   [driver: poll_progress → wake when r.ready()]
resume: task.await_link = nullptr; obs = r.take_result()
return obs result                      (borrow ended at acquired publication)
```

- The `AwaitLink` object itself is constructed after acceptance on the
  task's own stack; what is pre-existing is the bounded observation capacity
  (the fixed slot/stack/await-link storage allocated at construction). After
  acceptance there is **no allocation, no throwing registration, and no
  fallible observation setup** — the post-acceptance observation-failure
  mode is eliminated by construction (OBS-04 reserve arm; H3's sibling
  observation shows the reserved state during suspension).
- The deadline bounds the driver's initial park window only (CANCEL-02,
  PROG-04): on expiry the host neither cancels nor settles the request, and
  the task stays suspended until its natural publication; the helper returns
  the operation's real terminal result whenever it reaches one — never a
  fictitious timeout success, never a returned Request, and never a
  host-initiated cancellation. A non-positive wait provides no effective
  bound (its deadline is already past).
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
stop while suspended (deadline expiry changes nothing: see §5)
    ↓
request keeps its responsibility → natural publication follows (L1)
    ↓
task resumes
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
core knowledge of host/fiber identity; timeout-as-cancellation; writing the
context's control plane from host stop; bounded-stop guarantees (stop
converges when the accepted operations reach their terminal states under the
L1/L4 environment assumptions; a stalled filesystem is outside REQ-06);
recoverable poison handling during host driving
(health failure during `run()` fails fast — the documented boundary until #401
owns a recoverable form, same as D1).

On the supported conforming driver path, `run()` returns after all spawned
tasks retire (§2); that is the promised structured exit. The exit performs a
zero-duration wait that observes and retires any control left pending by a
progress return: the wait path may reap a completion in the pass that follows
a control wake and return progress without ever reporting the control
(deliberate progress-first ordering), and an owner that stops waiting there
would release with unacknowledged control — the exit observation closes that
window and is the same owner discipline as the in-loop acknowledgement
(both points are mutation-discriminated together). The remaining `run()`
error escapes (progress/wait infrastructure failures surfacing through
`poll_progress`/`wait_one`) are contract-invalid or unreachable under
conforming use, no structured cleanup behavior is promised for them, and
control hygiene on a failed `run()` stays with the #401 health/shutdown
domain.

## 10. Blocking operation boundary and the composition conveniences

`File::open`, explicit close and `resize` are **outside the supported task
region**: the host offers no task-facing form of them and documents that they
must be performed by the application on the host thread outside `run()` or
before/after task execution (W-04 second arm). The task surface is the four
admitted request operations plus the two composition conveniences below.
`file_info`/`size` request forms stay with the #400 operation matrix.

SEM-01 adjudication: the root's operation-matrix row for
`read_exact` / `write_all` composition requires them as "direct and supported
host conveniences" while deferring any new compound low-level request. Since
D2-0 ships the host as supported, the conveniences are required host surface,
not optional. They are implemented exactly as that row's basis prescribes —
safe repeated primitive use under SEM-05 — as `IoTaskContext::read_exact` /
`IoTaskContext::write_all`:

- pure composition over the primitive host helpers: each step acquires,
  awaits and settles exactly one primitive request; no new low-level request
  kind, no new backend capability, no new RequestCore operation;
- invocation-boundary validation first, structurally identical to the direct
  forms: the shared data-operation oracle (`detail::precheck_data_op`) runs on
  the file state, access, offset and length before anything is submitted, so
  a closed file (`invalid_state`), illegal access (`invalid_argument`), an
  invalid range (`invalid_argument`) or an already-stopped host (admission
  rejection, `canceled`) rejects the whole invocation through the outer
  `Result` error with nothing accepted — invocation rejection is never folded
  into the composition outcome;
- a zero-length invocation is a logical no-op that still crosses admission as
  one zero-length primitive request: the root's request-path no-op rule (an
  explicit request no-op still requires an open healthy context, compatible
  resource provenance, operation support and one slot; once accepted it is
  immediately published ready and never dispatches a data operation) decides
  the empty case, so the convenience inherits the context's admission, health
  and slot semantics instead of short-circuiting to completion, and completes
  with zero confirmed bytes;
- the public result contract is the direct forms' own
  `Result<CompositionOutcome>`: the canonical payload
  (`sluice::blocking::CompositionOutcome`) produced by the shared composition
  rule (`detail::compose_progress`/`compose_error`), wrapped exactly as the
  direct forms wrap it — payload and public result shape are both reused,
  with no host-specific result type;
- after the boundary, per-primitive failures stay in the outcome with the
  confirmed prefix: the loop advances buffer/offset by confirmed bytes only
  (never replays confirmed bytes), stops on full completion, EOF before full
  (read), zero write progress (write), primitive error, or an observed host
  stop at the next primitive boundary (that step's precheck rejects with
  `canceled` before acceptance), reporting the confirmed byte count, the
  EOF-before-full / write-no-progress distinctions and the real primitive
  error exactly as on the direct path; a stop-at-the-boundary rejection
  reports `canceled` through the primitive-error arm with the conservative
  `unknown` effect remainder (the shared rule's primitive-error mapping;
  ERR-02 permits but does not require a known-zero accounting there);
- no atomicity promise, no rollback promise; one convenience call holds at
  most one owned `Request` at any instant and retains no unbounded state.

## 11. Validation obligations

W-04 tracer (sequential read-fill → write → sync chain, ThreadPool and a
successfully constructed io_uring); the HOST-03 matrix H1–H6 (task failure
while another task's I/O is outstanding; stop during suspension with the
accepted request settling on its natural outcome; post-acceptance
observation-setup failure eliminated by construction with mutation
discrimination; deadline expiry returning the real terminal result without
cancellation; exception during resumed processing; delivery/stop races with
retirement); run-reuse evidence (a failed run leaves no error state for the
next run); stop control-plane evidence (host stop plants no context control —
observed directly as notification-fd readability at stop time, plus a clean
next owner); progress-owner composition evidence (second claim,
`RequestScope` on host context, re-claim after `run()`); bound evidence
(capacity rejection, fixed stacks); the accepted-undispatched stop
discriminator (a victim accepted while the only worker is occupied and still
queued when stop lands must execute naturally — kills the stop-implicit-cancel
mutant); the expired-deadline park bound (after expiry the driver parks
unboundedly; the context's poll/wait counters stay orders of magnitude below
any zero-duration spin — kills the expired-deadline-spin mutant); the
external-control acknowledgement regression (control planted while the driver
is committed to a park is either reported by the next wait or consumed by a
progress return and retired at run exit; the next owner sees no stale control
under both legal paths — kills the skip-control-ack mutant at both
acknowledgement points, with the consumed-by-progress-return path forced
deterministically by holding the driver at its prepark pause while the worker
publishes); nested-spawn evidence
(capacity-1 refusal with clean retirement, capacity-2 child admission and
single execution); first-task-error selection evidence (execution order, not
admission order and not last-wins); the composition-convenience evidence
(full completion, EOF-before-full prefix, stop at the next acceptance
boundary with the confirmed prefix, one physical operation per step); the
direct-vs-host invocation parity table (identical inputs through
`blocking::read_exact_at`/`write_all_at` and the host conveniences across
closed/illegal-access/invalid-range/empty/nonempty/full/EOF rows and both
convenience directions — invocation rejection is distinguished from
composition primitive failure, a stopped host rejects before acceptance, the
zero-length rows complete with no data operation, and a backend error after
a confirmed prefix keeps the prefix while its step never reaches the kernel —
kills the empty-composition-precheck-bypass mutant);
mutation kills M1–M13 (recorded in the v1 conformance ledger D2 entry);
ASan/TSan including fiber switches; core-only probes unchanged.

## 12. Known limitations

Plain stacks without guard pages (same as the legacy fiber substrate); x86_64
Linux only (`fiber_ctx::supported`, explicit setup failure elsewhere); stop
convergence requires the accepted operations to reach terminal states and
cooperative tasks to observe the token (no operation preemption, no bounded
stop promise, no driver wake from stop); a task that ignores its token and
never awaits can defer host exit indefinitely (documented cooperative rule,
HOST-01); deadline forms bound the driver's initial park window only and
never cancel (CANCEL-02, PROG-04); a non-positive wait provides no effective
bound; health failure during `run()` fails fast rather than returning an
error result.

Contraction debt recorded for #402, deliberately not redesigned here: the
`stop_requested_` atomic, the host-level `CancelToken` and the public
`stop_requested()` readout represent overlapping stop projections (one
authority, one task-visible projection, one convenience readout); the
task-visible read-only stop projection is functional as-is, and no
speculative bool-view abstraction is added in this slice.
