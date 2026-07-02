# Zig `std.Io` ↔ cppio gap calibration

Compares the local Zig source (`./zig/lib/std/Io/`) against the current C++
core. Status: after CPPIO-CORE-005. Zig is a **design reference only** — never a
build/runtime dependency.

## 1. Scope

This document tracks how faithfully the cppio C++ core mirrors Zig `std.Io`'s
*abstractions*, and where the divergences are. It is not a line-by-line port
log; it is a calibration table for deciding which gaps matter and in what order
to close them. No performance claims — measurements land in CPPIO-CORE-010/011.

## 2. Mapping table

| Zig `std.Io` idea | cppio equivalent | Fidelity |
|---|---|---|
| `Reader` (vtable + interface-owned buffer) | `cppio::Reader` (virtual, **no** internal buffer) | partial — see §4.1 |
| `Writer` (vtable + interface-owned buffer) | `cppio::Writer` (virtual, **no** internal buffer) | partial — see §4.1 |
| `Reader.readSliceAll` (exact-fill) | `Reader::read_exact` | faithful |
| `Reader.readSliceShort` (may short-read) | `Reader::read_some` | faithful |
| `Reader.stream` / `streamExact` | `Reader::stream_to` / `copy_all` (+ `CopyLimit`) | faithful semantics, **not** zero-copy — see §4.2 |
| `Writer.writeAll` | `Writer::write_all` | faithful (stricter on zero-progress) |
| `Writer.write` (may short-write) | `Writer::write_some` | faithful |
| `Writer.flush` (unified drain) | split: `BufferedWriter::flush` drains, `FileWriter::flush` is a no-op | intentional split — see §4.3 |
| `Io.File.Reader`/`Writer` (POSIX) | `FileReader`/`FileWriter` | faithful (RAII, move-only, errno-mapped) |
| `Io.Limit` (sentinel enum) | `CopyLimit` (Kind + remaining) | C++-friendly equivalent |
| `Io.Reader.Limited` wrapper | (none — `CopyLimit` covers the bound) | not needed |
| `std.testing.io` fault model | `FaultReader`/`FaultWriter` + `FaultPlan` | original design, not a port — see §4.5 |
| `Reader.peek` / buffered fast path | `BufferedReadable` + `copy_all` fast path | **Partial / Improved in CPPIO-CORE-006** — see §4.2 |
| `readv`/`writev` (vector I/O) | `read_vec`/`write_vec`/`write_all_vec` + POSIX overrides | **Partial / Improved in CPPIO-CORE-005** — see §4.6 |

## 3. High-fidelity areas

- **Reader/Writer virtual abstractions** with derived primitives (`read_some`,
  `write_some`, `flush`) and derived operations (`read_exact`, `write_all`,
  `stream_to`, `copy_all`). Short I/O and EOF (`0` not exception) match Zig.
- **Error propagation**: errors are returned through `Result<T>`/`IoError`, never
  swallowed; `write_all` rejects zero-progress on non-empty input (stricter than
  Zig, which relies on a documented `drain` contract).
- **POSIX file backend**: RAII fd ownership, move-only, real `errno` preserved
  on failed `open()`, `EINTR` retried via `detail::retry_on_eintr`.
- **`CopyLimit` bounded copy**: `unlimited`/`bytes(n)`/`nothing` honor Zig's
  "EOF before the limit is success" and "never over-read past remaining" rules.
- **Fault injection**: deterministic `FaultPlan` with byte-granular clamping —
  the project's first correctness harness.

## 4. Intentional divergences

### 4.1 Buffer ownership: wrapper model vs interface-owned
Zig keeps buffer state **inside** `Reader`/`Writer` (`r.buffer/seek/end`,
`w.buffer/end`); every reader is inherently buffered and the vtable negotiates
around the buffer. cppio deliberately uses **external** `BufferedReader`/
`BufferedWriter` wrappers (approved by the CPPIO-001 design table). This makes
Zig's `rebase`, `preserve_len`, and the `Discarding`/`Allocating` writers have
no direct analog here. Approved divergence.

### 4.2 Copy fast path: partial — improved in CPPIO-CORE-006
Zig `Reader.stream` (`Reader.zig:168`) writes `r.buffer[r.seek..r.end]` (already
buffered, unread bytes) directly to the writer before invoking the vtable stream
— a zero-copy fast path because the buffer lives inside the reader. cppio's base
`Reader` is unbuffered, so 006 closes this gap **without a rewrite**: an opt-in
`BufferedReadable` capability interface lets `BufferedReader` expose its
`buf_[seek_..end_)` region, and `copy_all` drains it via `peek_buffered()`/
`consume_buffered()` before falling back to the scratch path. Detection is a
`dynamic_cast` probe, so unbuffered readers pay nothing. The wrapper-vs-
interface-owned-buffer divergence remains, and the scratch path still runs when
no buffered bytes are exposed — see `docs/buffered-fast-path.md`. Not a
performance claim; measurement lands in CPPIO-CORE-010.

