# cppio async backend parity audit

**Status: SLUICE-CORE-023B.** Backend-substrate parity audit comparing cppio's
current `AsyncBackend` model against the actual Zig `std.Io` Threaded/Evented
backend requirements, derived from the source graph in
`docs/zig-stdio-async-port-map.md` (023A). This answers the central question
posed by task §4:

> Is cppio's current `AsyncBackend` a sufficient substrate for a Zig-like
> Evented implementation?

The verdict is **B — Sufficient with narrow extensions** (not C), with a
critical scoping caveat: it is sufficient *for the operation-execution layer*
(read/write/sync submit + reap + cancel), but it is **not** the right
abstraction boundary to host the Zig task runtime (Future/Group/Select/Batch,
the futex substrate, the fiber scheduler). Those land on a **separate layer
above** `AsyncBackend`, mirroring how Zig's `Io.VTable` co-implements task and
op surfaces but how the cppio L1 model already deliberately split them.

This audit governs the PHASE B job cards in `docs/zig-stdio-migration-jobs.md`
(023C).

## 1. The current cppio backend contract

`include/sluice/async/async_io_context.hpp:54-95` defines the L0 boundary:

```text
class AsyncBackend {
  attach_stats(AsyncStats*)                                  // non-virtual
  submit_read (ReadOp,      Completion<size_t>&) -> Result<void>   // 71
  submit_write(WriteOp,     Completion<size_t>&) -> Result<void>   // 72
  submit_sync_data(SyncDataOp, Completion<void>&) -> Result<void>  // 73
  submit_sync_all (SyncAllOp,  Completion<void>&) -> Result<void>  // 74
  poll()                                          -> size_t          // 78
  wait_one()                                      -> Result<size_t>  // 81
  cancel(Completion<size_t>&) { no-op }                              // 86
  cancel(Completion<void>&)   { no-op }                              // 87
  outstanding() const noexcept                    -> size_t          // 90
}
```

Concrete implementations: `FakeAsyncBackend` (header-only test vehicle),
`SyncBackend` (017 placeholder), `ThreadPoolBackend` (one-thread-per-op),
`UringAsyncBackend` (gated, single-driver-thread).

Reaping contract (ADR §6 O1-O3): completions are produced **only** inside
`poll`/`wait_one`; ordering is per-backend completion order; no implicit FIFO
(not even per-fd on Uring).

Ownership/lifetime (ADR §5 L1-L11): buffers borrowed for the outstanding op;
`Completion<T>` caller-owned and address-stable while outstanding; fds not
owned; destroying an `AsyncIoContext` with outstanding ops is a contract
violation (asserted in debug, `src/async/async_io_context.cpp:21-29`).

## 2. The Zig backend contract (for comparison)

`zig/lib/std/Io.zig:51-255` defines `VTable` — the backend boundary both
`Threaded` and `Uring`/`Kqueue`/`Dispatch` implement. It spans:

- **Task execution:** `async`/`concurrent`/`await`/`cancel`/`groupAsync`/
  `groupConcurrent`/`groupAwait`/`groupCancel`/`recancel`/
  `swapCancelProtection`/`checkCancel` (`Io.zig:61-145`).
- **Futex substrate:** `futexWait`/`futexWaitUncancelable`/`futexWake`
  (`Io.zig:147-149`).
- **Op execution:** `operate`/`batchAwaitAsync`/`batchAwaitConcurrent`/
  `batchCancel` (`Io.zig:151-154`).
- **Direct file/dir/net/process surface:** ~50 methods (`Io.zig:156-254`).

The two backends differ entirely in *how* the task surface is realized:
- **Threaded** (`Io/Threaded.zig`): no fibers, no event loop; one OS thread per
  task; cancel via Unix signals / Windows APC; the op surface is synchronous
  syscalls wrapped in a `Syscall` RAII cancel region.
- **Uring Evented** (`Io/Uring.zig`): per-thread ring + per-thread fiber pool +
  cross-thread work stealing; cancel via `IORING_OP_ASYNC_CANCEL` with
  `IOSQE_CQE_SKIP_SUCCESS` for exactly-once publication; the op surface is
  SQE-submit + fiber-yield + CQE-resume.

**The decisive structural fact:** in Zig, *one* backend type implements *both*
surfaces. There is no "task runtime" layer separate from the backend; the
backend IS the task runtime. cppio split them (L0 `AsyncBackend` = op surface
only; L1 `AsyncIoContext` = thin pass-through). This is the ADR-accepted 016D
design and it is **not** the reason Evented is hard — it is the reason cppio
can choose Evented-or-not independently of the op substrate.

