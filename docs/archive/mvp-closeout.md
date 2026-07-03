# MVP closeout

**Status: SLUICE-CORE-012A.** This document closes out the MVP (SLUICE-CORE-001
through 011) and sets up the post-MVP readiness gate (012D) for an experimental
io_uring spike (013).

```text
The MVP is complete as a blocking, measurable, Zig-inspired C++ I/O core.
io_uring is a post-MVP experimental backend candidate.
```

## 1. MVP scope

A blocking, measurable, Zig-`std.Io`-inspired C++ I/O core: minimal
`Reader`/`Writer` primitives, composable wrappers (buffering, observing, fault
injection), a POSIX file backend, a copy primitive with an explicit strategy
layer, separated flush/sync/durability semantics, a backend capability boundary,
and a microbench harness feeding an evidence-linked optimization matrix. The MVP
is explicitly **not** async, **not** io_uring, and makes **no** universal
performance claims.

## 2. Completed components

| Job | Component |
|---|---|
| 001 | Zig-`std.Io`-inspired blocking C++ core (`Reader`/`Writer`/`Result`/`IoError`) |
| 002 | Correctness hardening (POSIX `FileReader`/`FileWriter`, EINTR, errno preservation) |
| 003 | Copy/stream limits (`CopyLimit`) + flush-contract docs |
| 004 | Measurement hooks (`SyscallStats`/`BufferStats`/`CopyStats`) + Zig gap calibration |
| 005 | Vector I/O (`read_vec`/`write_vec`/`write_all_vec`) + POSIX `readv`/`writev` overrides |
| 006 | MVP model + buffered fast path (`BufferedReadable` + `copy_all` probe) |
| 007 | Copy strategy layer (`CopyStrategy`/`CopyOptions`/`CopyDecision`) |
| 008 | Flush/sync/durability separation (`SyncableWriter`, `wal::WalWriter` LSN invariant) |
| 009 | Backend boundary (`IoContext` / `BlockingIoContext`) |
| 010 | Core microbench harness (CSV + 4 benches + run script) |
| 011 | Optimization runbook + summarizer + decision matrix |

## 3. Examples that prove composition

- `mvp_copy_pipeline` — FileReader → BufferedReader → copy_all → BufferedWriter → ObservedWriter → FileWriter
- `mvp_limited_copy` — `CopyLimit::bytes(N)` with stop-reason stats
- `mvp_wal_vector` — WAL records via `write_record_vec`, read back with `read_record`
- `mvp_copy_strategy` — Scratch / BufferedFirst / Auto / deferred-rejected / deferred-fallback
- `mvp_wal_durable` — WAL durability boundary with written/flushed/durable LSN output
- `mvp_io_context_copy` — opens reader/writer through `BlockingIoContext`, copies, syncs

## 4. Tests and verification summary

32 tests, all green in both debug and release. Coverage spans result/error
semantics, every wrapper, vector I/O (default + POSIX), the copy strategy layer
(all strategies + deferred handling + counters), sync/durability (LSN invariant
on all paths), and the IoContext boundary (open errors at open time, stats
wiring, SyncableWriter detection). Review gates stay clean: no io_uring
implementation, no universal performance claims in source.

## 5. Current backend model

A single blocking POSIX backend (`BlockingIoContext` → `FileReader`/`FileWriter`),
with the direct constructors still valid. `IoContext` is the abstract seam; the
backend choice is centralized there, handle *use* is backend-agnostic. No async,
no thread pool, no io_uring.

## 6. Known limitations

```text
No io_uring implementation yet.
No async backend yet.
No cancellation model yet.
No production backend switch.
No group commit coordinator.
FileWriter::flush() does not imply durability (by design).
No MemoryIoContext yet (deferred 009D).
Microbench results are local/workload-specific, NOT portable claims.
Zig stdlib remains a design reference only.
```

## 7. Why io_uring is not part of MVP

io_uring requires an async/evented submission-completion model, a cancellation
story, and careful buffer-lifetime discipline — none of which the blocking MVP
models. Adding it prematurely would either conflate flush/sync/durability,
require a scheduler the core does not have, or risk a half-built async runtime.
The MVP deliberately stays blocking so the correctness + measurement foundation
(004–011) can mature first. io_uring is explored as an **isolated experimental
spike** in 013, never as the default backend.

## 8. Next readiness gate

`docs/io-uring-readiness-gate.md` (012D) decides whether sluice is ready for the
narrow experimental spike and records the chosen first slice, risks, and abort
conditions. The audit in `docs/zig-std-io-parity-audit.md` (012C) and the source
inventory in `docs/zig-std-io-source-inventory.md` (012B) feed that decision.
