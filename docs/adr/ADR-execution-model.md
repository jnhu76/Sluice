# ADR: Dual Threaded/Evented Execution Model

**Status: sluice-CORE-E0.** Architecture Decision Record.
**State: Accepted (decision only).** The ADR records the execution-model
decision; implementation follows in jobs E0-A through E6 (see
`docs/zig-stdio-migration-jobs.md` PHASE E).
**Decides:** the execution-strategy contract for logical task waiting in
cppio's async layer, and the relationship between the Threaded and Evented
strategies. This is the **principal experiment** of the project (task §9): can
C++ preserve Zig-like blocking-shaped application/task control flow across
Threaded and Evented execution strategies?

- Sits above `docs/adr/ADR-async-io-model.md` (016D, the L1 completion-based
  foundation). Does not supersede it; this ADR adds the execution-strategy
  layer the L1 ADR deferred (016D §13 Phase 9, "separate ADR").
- Consumes the source graph in `docs/zig-stdio-async-port-map.md` (023A),
  especially `Io/fiber.zig` (the 3-arch stack-switching seam) and
  `Io/Uring.zig` (the Evented scheduler).
- Governs the PHASE E job sequence in `docs/zig-stdio-migration-jobs.md`.

This ADR makes **one** recommendation. It introduces **no dependency** without
explicit evaluation (§11). It makes **no performance claim** merely from
implementing fibers (§10).

---

## 1. Context

cppio's async foundation (jobs 017-030) is complete: `Completion<T>`,
`AsyncIoContext`, `AsyncBackend` (Fake/ThreadPool/Uring), cooperative
cancellation (`CancelToken`/`CancelState`/`check_cancel`), and the task
abstractions `Future<T>`, `Group`, `Batch`. The PHASE B backend substrate is
GREEN (023B §9).

The **current** wait mechanism in `Future<T>::await()` is `std::condition_variable`
+ `std::mutex` — it blocks the calling OS thread until the result is ready
(see `include/sluice/async/future.hpp`). `Group::await()` composes `Future`,
inheriting the cv-based wait. `Batch::await_one()` drives `AsyncIoContext::wait_one()`,
which on ThreadPool is cv-based and on Uring is `io_uring_submit_and_wait`.
This is the **Threaded-equivalent** shape (mirrors Zig `Io.Threaded`, which
dedicates one OS thread per task and blocks it in a syscall).

The Zig `std.Io` reference offers a **second** execution strategy: `Evented`
(the internal name of `Io/Uring.zig` and `Io/Kqueue.zig`; `Io/Dispatch.zig` on
Apple). Evented does NOT block an OS thread per wait — it suspends the current
**user task** (a fiber), returns the OS worker to the scheduler, and resumes
the task when the awaited state completes. The seam is `Io/fiber.zig` —
architecture-specific stack-switching (aarch64/riscv64/x86_64).

The principal experiment (task §9) is to determine whether C++ can preserve
**blocking-shaped application/task control flow** while the execution strategy
changes from Threaded to Evented. Stopping at Threaded-only leaves the
experiment incomplete.

