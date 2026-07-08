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
E9 Scheduler park/wake and external-wake protocol [CLOSED]
  ↓
E10 WaitNode and cancellation-safe wait queue core
  ↓
E11 Deadline / timer wait integration
  ↓
E12 Async synchronization primitives (E12-A Mutex, E12-B Event, E12-C Condition,
    E12-D Queue, E12-E Semaphore, E12-F RwLock, E12-G cross-primitive audit)
  ↓
E13 Select / multi-wait winner protocol
  ↓
E14 Threaded vs Evented semantic parity and runtime closure
```

### Historical frontier refinement

The original ADR defined the future construction frontier coarsely:

```text
Original E9:  Io-aware wait queues / futex layer
Original E10: Mutex / Condition / Event / Queue / RwLock / Semaphore
Original E11: Select
Original E12: Threaded vs Evented semantic parity audit
```

Construction and formal review refined the original E9 scope. As-built
mapping:

```text
Original E9
  -> E9  Scheduler Park/Wake and External-Wake Protocol [CLOSED]
  -> E10 WaitNode and Cancellation-Safe Wait Queue Core
  -> E11 Deadline / Timer Wait Integration

Original E10
  -> E12 Async Synchronization Primitives

Original E11
  -> E13 Select / Multi-Wait Winner Protocol

Original E12
  -> E14 Threaded vs Evented Semantic Parity and Runtime Closure
```

Completed E7/E8/E9 identifiers remain frozen historical IDs. This mapping
refines the future construction frontier and does not retroactively
renumber completed work.

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

## 9.3 E8 runnable ownership-transfer contract (added by E8-ADR)

This section defines the E8 runnable-ownership-transfer decision. E8
extends E7's pinned-ownership contract to allow an idle Worker to execute
a Runnable Fiber currently owned by another Worker, by *transferring
ownership* of the Fiber to the thief as part of the steal. The E8-0
ownership topology audit (`docs/e8-0-ownership-topology-audit.md`)
established that no E7 invariant contradicts this; this section records
the chosen protocol.

### 9.3.1 Protocol decision — Model B (ownership-transfer steal)

Four candidate models were evaluated:

```text
Model A — ticket-only steal:
    ticket W0Local -> W1Local; owner stays W0.
Model B — ownership-transfer steal:
    ticket W0Local -> W1Local; owner W0 -> W1, in one transition.
Model C — ownership transfer at first execution on thief:
    steal ticket (owner stays W0); owner := W1 at run_next_on.
Model D — generation-tagged ownership:
    owner[F] = (W, gen); steal bumps gen.
```

Evaluation against the E8 load-bearing path
(steal → run on thief → suspend → wake → resume on thief):

- **Model A is unsound.** If the steal moves the ticket to W1's
  `local_runnable` but does *not* transfer the runnable owner record
  (`fiber_owner_`), the post-steal state has the ticket on W1's queue while
  `fiber_owner_[F]` still names W0. The thief can never pop the stolen
  ticket (`try_steal`/pop require the runnable owner record to agree with
  the queue the ticket sits on), so the ticket is stranded on the wrong
  owner's queue — the **runnable ticket / runnable owner-record split**.
  This is the defect the negative TLA+ model must produce (§9.3.6).
  *(Note on a corrected framing: production wake routing reads
  `WaitReg.owner`, not `fiber_owner_`; the unsoundness of Model A is the
  broken steal/pop consistency, not a stale wake route. See §9.3.5.1.)*
  **Rejected.**
- **Model B is the smallest sound model.** One abstract transition moves
  the ticket AND transfers the runnable owner record. The ticket and the
  owner record always agree, so steal and pop are mutually consistent.
  Because the thief becomes the executor on Pop, the subsequent suspend
  captures `WaitReg.owner = g_worker = thief`, and wake routes to the
  thief. No transient inconsistent state. Linearizes under `global_mtx_`
  (the existing coordination domain — E8-0 O8). **Selected.**
- **Model C defers the inconsistency, does not eliminate it.** Between
  steal and `run_next_on`, the owner record says W0 while the ticket is
  in W1's queue. If the thief pops and runs the fiber before any other
  action, this is observationally equivalent to B — but the audit
  (`docs/e8-0-ownership-topology-audit.md` O10) shows the wake path and
  `classify_locked` read owner/ticket-location independently; the
  in-between window is a real observable state that would require an
  IN_TRANSIT state to model. §4.4 forbids inventing such a state unless
  genuinely exposed. Model C *creates* the exposure Model B avoids.
  **Rejected.**
- **Model D adds machinery (generation tags) for a hazard E8 does not
  have.** A stolen runnable fiber has no active wait registration (E7
  `InvRunnableUnregistered`), so there is no stale-registration race on
  the stolen epoch. Generations solve "is this wake for the current
  epoch?" — a problem that arises only with cancellation/multi-wait
  (E10/E13), not with runnable stealing. **Rejected as over-engineering
  for E8; reserved for E10+ if a real epoch hazard appears.**

**Selected protocol: Model B.** Steal is one abstract transition that
moves the runnable ticket and transfers the runnable owner record. It is
the smallest model that keeps the ticket and the runnable owner record in
agreement across steal, so the thief can pop and run the stolen Fiber and
the subsequent wait-epoch resume owner is captured correctly.

### 9.3.2 Stealable Fiber state

Only `FiberState::runnable` may be stolen. `created`, `waiting`,
`running`, `done` are not stealable.

`pending_spawn_` is **not stealable** in E8. It is the pre-`run` initial-
assignment buffer; stealing from it would conflate "steal" with "initial
owner assignment" (the E8 spec §4.1 forbids silently merging these). The
smallest E8 steals from a worker's `local_runnable` only.

### 9.3.3 Stealable ticket location

The stealable location is **`WorkerState::local_runnable`** of the victim.

The E8-0 audit established that `WorkerState::inbox` (the `std::deque`) is
**dead storage** — no production path pushes to it; all routed tickets go
directly to `local_runnable` under `inbox_mtx`. Therefore the spec's
§4.2 question "is inbox stealable?" is moot for the current
implementation: there is no inbox ticket to steal. E8 steals from
`local_runnable`. If a future revision revives `inbox` as a real
transport queue, this decision must be revisited.

E8 does **not** steal from: wait maps, `current`/running, `done`, or
`pending_spawn_`.

### 9.3.4 Ownership-transfer linearization point

One abstract transition:

```text
Steal(F, W0, W1)
```

Required preconditions:
```text
fiberState[F] = Runnable
owner[F] = W0
ticketLocation[F] = W0Local
W0 != W1
```

Required postconditions:
```text
fiberState[F] = Runnable
owner[F] = W1
ticketLocation[F] = W1Local
```

The transition:
```text
does not create a new runnable ticket
does not call make_runnable()
does not perform Runnable -> Runnable publication
```

It MOVES an existing ticket and TRANSFERS ownership.

### 9.3.5 No abstract in-transit state in E8 baseline

The steal linearizes under `global_mtx_` (the existing coordination
domain that already serializes routing and owner reads — E8-0 O8). The
production sequence is:

```text
lock global_mtx_
  verify F.state == runnable && F.owner == W0
  remove F from W0.local_runnable
  set F.owner = W1            (the new owner record)
  push F to W1.local_runnable