### 4.3 Flush is split, not uniform
Zig's single `flush` contract does the right thing regardless of buffering.
cppio splits it: `FileWriter::flush()` is a documented no-op (no `fsync` this
phase), `BufferedWriter::flush()` does the work. Callers must know which layer
is buffered. See `README.md` "Flush contract" and CPPIO-CORE-008.

### 4.4 Error model flattened
Zig: `Reader.Error = {ReadFailed, EndOfStream}`, `Writer.Error = {WriteFailed}`;
backend detail lives in the implementation. cppio's `IoError::Code` carries
eight categories + raw `os_errno`. The `backend_error` code plays Zig's
`ReadFailed`/`WriteFailed`. Considered an improvement for C++ branching.

### 4.5 Fault wrappers are original, not a Zig port
Zig's current `std.testing.io` is built on an `Io.Threaded` context, not a
simple fault-injecting recorder. `FaultReader`/`FaultWriter` are this project's
design to satisfy the "deterministic fault injection" requirement.

### 4.6 Vector I/O: partial fidelity, improved in CPPIO-CORE-005
CPPIO-CORE-005 added `read_vec`/`write_vec`/`write_all_vec` plus POSIX
`readv`/`writev` overrides on the file backends and a `write_record_vec` WAL
path; CPPIO-CORE-005B then aligned the default fallback to conservative
readv/writev-style semantics (stop on EOF / error / first positive short result)
— see `docs/readv-writev-design-note.md`. The *shape* now matches Zig's
`readVec`/`readVecAll`/`writeVec`/`writeVecAll`. One deliberate semantic
divergence remains (not a bug):

- **Buffer model.** Zig's vector primitives negotiate around the
  interface-owned buffer inside each Reader/Writer; cppio's default fallback
  loops directly over `read_some`/`write_some` (no internal buffer — §4.1).

cppio's error model on partial progress is also stricter by decision: errors are
propagated immediately even after some slices made progress (matching
`read_exact`/`write_all`), where Zig's `readVec` swallows `EndOfStream` when
bytes were already read. This is documented as intentional and may be revisited.

cppio now has vector primitives. It still does not have Zig's full
interface-owned buffer model, and it still does not implement async / uring /
cancellation. Zig `std.Io` remains a design reference only, not a dependency.

## 5. Measurement gap (post-005)

CPPIO-CORE-004 added the measurement structs (`SyscallStats`, `BufferStats`,
`CopyStats`) and CPPIO-CORE-005 added `VectorStats`; all are wired through
optional, caller-owned `Stats*` pointers. This closes the *observability* gap —
we can now count syscalls, buffer hits/misses, copy stop reasons, and vector
calls/bytes/iovecs (split into fallback vs real readv/writev). What remains
unmeasured until CPPIO-CORE-010: actual wall-clock throughput/latency, and the
relative cost of the scratch copy vs a would-be zero-copy fast path.

## 6. Deferred fidelity items

| Item | Job | Why deferred |
|---|---|---|
| ~~Buffered fast path (zero-copy copy)~~ | ~~CPPIO-CORE-006~~ | **done in 006 (partial — see §4.2)** |
| ~~`readv`/`writev` vector I/O~~ | ~~CPPIO-CORE-005~~ | **done in 005 (partial — see §4.6)** |
| Copy strategy layer | CPPIO-CORE-007 | needs 005/006 to choose between paths |
| Flush/sync/durability split | CPPIO-CORE-008 | durability is out of correctness-phase scope |
| `IoContext` capability object | CPPIO-CORE-009 | backend boundary before any async |
| io_uring | CPPIO-CORE-012 | requires 004–011 preconditions |

## 7. Next-step recommendation

Follow the roadmap's strict order: **007 (copy strategy layer) → 010 (microbench)
→ 011 (decision matrix)**. With 006 landed, `copy_all` now has multiple paths
(scratch, buffered fast path, vector write) and a strategy layer should choose
between them explicitly rather than accumulate heuristics. Do not attempt
io_uring (012) until measurement data from 010 exists — the discipline is "do
not optimize before measuring."