This ADR decides the execution-strategy contract BEFORE context-switching
code lands, per task §10 ("Only begin stack-switching implementation after the
source audit and required prerequisite jobs are complete").

## 2. Decision (accepted)

Adopt a **dual Threaded/Evented execution model**. The **public task contract
is logical control-flow waiting**: `await` means "wait until terminal
completion." It must NOT promise that await blocks an OS thread, suspends a
fiber, polls inline, or uses any other specific physical wait mechanism. The
execution strategy determines the physical behavior:

```text
task state / result / cancel state
              |
              v
       logical wait request
              |
              v
       Io execution strategy
          /             \
   Threaded            Evented
      |                   |
 block/park thread    suspend user task
```

| Strategy | Logical wait physical mechanism | Availability |
| --- | --- | --- |
| **Threaded** | may block the current OS thread (cv/park). The portable baseline. | always available (default) |
| **Evented** | must suspend the current user task/fiber, return the worker to the scheduler, and make the task runnable again on completion/wakeup. | experimental; capability/target gated (§6) |

**Threaded remains the portable baseline, unchanged in semantic contract.**
Evented is additive: a new execution strategy that implements the SAME logical
wait contract via a different physical mechanism. **Do not emulate unsupported
Evented targets using one OS thread per task** — Evented either has real
fiber support on a target or it is unavailable (fail/disable cleanly, §7).

This decision is the core experiment of cppio. The project goal is to preserve
blocking-shaped application/task control flow across Threaded and Evented
execution strategies. Therefore stopping at Threaded-only execution would
leave the primary Zig `std.Io` translation experiment incomplete.

## 3. Required boundary (the load-bearing contract)

The required separation (task §9, task §11):

```text
task state / result / cancel state
              |
              v
       logical wait request
              |
              v
       Io execution strategy
          /             \
   Threaded            Evented
      |                   |
 block/park thread    suspend user task
```

Concretely, the responsibilities that MUST be separate:

1. **Task state / result / cancel state** — owned by the task abstraction
   (`Future`/`Group`/`Batch` + `CancelToken`/`CancelState`). Strategy-
   independent. The semantic contracts (idempotent await/cancel, exactly-once
   terminal, cancel-propagation boundary, single-shot cancel delivery) are
   preserved unchanged across strategies.
2. **Logical wait request** — the abstraction asks "wait until terminal." It
   does NOT know whether that blocks a thread or suspends a fiber.
3. **Io execution strategy** — Threaded or Evented — implements the physical
   wait. This is where `std::condition_variable` (Threaded) vs fiber-yield
   (Evented) lives.
4. **Backend completion → task runnable → scheduler execution** — separate
   responsibilities (§4). The backend observes completion; the task layer
   updates state and marks the task runnable; the scheduler chooses execution.

**Do not change Future/Group/Batch semantic contracts merely to add fibers.**
If their current implementation has prematurely embedded Threaded wait policy
(`std::condition_variable` directly in `Future`), refactor the WAITING
MECHANISM behind the Io execution boundary while preserving the result,
idempotency, lifetime, and cancellation contracts (job E0-A).

## 4. Responsibility separation

```text
backend completion (Uring CQE / ThreadPool worker / Fake poll)
        |
        v
task state updated + waiting task becomes runnable
        |
        v
scheduler chooses execution (runnable queue)
        |
        v
resume the task at the original blocking-shaped call site
```

- **Backend completion** is the responsibility of `AsyncBackend` (L0). A
  completion makes a `Completion<T>` ready.
- **Task wakeup** (waiting task → runnable) is the responsibility of the task
  layer (`Future`/`Group`/`Batch` + the scheduler integration). This is the
  boundary a completion crosses.
- **Runnable queue / worker scheduling / work stealing** is the
  responsibility of the scheduler (Evented: the per-thread run queue + work
  stealing; Threaded: not applicable — each wait owns its thread).

**`UringAsyncBackend` is an operation backend and must NOT become the task
scheduler.** A backend completion must not directly execute arbitrary user
work. It crosses the boundary: backend observes completion → task state
updated → task becomes runnable → scheduler selects (§3 of task §11).

## 5. Why dual and not single-strategy

- **Threaded-only** leaves the principal experiment incomplete (task §9). It
  is the floor, not the ceiling.
- **Evented-only** abandons the portable baseline. Evented is target-gated
  (§6); making it the only strategy would leave cppio unavailable on targets
  without `fiber.zig`-equivalent support.
- **Dual** preserves the portable baseline AND completes the experiment. The
  logical-wait contract is the same; only the physical mechanism differs.

## 6. Evented capability / target gating

Evented is **experimental and source-derived capability gated.** Derived from
Zig (`Io.zig:28-39`, `fiber.zig:1-4`): Evented requires (a) `fiber.supported`
(stack-switching code for the target arch) AND (b) an event backend
(`Io/Uring.zig` on Linux, `Io/Kqueue.zig` on BSD, `Io/Dispatch.zig` on Apple).

cppio's initial Evented target set: **x86_64 Linux** (the development host;
`Io/Uring.zig` is the reference). aarch64 Linux is a natural follow-on
(`fiber.zig` supports it; the `Context` is 3 words like x86_64). Other
architectures/OSes are deferred until their `fiber.zig`-equivalent port +
event backend exist.

Gating mechanism: a compile-time gate (`SLUICE_HAS_EVENTED` or equivalent)
that is OFF by default. Evented builds fail/disable cleanly on unsupported
targets (§7); they do NOT silently substitute one-thread-per-task.

## 7. Unsupported-target policy