unlock global_mtx_
notify W1.inbox_cv
```

No `IN_TRANSIT`, `STEALING`, or `MIGRATING` state is introduced. The
intermediate "removed from W0, not yet on W1" window exists only while
`global_mtx_` is held, which is the same domain that reads owner/ticket
state — so it is not observable.

### 9.3.5.1 State-indexed authority (E8-FORMAL-CORRECTIVE correction)

The as-built production authority is **state-indexed**, not global. Three
distinct representations carry ownership/execution/resume authority, one
per Fiber phase:

```text
Runnable ownership:
    Scheduler::fiber_owner_[F]  -- the runnable owner record. Records which
    Worker's local_runnable queue currently holds F's runnable ticket.
    Mutated by StealRunnable (victim -> thief). Read by the steal
    eligibility check (try_steal verifies the victim still owns the
    stealable ticket). NOT read by any wake path.

Running execution authority:
    g_worker (TLS) / WorkerState::current -- the Worker currently executing
    F. Set when the worker pops and runs F (run_next_on). Cleared at
    Suspend.

Waiting wait-epoch resume authority:
    WaitReg.owner -- captured as g_worker at suspend time. await_* stores
    WaitReg{me, ws} with ws = g_worker. Read by wake_ready_*_locked to
    route the woken Fiber. This is the routing authority.
