# Async I/O source inventory

**Status: sluice-CORE-016A.** This is a design artifact, not code. It inventories
what exists today (synchronous sluice abstractions, the experimental io_uring
spike, and the local Zig `std.Io` reference) so that the async problem statement
(016B), the design alternatives (016C), and the ADR (016D) rest on what is
actually present — not on assumptions. **No Zig code is copied** — only symbolic
references and one-line concepts. Zig remains a design reference, never a
dependency.

This inventory feeds directly into:
- `docs/async-problem-statement.md` (016B) — why async, for which workloads.
- `docs/async-design-alternatives.md` (016C) — option comparison.
- `docs/adr/ADR-async-io-model.md` (016D) — the recommendation.

## 1. Current synchronous sluice abstractions (what exists)

All of the following are blocking and live in the `sluice` namespace. They are
the surface any async model must coexist with — and must **not** break.

| Component | Header | Concept | Async-relevant notes |
|---|---|---|---|
| `Reader` | `include/sluice/reader.hpp` | Abstract byte source. Primitive `read_some(dst) -> Result<size_t>` (0 == EOF); derived `read_exact`, `stream_to`; vector `read_vec` / `read_vec_all`. | Caller-owned `dst` buffer. Blocking primitive: no return until data, EOF, or error. The ownership shape a completion model must preserve. |
| `Writer` | `include/sluice/writer.hpp` | Abstract byte sink. Primitives `write_some(src) -> Result<size_t>` (0 on non-empty == backend failure) and `flush()` (no-op vs fsync — see flush contract); derived `write_all`; vector `write_vec` / `write_all_vec`. | Caller-owned `src` buffer. `write_some` returns once the kernel has accepted some bytes — the natural place a completion-based async write attaches. |
| `FileReader` / `FileWriter` | `include/sluice/file.hpp` | Blocking POSIX file backend (move-only, RAII close). `readv`/`writev` overrides; `sync_data`/`sync_all`. | The blocking backend that must remain the default. Open errors deferred to first I/O via the direct ctor, surfaced at open time via `IoContext`. |
| `IoContext` | `include/sluice/io_context.hpp` | Abstract backend **factory** boundary: `open_reader`/`open_writer -> unique_ptr<Reader/Writer>`. | The seam where a future async context plugs in. It is **not** async and **not** io_uring today. |
| `BlockingIoContext` | `include/sluice/io_context.hpp` | Concrete POSIX context. Surfaces open errors at open time. | Stays the default. Must not change behavior. |
| `MemoryIoContext` | `include/sluice/memory_io_context.hpp` | In-memory named-file fake (deterministic tests). | Proves the `IoContext` seam is backend-agnostic and gives a pattern a future *fake async* context can mirror (see 016F job 019). |
| `Result<T>` / `IoError` | `include/sluice/result.hpp`, `error.hpp` | `expected`-like value-or-error; codes incl. `eof`, `canceled`, `interrupted`, `would_block`, `no_space`, `permission_denied`, `invalid_state`, `backend_error` + `os_errno`. | `IoError::Code::canceled` **already exists** — an async cancellation model can reuse it rather than inventing a new error vocabulary. `would_block` also exists (unused by blocking POSIX today; relevant to a pollable backend). |
| Measurement structs | `include/sluice/measurement.hpp` | `SyscallStats`, `BufferStats`, `CopyStats`, `SyncStats`, `VectorStats`, `UringStats` — caller-owned, nullable, never global. | The pattern (raw optional pointer, null = no counting) an async stats hook must follow. `UringStats` already counts submit/completed ops. |
| `IoSlice` / `ConstIoSlice` | `include/sluice/iovec.hpp` | `std::span` wrappers for gather/scatter. | The buffer-unit a vector async op would operate over. |
| Buffered / observed / fault / wal / copy layers | `include/sluice/*.hpp` | Composition wrappers built on `Reader`/`Writer`. | Pure synchronous layers; an async model coexists with, not replaces, them. |

### Ownership invariants already enforced (must be preserved)

```text
dst buffer for read_some        : caller-owned; outlives the call (synchronous today)
src buffer for write_some       : caller-owned; outlives the call (synchronous today)
stats pointers                  : caller-owned; outlive the reader/writer; null = no-op
fd in UringWriteBatch::write_all: NOT owned by the batch (caller owns open/close)
IoContext handle ownership      : returned unique_ptr; caller owns the Reader/Writer
```

A completion-based async model will relax "outlives the call" to "outlives the
outstanding operation" — the single biggest behavioral change, and the reason
buffer lifetime is its own readiness-gate item (016E).

## 2. Current experimental io_uring abstractions (what exists, build-gated)