On a target without Evented support, Evented must **fail/disable cleanly** —
not silently emulate using one OS thread per task (which would be Threaded in
disguise and would falsify the experiment). The build either does not compile
the Evented sources or compiles them to an unavailable stub, and the Threaded
strategy remains fully functional. This matches how Zig models it
(`Evented = void` on unsupported targets, `Io.zig:31-39`).

## 8. Out of scope (unchanged from task §1)

Actor runtimes, P2300 / `std::execution`, sender/receiver adapters, generic
executors, `std::promise`/`std::future` adapters, Boost.Asio compatibility,
generic coroutine frameworks, C++ async/await API redesign, libuv clone,
IOCP, networking expansion, arbitrary scheduler research — all OUT of scope
for this ADR and the PHASE E jobs. They may be studied later as post-Zig
comparison models. This ADR adds fibers ONLY because the Zig `Io.Evented`
architecture requires them.

## 9. Construction frontier (governs PHASE E job order)

```text
E0 ADR (this document)
  ↓
E0-A waiting-policy audit/refactor (refactor Future/Group/Batch waits behind
     the execution boundary IF Threaded policy is embedded — it IS, see E0-A)
  ↓
E1 minimal Task/Fiber state model (source-derived)
  ↓
E2 source-derived stack context representation (fiber.zig Context: sp/fp/pc)
  ↓
E3 context switch + trampoline (x86_64 first, gated)
  ↓
E4 single-worker suspend/yield/wake — PROVE the full state transition:
     running task → submit op → waiting → switch to scheduler → run another
     task → backend completion → waiting task runnable → scheduler selects →
     resume at the original call site.
     (Do NOT begin multi-worker/work-stealing before this is proven end-to-end.)
  ↓
E5 Future/Group logical waits on Evented (refactor onto the fiber seam)
  ↓
E6 backend completion → task runnable transition (the boundary in §4)
  ↓
E7 multi-worker scheduler
  ↓
E8 work stealing
  ↓
E9 Io-aware wait queues / futex layer
  ↓
E10 Mutex / Condition / Event / Queue / RwLock / Semaphore (PHASE S)
  ↓
E11 Select (T5 — was blocked; unblocked once Queue exists)
  ↓
E12 Threaded vs Evented semantic parity audit
```

**Do not begin with work stealing.** First prove the complete single-worker
state transition (the E4 cycle above). This is the experiment's load-bearing
proof.

## 9.1 E4 success criterion — scheduler-liveness invariant (added by E4-GATE)

The load-bearing semantic invariant for Evented execution is about the
**scheduler worker**, not about whether a blocking syscall exists anywhere:

> **While an asynchronous operation is pending, an Evented scheduler worker
> must remain available to execute another runnable task.**

This refines §2's "must suspend the current user task/fiber, return the worker
to the scheduler." It makes the success criterion **externally observable and
testable** without inspecting syscall names or implementation mechanism.

The three execution paths are semantically distinct:

```text
P1 — FORBIDDEN (scheduler-worker blocking):
  scheduler worker -> fiber body -> blocking syscall -> scheduler worker pinned.
  A fiber directly executing a blocking operation on the scheduler worker
  violates Evented semantics when another task is runnable.

P2 — PERMITTED (blocking-pool offload):
  scheduler worker -> submit async op -> blocking backend worker -> blocking
  syscall -> completion -> scheduler wakes task.
  Blocking-pool offload is valid because the scheduler worker remains free.
  (The §4 boundary already lists ThreadPool workers as a backend completion
  source alongside Uring CQEs and Fake poll.)

P3 — PERMITTED (kernel async / completion backend):
  scheduler worker -> submit kernel async op -> kernel/completion backend ->
  completion -> scheduler wakes task.
  io_uring / completion-based execution is valid.
```

This ADR does **NOT** require every Evented backend to use kernel-native async
I/O. P2 (blocking-pool offload) is a legitimate Evented execution path because
it satisfies the scheduler-liveness invariant. Classifying P2 as a category
error would contradict §4, which already names ThreadPool workers as a
completion source.

**E4 success criterion (externally observable):**

```text
single scheduler worker

Fiber A: submit an async op; op remains pending; await/suspend on it.
Fiber B: is runnable; records progress.

Required observation: Fiber B runs while Fiber A's op is still pending.
Then: backend completes A's op; A becomes runnable; A resumes at the exact
suspension point; A observes the terminal result.
```

