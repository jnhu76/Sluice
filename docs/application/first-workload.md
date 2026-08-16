# First Application Workload (Post-Freeze Design)

**Status:** DESIGN (2026-08-16) — not implemented in the R0/R1 pass
**Purpose:** validate that a real application can live entirely on the
public/application layer without entering Foundation internals
(`docs/post-freeze/structural-audit.md` §8, task §22–§23).

---

## 1. The workload: bounded parallel file pipeline ("sluice-pipeline")

A CLI daemon that copies/transforms a set of files through a bounded
concurrency pipeline:

```text
discover input files
    -> admit at most K concurrent pipeline tasks (Semaphore)
    -> per task: streaming read (deadline-bounded) -> transform -> streaming
       write + fsync policy -> propagate Result
    -> Group::await aggregation; first hard error requests cooperative cancel
       of the remaining tasks (CancelToken), soft errors are collected
    -> graceful shutdown: stop admission (close runtime barrier), drain
       accepted tasks, join workers, report
```

Deliberately **not** a network server: Sluice has no sockets (DIV-08, `X` in
the conformance map). Networking is the canonical *second* workload and the
application-evidence trigger for any networking API discussion.

Why this workload exercises the load-bearing surface:

| Requirement | Exercised capability |
|---|---|
| file streaming | positional/stream `ReadOp`/`WriteOp` via `RuntimeTaskContext::submit_*` |
| durability decision | `SyncDataOp`/`SyncAllOp` as first-class ops (DIV-06) |
| concurrency bound | `Semaphore` + `Group` + multi-worker `Scheduler` (steal, park/wake) |
| per-task timeout | `deadline_t` + `await_*_deadline` / timer select case (E11) |
| multi-source wait | `select()` over Event (pipeline tick) + Timer (deadline) |
| cancellation | `CancelToken`/`CancelProtection` cooperative cancel + `cancel_waiter` on an op |
| graceful shutdown | `ApplicationRuntime::shutdown/drain/join` lifecycle (ADR-application-runtime) |
| error propagation | `Result<T>`/`IoError` end-to-end; no exceptions across the seam |
| backend variance | same app on `ThreadPoolBackend` and real-liburing `UringAsyncBackend` |

## 2. Which cppio APIs it uses (public surface only)

`RuntimeBuilder` / `ApplicationRuntime` / `RuntimeTaskContext`
(`application_runtime.hpp`); `Group`, `Future`, `Semaphore`, `Event`,
`select()`, `CancelToken/CancelGuard` (`group.hpp`, `future.hpp`,
`semaphore.hpp`, `event.hpp`, `select.hpp`, `cancel.hpp`); `TimerRegistration`
(`timer_registration.hpp`); `AsyncIoContext` + `Completion<T>` +
`ThreadPoolBackend`/`UringAsyncBackend` (`async_io_context.hpp`,
`completion.hpp`, backends); sync core `Reader/Writer/copy` for the
transform stage (`reader.hpp`, `writer.hpp`, `copy.hpp`).

**Foundation modules touched: NONE.** No file under `src/async/scheduler*.cpp`,
`include/sluice/async/detail/request_arena.hpp`, backend sources, or `include/sluice/async/`
headers should need edits. That is the pass/fail bar of this spike.

## 3. Expected friction points (record as evidence, do not pre-fix)

Per task §23, anything found lands in the ledger as
**Application Evidence Candidate** — not as a foundation change:

1. **No socket/poll primitive** — expected (DIV-08); networking workload will
   be the evidence source.
2. **No directory enumeration / fs_event** — the discovery stage will need
   plain `std::filesystem` outside the runtime; fine for v1, evidence for a
   future decision.
3. **`select()` lacks an I/O-Completion case** (issue #99): deadline + tick
   select works; awaiting "first of N ops" must compose via Futures — v1
   accepts the composition cost; the app ledger records the ergonomics.
4. **No public `sleep_for`/`Timeout` convenience** (PF-3): deadlines compose,
   with more ceremony than Zig's `Io.Timeout`.

## 4. Acceptance evidence the spike must produce

- The app compiles/links against installed public headers only.
- A failing-to-read input produces `Result` error propagation to the
  aggregation point — no crash, no hang.
- SIGTERM-equivalent path: admission closes, accepted tasks drain, join
  returns, destructors run quiescent (§14 contract from the app side).
- Deadline expiry mid-read cancels the wait (not the syscall), and the real
  syscall result still wins verbatim (§11 contract from the app side).
- Same binary behaves identically on ThreadPool and real-liburing backends
  (conformance manifest philosophy, app-level).

## 5. Suggested shape

`apps/sluice-pipeline/{main.cpp,pipeline_task.cpp,admission.cpp}` following
the existing `apps/sluice-copy` target pattern (`xmake/apps.lua`), linking
`sluice_async` + `sluice_core`. Tests: a contract test driving the pipeline
against `FakeAsyncBackend`/scripted backend, plus a real-file integration
test mirroring `sluice_copy_pipeline_*` tests.