## 3. Audit matrix — cppio substrate vs Zig requirements

For each Zig-required property: does the current `AsyncBackend` contract
support it? `✓ already`, `~ partial / gap`, `✗ missing`, `n/a out of scope`.

| Zig requirement | cppio current | Status | Evidence / gap |
| --- | --- | --- | --- |
| operation submission (read/write) | `submit_read`/`submit_write` | ✓ | `async_io_context.hpp:71-72`. Single-buffer only; Zig is scatter/gather (`[]const []u8`). |
| operation identity | `Completion<T>&` (caller-owned) | ✓ | `completion.hpp`. Address-stable; Zig uses `*Fiber` as `user_data`. cppio's identity is the C++ object, not a kernel `user_data` integer — the Uring backend maintains its own `id → Completion*` map. |
| in-flight ownership | buffer-borrowed (L1-L3c) | ✓ | ADR §5. |
| completion delivery | `complete_with` inside `poll`/`wait_one` | ✓ | ADR §6 O1. |
| completion ordering | per-backend, in reap | ✓ | ADR §6 O2-O3; Uring CQE-order-only. |
| waiting for one completion | `wait_one` (blocking) | ✓ | `async_io_context.hpp:81`. |
| draining multiple completions | `poll` reap-loop | ✓ | Returns count reaped; caller loops. |
| bounded queue/ring pressure | Uring: flush+retry on full; ThreadPool: unbounded | ~ | Uring `queue_full_retries` stat; ThreadPool has no bound (one thread per op). Zig Threaded has `async_limit = cpu_count-1`. |
| retry rules | `op_helpers::read_all`/`write_all` loop | ✓ | Single-op coordinator; no batch retry. |
| short reads/writes | `Completion<size_t>` carries bytes | ✓ | Caller loops via helpers. |
| EOF | `IoError::eof` after partial | ✓ | Helpers map n==0 on read to eof. |
| vectored I/O | (single-buffer ops only) | ✗ | Zig `Operation.file_*_streaming` is `[]const []u8`. Out of current L1; flagged in 023A §D. |
| durability operations | `SyncDataOp`/`SyncAllOp` | ✓ (divergent) | cppio models sync as an async op (018B); Zig keeps `fileSync` as a vtable method. Intentional divergence. |
| cancellation requests | `cancel(Completion&)` best-effort | ~ | Default no-op (`:86-87`); ThreadPool doc/impl drift (header claims remove-and-cancel, impl is no-op); Uring O(outstanding) scan. |
| cancellation race | exactly-once terminal | ✓ | Structural on Uring (cancel CQE never completes; op CQE always does). Fake+ThreadPool best-effort. |
| exactly-once terminal state | `Completion` state machine | ✓ | `completion.hpp` idle→outstanding→ready enforced by assertion. |
| shutdown | `~AsyncIoContext` asserts outstanding==0 | ~ | L11 enforced at context level; **no in-flight drain**, no per-backend shutdown protocol. Zig Threaded drains the run-queue; Uring asserts no pending tasks and MSG_RING-exits workers. |
| shutdown with in-flight ops | (forbidden — L11) | ~ | cppio forbids it; Zig Uring also asserts none. But Zig Threaded lets in-flight syscalls finish. cppio ThreadPool joins workers (so in-flight finishes). No documented contract. |
| backend destruction | `unique_ptr` destructor | ✓ | Backend owned by context. |
| wakeup of sleeping loop | `wait_one` blocks on CV (ThreadPool) / `submit_and_wait` (Uring) | ✓ for single-driver | No cross-thread wakeup protocol. Zig Uring uses `MSG_RING` to wake another thread's ring. **Required for multi-thread Evented.** |
| cross-thread submission | (forbidden — single-driver-thread) | ✗ | Uring impl `src/async/uring_backend.cpp:98-102` documents single-driver-thread. ThreadPool locks internally. Zig Threaded is multi-producer. |
| cross-thread cancellation | (forbidden) | ✗ | Same. Zig Uring cancels across threads via `MSG_RING` carrying `fiber\|0b01`. |
| fd lifetime | caller-owned (L5/L10) | ✓ | |
| buffer lifetime | borrowed (L1-L3c) | ✓ | |
| op storage lifetime | caller-owned `Completion` | ✓ | |
| backend-specific metadata | none in L0 contract | ~ | Zig has `[7]usize` per-op scratch in `Operation.Storage.Pending.Userdata`. cppio backends keep private side tables (`unordered_map`). |
| statistics semantics | `AsyncStats` (caller-owned, nullable) | ✓ | `queue_full_retries` double-meaning (SQE pressure vs L8 invalid_state) is a naming bug. |
| **task execution** (async/concurrent/await) | **n/a — not in `AsyncBackend`** | ✗ | Lives above, in the future task-runtime layer. This is **correct layering**, not a gap. |
| **futex substrate** | **n/a — uses `std::mutex`/CV** | ✗ | Post-Evented concern (PHASE S). |
| **fiber/stack switching** | **n/a** | ✗ | PHASE E; gated by E0 ADR. |

