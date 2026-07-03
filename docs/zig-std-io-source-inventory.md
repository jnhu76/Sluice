# Local Zig `std.Io` source inventory

**Status: CPPIO-CORE-012B.** Inventory of the local Zig source tree
(`./zig/lib/std/`), recorded so the parity audit (012C) and the io_uring
readiness gate (012D) rest on what is actually present, not on assumptions.
**No Zig code is copied** — only symbolic references and one-line concepts.

The local tree is notably richer than the MVP target: Zig `std.Io` is a full
cross-platform I/O + concurrency interface, while cppio deliberately captures
only the blocking-I/O subset.

## Layout discovered

```
zig/lib/std/Io.zig                 # the Io context + namespace
zig/lib/std/Io/Reader.zig          # Reader (interface-owned buffer)
zig/lib/std/Io/Writer.zig          # Writer (interface-owned buffer)
zig/lib/std/Io/File.zig            # File (open/read/write/sync)
zig/lib/std/Io/File/               # File helpers
zig/lib/std/Io/Uring.zig           # io_uring backend
zig/lib/std/Io/Threaded.zig        # thread-pool blocking backend
zig/lib/std/Io/Threaded/           # Threaded helpers
zig/lib/std/Io/Kqueue.zig          # BSD kqueue backend
zig/lib/std/Io/Dispatch.zig        # dispatch / completion plumbing
zig/lib/std/Io/RwLock.zig          # async-aware RwLock
zig/lib/std/Io/Semaphore.zig       # async-aware Semaphore
zig/lib/std/Io/Dir.zig             # directory operations
zig/lib/std/Io/Terminal.zig        # terminal
zig/lib/std/Io/net.zig + Io/net/   # networking
zig/lib/std/Io/fiber.zig           # fibers
zig/lib/std/os/linux/IoUring.zig   # low-level Linux io_uring wrapper
zig/lib/std/os/linux/IoUring/      # IoUring helpers
zig/lib/std/os/linux/io_uring_sqe.zig
```

There is **no** `Io/Evented.zig` in this tree; the BSD evented equivalent is
`Io/Kqueue.zig`. The spec's `Evented` candidate is "not applicable" here.

## Per-file inventory

| Zig path | Main concept | cppio component | cppio implemented? | Notes |
|---|---|---|---|---|
| `Io.zig` | The cross-platform I/O + concurrency context/namespace; re-exports backends (`Threaded`, `Uring`, `Kqueue`, `Dispatch`), `Reader`, `Writer`, `File`. Top doc lists fs/net/processes/time/random/async-await-cancel/locks/mmap. | `cppio::IoContext` (009) | Partial | cppio's `IoContext` is only the backend *factory* boundary; it has no concurrency/async/await/cancel. Zig `Io` is far broader. |
| `Io/Reader.zig` | Reader with an interface-owned buffer; `stream`, `readVec`/`readVecAll`, `peek`, exact-read, vtable. | `cppio::Reader` + `BufferedReadable` | Partial | `readVec`/`stream`/`peek` shapes are mirrored; buffer is external (wrapper) not interface-owned (gap). |
| `Io/Writer.zig` | Writer with interface-owned buffer; `writeVec`/`writeVecAll`, `drain`, `flush`. | `cppio::Writer` + `BufferedWriter` | Partial | `writeVec`/`flush` mirrored; `drain`/owned-buffer gap. |
| `Io/File.zig` | File: open/read/write/sync, backend-vtable driven. | `cppio::FileReader`/`FileWriter` | Partial | Blocking POSIX only; no backend vtable, no async open. |
| `Io/Uring.zig` | io_uring backend built on `os/linux/IoUring`. | (none) | **Not implemented** | The post-MVP spike target (013). |
| `Io/Threaded.zig` (+ dir) | Thread-pool blocking backend. | (none) | **Not implemented** | No async/thread-pool in cppio. |
| `Io/Kqueue.zig` | BSD kqueue evented backend. | (none) | **Not applicable** | Linux-only target; not pursued. |
| `Io/Dispatch.zig` | Dispatch / completion plumbing shared by backends. | (none) | **Not implemented** | Needs an async runtime cppio does not have. |
| `Io/RwLock.zig`, `Io/Semaphore.zig` | Async-aware locks/semaphores. | (none) | **Not applicable** | Concurrency primitives; out of scope. |
| `Io/Dir.zig`, `Io/Terminal.zig`, `Io/net*`, `Io/fiber.zig` | Dir/terminal/net/fibers. | (none) | **Not applicable** | Networking/timers/processes explicitly excluded from cppio scope. |
| `os/linux/IoUring.zig` (+ dir) | Low-level Linux io_uring syscall wrapper (`io_uring_setup`/`enter`/`register`, SQE/CQE). | (none) | **Not implemented** | The primitive layer under `Io/Uring.zig`. cppio's 013 spike will use liburing, not this directly. |

## Key confirmations for the audit (012C)

- `Reader.stream` exists at `Io/Reader.zig:168` — confirms the buffered fast-path
  model cppio mirrored in 006 (writes `r.buffer[r.seek..r.end]` first).
- `readVec`/`readVecAll` at `Io/Reader.zig:415`/`480`, `writeVec`/`writeVecAll`
  at `Io/Writer.zig:174`/`454` — confirms the vector-I/O shape cppio mirrored in 005.
- `Writer.flush` at `Io/Writer.zig:312` is the drain operation — confirms the
  flush≠durability separation cppio adopted in 008.
- `Io/Uring.zig` exists and is the richest backend — confirms io_uring is the
  meaningful parity gap, and that cppio's narrow spike (013) is a tiny fraction
  of what Zig's `Io.Uring` provides.

Zig source remains a **design reference only** — never a build/runtime
dependency, and no code is copied.
