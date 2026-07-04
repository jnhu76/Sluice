# Sync I/O architecture

**Status: SYNC-IO-COMPLETE Phase 2 (sync doc reconciliation).** This is the
*architecture* layer of the synchronous/blocking I/O model. It defines what the
sync model **is** — the backend taxonomy, the execution-model (runtime) taxonomy,
what is borrowed from Zig `std.Io`, and what is explicitly not borrowed. It sits
above the planning layer (`docs/sync-io-model-gap-audit.md`, gaps G1–G7;
`docs/sync-io-next-jobs.md`, job cards 017S–023S) and below the per-topic contract
docs (`docs/sync-io-model.md` primitive contract, `docs/sync-durability-model.md`,
`docs/sync-bench-methodology.md`).

This is the blocking-first companion to the async design (`docs/adr/ADR-async-io-
model.md`, 016D). Async implementation is blocked behind the sync-first gate
(`docs/sync-before-async-readiness-gate.md`) until this architecture is realized
by jobs 017S–023S. **No async code exists yet.**

## 1. Scope

Complete the synchronous/blocking I/O model so sluice has a strong, **engineered**
blocking baseline before any async work starts. The model may learn from Zig
`std.Io`, but only the sync-relevant ideas. It does **not** migrate Zig's async
runtime, fibers, event loop, `Completion`, coroutine wrappers, P2300, or any
actor/fiber model.

The final sync model:

```text
I/O capability:
  IoContext                         explicit, caller-owned capability boundary

Real sync backend:
  BlockingIoContext + FileReader/FileWriter   real POSIX blocking file I/O (DEFAULT)

Test backends / wrappers:
  MemoryIoContext                   deterministic in-memory test I/O
  FaultReader/FaultWriter           inject short read/write/error/EOF/zero progress
  ObservedReader/ObservedWriter     collect stats around Reader/Writer behavior

Sync execution models (how blocking jobs are scheduled — NOT I/O backends):
  blocking_sequential               one caller thread, serial jobs
  blocking_bounded_pool             fixed std::thread workers (BlockingIoPool)
  blocking_thread_per_stream        one worker thread per stream (bench baseline only)

Experimental comparison row only:
  existing synchronous-over-uring spike (013), build-gated, NOT default
```

Future async must compare against **engineered blocking baselines**, not only a
naive sequential loop.

## 2. Backend taxonomy

A **backend** defines *how I/O is performed*. Backends live behind the `IoContext`
capability seam.

| Backend / wrapper | Role | Default? | Namespace |
|---|---|---|---|
| `BlockingIoContext` | real POSIX blocking file I/O | **YES (production default)** | `sluice` |
| `MemoryIoContext` | deterministic in-memory test I/O | no (tests) | `sluice` |
| `FaultReader`/`FaultWriter` | inject short read/write/error/EOF/zero-progress | no (testing layer) | `sluice` |
| `ObservedReader`/`ObservedWriter` | collect stats around a Reader/Writer | no (measurement layer) | `sluice` |

**Hard rule:** Fault/Observed wrappers are **not** separate production backends.
They are testing and measurement *layers* that wrap a real Reader/Writer. Do not
call them backends in user-facing docs.

The existing synchronous-over-uring spike (`sluice::experimental::UringWriteBatch`/
`UringIoContext`, job 013) is **experimental only**, build-gated behind
`SLUICE_HAS_LIBURING`, and is a **comparison row** — never the default, never
plugged into `BlockingIoContext`.

## 3. Runtime / execution-model taxonomy

A **runtime** (execution model) defines *how blocking jobs are scheduled*. This is
orthogonal to backends: any execution model can drive any backend.

For the sync phase, the allowed execution models are:

| Execution model | Meaning | Use |
|---|---|---|
| `blocking_sequential` | one caller thread, serial jobs | weak baseline; current default behavior |
| `blocking_bounded_pool` | fixed number of `std::thread` workers (`BlockingIoPool`) | **engineered portable baseline** for benchmarks |
| `blocking_thread_per_stream` | one worker thread per stream | strong-but-expensive baseline; benchmark only |

**Hard rule:** The pool is **not async**. `BlockingIoPool` runs ordinary blocking
operations on worker threads. It is an **execution model, not an I/O backend** —
it does not implement `IoContext` and is not selectable as a backend. The
production API lives in `include/sluice/blocking_io_pool.hpp` and
`src/blocking_io_pool.cpp`; `bench/support/` is only a thin benchmark adapter.

The pool uses only standard C++ primitives (`std::thread`, `std::mutex`,
`std::condition_variable`, `std::queue`, `std::function`). No lock-free queue, no
work stealing, no fiber, no custom scheduler, no global executor, no external
thread-pool dependency.

## 4. What is borrowed from Zig `std.Io`

Borrowed **symbolically** (concepts, not code — Zig is a design reference, never a
dependency):