The critical proof is: **B progresses before A's pending operation completes.**
With exactly one scheduler worker, this proves A returned the worker to the
scheduler. The proof must NOT rely on syscall-name inspection, `strace`
assertions, "Uring must not call pread," `EAGAIN` as the primary liveness
proof, or sleep-based timing races. Test semantics, not implementation
mechanism.

E4-T1 (single-worker scheduler liveness) is the **primary E4 gate**. E4-T2
(completion resumes waiting fiber), E4-T3 (exactly-once runnable transition),
E4-T4 (runnable task not starved) are required supporting proofs.

## 9.2 E7 multi-worker scheduler contract (added by E7-ADR)

This section defines the E7 multi-worker scheduler ownership contract. E7
extends E4–E6 (single-worker Evented scheduler) to multiple Scheduler workers
while preserving every already-proven semantic: Completion outstanding
semantics, the `submit_*` postcondition, Future/WaitPolicy public contracts,
Group public API, and E4/E5/E6 liveness/progress semantics. The E7
architecture-closure audit concluded `E7 ADR READY` based on (a) the
serialized-access (not exact-thread-affinity) nature of the current backends,
and (b) generalizing E6's blocking-wait gate from single-worker idle to global
Scheduler idle.

### 9.2.1 Preserved submission / Completion semantics

E7 preserves the current submission postcondition **unchanged**:

```text
submit-success
    -> operation is accepted/recorded by the backend
    -> Completion is outstanding
```

`Completion::outstanding` continues to mean **BACKEND-ACCEPTED** — the op is
in the backend and the Completion is outstanding, both happening atomically
inside the backend's `submit_*`. The `submit_*` postcondition is **not
weakened**. E7 does **not** introduce `queued`, `logically-submitted`, or
`driver-accepted` Completion states. E7 does **not** introduce command-ingress
handoff. E7 does **not** move `mark_outstanding` from backend-private submit
paths to AsyncIoContext. The E4–E6 pattern

```text
ctx.submit_*(op, completion)
scheduler.await_completion_*(completion)
```

where immediate await after submit remains valid, is preserved.

### 9.2.2 Worker-local execution state

Each Scheduler worker has its own:

```text
scheduler context (sched_ctx — the saved native stack continuation)
current Fiber slot (current_ — the Fiber currently executing on this worker)
```

Reason: `sched_ctx` stores a native-thread stack continuation — `rsp`/`rbp`
point into the OS worker thread's stack; `current` means the Fiber currently
executing on this OS worker. These are worker-local because they reference the
OS thread's own stack and currently-running task.

Required ADR statement:

```text
Each Scheduler worker has its own scheduler context and current-Fiber slot.
There is no Scheduler-global current Fiber in E7.
```

### 9.2.3 Conservative pinned-Fiber contract

E7 uses a conservative non-migrating contract:

```text
Once a Fiber first begins execution on Worker W,
every later E7 resume of that Fiber occurs on Worker W.
```

Work stealing and Fiber migration are deferred to E8.

```text
E7 does not prove cross-worker Fiber migration.
E7 does not prove work stealing.
E8 owns the migration/stealing decision.
```

E7 does **not** choose a global runnable queue that allows an already-started
Fiber to resume on any worker.

### 9.2.4 Owner-preserving wake routing

```text
Every started Fiber has one owning Worker during E7.

When a Fiber is running:
    owner = the current Worker.

When a Fiber is waiting:
    the wait registration preserves the owner,
    either by map partitioning or by an owner tag.

When a Fiber is runnable:
    it resides in the owning Worker's runnable path
    such as local queue or inbox.

Every wake routes the Fiber back to its owning Worker.
```

E7 does **not** require a public `Fiber::owner_worker` field. E7 does **not**
require a global `Fiber* -> Worker*` table unless implementation later proves
it necessary. The ADR defines the invariant, not the storage class.

### 9.2.5 Serialized backend access domain

```text
At most one Scheduler worker may call into AsyncIoContext / AsyncBackend at a
time. Calls may come from different OS worker threads over time, but they must
be externally serialized and never concurrent.
```

This is a **serialized access domain**, not an exact owner-thread contract.

Rationale:

```text
UringAsyncBackend is not proven exact-thread-affine:
    cppio initializes io_uring with flags=0,
    no IORING_SETUP_SINGLE_ISSUER,
    no thread-id assertion,
    no backend thread_local state.

But it is not concurrent-safe:
    ring state and backend maps are unsynchronized.

Therefore E7 requires serialized access, not a dedicated owner thread.

FakeAsyncBackend and AsyncIoContext stats are also not concurrent-safe but are
serializable. ThreadPoolBackend is internally locked, but the accepted contract
must fit all backends.
```