```

These three agree at lifecycle transition boundaries (the formal
invariants E8-AUTH-Inv4/Inv5 bind `ownerRecord = execWorker` at Running
and `ownerRecord = waitOwner` at Waiting), but they are **different
production fields**. In particular:

```text
Wake routing reads WaitReg.owner, NOT fiber_owner_.
```

The earlier wording of this section described the owner record as "the
authoritative current-owner source" that "the wake path can read the
current owner." That does not match as-built production: `wake_ready_*
_locked` read `it->second.owner` (the registration's captured
`WaitReg.owner`) and call `route_runnable_locked(f, owner)`; no wake
path references `fiber_owner_`. The `fiber_owner_` record is the
runnable ownership / steal-consistency record, not the routing authority.
The TLA+ model has been corrected to separate `ownerRecord[f]`,
`execWorker[f]`, and `waitOwner[f]`, with `WakeReady` routing by
`waitOwner[f]`. See `docs/spec/e8_ownership_transfer/` and the audit at
`docs/e8-formal-corrective/audit.md`.

### 9.3.6 Wake routing after transfer

After `Steal(F, W0, W1)`, the runnable owner record `fiber_owner_[F]`
becomes W1, so the ticket sits on W1's `local_runnable`. W1 then pops and
runs F; at that point `g_worker = W1` and `WorkerState::current = F`.
When F suspends, `await_*` captures `WaitReg.owner = g_worker = W1`. The
subsequent wake reads `WaitReg.owner = W1` and routes F to W1. The
lifecycle transition protocol preserves consistency between the runnable
owner record, the execution Worker, and the captured wait resume owner;
it does **not** route from `fiber_owner_`.

A stolen Runnable Fiber has **no active wait registration** (E7
`InvRunnableUnregistered`: `fiberState = Runnable => waitReg = None`).
Therefore steal and wake do not race on the same valid Fiber epoch:
**steal is Runnable-only, so a valid steal never races with a live
`WaitReg` for the same Fiber epoch.** The stale-owner hazard exists *only*
in the negative TLA+ model where the steal moves the ticket but does NOT
transfer the runnable owner record: the post-steal state has the ticket on
W1's queue while `ownerRecord` still names W0, violating the local-ticket
/ owner-record agreement (`InvLocalMatchesOwner`) — the runnable
ticket / runnable owner-record split. Production does not read
`fiber_owner_` on wake, so this negative model demonstrates the
runnable-ownership defect class, not a stale `WaitReg` wake route.

If the production code ever permits a wait registration to remain active
on a Runnable Fiber, that is E8-ABORT-4 (a violated E7 publication/wait
invariant) — stop and report, do not patch around it.

### 9.3.7 Steal and blocking admission

Stealable runnable work is MW-S1 work. The E8-0 audit (O7) established
that `classify_locked` already counts every worker's `local_runnable`,
so stealable work is automatically MW-S1-visible with no classifier
change.

An idle Worker must not commit MW-S2 blocking admission while globally
visible stealable work exists at the final admission recheck. This is
already enforced by the E7 two-phase admission protocol
(`FinalAdmissionRecheckAndCommit` requires `MW_S2`, which requires
`~MW_S1`, which requires no `local_runnable` anywhere). E8 reuses the
accepted E7 progress protocol unchanged. E8 does **not** create a second
idle classifier for work stealing.

### 9.3.8 Victim selection

E8 victim selection is non-normative scheduling strategy. The baseline
policy is **round-robin over other workers, first non-empty victim** —
deterministic enough to test. No NUMA, no priority, no affinity. The
work-stealing protocol must not depend on a sophisticated victim
selector.

### 9.3.9 Stability gates

See the E8 spec §13. The load-bearing gates are E8-T3 (steal→wait→wake→
new owner) and E8-T4 (deterministic steal-vs-pop), each 1000/1000, and
E8-T11 (exactly-once stress) 2000/2000 debug / 1000/1000 release. E7
regression suites (`e7_dup_publication_test`, `e7_worker_test`,
`e7_coord_test`) must remain GREEN at their specified repetition counts.

### 9.3.10 Scope boundaries

E8 proves:
```text
runnable-only work stealing
explicit runnable ownership transfer (ticket + fiber_owner_ record)
ticket/owner-record agreement across steal (the steal-consistency record)
steal-vs-pop exclusivity (the consumer of the owner-local ticket becomes executor)
wait-epoch resume routing: wake reads WaitReg.owner (captured g_worker at suspend)
MW-S1 integration (stealable work is MW-S1)
steal is Runnable-only => a valid steal never races a live WaitReg epoch
```

E8 does **not** prove:
```text
stealing a Running / Waiting / Done Fiber
preemptive migration, stack copying, live stack migration
Chase-Lev or any lock-free deque (deferred to E16)
NUMA / priority / affinity
external producer wake (E9)
timer subsystem (E11)
Select / Mutex / Condition / ... (E10/E12)
```

A stackful Fiber may resume on another OS Worker only after being stolen
while Runnable and after its ownership transfer is committed.

## 9.4 E9 Scheduler park admission and unified wake-source protocol (added by E9-ADR)

This section defines the E9 park-admission and wake-source decision. E9
closes the two gaps identified by the E9-0 wake-source topology audit
(`docs/e9-0-wake-source-topology-audit.md`):

```text
GAP-1 (external wake, source W8):
    An external OS thread completes a Future (Future::complete_with sets
    ready_=true) while one or more Scheduler Workers are parked. There is
    no path from that publication to any Scheduler Worker. Recovery today
    is the 1ms inbox_cv timed park (a de-facto periodic poll) or
    caller-driven run() re-entry — both rejected as primary strategies.

GAP-2 (MIXED-WAKE, sources W5 + W8 together):
    backendOutstanding = TRUE and an external-wake-capable wait is
    registered. classify_locked returns MW-S2; the elected participant
    blocks in ctx_.wait_one() on backend progress ONLY. An external-ready
    publication cannot interrupt that wait. For a single-Worker run this
    is a hard stall until the backend completes.