Namespace `sluice::experimental`, gated behind `SLUICE_HAS_LIBURING`. Documented
in `docs/io-uring-spike.md` and `docs/io-uring-liburing-validation.md`. These are
**experiments**, not the default backend and **not** plugged into
`BlockingIoContext`.

| Component | Header / src | Concept | Async-relevant notes |
|---|---|---|---|
| `UringWriteBatch` | `experimental/uring_write_batch.hpp` | Synchronous-over-uring batched file writes. `write_all(fd, bytes, offset) -> Result<UringWriteResult>`. | **Submit, then block on completion** (`io_uring_submit` + `io_uring_wait_cqe`, looped). Proves the SQE/CQE seam works with no async runtime. Caller's `bytes` must outlive the call *because the call blocks to completion*. |
| `UringIoContext` | `experimental/uring_io_context.hpp` | Thin standalone wrapper: open(POSIX) → `UringWriteBatch::write_all` → close(RAII). | **Not** a subclass of `sluice::IoContext`. Intentionally separate so the spike cannot touch default-backend selection. |
| `UringStats` | `sluice/measurement.hpp` | `queue_init_calls`, `submit_calls`, `submitted_ops`, `completed_ops`, `completion_errors`, `bytes_completed`. | The counter set an async uring backend would extend (cancellations, SQE pressure). |
| Build gate | `xmake.lua` `--with-liburing` | Optional; default build has zero liburing dependency. | The gate an async uring backend would reuse. |

### What the spike deliberately does NOT have (gaps an async backend must fill)

```text
no async runtime / scheduler / executor
no cancellation surface (synchronous: cancel is meaningless while blocking)
no buffer-lifetime-beyond-the-call contract (caller buffer reused after return is safe today)
no generic Reader/Writer uring backend (only a narrow write slice)
no read path, no networking, no timers, no mmap
no completion ordering guarantees beyond "one submit, one wait"
no deferred submit / batching across callers (submit+wait per op)
no registered buffers / registered files (kernel-pinned buffer lifetime)
```

The spike's synchronous-over-uring shape is the *floor*: it proves the kernel
seam, but it cannot be the async ceiling. See 016B §"why synchronous-over-uring
is insufficient".

## 3. Relevant Zig `std.Io` concepts (reference, not a dependency)

Recorded from the local tree `zig/lib/std/Io.zig` and `zig/lib/std/Io/*.zig`.
**Sluice mirrors concepts symbolically; it does not link or copy Zig.** The Zig
model is far broader than sluice's scope (fs + net + processes + time + random +
async/await/cancel + locks + mmap); only the parts relevant to *file async I/O*
are inventoried here.

| Zig concept | Where | One-line meaning | Reuse in sluice? |
|---|---|---|---|
| `Io` context | `Io.zig` | Carries the backend (`userdata` + `vtable`); owns submit/completion machinery. | Conceptual only. sluice has `IoContext` as a *factory*; it has no vtable-owned completion path and must not grow a global one (see 016E). |
| `Io.Operation` | `Io.zig:257` | Tagged union of all async-able ops (`file_read_streaming`, `file_write_streaming`, net, dev-ioctl…). Each variant carries its own buffers + its own `Result = Error!usize`. | Strong reference for an *async op descriptor* shape (one op = buffers + result type). sluice's `read_some`/`write_some` map to streaming file ops. |
| `Io.operate(io, op)` | `Io.zig:452` | Submit one operation, return a task/`Completion`. | Reference for a submit→await split. |
| `Completion` | `Io.zig:404/419`, `Io.zig:538`, `Uring.zig:1089` | The unit of an outstanding operation: pre-allocated storage the caller provides; awaited to get the result; groupable into a `Batch`. | The single most reusable idea: **an explicit, caller-provided completion object** rather than an implicit future. Decouples allocation from the op. |
| `Batch` (await/cancel) | `Io.zig` `Batch` | Group completions; `awaitAsync`, `awaitConcurrent`, `next`, `cancel(batch, io)`. | Reference for completion *ordering* and group cancellation. |
| `Cancelable` | `Io.zig:704` | `error{Canceled}` (an error set), returned at await points. | Maps cleanly onto sluice's existing `IoError::Code::canceled`. |
| `CancelProtection` | `Io.zig:1322` | Per-task `blocked`/`unblocked` blocking of cancellation at protected regions. | Reference for *structured* cancellation (non-trivial; deferred per ADR). |
| `Io.Uring` (Linux) | `Io/Uring.zig` | Real async io_uring backend: per-thread ring + fiber pool + work stealing; submit SQEs, drive CQEs, resume fibers. | Proves io_uring async is feasible; but it is **fiber + scheduler based** (see below — do not copy blindly). |
| `Io.Threaded` | `Io/Threaded.zig` | Thread-pool blocking backend (no fibers). | Reference for a pollable/thread-pool fallback backend. |
| `Io.Dispatch` (macOS) | `Io/Dispatch.zig` | `dispatch_*` + fibers. | Not applicable (platform). |
| `Io.Kqueue` (BSD) | `Io/Kqueue.zig` | kqueue evented backend. | Not applicable (platform / not Linux io_uring). |
| `Io.fiber` | `Io/fiber.zig` | Architecture-specific context-switch asm (aarch64/riscv64/x86_64); stack switching. | **Explicitly not copied** — hand-written asm fiber switching is out of scope and an avoidable risk for sluice (see §5). |