E7 does **not** introduce a dedicated backend driver thread. E7 does **not**
introduce command ingress.

### 9.2.6 Global MW-S1 / MW-S2 progress rule

E6's S1/S2 rule generalizes to multiple workers.

**MW-S1 — global executable work exists:**

```text
At least one Worker:
    is running a Fiber
or
    has runnable Fiber work
or
    has routed runnable inbox work
```

Rule: backend progress observation must be non-blocking — `poll` only. No
worker may call blocking `wait_one()` under MW-S1.

**MW-S2 — globally no Fiber can execute, backend progress pending:**

```text
no Worker is running a Fiber
all Worker runnable queues/inboxes are empty
no Scheduler-internal routed runnable work is pending
at least one Completion-backed backend operation is outstanding
```

Rule: one elected participant may enter `wait_one()`.

When `wait_one` returns and a Completion becomes ready:

```text
wake path routes the Fiber back to its owning Worker
MW-S2 ends
execution resumes under MW-S1
```

This is the multi-worker generalization of E6. Do **not** call `wait_one()`
while any Scheduler Fiber can run. Do **not** busy-spin as the default idle
strategy.

**MW-S3 — unresolved wait / no Scheduler-driven progress:**

```text
no Worker is running a Fiber
all Worker runnable queues/inboxes are empty
no Completion-backed backend operation is outstanding
one or more Scheduler wait registrations remain
```

Required contract:

```text
MW-S3 is NOT global quiescence.
MW-S3 is NOT MW-S2.
Scheduler must not call backend wait_one merely because MW-S3 exists.
E7 does not prove progress from MW-S3.
```

MW-S3 may represent an internal logical dependency cycle, unresolved logical
waiting, a wait dependent on an external producer, or another dormant/stalled
condition. E7 does **not** need to distinguish these causes. Do **not** add
producer provenance to Future or WaitPolicy. Do **not** add a deadlock
detector. Do **not** add external producer wake. The purpose is only to avoid
classifying unresolved waiting as completed quiescence.

**Observable-readiness observation before idle-state admission (E7 invariant):**

E5 uses level-triggered readiness: `Future::complete_with` stores
`ready.store(true)` and does NOT actively notify or mutate Scheduler state;
the Scheduler polls registered readiness, erases the registration, and routes
the waiting → runnable Fiber. The same classification discipline applies to
Completion-backed waits: a Completion may become terminal-ready via a backend
`poll()`/`wait_one()` whose reap-count signal is not the authoritative source
of "is this Completion ready" (E6 bug fix — see below). Therefore, before the
coordinated Scheduler admits MW-S2, MW-S3, or global quiescence, it MUST
observe registered wait readiness and route every currently-ready waiter to its
owning Worker's runnable path. This covers both registration kinds:

```text
Completion-backed registrations: Completion is already terminal-ready.
persistent readiness registrations: ready flag currently loads true.
```

Conceptually:

```text
observe registered waits
    |
    +-- ready Completion
    |       -> erase registration; waiting -> runnable; route to owner Worker
    |
    +-- ready persistent flag
            -> erase registration; waiting -> runnable; route to owner Worker

then evaluate MW-S1 / MW-S2 / MW-S3 / QUIESCENT
```

The implementation may use global maps, per-Worker maps, partitioned maps, or
coordinated scans — the ADR defines only the semantic observation requirement.
This preserves E5's level-triggered protocol. A Scheduler must not enter
MW-S3/quiescence while a registered ready flag is already true.

**Backend progress and waiting-Fiber wake routing are distinct.** A backend
`poll()` or `wait_one()` may make a Completion terminal-ready even when a later
backend poll reports zero newly reaped operations. Therefore Completion wait
registrations must be scanned/routed based on Completion readiness, NOT gated
solely by the current backend progress call's reap count. The contract is:

```text
backend progress count == 0
    does NOT imply
no registered Completion waiter is ready
```

This preserves the E6 bug fix (in which a Completion made ready by `wait_one`'s
internal poll had to be scanned unconditionally — the prior `if (reaped == 0)
return 0` early-return stranded the Fiber).