```

E9 defines an explicit park/wake protocol that closes both gaps without
adding a hidden driver thread, without a global singleton, without
polling as the primary strategy, and without weakening E7/E8.

### 9.4.0 Run invocation lifetime contract (added by E9-CORRECTIVE)

**Normative.** Run invocation lifetime is an EXPLICIT protocol dimension,
separate from wake capability. The shipped E9 defect was a semantic
conflation: `external_wake_possible_locked()` was used both for MW-S2
park-domain selection (legitimate wake capability) AND for the MW-S3
run-lifetime decision (park instead of return STALLED). That conflation
broke the E7/E8 Drain contract — a normal `run(1)` with an unresolved
external-wake-capable wait parked forever instead of returning.

E9-CORRECTIVE makes the policy explicit:

```text
enum class RunMode { drain, live };
```

```text
run(worker_count)              -> RunMode::drain  (E7/E8 compatibility)
run_live(worker_count)         -> RunMode::live   (explicit E9 entry)
```

One internal implementation: `run_impl(worker_count, RunMode)`. There is
ONE worker loop, ONE classifier, ONE publication protocol, ONE ownership
protocol, and ONE backend admission protocol. RunMode differs ONLY at the
idle-action selection boundary (after the authoritative global state has
been classified).

#### Mode/state decision matrix

| Global state                          | Drain                               | Live                                |
| ------------------------------------- | ----------------------------------- | ----------------------------------- |
| MW-S1                                 | execute                             | execute                             |
| MW-S2 backend-only                    | E7 backend wait (`ctx_.wait_one`)   | E7 backend wait                     |
| MW-S2 mixed                           | bounded Scheduler-domain observation park | bounded Scheduler-domain observation park |
| MW-S3 + effective external wake       | **RETURN STALLED**                  | PARK (Scheduler domain)             |
| MW-S3 without effective external wake | RETURN STALLED                      | RETURN STALLED                      |
| QUIESCENT                             | RETURN QUIESCENT                    | RETURN QUIESCENT                    |
| shutdown                              | return                              | wake parked Workers + return        |

The distinction is normative: Drain MUST NOT park merely because an
external-wake-capable wait exists; Live MAY remain resident while an
unresolved wait has an effective Scheduler wake source.

```text
state = classify_locked()
action = select_idle_action(run_mode, state, effective_wake_capability)
```

#### Run mode does not modify

```text
MW classification                 (classify_locked unchanged)
runnable publication              (spawn / route_runnable_locked unchanged)
owner transfer / steal            (E8 MOVE + OWNER TRANSFER unchanged)
WaitReg routing                   (wake_ready_*_locked route by WaitReg.owner)
backend outstanding semantics     (ctx_.outstanding() unchanged)
```

Run mode ONLY selects the allowed idle action after the authoritative
global state has been classified.

#### Drain compatibility (mandatory)

The existing `run(...)` API remains Drain-compatible. Existing E7/E8
callers and tests use Drain unless they explicitly opt into Live. Drain
semantics:

```text
MW-S1                -> execute runnable work
MW-S2 backend-only   -> accepted E7 backend-progress admission / wait_one
MW-S2 MIXED-WAKE     -> bounded Scheduler-domain observation park
                        (still governed by the same state classifier)
MW-S3                -> RETURN STALLED
QUIESCENT            -> RETURN QUIESCENT
```

Normative rule:

```text
Drain mode MUST NOT park merely because an external-wake-capable wait exists.
```

The E7/E8 obligations remain valid (`e7_t9_unresolved_wait_not_quiescence`,
the worker-local tests, and the E8 waiting-Fiber tests MUST still return).
Do not rewrite these tests to accept permanent park.

#### Live semantics

```text
MW-S1                              -> execute
MW-S2 backend-only                 -> E7 backend wait_one path
MW-S2 MIXED-WAKE                   -> Scheduler-domain bounded observation park
MW-S3 + effective external wake    -> Scheduler-domain park
MW-S3 without effective external   -> RETURN STALLED
QUIESCENT                          -> RETURN QUIESCENT
shutdown/termination               -> wake parked Workers, RETURN
```

Live does NOT mean "never return until shutdown under every idle state."
It means: remain resident while unresolved logical work has an effective
wake source that can make the Scheduler observable again. No effective
wake source ⇒ RETURN STALLED. No logical work ⇒ RETURN QUIESCENT.

#### Wake handle does not control run mode (mandatory)

```text
SchedulerWakeHandle  = wake capability
RunMode              = invocation lifetime contract
```

The existence, copy count, retention, or destruction of a wake handle
MUST NOT implicitly switch Drain → Live or Live → Drain. Forbidden:

```text
if wake handle attached: keep run alive
else: return
```

Run lifetime is an explicit invocation policy (M4). E9-T1 remains a
Live-mode proof with NO caller-driven re-entry.

### 9.4.1 Architecture decision — Model P3 (decoupled wake domains)

Six candidate models were evaluated against the E9 load-bearing path
(Fiber A awaits an external Future; all Workers park; an external thread
completes the Future; A resumes without caller re-entry) and the
MIXED-WAKE path (§9.4.7).

```text
P1 — Independent Scheduler CV + unchanged backend wait_one:
    external publication -> Scheduler CV; backend-only MW-S2 -> wait_one.
    REJECTED. A MIXED-WAKE participant that enters wait_one is not on the
    Scheduler CV wake set, so an external-ready publication cannot wake it
    (GAP-2 un-closed). The audit (§9.4.7) shows the MW-S2 participant can
    remain in backend wait while external work is ready.

P2 — Wake epoch + Scheduler park, backend progress ALSO signals the epoch:
    All wake-relevant producers (including the backend) increment the
    Scheduler wake epoch and signal.
    REJECTED as primary. It requires the backend to call a Scheduler wake
    callback on every completion. io_uring kernel completions cannot call
    a C++ callback (a CQE is reaped by a Userspace poll, not pushed); so
    P2 either needs a backend driver thread (forbidden, §9.4.10) or
    reduces to P5. ThreadPoolBackend *could* signal, but io_uring cannot,
    so P2 is not uniform across backends.