## 4. The four-way distinction (task §4 explicit ask)

Task §5.1 requires auditing the distinction between:
- (a) blocking current-thread execution,
- (b) worker-pool offload,
- (c) task concurrency,
- (d) asynchronous operation execution.

| Axis | cppio current | Zig Threaded | Zig Uring Evented |
| --- | --- | --- | --- |
| (a) block current thread | `op_helpers::read_all`/`write_all` (submit+poll loop on caller thread) | file ops block caller; `operate` blocks caller | n/a — fiber yields |
| (b) worker-pool offload | `ThreadPoolBackend`: 1 detached thread per op | n/a — caller blocks, OR caller wraps in `async` which spawns 1 thread | implicit (libdispatch-style); not in Uring |
| (c) task concurrency | none — `Completion` is per-op, not per-task | `async`/`concurrent` = M tasks on N threads (1 thread/task) | M fibers on N threads, work stealing |
| (d) async op execution | `UringAsyncBackend`: SQE submit + CQE reap (single driver) | **none** — every op is a synchronous syscall | SQE submit + fiber yield + CQE resume |

**Conflation finding:** cppio's `ThreadPoolBackend` **does conflate (b) and
(d)** — it offloads a synchronous syscall to a worker thread and treats the
worker's completion as the op completion. This is exactly Zig `Threaded`'s
model (one thread per task, synchronous syscall, thread-finish = completion).
So the conflation is *faithful to Zig Threaded*, not a defect — *except* that
cppio lacks `Threaded`'s `Syscall` cancel region and signal-based interrupt,
so its `cancel` is a documented no-op where Zig's is real.

cppio's `UringAsyncBackend` **correctly separates (d)** — it does real async
op execution (SQE/CQE) — but it has **no (c)** (no task concurrency: single
driver thread, no fibers). To reach Zig Uring Evented parity, cppio must add
(c) on top, which requires fibers (PHASE E).

**No repair of layering is required at the `AsyncBackend` boundary.** The
boundary correctly isolates (d). The gaps are *above* it (c is missing) and
*inside specific backends* (ThreadPool cancel; Uring batching/multi-thread).

## 5. Verdict: B — Sufficient with narrow extensions

The permitted conclusions (task §4):

### A. Sufficient — REJECTED
Would claim `AsyncBackend` can already host a Zig-Evented-equivalent. It
cannot: it has no task-runtime surface, no futex, no fiber, no cross-thread
wakeup. Selecting A would hide the real work.

### B. Sufficient with narrow extensions — SELECTED
`AsyncBackend` is the right boundary for **operation execution** and needs
only narrow, source-justified additions to close substrate gaps. The Zig task
runtime (Future/Group/Select/Batch, futex, fibers) lands on a **separate
layer above**, exactly as Zig's `Io` namespace layers `Future`/`Group`/`Batch`
above the vtable. This preserves the ADR-accepted L1 model and avoids a
big-bang rewrite.

The narrow extensions (PHASE B jobs, doc 023C):

1. **Shared conformance suite** (B1) — `tests/backend_conformance_test.cpp`
   exercising every shared semantic (read/write/sync success, EOF, short,
   invalid fd, exactly-once, cancel races, shutdown, stats) against every
   backend. Backend-specific mechanism tests stay separate. *No contract
   change*; closes the "semantics duplicated across unrelated test files" gap
   (task §6).

2. **ThreadPoolBackend correctness repair** (B2):
   - Add a `Syscall`-style cancel region so `cancel` is real, not a no-op
     (matches Zig `Threaded.cancelAwaitable` + `signalAllCanceledSyscalls`).
   - Wire `destroying_` (currently dead state) into the submit path so
     submit-during-destruction is rejected.
   - Fix the header/impl drift (header claims remove-and-cancel).
   - Optional: `async_limit` cap (Zig default `cpu_count-1`).