**Latent executable work.** A registered waiter whose wait source is already
observably ready is latent executable work. It must be materialized into the
owning Worker's runnable path before MW-S2 admission. Example:

```text
Completion C1: already ready; Fiber A still registered waiting.
Completion C2: outstanding.
no Worker currently running/runnable.
```

The Scheduler must first route A; it must NOT classify the state as MW-S2 and
block in `wait_one()` for C2. This preserves the E4/E6 liveness invariant: if a
Fiber can already be made runnable, blocking backend wait is forbidden.

**MW-S3 refinement.** MW-S3 contains unresolved waits whose readiness is NOT
currently observable as ready after the coordinated pre-admission observation
pass. A ready-but-not-yet-routed registration is NOT MW-S3.

**Coordinated MW-S2 admission:**

MW-S2 is a coordinated global Scheduler state transition, not a per-Worker
inference. Admission to blocking `wait_one()` must be coordinated with Worker
running-state and runnable/inbox publication: a runnable publication visible
before MW-S2 admission prevents blocking admission. A Worker must not infer
MW-S2 from stale/local observations such as "my local queue is empty and I
currently see running_count == 0." E7 does **not** specify the coordination
mechanism (mutex type, atomic counter layout, epoch protocol, barrier, leader
election) — those remain implementation details.

### 9.2.7 Internal worker notification

E7 requires internal worker-to-worker notification for routed runnable work.

```text
Worker 0 observes Completion C ready.
C belongs to Fiber A.
Fiber A is owned by Worker 2.

Worker 0 routes A to Worker 2's runnable path.
Worker 2 must be notified that runnable work exists.
```

The mechanism is implementation-local: `condition_variable`, `eventfd`,
`futex`, `pipe`, lock-free inbox, or other. E7 does **not** specify the
primitive in this contract. But the contract must say:

```text
E7 includes Scheduler-internal cross-worker notification.
```

This is **distinct** from arbitrary external producer wake (§9.2.8).

### 9.2.8 External producer / arbitrary external callers are out of E7

E7 coordinates Scheduler-managed workers. E7 does **not** require support for
arbitrary user OS threads concurrently calling `AsyncIoContext::submit_*`,
`AsyncIoContext::cancel`, or `Future::complete_with` while the Scheduler is
idle.

```text
External producer wake is not proven by E7.
```

This is classified as an orthogonal, currently unassigned external-wake /
external-concurrency frontier. E7 does **not** silently assign it to E9.

### 9.2.9 Logical global quiescence

Global quiescence requires:

```text
no Worker is running a Fiber
no Worker has runnable Fiber work in local queues or inboxes
no Scheduler-internal routed runnable work is pending
no Completion-backed backend operation remains outstanding
no Scheduler-managed Fiber remains registered as waiting
```

The final condition includes waits on Completion-backed wait registrations and
persistent readiness / ready-flag registrations.

Semantic rule:

> A Scheduler with unresolved waiting Fibers is not globally quiescent merely
> because no Fiber is currently executable.

Distinguish:

```text
quiescent:        no logical Scheduler work remains.
stalled/dormant:  logical waiting remains (MW-S3), but E7 has no
                  Scheduler-driven progress source.
```

Quiescence is **not** defined as "no thread is blocked in `wait_one`" — thread
parking is implementation state, not logical work. The current E5 wait
registration identifies the waitable readiness identity, the waiting Fiber, and
the owning Worker; it does **not** identify the producer that will eventually
make readiness true, so the Scheduler cannot generally distinguish "Future
completed by another Scheduler Fiber" from "Future completed by an arbitrary
external OS thread" from registration alone. Therefore if any wait registration
remains, no backend progress source remains, and no Fiber is executable, the
state is MW-S3 — not quiescence. External producer wake remains outside E7;
internal dependency-cycle detection also remains outside E7.

### 9.2.10 Explicitly rejected alternatives

E7 does **not** choose:

```text
global runnable queue with migratable Fibers
dedicated backend driver thread
command ingress / queued backend commands
new Completion queued state
driver wait over backend completion OR command ingress
exact owner-thread backend affinity
per-worker AsyncIoContext/backend
```

Reason: they are either unnecessary for E7 after the global-idle rule, or they
belong to E8/later frontiers, or they would change already-proven E4–E6
contracts.

### 9.2.11 Scope boundaries

E7 proves:

```text
multiple Scheduler workers
worker-local execution state
pinned Fiber ownership
owner-preserving wake routing
serialized backend access
global MW-S1/MW-S2 progress rule
MW-S3 unresolved-wait classification (distinct from quiescence)
internal worker notification
logical global quiescence
```

E7 does **not** prove:

```text
work stealing
Fiber migration
external producer wake
arbitrary external AsyncIoContext multi-caller API
new resource identity
Reader/Writer async bridge
network/process
Batch integration changes
timer subsystem
io-aware futex/wait queues
sync primitives
```

### 9.2.12 Verification expectations for later implementation

E7 implementation (later) must demonstrate:

```text
E7-T1 — worker-local current:
    Two workers run two Fibers concurrently; each Fiber awaits a different
    ready flag or Completion. Assert: Worker 0 registers Fiber A; Worker 1
    registers Fiber B; no cross-registration occurs.

E7-T2 — worker-local scheduler context:
    Two workers suspend/resume separate Fibers. Assert: each Fiber returns to
    its own worker scheduler context; no shared sched_ctx overwrite.

E7-T3 — pinned resume:
    Fiber A starts on Worker W; A suspends; A is woken by another worker's
    poll. Assert: A resumes on W.

E7-T4 — no wait_one under MW-S1:
    One worker has runnable Fiber B while Fiber A awaits backend Completion.
    Assert: no worker enters blocking wait_one; B makes progress.

E7-T5 — wait_one under MW-S2:
    All workers idle except one backend Completion pending. Assert: one elected
    participant calls wait_one; backend completion wakes pinned Fiber.

E7-T6 — serialized backend access:
    Stress multiple workers submitting/canceling/polling. Assert: AsyncBackend
    calls are never concurrent.

E7-T7 — internal worker notification:
    Worker 0 wakes Fiber A owned by Worker 2. Assert: A appears in Worker 2's
    runnable path; Worker 2 observes it; A resumes on Worker 2.

E7-T8 — true global quiescence:
    Construct a completed workload. Assert: no running Fiber; no runnable/inbox
    Fiber; no backend Completion outstanding; no wait registration remains.
    Then the coordinated Scheduler run terminates.

E7-T9 — unresolved wait is not quiescence:
    Construct: Fiber A awaits an unready persistent readiness flag/Future; A is
    waiting; no Fiber runnable/running; no backend Completion outstanding.
    Assert: Scheduler does not classify the state as global quiescence; the
    wait registration remains; the state is MW-S3 / unresolved wait. E7 is not
    required to make progress from this state; do not use an external producer
    to wake it.

E7-T10 — observable readiness precedes global-state admission (covers both
    registration kinds):

    E7-T10A — persistent ready flag:
        Two Scheduler-managed Fibers on different Workers: Fiber A waits on
        Future X / persistent ready flag; Fiber B makes X ready. Before global
        idle/quiescence admission: X readiness is observed; A's registration is
        erased; A is routed to its owning Worker; A resumes. Assert the
        Scheduler does not enter MW-S3/quiescence while a registered ready flag
        is already true.

    E7-T10B — Completion ready after backend wait/progress:
        Fiber A waits on Completion C1; backend progress / wait_one makes C1
        terminal-ready; the subsequent backend progress observation reports
        zero newly reaped work (or otherwise provides no new-progress signal).
        Before MW-S2/MW-S3/quiescence admission: C1 readiness is observed from
        Completion state; A's registration is erased; A is routed to its
        owning Worker; A resumes. Assert global-state admission is not gated
        solely by the current poll/reap count. This preserves the E6 semantic
        bug fix under E7's multi-worker classification. No sleeps.

E7-T11 — MW-S2 admission race:
    Create a deterministic test seam around MW-S2 admission. Interleave: a
    candidate progress participant begins global-idle admission; another Worker
    publishes routed runnable work before blocking admission commits. Assert:
    blocking wait_one is not entered; the runnable Fiber progresses. Do not use
    sleeps; the test should prove the coordinated admission invariant, not a
    timing accident.
```

This ADR patch does not require implementation.

## 10. No performance claim

Implementing fibers does NOT constitute a performance claim. Any performance
evidence is workload-specific, machine-specific, and recorded under the
existing bench framework (job 022). Per ADR 016D §12 R6 and task §15: no
universal performance claim is permitted.

## 11. Dependencies (explicit evaluation)