P3 — Decoupled wake domains (SELECTED):
    Backend progress and external-ready wake are handled by SEPARATE
    parked domains, unified by a single Scheduler wake epoch.
      - At most one Worker (the MW-S2 participant) may block in
        ctx_.wait_one() for backend progress (E7 rule preserved).
      - ANY OTHER idle Worker parks on the Scheduler wake source
        (wake_cv + wake epoch), whose wake set includes external-ready
        publication AND runnable publication AND shutdown.
    Single-Worker correctness (N=1): when an external-wake-capable wait is
    registered, the lone Worker MUST NOT commit to a backend-only
    wait_one; it parks on the Scheduler wake source instead, and backend
    progress is observed by periodic re-poll under the SAME park (a
    bounded timed wait on wake_cv, NOT a busy poll). This keeps N=1
    correct without a second thread.

P4 — Timed park / periodic poll:
    park_for(T); wake; poll everything; repeat.
    REJECTED as primary. It is the current behavior (the 1ms inbox_cv
    wait) and is exactly the workaround E9 §6 forbids choosing. It may
    remain as DEFENSE-IN-DEPTH under the Scheduler wake source (a bounded
    timed wait that re-drains on timeout), never as the authority.

P5 — Unified interruptible backend wait seam:
    wait for: backend progress OR Scheduler wake.
    REJECTED as primary (would be sound, but costs a backend-facing seam
    change). It requires AsyncBackend::wait_one to be interruptible by an
    external signal on all three backends. io_uring would need an
    eventfd registered in the ring; ThreadPoolBackend would need its cv_
    tied to the Scheduler wake source; FakeBackend does not block. This
    is a real BACKEND-WAKE-SEAM-GAP, but it is LARGER than the minimum
    protocol-enabling change. P3 closes the same gaps with a
    Scheduler-internal seam only; P5 is reserved if P3 proves insufficient
    under the formal gate or the load-bearing tests.

P6 — OS multiplexed wake source (eventfd/pipe in io_uring, etc.):
    REJECTED for E9. Protocol-first: P3 expresses the protocol without
    selecting an OS mechanism. eventfd-in-ring is an implementation of P5
    for the io_uring backend specifically, deferred until P3 is proven
    insufficient.
```

**Selected protocol: Model P3 (decoupled wake domains).** It is the
smallest protocol that closes both gaps, preserves the E7 at-most-one
backend-wait-participant rule, does not require a backend-facing seam
change, and is correct for N=1 (the single-worker Evented case that is
the project's load-bearing proof, ADR §9.1).

### 9.4.2 Park admission

A Worker must not park from a local-empty observation. Park admission is
globally coordinated. The phases (names map to the TLA+ actions,
`docs/spec/e9_park_wake/`):

```text
ACTIVE
    -> (no local work; re-drain under global_mtx_; classify)
PARK_CANDIDATE        [BeginParkCandidate]
    -> (final persistent-readiness drain under global_mtx_)
    -> (final global classifier recheck under global_mtx_)
    -> (observe wake epoch E; record observedEpoch[w] = E)
    -> validate: if current epoch != E, abandon (do not park)
PARK_COMMITTED        [FinalParkRecheckAndCommit]
    -> choose park domain (see §9.4.3)
    -> release global_mtx_
    -> physical park on the chosen domain [EnterPhysicalPark]
    -> (wake returns) [LeavePark]
    -> re-drain; reclassify; loop
```

Park admission REUSES the E7 two-phase admission shape (NONE → CANDIDATE
→ COMMITTED) but the COMMITTED decision is now gated by the wake-epoch
validation, not merely by the MW-S2 reclassify. A publish between
CANDIDATE and COMMITTED that routes runnable work demotes admission (E7
rule, preserved); a publish that only advances the wake epoch (e.g. an
external-ready flag set with no runnable ticket yet) is caught by the
epoch validation at COMMIT.

### 9.4.3 Park domain selection (the P3 rule)

```text
Let W be the Worker about to enter physical park (PARK_COMMITTED).

IF W is the elected MW-S2 participant (E7 two-phase admission COMMITTED
   on backend progress) AND no external-wake-capable wait is registered:
    park domain = BACKEND   (ctx_.wait_one(), E7 rule unchanged)

ELSE IF W is the elected MW-S2 participant AND an external-wake-capable
   wait IS registered:
    park domain = SCHEDULER  (do NOT enter backend-only wait_one)
    -- this is the MIXED-WAKE fix: the lone backend-wait participant
       yields its backend-wait privilege when external wake is possible,
       so external-ready can wake it. Backend progress is still observed
       by the bounded timed wait on the Scheduler wake source.

ELSE (W is an idle non-participant, or single-worker with external wake):
    park domain = SCHEDULER  (wake_cv + wake epoch)
