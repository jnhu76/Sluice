# readv/writev design note

**Status: prototype implemented in SLUICE-CORE-005.** The default fallback, the
POSIX `readv`/`writev` overrides, `write_all_vec`, the WAL vector path, and
`VectorStats` measurement all exist and are tested. This is **not** a
performance claim — see §7.

## 1. What was implemented

| Piece | Location |
|---|---|
| `IoSlice` / `ConstIoSlice` slice types | `include/sluice/iovec.hpp` |
| `Reader::read_vec` default fallback | `include/sluice/reader.hpp`, `src/reader.cpp` |
| `Writer::write_vec` default fallback | `include/sluice/writer.hpp`, `src/writer.cpp` |
| `Writer::write_all_vec` derived helper | `src/writer.cpp` |
| `FileReader::read_vec` / `FileWriter::write_vec` POSIX overrides | `src/file.cpp` |
| `VectorStats` struct | `include/sluice/measurement.hpp` |
| Vector counting on the observed (fallback) layer | `src/observed.cpp` |
| Vector counting on the file (real readv/writev) layer | `src/file.cpp` |
| `wal::write_record_vec` | `src/wal.cpp` |
| Tests | `tests/{writer_vec,reader_vec,file_vec,wal_vec,vector_stats}_test.cpp` |

The old scalar APIs (`read_some`, `write_some`, `read_exact`, `write_all`,
`copy_all`, `wal::read_record`, `wal::write_record`) are unchanged.

## 2. What remains deferred

- No io_uring (scheduled for SLUICE-CORE-012).
- No async / thread-pool backend.
- No benchmark integration (SLUICE-CORE-010). No performance conclusion.
- No zero-copy buffered fast path (SLUICE-CORE-006): the default fallback still
  routes each slice through `read_some`/`write_some`.
- No copy strategy layer (SLUICE-CORE-007).
- No multi-record WAL batching (single record per `write_record_vec` call).
- `FileWriter::flush()` still does not imply durability.

## 3. How the default fallback works

`Reader::read_vec` / `Writer::write_vec` are virtual with a default body, so
every existing reader/writer gains vector I/O without a POSIX backend — important
for the in-memory test doubles and fault wrappers. The default fallback may call
`read_some`/`write_some` multiple times; it is **not** a performance replacement
for `readv`/`writev` (that is the POSIX override in §4).

The vector primitives are **conservative readv/writev-style operations**, not
exact-read/exact-write helpers. Final semantics (SLUICE-CORE-005B):

**`read_vec` (default):** for each non-empty slice in order, call `read_some`.
- error → return it immediately (even after prior slices made progress)
- `n == 0` (EOF) → stop, return total read so far (0 before any progress)
- `0 < n < size` (positive short read) → **stop**, return total including that
  short read; **later slices are left untouched**
- `n == size` (full) → continue to next slice
- empty slices are skipped (no read)

This stops on the first positive short read. It deliberately does **not** behave
like `read_exact` over slices (which would keep filling the same slice on a short
read); that is the job of a future `read_exact_vec`.

**`write_vec` (default):** for each non-empty slice in order, call `write_some`.
- error → return it immediately (even after progress)
- `n == 0` on a non-empty slice → stop, return total (0 if nothing written yet)
- `0 < n < size` (positive short write) → **stop**, return total including that
  short write
- `n == size` (full) → continue to next slice
- empty slices are skipped; all-empty input returns 0

This stops on the first short write. It is **not** `write_all` over slices —
`write_all_vec` is the all-or-error helper built on top.

**`write_all_vec`:** the derived all-or-error helper. Loops `write_vec` over the
remaining tail, tracking a partial offset into the first remaining slice so a
resume re-issues the tail-end of that slice rather than skipping or duplicating
bytes. Zero progress while non-empty data remains is `invalid_state` (same rule
as `write_all`). All bytes written in order; no skip, no duplication.

### Strict error decision

sluice currently propagates vector errors **immediately**, even after partial
progress (a `read_vec`/`write_vec` that has already delivered bytes to some
slices still returns the error rather than the partial count when a later slice's
`read_some`/`write_some` fails). This is stricter than some POSIX-style
byte-count-first designs. The decision is intentional for now and may be
revisited in a future semantic review. It matches the existing `read_exact` /
`write_all` principle: errors are returned, not swallowed.

## 4. How the POSIX `readv` / `writev` override works

`FileReader` and `FileWriter` override `read_vec` / `write_vec` to collapse the
non-empty slices into `struct iovec` arrays and issue `::readv` / `::writev`. The
override represents the kernel vector operation and exposes the **same** vector
primitive semantics as the default fallback (stop on EOF/error/first short
result). It does not try to emulate the default fallback's per-slice loop.

- Empty slices are filtered out and never cause a syscall.
- If the slice count exceeds `IOV_MAX`, the override issues multiple
  `readv`/`writev` calls in `IOV_MAX`-sized chunks (validated with a 4096-slice
  test). `IOV_MAX` is resolved at compile time when defined, otherwise via
  `sysconf(_SC_IOV_MAX)`, with a conservative fallback of 16.