```text
1. Explicit I/O capability passed from above
   Zig passes `Io` like `Allocator`. sluice keeps `IoContext` explicit and
   caller-owned — never global, never thread-local.

2. Swappable implementation behind one capability
   Zig chooses Threaded/Evented implementations. sluice sync keeps
   BlockingIoContext / MemoryIoContext behind the IoContext seam.

3. Threaded sync execution as a baseline
   Zig Io.Threaded uses threads for explicit concurrency. sluice sync adds a
   bounded std::thread pool baseline (BlockingIoPool) for benchmarks.

4. Deterministic testing via a non-real backend
   sluice already has MemoryIoContext; strengthen it where needed.

5. Caller-owned measurement
   Stats remain optional, caller-owned, nullable, never global — exactly like
   Zig's and like sluice's existing SyscallStats/UringStats/etc.
```

## 5. What is explicitly NOT borrowed (out of scope this phase)

```text
1. Zig async/await interface
2. Completion / Batch / cancellation model
3. fiber / stack switching
4. evented io_uring runtime
5. universal Operation union spanning files/net/process/timers
6. implicit backend selection based on OS/arch
7. global or thread-local scheduler
```

The sync stage stays **simple, explicit, blocking-first**.

## 6. Current sync components (inventory snapshot)

Already present (see `docs/sync-io-model-gap-audit.md` §1 for the evidence table):

```text
Reader / Writer (reader.hpp, writer.hpp)
  read_some/write_some/read_exact/write_all/stream_to
  read_vec/write_vec/read_vec_all/write_all_vec
FileReader / FileWriter (file.hpp, src/file.cpp)   blocking POSIX, EINTR retry
IoContext / BlockingIoContext (io_context.hpp)      capability seam + default backend
MemoryIoContext (memory_io_context.hpp)             deterministic test backend
Fault / Observed wrappers (fault.hpp, observed.hpp) testing/measurement layers
SyncableWriter (sync.hpp) + sync_data/sync_all      durability primitives
WalWriter (wal.hpp)                                 written<=flushed<=durable LSN
Measurement structs (measurement.hpp)               caller-owned raw counters
Microbench harness (bench/, docs/bench-methodology.md) 5 single-stream targets
```

## 7. Missing sync pieces (the gaps this architecture must close)

From `docs/sync-io-model-gap-audit.md` (gaps G1–G7), restated in this architecture's
terminology:

```text
G1 Positional I/O absent        → add read_at/write_at (+read_vec_at/write_vec_at)
                                   via pread/pwrite/preadv/pwritev. (job 018S)
G2 Vector I/O non-positional    → positional vector forms alongside G1. (job 018S)
G3 Derived helper closeout      → read_exact/write_all + positional _at variants,
                                   vector-all advancement correctness. (job 019S)
G4 No durability POLICY         → document flush/sync_data/sync_all semantics +
                                   benchmark sync-policy names. (job 020S,
                                   docs/sync-durability-model.md)
G5 No bounded pool/concurrency  → BlockingIoPool execution model (NOT a backend).
                                   (job 021S)
G6 Stats lack multi-stream/lat  → active-streams + per-cell concurrency fields.
                                   (job 022S)
G7 Bench single-stream only     → W1-W4 blocking benchmark matrix across modes.
                                   (job 022S; tune in 023S)
```

## 8. Future async dependency on this baseline

Async implementation (jobs 017+, `docs/async-next-jobs.md`) is **blocked** behind
the sync-first readiness gate (`docs/sync-before-async-readiness-gate.md`). The
gate requires: positional I/O decision made, durability model documented, bounded
pool baseline exists, W1–W4 blocking benchmark matrix exists, baseline engineered.

When async eventually runs, its benchmark comparison rows **must** be the
engineered concurrent W1–W4 blocking rows produced by this architecture (gate item
7) — not the sequential single-stream rows. Sequential rows may be shown for
reference but must not be the basis of the async-vs-blocking comparison.

This is **not** a claim that blocking-with-a-pool will match async (it may or may
not, workload-dependently; no universal claim). It is a claim that the comparison
must be **defined** before it is run, against an engineered baseline.

## 9. Cross-links

- Primitive contract: `docs/sync-io-model.md`.
- Durability model: `docs/sync-durability-model.md`.
- Bench methodology: `docs/sync-bench-methodology.md`, `docs/sync-bench-matrix.md`.
- Optimization notes: `docs/sync-optimization-notes.md`.
- Planning layer (gaps + jobs): `docs/sync-io-model-gap-audit.md`, `docs/sync-io-next-jobs.md` (017S–023S).
- Sync-first readiness gate (blocks async): `docs/sync-before-async-readiness-gate.md`.
- Async deferral note: `docs/async-deferred-until-sync-baseline.md`.
- Async design (accepted, frozen): `docs/adr/ADR-async-io-model.md` (016D).
- Existing durability design: `docs/design-flush-sync-durability.md`, `docs/design-wal-durability.md`.
- Existing IoContext design: `docs/design-io-context.md` (009).