```

In all cases the Scheduler-domain park is a `condition_variable` wait on
`wake_cv_` with a **bounded timeout** (defense-in-depth, not the
authority). The wake epoch is the authority; the timeout only re-drains
on the (correctness-irrelevant) case of a lost wake.

At most one Worker may be in the BACKEND park domain (E7 Inv6 preserved,
E9-Inv6). Any number of Workers may be in the SCHEDULER park domain.

### 9.4.4 Wake obligation

A publication creates a wake obligation when it makes previously
non-executable persistent state capable of producing executable Scheduler
work while one or more Workers may be parked.

```text
For every accepted wake source: publish persistent state FIRST,
then signal the Scheduler wake source SECOND. The wake signal is
advisory; persistent state is authoritative.
```

Classification:

```text
runnable publication (W1/W2/W3):
    obligation: YES. route_runnable_locked already clears global_terminate_
    and notifies inbox_cv; E9 ALSO advances the wake epoch and notifies
    wake_cv_ so a SCHEDULER-parked Worker resumes. (Production today
    notifies inbox_cv only; E9 adds the wake-epoch path.)

external persistent-ready transition (W8):
    obligation: YES. The external producer calls the Scheduler wake handle
    (notify_external_wake), which advances the wake epoch and notifies
    wake_cv_. The producer MUST NOT call make_runnable / route_runnable /
    mutate any Scheduler queue (§9.4.9).

backend progress (W4/W5/W6):
    obligation: the MW-S2 BACKEND-domain participant observes it directly
    via wait_one return. If the participant is in the SCHEDULER domain
    (MIXED-WAKE case), backend progress is observed by the bounded timed
    wait's timeout re-drain, OR by the wake handle if the backend is later
    wired to signal it (P5, deferred). The backend does NOT call the
    Scheduler wake handle in the E9 baseline.

shutdown/termination (W9):
    obligation: YES. global_terminate_ advances the wake epoch and notifies
    wake_cv_ + inbox_cv, waking every parked Worker regardless of domain.
```

### 9.4.5 Wake epoch / generation semantics

```text
wakeEpoch : monotonically non-decreasing counter (std::atomic<uint64_t>),
            protected by wake_mtx_ (the wake_cv_ mutex).

A Worker about to enter the SCHEDULER park domain, under wake_mtx_:
    observedEpoch[w] = wakeEpoch.load()
    if (drain predicate false) {  // spurious / already-serviced
        do not park; loop
    }
    wake_cv_.wait_for(lk, bounded_T, [&] {
        return wakeEpoch.load() != observedEpoch[w]
            || global_terminate_
            || !local_runnable.empty();
    });

A producer (internal or external):
    publish persistent state
    { lock wake_mtx_; ++wakeEpoch; }
    wake_cv_.notify_all()   (or notify_one; see §9.4.8)
```

The epoch is the authority for "did a wake-relevant publication happen
after I decided to park?" The cv/notify is the physical delivery. A
consumed/coalesced/spurious wake does NOT erase persistent state
(E9-Inv3); the Worker re-drains on every wake.

### 9.4.6 Lost-wake closure

The protocol closes every interleaving of publish-vs-park:

```text
(a) publish before CANDIDATE:
    the publication routes runnable / sets the flag; the candidate's
    final drain observes it; admission is abandoned (E7 rule).

(b) publish after CANDIDATE, before COMMITTED (epoch observed):
    the publication advances wakeEpoch. The candidate's epoch validation
    at COMMIT observes current != observed (observed not yet recorded, so
    validation uses a fresh read) and abandons. [modeled as the
    FinalParkRecheckAndCommit epoch check]

(c) publish after COMMITTED, before physical sleep:
    the Worker has recorded observedEpoch[w] = E under wake_mtx_ and is
    about to wait_cv.wait. The producer's ++wakeEpoch happens-before its
    notify; the Worker's wait predicate sees wakeEpoch != E and does NOT
    block. (classic condition-variable pre-wait race, closed by the
    epoch predicate under the wake mutex.)

(d) publish while sleeping:
    the producer's notify wakes the Worker; epoch != observed; re-drain.

(e) spurious wake:
    the Worker re-drains; if nothing changed it may re-park. No duplicate
    runnable publication (make_runnable is exactly-once, E7-T2).

(f) coalesced multiple wakes:
    multiple producers advance wakeEpoch and notify; the Worker wakes
    once, drains ALL persistent ready state (wake_ready_*_locked scans
    every registered waiter), and routes every ready Fiber. One wake per
    publication is NOT required.