- Each syscall is retried on `EINTR` via `detail::retry_on_eintr`, exactly as
  `read_some` / `write_some` are.
- The `open_error_` behavior from SLUICE-CORE-002 is preserved: a failed-open
  file surfaces its real errno on the first vector call too.
- In `SyscallStats`, each `readv`/`writev` counts as one read/write syscall with
  the returned byte count (an EOF `readv` returning 0 still counts as one read
  syscall with 0 bytes); errors increment the error counter. Skipped empty slices
  do not count as syscalls.
- The override stops after the first kernel short read/write result, exactly like
  the default fallback.

## 5. How the WAL vector write uses it

`wal::write_record_vec` frames the same record layout as `wal::write_record` —

```
magic:    u32 little-endian
length:   u32 little-endian
payload:  bytes
checksum: u32 little-endian
```

— but emits `header | payload | checksum` through a single
`writer.write_all_vec({header, payload, checksum})` instead of three separate
`write_all` calls. The byte output is identical (asserted by a test). The
`checked_u32_len` payload-overflow guard is reused unchanged. `read_record` and
`write_record` are untouched.

## 6. Measurement fields

`VectorStats` (`include/sluice/measurement.hpp`):

```cpp
struct VectorStats {
    std::uint64_t read_vec_calls = 0;
    std::uint64_t read_vec_bytes = 0;
    std::uint64_t read_vec_iovecs = 0;
    std::uint64_t read_vec_fallback_calls = 0;
    std::uint64_t write_vec_calls = 0;
    std::uint64_t write_vec_bytes = 0;
    std::uint64_t write_vec_iovecs = 0;
    std::uint64_t write_vec_fallback_calls = 0;
};
```

`*_fallback_calls` distinguishes "used the default `read_some`/`write_some`
loop" from "used a real `readv`/`writev` syscall". This split is wired two ways:

- **Observed wrappers** (`ObservedReader`/`ObservedWriter`) take an optional
  `VectorStats*`. Because they observe the default-fallback layer, every vector
  call increments `*_fallback_calls` (the real readv/writev path is not in play
  here).
- **File backends** (`FileReader`/`FileWriter`) take an optional `VectorStats*`
  in their existing `SyscallStats*` constructor style. Because they use the real
  `readv`/`writev` override, they increment calls/bytes/iovecs but **not**
  fallback.

Stats are caller-owned, never global (same rule as SLUICE-CORE-004). A null
pointer means no counting and zero overhead. Each `VectorStats` sink is
independent: a single logical vector call shows up once per sink it is attached
to. Double-counting only happens if a caller deliberately attaches the **same**
`VectorStats` to multiple layers (e.g. an `ObservedWriter` wrapping a
`FileWriter`), and that is intentional caller behavior, not a hidden bug.

## 7. Why this is still not a performance claim

Collapsing N slices into one `readv`/`writev` *can* reduce syscall count, but
this task deliberately does not measure or claim that. Reasons:

- No microbenchmark exists yet (SLUICE-CORE-010). The relative cost of the gather
  setup, the scratch copy in the fallback, and the kernel scatter path is
  unmeasured.
- The default fallback still loops over `read_some`/`write_some` — for
  in-memory and fault readers/writers, vector I/O is a convenience, not a
  fast path.
- The `VectorStats` counters are observability hooks (how often did we reach a
  real readv/writev?), not throughput numbers.

The discipline from the gap-calibration doc holds: **do not optimize before
measuring.** SLUICE-CORE-010/011 will produce the decision matrix.

## 8. Relation to Zig `std.Io`

Zig `std.Io.Reader`/`Writer` provide `readVec`/`readVecAll`/`writeVec`/
`writeVecAll` over `[][]u8` / `[]const []const u8`. sluice's `read_vec`/
`write_vec`/`write_all_vec` mirror that shape with `std::span<IoSlice>` /
`std::span<const ConstIoSlice>`. Key differences, all intentional:

- Zig's vector primitives negotiate around the **interface-owned buffer** inside
  each Reader/Writer. sluice has no internal buffer (external `BufferedReader`/
  `BufferedWriter` wrappers — see `docs/zig-std-io-parity-audit.md`), so
  the fallback loops directly over `read_some`/`write_some`.
- Zig's `readVec` swallows `EndOfStream` when bytes were already read
  (`if (n == 0) return error.EndOfStream else 0`). sluice instead models EOF as a
  clean `n==0` return and propagates errors immediately regardless of partial
  progress (consistent with this project's `read_exact`/`write_all` and the
  "errors returned, not swallowed" principle).
- sluice has no async / uring / cancellation; Zig's `std.Io` is built around an
  `Io.Threaded` context. Those remain design references, not dependencies.

Zig remains a **design reference only**, never a build/runtime dependency, and no
Zig stdlib code is copied.
