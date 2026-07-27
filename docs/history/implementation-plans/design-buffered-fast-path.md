# Buffered fast path

**Status: prototype implemented in SLUICE-CORE-006C–006G.** The opt-in buffered
fast path lets `copy_all` drain a `BufferedReader`'s already-buffered unread
bytes directly to the writer, before falling back to the scratch read path. This
is the largest Zig `std.Io` fidelity gap, now partially closed. **Not** a
performance claim.

## 1. What is implemented

- **`BufferedReadable`** (`include/sluice/buffered_readable.hpp`) — an opt-in
  capability interface:
  ```cpp
  class BufferedReadable {
  public:
      virtual std::span<const std::byte> peek_buffered() const = 0;
      virtual Result<void> consume_buffered(std::size_t n) = 0;
  };
  ```
  `peek_buffered()` returns the currently buffered unread bytes (read-only, no
  inner-reader call); `consume_buffered(n)` advances the cursor by exactly `n`
  and rejects `n > peek_buffered().size()` with `invalid_state`.
- **`BufferedReader`** implements `BufferedReadable`. Its buffered region is
  `buf_[seek_..end_)`, matching Zig's `r.buffer[r.seek..r.end]`.
- **`copy_all` fast path** (`src/copy.cpp`) — each loop iteration first
  `dynamic_cast`s the reader to `BufferedReadable*`. If non-null and
  `peek_buffered()` is non-empty, it writes that region (bounded by the
  `CopyLimit`) via `write_all`, then `consume_buffered`s exactly the written
  count. When no buffered bytes remain, it falls back to the existing scratch
  read path. Mirrors Zig's `Reader.stream` (`Reader.zig:168`), which writes
  `r.buffer[r.seek..r.end]` before invoking the vtable stream.
- **`CopyStats` counters** — `buffered_fast_path_calls`/`_bytes` and
  `scratch_path_calls`/`_bytes` split the copy work between the two paths.

As of SLUICE-CORE-007 this fast path is also an **explicit `CopyStrategy`**
(`BufferedFirst`, with `Auto` resolving to it). Forcing the pre-006 scratch path
is now `CopyStrategy::Scratch`. See `docs/design-copy-strategy.md`.

Detection uses `dynamic_cast` (not a virtual hook on the `Reader` base) so that
unbuffered readers (`FileReader`, `MemoryReader`, `FaultReader`, ...) carry zero
overhead and no dead virtuals. See §3 of this doc.

### Writer-error rule

If `write_all` fails while draining buffered bytes, `copy_all` consumes
**nothing** and returns the writer error. `write_all` does not expose partial
completion, so consuming an assumed-partial count would risk data loss; the
conservative choice is to leave the buffer untouched and let the caller retry.

## 2. What remains different from Zig

- **Zig keeps buffer state inside `Reader`.** sluice keeps the base `Reader`/
  `Writer` unbuffered and puts buffering in an external `BufferedReader`/
  `BufferedWriter` wrapper. The fast path bridges the two models via the
  `BufferedReadable` capability interface instead of integrating stream/drain
  into the reader vtable.
- **Zig can integrate `stream`/`drain` more deeply into the vtable** (e.g.
  negotiate buffer ownership, `rebase`). sluice's fast path is an opt-in probe,
  not a vtable extension, so it cannot do buffer rebasing or ownership transfer.
- **sluice still uses the scratch path when no buffered bytes are exposed.** A
  plain `Reader` (not `BufferedReadable`) always goes through
  `read_some(scratch) → write_all(scratch)`, identical to pre-006 behavior.

## 3. Why this is still not a performance claim

The counters are **observability hooks** (how often did the fast path vs the
scratch path serve bytes?), not throughput/latency numbers. No microbenchmark
exists yet (SLUICE-CORE-010). Whether draining the buffer saves enough to matter
— vs the `dynamic_cast` probe, the per-iteration `peek`/`consume`, or the
`write_all` round-trip — is unmeasured and must not be asserted. The discipline
from `docs/zig-std-io-parity-audit.md` holds: do not optimize before
measuring.

## 4. Deferred

- **Copy strategy layer** (SLUICE-CORE-007) — now that there are multiple copy
  paths (scratch, buffered fast path, vector write), a strategy layer should
  choose between them explicitly instead of letting `copy_all` accumulate
  heuristics.
- **Zero-copy kernel paths** (sendfile, splice, copy_file_range) — orthogonal,
  require a capability boundary (SLUICE-CORE-009).
- **`readv`/`writev` integration into the copy strategy** — vector I/O exists
  (SLUICE-CORE-005) but is not yet wired into a copy strategy.
- **io_uring** (SLUICE-CORE-012) — requires the 004–011 preconditions.
- **Async backend** — out of scope for the blocking core.

Zig `std.Io` remains a **design reference only**, not a dependency; no Zig
stdlib code is copied.