```

### 9.4.7 MIXED-WAKE semantics

When `backendOutstanding = TRUE` AND an external-wake-capable wait is
registered, the protocol is explicit (no implicit behavior):

```text
The MW-S2 participant does NOT enter a backend-only wait_one. It parks
on the SCHEDULER wake domain (§9.4.3). Its wake set therefore includes
external-ready publication. Backend progress is observed by the bounded
timed wait's re-drain; external-ready is observed immediately on wake.
```

This is the load-bearing difference from E7 MW-S2: E7 admits backend
wait_one whenever backendOutstanding; E9 overrides that ONLY when an
external-wake-capable wait is registered. When no such wait is registered,
E7 MW-S2 behavior is unchanged (E9-T10 preserves the E7-T5 proof).

"External-wake-capable" is determined structurally: a wait registered in
`waiting_ready_` whose flag address was handed a live
`SchedulerWakeHandle` is external-wake-capable. (In the E9 baseline every
`waiting_ready_` registration is treated as external-wake-capable; a
finer distinction is deferred.)

#### 9.4.7.1 Bounded Scheduler-domain observation is normative (E9-CORRECTIVE)

The backend wake domains are PHYSICALLY DISJOINT from the Scheduler wake
condition variable in the E9 baseline:

```text
Scheduler wake source:    runnable publication, external Future wake signal, shutdown
backend wake source:      ThreadPoolBackend cv, io_uring CQE/wait_one, Fake poll/staging
```

For the E9 baseline, the Scheduler does NOT add callbacks to every backend
(no eventfd, no epoll, no io_uring wake integration, no dedicated backend
driver thread, no new universal backend wake API). The locked baseline is:

```text
backend-only MW-S2              -> ctx_.wait_one()
Scheduler-domain external MW-S3 -> wake-epoch park (Live mode only)
MIXED-WAKE                      -> bounded Scheduler-domain observation park
```

The bounded observation park waits for:

```text
wake epoch change   OR   shutdown   OR   appropriate Scheduler signal
OR   observation interval expiry
```

After any return it: drains backend persistent readiness, drains external
persistent readiness, and reclassifies globally.

**The bounded observation interval is LOAD-BEARING for backend progress
latency in MIXED-WAKE.** In MIXED-WAKE Scheduler-domain park, backend
readiness does NOT directly signal the Scheduler wake source in the E9
baseline; backend readiness is observed through the bounded observation
return. Therefore the observation interval is protocol authority for
backend observation in this mode, NOT "defense in depth only."

The current observation interval is **2 ms**
(`park_on_wake_source` `wake_cv_.wait_for(... 2ms ...)`). It is an
implementation policy; tests must NOT depend on an exact 2 ms scheduling
race (deterministic seams are used for causal proof, §9.4.15).

The unification across disjoint wake domains is:

```text
post-observation drain + authoritative global reclassification
```

NOT one physical wake primitive. Do not claim unified physical wake
semantics. Future optimization to OS-multiplexed wake (eventfd-in-ring)
belongs to E16/runtime hardening unless separately reprioritized.

### 9.4.8 Worker notification cardinality

```text
wake_cv_.notify_all() on every wake obligation (initial E9 baseline).
```

Correctness-first: notify_all guarantees every SCHEDULER-parked Worker
observes the epoch change. wake_one would be a performance refinement
requiring a "which Worker should resume this publication" decision the
Scheduler does not currently make (runnable publication routes to an
OWNER, but external-ready is drained by whichever Worker wakes first).
The conservative notify_all baseline is documented; a later commit may
refine to wake-one-per-routed-owner. Wake coalescing (E9-Inv5) means
notify_all is not one-wake-per-ticket.

### 9.4.9 External producer boundary (mandatory review invariant)

```text
external producer MAY:
    publish persistent readiness (Future::complete_with -> ready_=true)
    signal the Scheduler wake source (SchedulerWakeHandle::notify)

external producer MUST NOT:
    mutate local_runnable
    call make_runnable on a waiting Fiber
    erase Scheduler wait registration
    route a Fiber to a Worker
    call any AsyncIoContext / AsyncBackend method
```

Scheduler Workers remain the only domain that drains registration,
performs waiting→runnable, publishes runnable tickets, and routes by
WaitReg.owner. This preserves E5's active-wake race closure and E7's
duplicate-publication bug class closure.

### 9.4.10 Wake-handle / notification lifetime

The external producer holds a `SchedulerWakeHandle` (or token) that is
**generation-invalidated**, not a raw `Scheduler*`. The handle is
issued by the Scheduler and stored as `shared_ptr`/`weak_ptr` semantics:

```text
SchedulerWakeHandle:
    - holds a control block: { weak_ptr<SchedulerWakeState>, uint64_t gen }
    - notify(): locks the weak control; if the Scheduler is alive AND gen
      is current, advances wakeEpoch + notify_all; otherwise no-op.
    - invalidated by Scheduler destruction (control block's parent weak
      expires) and by any future gen-bump (reserved for E10+).
```

Answers (E9 spec §19):

```text
Can producer retain notifier after Scheduler destruction?
    YES, safely — notify() is a no-op (weak_ptr expired).
Can a Future outlive Scheduler?
    Contract: no (caller-owned, ADR §5 L1-L3c). If it does, the handle's
    notify is a no-op; no use-after-free.
Can Scheduler be destroyed with an Evented wait registered?
    YES — the wait maps are destroyed with the Scheduler; any later
    producer notify() is a no-op via the weak control block.
Who invalidates the notifier?
    Scheduler destruction (weak expiry). No explicit invalidation call.
Is signal after invalidation a no-op, error, or contract violation?
    NO-OP. (A raw Scheduler* callback would be use-after-free; the weak
    handle makes it safe.)
