# MVP core model

**Status: documented in SLUICE-CORE-006A.** This freezes the intended MVP
pipeline *before* the buffered fast path (006C–006F) lands. It is a composition
and correctness target, not a benchmark.

## 1. Scope

> This MVP demonstrates composition and correctness, not performance.

The MVP is the smallest runnable model showing how the core pieces compose:
`Reader`/`Writer`, the buffering/observing/faulting wrappers, the POSIX file
backend, the copy primitive, and the measurement hooks. It is deliberately not
io_uring, not async, and not a performance claim.

## 2. Core abstractions

Two primitives, derived helpers, and opt-in wrappers:

```
Reader  ── read_some (primitive)
         ├── read_exact        (derived: exact-fill or EOF/error)
         ├── read_vec          (derived vector primitive, SLUICE-CORE-005)
         └── stream_to         (derived: copy to a Writer)

Writer  ── write_some (primitive)
         ├── flush             (primitive: layer-specific)
         ├── write_all         (derived: exact-write or error)
         └── write_vec / write_all_vec  (derived vector, SLUICE-CORE-005)

optional wrappers (compose by reference, outermost is driven):
  BufferedReader / BufferedWriter   (caller-owned buffer)
  ObservedReader / ObservedWriter   (transparent stats)
  FaultReader / FaultWriter         (deterministic fault injection)
  FileReader / FileWriter           (POSIX, RAII, move-only)
```

The current main model:

```
Reader / Writer
  +
optional wrappers:
  BufferedReader / BufferedWriter
  ObservedReader / ObservedWriter
  FaultReader / FaultWriter
  FileReader / FileWriter
```

## 3. MVP pipeline diagram

```
FileReader ──▶ BufferedReader ──▶ copy_all(scratch, CopyLimit) ──▶ BufferedWriter ──▶ ObservedWriter ──▶ FileWriter
(input fd)     (read buffer)        │                                  (write buffer)      (stats)            (output fd)
                                    └─ CopyStats (optional) ─────────────┘
```

The composite in code:

```cpp
FileReader file_in(...);
BufferedReader buffered_in(file_in, read_buffer, &buffer_stats);

FileWriter file_out(...);
ObservedWriter observed_out(file_out, writer_stats);
BufferedWriter buffered_out(observed_out, write_buffer, &buffer_stats);

CopyStats copy_stats;
auto copied = copy_all(
    buffered_in,
    buffered_out,
    scratch,
    CopyLimit::unlimited(),
    &copy_stats
);

buffered_out.flush();   // outermost flush; propagates inward
```

## 4. Example programs

Landed in SLUICE-CORE-006B:

- `examples/mvp_copy_pipeline.cpp` — the pipeline above; verifies output bytes
  match input and prints copied bytes / buffer hits / copy loop count / syscall
  counts.
- `examples/mvp_limited_copy.cpp` — `CopyLimit::bytes(N)`; verifies the first N
  bytes are copied and prints the stop reason from `CopyStats`.
- `examples/mvp_wal_vector.cpp` — writes WAL records via
  `wal::write_record_vec`, reads back with `wal::read_record`, verifies
  payloads, prints the record count (no vector-I/O performance claim).

## 5. Flush semantics

`flush()` is layer-specific and **must be called on the outermost writer used**:

| Layer | `flush()` does | Errors? |
|---|---|---|
| `Writer::flush()` | generic operation on the abstraction | yes (virtual) |
| `BufferedWriter::flush()` | drains all dirty bytes into the inner writer, then calls `inner.flush()` | yes |
| `FileWriter::flush()` | **documented no-op this phase** — no `fsync`/`fdatasync`, no durability | n/a |

Flushing buffered bytes is **not** a durable commit. `BufferedWriter`'s
destructor does not flush (a failing flush cannot be reported from a dtor); a
debug assert catches "forgot to flush" loudly.

## 6. Measurement semantics

Stats are pure-data structs attached by nullable, caller-owned pointer. A null
pointer means no counting and zero overhead. No global stats.

- `BufferStats` — buffer hit/miss/refill (read) and buffered/direct/flush (write)
- `SyscallStats` — read/write syscall counts on `FileReader`/`FileWriter`
- `CopyStats` — copy loop behavior and stop reason (extended in 006E)
- `VectorStats` — `read_vec`/`write_vec` calls/bytes/iovecs/fallback (005)

## 7. What this MVP deliberately does not do

- No io_uring.
- No async / evented / thread-pool backend.
- No benchmark integration (SLUICE-CORE-010). No performance conclusion.
- No durability: `FileWriter::flush()` does not imply `fsync`.
- No copy strategy layer (SLUICE-CORE-007).
- No removal of scalar `read_some`/`write_some`.

## 8. Relation to Zig `std.Io`

Zig `std.Io` keeps buffer state **inside** the `Reader`/`Writer` interface
(`r.buffer/seek/end`, `w.buffer/end`); every reader is inherently buffered and
the vtable (`stream`, `drain`, `rebase`) negotiates around that buffer
(`Reader.zig:168` — `stream` writes `r.buffer[r.seek..r.end]` first). sluice
currently uses **external** `BufferedReader`/`BufferedWriter` wrappers holding
caller-owned storage, with `copy_all` staging through a caller scratch buffer.

The buffered fast path (SLUICE-CORE-006C–006F) closes this gap **without** a
rewrite: it exposes the wrapper's already-buffered unread bytes through an
opt-in `BufferedReadable` interface so `copy_all` can drain them directly,
mirroring Zig's `stream` fast path. The divergence from Zig (wrapper vs
interface-owned buffer) is documented in
`docs/buffered-fast-path.md` and `docs/zig-std-io-gap-calibration.md`.

Zig `std.Io` is a **design reference only** — never a build/runtime dependency,
and no Zig stdlib code is copied.
