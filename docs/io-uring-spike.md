# Experimental io_uring spike

**Status: implemented in CPPIO-CORE-013A–013G; documented/closeout 013H–013I.**
This is an **experiment**, not a production backend.

## What was implemented (013B–013G)

- **Optional liburing build gate** (`CPPIO_HAS_LIBURING`, `--with-liburing`).
  Normal builds have **zero** liburing dependency.
- **`cppio::experimental::UringWriteBatch`** — synchronous-over-uring batched
  file writes (`write_all(fd, bytes, offset)`). Stub when liburing is absent.
- **`cppio::experimental::UringIoContext`** — standalone open/write/close
  wrapper (`write_file_all`). Not a subclass of `cppio::IoContext`; not plugged
  into `BlockingIoContext`.
- **`cppio::UringStats`** — optional counters wired via `set_stats`.
- **`bench/uring_write_bench`** — guarded bench (skip row without liburing).
- **`examples/experimental_uring_write`** — skip-cleanly-without-liburing demo.

## What was NOT implemented

- No production backend, no generic Reader/Writer uring backend.
- No async runtime / scheduler / cancellation.
- No networking / timers / mmap.
- No default backend switch (`BlockingIoContext` stays the default).
- No durability claim (uring write completion ≠ fsync).
- No broad performance claim (only a local, workload-specific bench row).

## 1. Scope

Add the narrowest possible io_uring-backed write path to cppio to answer one
question:

```text
Can cppio express a narrow io_uring-backed write path
without breaking Reader/Writer semantics,
without changing the default backend,
and with measurable comparison to blocking baseline?
```

Everything else is explicitly out of scope (§4).

## 2. Why experimental

cppio's MVP is blocking and has no async runtime, scheduler, or cancellation
model. A real io_uring backend (like Zig's `Io.Uring`) needs all three. Rather
than block the experiment on missing infrastructure, the spike is
**synchronous-over-uring**: submit, then block on completion
(`io_uring_submit_and_wait`), so it needs no async runtime. This proves whether
the seam works and whether even the synchronous-over-uring path is worth
pursuing — without committing to an async redesign.

The code lives under `include/cppio/experimental/` + `src/experimental/` in
namespace `cppio::experimental`, build-gated behind `CPPIO_HAS_LIBURING`.

## 3. Chosen vertical slice

**Batched file writes** via `cppio::experimental::UringWriteBatch::write_all(fd,
bytes, offset)`:

- submits one or more write SQEs,
- blocks until all complete,
- loops on short writes,
- returns `UringWriteResult{submitted, completed, bytes_written, errors}` or an
  error.

A thin `UringIoContext::write_file_all(path, bytes)` wraps it: open(POSIX) →
write via the batch → close (RAII). Not polymorphic; not plugged into
`BlockingIoContext`.

## 4. Non-goals

```text
No default backend switch
No full async runtime
No scheduler
No networking
No timers
No cancellation
No durability claim
No production copy strategy yet
```

## 5. Error model

- Errors are `cppio::Result<T>` / `cppio::IoError`, the same model as the rest of
  cppio — errors are returned, not swallowed.
- A CQE with a negative `res` maps through `from_errno_value(-res)`.
- If liburing is unavailable at build time, the type still compiles (or the
  target is skipped) and `write_all` returns `backend_error`/`unsupported` — no
  crash, no link failure for the rest of the project.

## 6. Buffer ownership model

**Caller-owned.** The caller's `bytes` buffer must outlive the `write_all` call.
Because the spike is synchronous-over-uring, the buffer is only referenced for
the duration of the call (completion is waited for before return), so lifetime is
straightforward — but it is documented and enforced by the API shape (no
async return that escapes the buffer). No internal copy is made.

## 7. Submission/completion model

- `UringWriteBatch` owns one `struct io_uring` (queue_depth entries).
- `write_all` pushes write SQEs, submits, and waits for completions until all
  requested bytes are confirmed written or an error occurs.
- The batch does **not** own the fd; the caller does. The batch never closes fd.
- Short writes resubmit the tail at the advanced offset.

## 8. Relation to IoContext

Deliberately **none** for the spike: `UringIoContext` is a standalone class, not
a subclass of `cppio::IoContext`, and is not plugged into `BlockingIoContext`.
This keeps the default backend untouched. A future job (014A, post-spike) may
promote it behind the `IoContext` seam if the spike proves out.

## 9. Build gating

- `CPPIO_HAS_LIBURING` is defined only when liburing headers + library are found.
- The experimental sources compile only when `CPPIO_HAS_LIBURING` is defined;
  otherwise they reduce to stubs that return `unsupported`, OR the targets are
  skipped entirely (chosen per-target in 013B).
- The normal `xmake build` / `xmake test` MUST work with no liburing installed.

## 10. Test strategy

- Tests are registered but **skip cleanly** (print a skip message, exit success)
  when `CPPIO_HAS_LIBURING` is not defined or the kernel rejects
  `io_uring_setup`.
- When available: construction, invalid fd → error, small write to temp file,
  bytes match, large chunked write, no fd ownership confusion.

## 11. Benchmark strategy

- `bench/uring_write_bench.cpp` adds a `uring_write_batch` mode alongside
  `blocking_file_writer` / `blocking_write_vec`.
- The uring mode emits a clear skip row (or is skipped at build time) when
  liburing is unavailable.
- Output is CSV; **no broad performance claim** — only a local, workload-specific
  row that the optimization matrix may reference if stable.

## 12. Abort conditions

(From `docs/io-uring-readiness-gate.md` §7.) Stop and revert if:

- the normal no-liburing build breaks;
- the blocking backend's behavior or tests change;
- buffer-lifetime discipline cannot be enforced cleanly;
- partial-completion handling cannot be made correct without async;
- liburing cannot be made a clean optional dependency;
- the spike touches default-backend selection.

The spike proceeds only behind these conditions.