```

Do NOT use a raw `Scheduler*` callback from arbitrary producer threads.
The weak/generation handle is the minimum proven lifetime contract.

### 9.4.11 Park state is not logical quiescence

```text
all Workers parked  does NOT imply  QUIESCENT.
```

The logical work classifier (`classify_locked`) remains authoritative. A
parked Worker is not logical work; a wake signal/epoch is not logical
work. An unresolved registered wait is still MW-S3 (E7 §9.2.6). E9 does
NOT collapse all-parked into quiescent (E9-Inv8).

### 9.4.12 BACKEND-WAKE-SEAM-GAP classification

E9 inspected whether `AsyncBackend::poll` / `wait_one` can express "wait
for backend progress OR Scheduler wake":

```text
FakeBackend:      wait_one does not block; N/A.
ThreadPoolBackend: wait_one blocks on its own cv_; could be tied to the
                   Scheduler wake source (a P5 seam repair) but is NOT
                   required under P3 (the MW-S2 participant parks on the
                   Scheduler domain instead when external wake is
                   possible).
io_uring:         wait_one blocks in io_uring_submit_and_wait (kernel);
                   a CQE cannot call a C++ callback. P5 would require
                   registering an eventfd in the ring. NOT required under
                   P3.
```

**Classification: BACKEND-WAKE-SEAM-GAP is real but NOT protocol-blocking
under P3.** The gap is sidestepped by parking the MW-S2 participant on
the Scheduler wake domain (not the backend) whenever external wake is
possible. A backend seam repair (P5) is reserved as E9-B1 only if the
formal gate or the load-bearing tests prove P3 insufficient. It is NOT
added speculatively.

### 9.4.13 Scope boundaries

E9 proves:

```text
explicit park admission with wake-epoch validation
Scheduler wake source (wake_cv + wake epoch) for external + runnable wake
external-thread Future completion wakes a parked Scheduler (no re-entry)
MIXED-WAKE closure (external wake not blind behind backend wait)
wake coalescing, spurious-wake safety
shutdown wakes parked Workers
park state is not quiescence
N=1 correctness (single-worker external wake, no second thread)
E7/E8 invariants preserved (steal = MOVE + OWNER TRANSFER; MW rules)
```

E9-CORRECTIVE additionally proves:

```text
explicit RunMode (Drain | Live) invocation lifetime contract (§9.4.0)
Drain MW-S3 returns STALLED (E7/E8 drain compatibility restored)
Live MW-S3 + effective external wake may remain resident
Live MW-S3 without effective external wake returns STALLED
WakeHandle does not control run mode (no hidden semantic switch)
bounded Scheduler-domain observation is normative in MIXED-WAKE (§9.4.7.1)
deterministic park seams replace sleep-based race proofs (§9.4.15)
```

E9 does NOT prove:

```text
E10 WaitNode / cancellation-safe wait queue
a backend-facing interruptible wait seam (P5; reserved as E9-B1)
eventfd-in-ring (P6; deferred)
wake_one routing refinement (notify_all baseline)
timers (E11)
per-wait producer provenance / deadlock detection
arbitrary external-thread submit_/cancel (E7 §9.2.8 still out of scope;
    only Future::complete_with + wake-handle signal from external threads)
```

### 9.4.14 Abort conditions (E9-ABORT-1 .. E9-ABORT-10)

Stop and report `E9: BLOCKED` if any of the selected protocol's
invariants cannot be met. These map 1:1 to the E9 spec §25 set and are
checked by the formal gate (§9.4 of the ADR) and the load-bearing tests
(E9-T1 .. E9-T14).

E9-CORRECTIVE adds (C-ABORT-1 .. C-ABORT-12): stop and report
`E9-CORRECTIVE: BLOCKED` if Drain can park indefinitely on MW-S3, if Live
requires rewriting E7/E8 publication/ownership, if a wake handle
implicitly controls RunMode, if Drain and Live require separate
classifiers or worker loops, if MIXED-WAKE external-ready progress
depends on backend completion, if the bounded observation timeout is
load-bearing but omitted/denied in ADR/refinement, if T3/T4 causal proof
still relies on timing sleeps, if the shipped Drain-park defect cannot be
reproduced by the negative formal model, if `local_runnable` is read
concurrently under an unrelated synchronization domain, if the normal
debug/release E7/E8 hang remains, or if required E9-T8..T14 obligations
remain absent.

### 9.4.15 Deterministic park seams (added by E9-CORRECTIVE)

Race proofs MUST use deterministic causal seams, not `sleep_for`
(M7). Two narrow TEST-only seams are added at the load-bearing causal
boundaries of park admission:

```text
seam A — after ParkCandidate (Phase-B recheck boundary)
seam B — after park commit / immediately before the physical wait
```

Each seam is a mutex + condition_variable (or latch) that pauses the
Worker at the exact causal boundary without modifying Scheduler state.
The test releases the seam only after the producer has published
persistent readiness and signaled the wake epoch. This proves the
commit-to-physical-wait window is closed by the epoch predicate, not by
timing luck. Stress repetition is gathered AFTER these deterministic
proofs.

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
              E9 park/wake [CLOSED] → E10 WaitNode → E11 timers → E12 sync
              primitives → E13 Select → E14 parity + runtime closure.
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
