# Zig `std.Io` parity audit

**Status: SLUICE-CORE-012C.** Full parity audit of sluice against the local Zig
`std.Io` source tree (inventoried in `docs/zig-std-io-source-inventory.md`).
This replaces ad-hoc gap notes with a single authoritative table.

```text
sluice intentionally models Zig std.Io concepts in C++ style.
It does not attempt source/API compatibility.
The largest remaining gaps are async/cancellation/backend scheduling and deeper interface-owned buffering.
```

Fidelity labels: **High** / **Partial** / **Intentional divergence** / **Deferred** / **Not implemented** / **Not applicable**.

## Parity table

| Zig concept | sluice equivalent | Fidelity | Status | Notes |
|---|---|---|---|---|
| `Io` capability/context (`Io.zig`) | `sluice::IoContext` (009) | Partial | Implemented | sluice's context is only a backend *factory* (`open_reader`/`open_writer`); Zig `Io` also owns concurrency/await/cancel/locks/mmap. |
| `Io.Threaded` (thread-pool blocking) | (none) | Not implemented | Out of MVP scope | No async/thread-pool in sluice. |
| `Io.Uring` (io_uring backend) | (none yet) | Not implemented | 013 spike | Narrow experimental write spike only; not parity. |
| `Io.Kqueue` (BSD evented) | (none) | Not applicable | — | Linux-only target. |
| `Io.Evented` | (none) | Not applicable | — | Not present in this Zig tree; kqueue is the BSD equivalent. |
| `Io.Dispatch` (completion plumbing) | (none) | Not implemented | Out of MVP scope | Needs an async runtime. |
| `Reader` (`Io/Reader.zig`) | `sluice::Reader` | Partial | Implemented | read_some/read_exact/stream_to mirrored; interface-owned buffer is the gap. |
| `Writer` (`Io/Writer.zig`) | `sluice::Writer` | Partial | Implemented | write_some/write_all/flush mirrored; `drain`/owned-buffer gap. |
| Reader-owned buffer | external `BufferedReader` + `BufferedReadable` | Intentional divergence | Implemented (006) | sluice keeps base Reader unbuffered; fast-path probe bridges to it. |
| Writer-owned buffer | external `BufferedWriter` | Intentional divergence | Implemented | Same wrapper-vs-owned divergence. |
| `Reader.stream` (`Reader.zig:168`) | `copy_all` buffered fast path | Partial | Implemented (006/007) | sluice drains `peek_buffered()` first; probe-based, not vtable-negotiated. |
| `Io.Limit` (copy/stream limit) | `sluice::CopyLimit` | High | Implemented (003) | unlimited/bytes/nothing; same shape. |
| exact-read equivalent | `sluice::Reader::read_exact` | High | Implemented | EOF/error semantics match sluice's stricter error model. |
| `writeAll` | `sluice::Writer::write_all` | High | Implemented | all-or-error, zero-progress → invalid_state. |
| `flush` (`Writer.zig:312`) | `sluice::Writer::flush` (drain) | Partial | Implemented (008) | Drain semantics are implemented for BufferedWriter; FileWriter::flush remains a non-durable no-op; durability is split into SyncableWriter. |
| `readVec`/`readVecAll` (`Reader.zig:415/480`) | `sluice::Reader::read_vec` | Partial | Implemented (005) | Conservative stop-on-short; no `readVecAll` yet. |
| `writeVec`/`writeVecAll` (`Writer.zig:174/454`) | `sluice::Writer::write_vec`/`write_all_vec` | High | Implemented (005) | shape + all-or-error helper both present. |
| File reader/writer (`Io/File.zig`) | `sluice::FileReader`/`FileWriter` | Partial | Implemented (002) | Blocking POSIX; no backend vtable, no async open. |
| Failure/error model | `sluice::Result<T>` + `sluice::IoError` | Intentional divergence | Implemented | sluice: errors returned, not swallowed; errno preserved on open. |
| Cancellation model | (none) | Not implemented | Out of MVP scope | Required before broad async. |
| Group/future/task model | (none) | Not implemented | Out of MVP scope | No async runtime. |
| Testing I/O model (`Io/test.zig`) | `FaultReader`/`FaultWriter` + in-memory readers/writers | Intentional divergence | Implemented | sluice's deterministic fault injection replaces Zig's `Io.Threaded`-based test io. |
| Async backend model | (none) | Not implemented | Out of MVP scope | — |
| io_uring backend model | `sluice::experimental::UringWriteBatch` / `UringIoContext` (013) | Partial — experimental spike only | Implemented (spike) | Narrow synchronous-over-uring write path, stub without liburing; NOT a production backend and NOT parity with Zig `Io.Uring`. See `docs/io-uring-spike.md`. |

## Conclusion

sluice intentionally models Zig `std.Io` **concepts** in C++ style (external
wrappers, explicit strategy layer, Result-based errors, opt-in capability
interfaces) rather than porting the source or API. The high/partial rows are the
MVP; the rows marked **Not implemented** are the post-MVP frontier. The largest
remaining gaps are:

1. **Async/cancellation/backend scheduling** — no `Io.Threaded`/`Dispatch`/
   await/cancel equivalent. This is the precondition gap before any production
   io_uring backend.
2. **Deeper interface-owned buffering** — sluice uses external wrappers + a
   fast-path probe instead of Zig's vtable-negotiated owned buffer (`stream`/
   `drain`/`rebase`). Bridged for the copy case, not generally.

The experimental io_uring write spike (013) does **not** close these gaps — it
probes whether a narrow write path is even expressible without them. See
`docs/io-uring-readiness-gate.md`.