### Key Zig design moves sluice can borrow (symbolically)

```text
1. Operation as a tagged descriptor (buffers + result type)        -> async op model
2. Caller-provided Completion storage                              -> lifetime is explicit
3. Cancellation as a first-class error + protection state          -> reuse IoError::canceled
4. Completion grouping + ordering (Batch)                          -> multi-op ordering rules
5. Backend boundary owns submit/completion plumbing                -> IoContext seam already exists
```

## 4. What is missing (gaps this job's ADR must close)

```text
ASYNC RUNTIME         no executor / scheduler / driver loop submits and pumps CQEs
COMPLETION OBJECT     no caller-visible completion storage (only blocking return values)
CANCELLATION          no cancel surface and no defined semantics (only an unused error code)
BUFFER LIFETIME       no contract for "buffer outlives the outstanding op, not just the call"
SUBMIT BATCHING       no deferred-submit across callers (submit+wait per op in the spike)
ORDERING MODEL        no documented completion order (FIFO? per-op? per-fd?)
ASYNC TEST FAKE       no deterministic async backend (MemoryIoContext is still synchronous)
ASYNC BENCH HARNESS   no microbench for async throughput/latency (only blocking + sync-uring)
CANCEL ON KERNEL      no use of IORING_OP_ASYNC_CANCEL / registered-buffer teardown discipline
```

Each gap maps to a readiness-gate item in 016E and to a future job in 016F.

## 5. What must not be copied blindly from Zig

The Zig `std.Io` async model is coherent and worth studying, but several of its
choices carry risks sluice should not inherit without an explicit evaluation in
the ADR (016D). These are flagged here so the alternatives doc and ADR treat them
as decisions, not defaults:

```text
1. FIBER / STACK SWITCHING (Io/fiber.zig)
   Zig drives async via hand-written asm context switching (rsp/rbp/rip save/restore).
   This is high-performance but architecture-specific, hard to audit, and a large
   correctness surface. sluice should NOT adopt fibers as the first async model.
   -> ADR will decide coroutine-vs-completion, with fibers explicitly deferred.

2. GLOBAL / SINGLE CONTEXT OBJECT
   Zig carries one `Io` value thread-locally in places. sluice's measurement rule
   forbids global state; the readiness gate (016E) requires "no global scheduler
   unless explicitly accepted". The async context must be a passed/owned object.

3. UNIVERSAL OPERATION UNION
   Zig's `Operation` spans files, net, dev-ioctl, etc. sluice's scope is file
   read/write only; networking, timers, mmap, process APIs are explicitly excluded
   (016B). Do not generalize the op union beyond file I/O.

4. IMPLICIT BACKEND SELECTION
   Zig picks Evented/Uring/Threaded by OS+arch at compile time. sluice's hard
   boundary is: BlockingIoContext stays the default; async is opt-in and behind
   a seam. Never auto-promote io_uring.

5. LIBURING-LIFETIME ASSUMPTIONS
   io_uring registered buffers/files pin kernel state for the ring's lifetime.
   Zig manages this via fibers + the Io context. sluice must NOT adopt registered
   buffers without an explicit lifetime+teardown contract in the ADR.
```

## 6. Cross-links

- Why async, for which workloads: `docs/async-problem-statement.md` (016B).
- Option comparison: `docs/async-design-alternatives.md` (016C).
- Decision: `docs/adr/ADR-async-io-model.md` (016D).
- Preconditions to start implementation: `docs/async-readiness-gate.md` (016E).
- Implementation split: `docs/async-next-jobs.md` (016F).
- Synchronous baseline this rests on: `docs/design-io-context.md` (009).
- io_uring spike detail: `docs/io-uring-spike.md` (013).
- io_uring readiness gate (the *prior* gate, for the spike): `docs/io-uring-readiness-gate.md` (012D).
- liburing validation runbook: `docs/io-uring-liburing-validation.md` (014C).
- Local Zig inventory (fuller): `docs/zig-std-io-source-inventory.md` (012B).