```text
E1. fiber.zig port (x86_64 contextSwitch)  — REQUIRED for Evented; gated to
                                              x86_64 (+ aarch64 follow-on).
                                              Architecture-specific asm, audited
                                              under sanitizer caveats (task §10).
                                              EVALUATED: accept, gated.
E2. io_uring (liburing)                    — already optional (020B/026); the
                                              Evented Uring backend reuses it.
                                              EVALUATED: accept (unchanged).
E3. Anything else                          — NONE introduced by this ADR.
```

No dependency enters without an explicit row here.

## 12. Risks

```text
R1. Stack-switching asm correctness — the core Evented risk (hand-written asm,
    architecture-specific). Mitigation: gate to x86_64 first; mirror fiber.zig's
    3-word Context + full-ABI-clobber switch; sanitizer caveats documented
    (task §10); the single-worker E4 cycle is the structural proof.
R2. Lifetime across suspension — a task holding a reference across a suspend
    point. Mitigation: the task layer owns task lifetime; the scheduler never
    destroys a runnable task; ADR 016D §5 L1-L3c buffer rules hold across
    suspension.
R3. Public-contract drift — accidentally promising OS-thread blocking in the
    logical-wait contract. Mitigation: this ADR §2/§3 state the contract is
    LOGICAL; the E0-A audit (next job) verifies no abstraction leaks the
    physical mechanism.
R4. Scheduler/backend conflation — letting UringAsyncBackend become the
    scheduler. Mitigation: §4 + task §11 explicitly forbid it; the boundary
    is tested (E6).
R5. Evented target unavailability — Evented unavailable on a target where it
    was assumed. Mitigation: §7 fail/disable cleanly; Threaded baseline always
    available.
R6. Sanitizer interaction — stack switching can make ASan/TSan results
    misleading without integration support. Mitigation: documented; Threaded
    path remains sanitizer-clean for correctness work; Evented path relies on
    the E4 structural proof + careful asm review.
```

## 13. Abort conditions

Stop Evented work (keep Threaded-only, record Evented as explored-but-abandoned)
if any of these occurs:

```text
AB-E1. The single-worker E4 state transition cannot be proven correct
       (running → waiting → switch → another task → completion → runnable →
       resume) — then the experiment fails and cppio stays Threaded.
AB-E2. fiber.zig's context switch cannot be translated to correct, audited
       x86_64 asm — then Evented is unavailable on the host and the experiment
       cannot run here (deferred to a target with a working switch).
AB-E3. The logical-wait contract cannot be preserved across strategies without
       breaking the existing Future/Group/Batch semantic contracts (028/029/030)
       — then dual-strategy is abandoned in favor of Threaded-only.
AB-E4. UringAsyncBackend cannot be kept out of the scheduler role (§4) — then
       Evented is abandoned (the boundary is the experiment).
AB-E5. The default (no-liburing, no-Evented) build breaks (ADR 016D AB1).
AB-E6. Any blocking test changes behavior or fails (ADR 016D AB2).
```

## 14. Decision summary

```text
ACCEPTED   : Dual Threaded/Evented execution model. The public task contract
             is LOGICAL control-flow waiting (await = wait until terminal
             completion); the physical mechanism is strategy-determined.
THREADED   : may block the current OS thread. Portable baseline. Unchanged.
EVENTED    : must suspend the current user task/fiber, return the worker to
             the scheduler, resume on completion/wakeup. Experimental,
             capability/target gated (x86_64 Linux first).
BOUNDARY   : backend completion → task state → runnable → scheduler. Four
             separate responsibilities; UringAsyncBackend is NOT the scheduler.
FRONTIER   : ADR → E0-A audit → Task/Fiber → context → switch → single-worker
             proof (E4) → logical-wait refactor → multi-worker → stealing →
             futex → sync primitives → Select → parity audit.
NO PERF    : fibers add no universal performance claim (§10).
DEPS       : fiber.zig port (x86_64, gated) + existing optional liburing.
```

## 15. Cross-links

- Source graph: `docs/zig-stdio-async-port-map.md` (023A) — `Io/fiber.zig`,
  `Io/Uring.zig` Evented.
- Parity audit: `docs/async-backend-parity.md` (023B).
- Job sequence: `docs/zig-stdio-migration-jobs.md` (023C) — PHASE E.
- Async foundation ADR (this sits above): `docs/adr/ADR-async-io-model.md` (016D).
- Existing async jobs (GREEN baseline): `docs/async-next-jobs.md` (016F).
