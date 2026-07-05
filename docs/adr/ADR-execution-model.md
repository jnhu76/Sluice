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