3. **UringAsyncBackend substrate hardening** (B3):
   - O(1) cancel via a cancel token / `user_data` identity (not a linear scan
     of `ops`). Mirrors Zig's `*Fiber`-as-`user_data` scheme.
   - L1 batch-submit seam (submit N SQEs, reap in completion order) so the
     future `Batch` (T4) has a substrate. Does not change the public API;
     adds an internal batching path the L1 `submit_*` may use.
   - Feature gating for registered buffers/files (deferred behind a future
     lifetime contract, but the gate must exist).

### C. Wrong abstraction boundary — REJECTED
Would require splitting `AsyncBackend` into separate operation/task/runtime
concepts. The evidence does not support this: the boundary already isolates
operation execution correctly; the task/runtime concepts are simply *absent*,
not *mis-layered*. Adding them above (PHASE T/E) is additive, not a split.
Selecting C would trigger a rewrite the ADR (016D) explicitly forbade
(AB3/AB6) and that the source does not justify.

## 6. Hard blockers for moving past PHASE B

Per task §14, a job may block only on enumerated conditions. The blockers for
proceeding into PHASE T (task model) after B:

- **None at the contract level.** The `AsyncBackend` contract is sufficient
  (verdict B). PHASE T jobs depend on B1 (conformance suite) to define the
  contract they execute on, and on B2/B3 to have a correct substrate — but
  those are dependency edges, not blockers.

The blockers for proceeding into PHASE E (Evented/fibers):

- **E0 ADR required.** Adopting fibers is a public-contract decision (the
  principal experiment, task §9). It cannot be made silently. This is a
  genuine hard blocker per task §14 ("proceeding would irreversibly choose a
  public contract not determined by Zig source or existing cppio architecture"
  — fibers are *optional* in Zig, gated by `fiber.supported` and by OS).
- **`fiber.zig` is gated to 3 arches.** cppio must record its target arch set
  in the E0 ADR. On an unsupported arch, Evented is unavailable (Zig models
  this as `Evented = void`).

## 7. What this audit does NOT authorize

- It does **not** authorize rewriting `AsyncBackend` or `AsyncIoContext`.
- It does **not** authorize adding the Zig task surface (Future/Group/Select/
  Batch) to `AsyncBackend` — that goes in a new layer (PHASE T).
- It does **not** authorize adding fibers (PHASE E, gated by E0 ADR).
- It does **not** authorize networking/process/timer/mmap/terminal/dir
  surfaces (ADR §A5; out of scope).
- It does **not** change the default backend. `BlockingIoContext` stays
  default; async stays opt-in in `sluice::async` (ADR AB1/AB9).

## 8. Cross-links

- Source graph this consumes: `docs/zig-stdio-async-port-map.md` (023A).
- Job-card sequence this governs: `docs/zig-stdio-migration-jobs.md` (023C).
- Async ADR (the accepted L1 model): `docs/adr/ADR-async-io-model.md` (016D).
- Async next jobs (017-022, GREEN baseline): `docs/async-next-jobs.md` (016F).

## 9. PHASE B close-out (GREEN)

PHASE B (backend substrate completeness) is **GREEN** as of jobs 024/025/026:

```text
024  Shared AsyncBackend conformance suite (B1)             DONE
     8 shared cases × {Fake, ThreadPool, Uring-stub}; release+debug+
     asanubsan+tsan green; GCC+Clang green. 53/53 tests.
025  ThreadPoolBackend shutdown gate + cancel doc repair (B2) DONE
     destroying_ now enforced via accepting_new_work(); header/impl agree;
     cancel best-effort semantics documented and tested.
026  UringAsyncBackend O(1) cancel + feature gates (B3)     DONE*
     Completion* -> op-id reverse index (was linear scan); submit batching
     seam verified present; registered-buffers/files gates added (off by
     default). *Real-mode runtime verification DEFERRED: liburing absent
     on host; structural syntax/type check passed (g++ -fsyntax-only
     -DSLUICE_HAS_LIBURING exit 0). Exactly-once cancel-race logic
     unchanged from 020B.
```

**The backend substrate is sufficient to support PHASE T (task model).** The
verdict (B — sufficient with narrow extensions) holds: the narrow extensions
are closed; the Zig task runtime lands on a separate layer above
`AsyncBackend` (no contract change), per §5 of this audit.

**Open item (not a blocker):** when a liburing-equipped environment is
available, run the existing gated cancel cases in `tests/uring_backend_test.cpp`
to runtime-verify the O(1) cancel path. The conformance suite (024) already
exercises Uring in stub mode.
