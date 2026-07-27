# Zig `std.Io` → cppio async port map

**Status: SLUICE-CORE-023A.** Authoritative source-graph, semantic inventory, and
migration dependency map for translating Zig `std.Io`'s async execution
architecture into idiomatic modern C++. Derived **from the local Zig source
tree** (`./zig/lib/std/`), not from memory or blog posts. Symbolic references
and one-line signatures only — **no Zig code is copied**; Zig remains a design
reference, never a build/runtime dependency (xmake.lua:1-2).

This document supersedes the design-level inventory/parity notes in
`docs/zig-std-io-source-inventory.md` (012B) and `docs/zig-std-io-parity-audit.md`
(012C) **for the async execution layer**. Those docs predate the async
foundation (jobs 017–022) and describe the MVP-era blocking-only baseline; they
remain accurate for the blocking Reader/Writer/File layer but understate the
Zig async model. The backend parity audit that consumes this map is
`docs/async-backend-parity.md` (023B). The job-card sequence built from it is
`docs/zig-stdio-migration-jobs.md` (023C).

## License / attribution

The local Zig source tree is the upstream Zig standard library (MIT-licensed,
`zig/LICENSE`). It is used here **as a semantic reference only**. cppio never
links, compiles, or copies Zig source. Where cppio's structure is strongly
derived from upstream, the relevant upstream file/function names are recorded
in this map and in commit/ADR provenance per task §0 (HARD SCOPE LOCK).

---

## A. Upstream source inventory

All paths under `./zig/lib/std/`. Line numbers are to the local tree and may
drift with upstream updates; the *names* are stable.

| Zig source | Important types / functions | Architectural role | cppio status |
| ---------- | --------------------------- | ------------------ | ------------ |
| `Io.zig` | `Io` (`:15`), `VTable` (`:51`), `Operation` (`:257`), `Operation.Storage` (`:400`), `Batch` (`:474`), `operate` (`:451`), `Future` (`:1176`), `Group` (`:1218`), `Select` (`:1367`), `Cancelable` (`:704`), `CancelProtection` (`:1322`), `recancel` (`:1310`), `Mutex` (`:1587`), `Condition` (`:1653`), `Event` (`:1766`), `Queue`/`TypeErasedQueue` (`:1872`), futex (`:1552`) | The capability context + the entire async/concurrency vtable boundary. Owns nothing; passed by value. | PARTIAL — `sluice::async::AsyncIoContext` is the L1 foundation only; no Future/Group/Select/Batch/Operation-union/futex/cancel-protection/recancel. |
| `Io/Threaded.zig` | `Threaded` (`:25`), `worker` (`:1736`), `Future` (`:632`), `Group.Task` (`:484`), `Syscall` RAII (`:1342`), `cancelAwaitable` (`:1199`), `signalAllCanceledSyscalls` (`:1265`), parking futex (`:17261`) | Thread-per-task backend. **No scheduler, no event loop, no fibers.** Every "operation" is a synchronous syscall; concurrency = #OS threads. Cancel via Unix signals / Windows APC. | MISSING — `ThreadPoolBackend` spawns one thread per op (same shape) but has no `Syscall` cancel region, no `cancelAwaitable`, no signal-based interrupt. |
| `Io/Uring.zig` | `Evented` (`:15`), `Thread` (`:94`), `Fiber` (`:149`), `idle` event loop (`:1147`), `yield` (`:964`), `Fiber.create/destroy` (`:273`/`:305`), `Fiber.requestCancel` (`:357`), `CancelRegion` (`:415`), `Completion.Userdata` (`:1093`) | Linux io_uring evented backend. Per-thread ring + per-thread fiber pool + per-thread ready/free queues + cross-thread work stealing. **Real scheduler, real fibers, real async op execution.** | PARTIAL — `UringAsyncBackend` is single-driver-thread, no fibers, no scheduler, no stealing. Submits SQEs and reaps CQEs from one thread. Cannot express the Evented model. |
| `Io/Kqueue.zig` | `Evented` (same skeleton as Uring) | BSD kqueue evented backend. Same `Thread`/`Fiber`/`yield`/`findReadyFiber`/`schedule` skeleton as Uring, kqueue-specific kevent wait queues. | NOT YET APPLICABLE — BSD-only; not pursued on Linux host. |
| `Io/Dispatch.zig` | `Evented` (`:25`), `Fiber` (`:99`), `mainLoop` (`:620`), `yield` (`:588`), `SwitchMessage` (`:1268`), emulated futex (`:1490`) | macOS libdispatch backend. Real scheduler but threads owned by libdispatch, not Zig. Fibers ride on dispatch queues. | NOT YET APPLICABLE — macOS/libdispatch; not pursued on Linux host. |
| `Io/fiber.zig` | `Context` (`:7`), `Switch` (`:26`), `contextSwitch` (`:29`), `supported` (`:1`) | Architecture-specific stack-switching asm. aarch64/riscv64/x86_64 only. Three-word save area (sp/fp/pc). | MISSING — no stack-switching in cppio. Required before any Evented/fiber model. |
| `Io/RwLock.zig` | `RwLock` (state + `Io.Mutex` + `Io.Semaphore`) | Io-aware reader/writer lock; suspends task on contention. | MISSING. |
| `Io/Semaphore.zig` | `Semaphore` (`Io.Mutex` + `Io.Condition` + permits) | Io-aware counting semaphore; suspends task on zero permits. | MISSING. |
| `Io/File.zig`, `Io/File/Reader.zig`, `Io/File/Writer.zig`, `Io/File/MultiReader.zig`, `Io/File/Atomic.zig`, `Io/File/MemoryMap.zig` | File + positional/streaming read/write + `fileSync` (`Io.zig:193`) | The file-I/O surface that backends implement. `fileSync` is a **direct vtable method**, NOT a batchable `Operation` variant. | PARTIAL — blocking `FileReader`/`FileWriter` exist; no backend-vtable-driven file, no async open. The async `SyncDataOp`/`SyncAllOp` (018B) carry the durability surface as a separate op set, not as a vtable method — a deliberate cppio divergence. |
| `Io/Reader.zig`, `Io/Writer.zig` | interface-owned-buffer Reader/Writer, `readVec`/`readVecAll`, `writeVec`/`writeVecAll`, `stream`, `drain`, `flush` | The buffer-owning streaming I/O vtables. | PARTIAL — cppio uses external `BufferedReadable`/wrapper strategy (intentional divergence, 012C). |
| `Io/net.zig`, `Io/net/HostName.zig`, `Io/Terminal.zig`, `Io/Dir.zig` | net, terminal, dir | Out of cppio scope (file-I/O only, ADR §A5). | INTENTIONALLY DIFFERENT. |

### Key structural confirmations from source

1. **There is no `Io/Evented.zig` file.** "Evented" is the *internal name* of the
   `Uring` and `Kqueue` structs (`Uring.zig:15`, `Kqueue.zig:ditto`). The Zig
   compile-time selector (`Io.zig:28-39`) picks `Uring` on Linux, `Kqueue` on
   BSD, `Dispatch` on Apple, gated by `fiber.supported`.
2. **The `Operation` tagged union has exactly four variants** (`Io.zig:257-389`):
   `file_read_streaming`, `file_write_streaming`, `device_io_control`,
   `net_receive`. **There is no `file_sync` variant** — durability is the
   synchronous `fileSync` vtable method (`Io.zig:193`). cppio's choice to model
   sync as an async op (`SyncDataOp`/`SyncAllOp`, job 018B) is a **deliberate
   divergence** documented in `docs/adr/ADR-async-io-model.md` §6 P3 / job 018B.
3. **Three distinct concurrency tiers** in the vtable (`Io.zig:61-154`):
   `operate` (sync, current task), `batchAwaitAsync` (may be concurrent),
   `batchAwaitConcurrent` (**must** be concurrent, can fail
   `ConcurrencyUnavailable`). cppio's L1 surface collapses these into one
   `submit_*` + `poll`/`wait_one` pair.
4. **Cancellation is single-shot per cancellation point** with explicit
   re-arming (`recancel`, `Io.zig:1310`) and delivery-blocking protection
   (`CancelProtection`, `Io.zig:1322`). cppio has `IoError::canceled` and a
   best-effort `cancel(Completion&)` but no protection regions and no recancel.
5. **`Operation.Storage.Pending.Userdata = [7]usize`** (`Io.zig:416`) is the
   per-op backend scratch contract — the maximum state a backend may stash per
   in-flight op. Worth mirroring exactly when cppio introduces a real op slot.

---

## B. Layer map

Derived from source. The model the source *actually* resembles (the schematic
in task §B is confirmed, with one refinement: Zig has no separate
"synchronization capabilities" layer — Mutex/Condition/Event/Queue/RwLock/
Semaphore all live directly on the Io futex substrate, which is itself a
vtable entry, not a separate layer):

```text
Application (caller-provided blocking-shaped code)
    │
    ▼
Io capability/context (Io.zig:15) ── passed BY VALUE, owns nothing
    │
    ├─── task/concurrency capabilities: Future / Group / Select / Batch
    │    (Io.zig:1174-1538, 474-624) ── the high-level awaitables
    │
    ├─── operation capabilities: Operation union (4 variants) + operate
    │    (Io.zig:257-467)
    │
    ├─── cancellation capabilities: Cancelable / CancelProtection / recancel
    │    (Io.zig:704, 1322, 1310)
    │
    ├─── synchronization capabilities: Mutex / Condition / Event / Queue /
    │    RwLock / Semaphore, all on the futex substrate (Io.zig:1552-2312,
    │    Io/RwLock.zig, Io/Semaphore.zig)
    │
    ▼
Io VTable (Io.zig:51-255) ── the backend boundary (function-pointer struct)
    │
    ├─── async/concurrent/await/cancel/group/futex ── task-execution surface
    ├─── operate/batchAwait*/batchCancel ── op-execution surface
    └─── fileXxx/dirXxx/netXxx/processXxx/now/sleep/random ── direct surface
    │
    ▼
Io implementation (selected at Io.zig:28-39)
    │
    ├─── Threaded (always available)
    │        └── synchronous syscalls + 1-OS-thread-per-task + signal-based cancel
    │
    └─── Evented (conditional on fiber.supported + OS)
             ├─── Uring  (Linux): per-thread ring + fiber pool + work stealing
             ├─── Kqueue (BSD):   per-thread kq + fiber pool + work stealing
             └─── Dispatch(Apple): libdispatch queues + fiber pool
                     │
                     ├─── task runtime: Fiber + yield + contextSwitch (fiber.zig)
                     ├─── stack switching: fiber.zig asm (sp/fp/pc, 3 arches)
                     ├─── scheduler: ready_queue/free_queue per Thread + stealing
                     └─── platform event backend: io_uring / kqueue / dispatch sources
```

**Critical layering fact for cppio:** in Zig, the *backend* (`Threaded`/
`Uring`/`Kqueue`/`Dispatch`) implements the **entire** `VTable` — both the
task-execution surface (async/await/cancel/group/futex) AND the op-execution
surface (operate/batchAwait). The two surfaces are *not* layered; they are
*co-implemented* by the backend. This is the source-derived reason the parity
audit (§D below, doc 023B) must decide whether cppio's `AsyncBackend` (which
only implements op execution) can ever host a task runtime, or whether a
task-runtime layer must be added *above* it.

---

## C. Semantic inventory

For each important abstraction: purpose, ownership, lifetime, terminal states,
blocking points, suspension points, wakeup rules, cancellation semantics,
thread-affinity assumptions, scheduler assumptions, platform-specific behavior.

### C.1 `Io` (the capability context) — `Io.zig:15`

- **Purpose:** fat pointer `{userdata, vtable}`; the single argument threaded
  through every Io-aware call.
- **Ownership:** none. The `Io` value owns no resource; the backend behind
  `userdata` owns rings/threads/fibers.
- **Lifetime:** copyable, no destructor. Backend lifetime is the
  responsibility of whoever constructed the backend (e.g. `Threaded.deinit`,
  `Uring.deinit`).
- **Terminal states:** n/a (value type).
- **Blocking points:** every vtable method *may* block (backend-defined).
- **Suspension points:** on Evented backends, every Io function is a potential
  fiber suspension point.
- **Wakeup rules:** backend-defined.
- **Cancellation semantics:** Io functions return `Cancelable!T` iff they are
  cancellation points; `Cancelable = error{Canceled}` (`Io.zig:704`).
- **Thread-affinity:** Evented backends are per-thread (`threadlocal var self`,
  `Uring.zig:108`); the `Io` value is bound to the thread that constructed it.
- **Scheduler assumptions:** the vtable assumes a backend that can park/wake
  the caller; on Threaded this is the parking futex, on Evented this is the
  fiber yield.
- **Platform:** all (with backend selection).

### C.2 `Operation` (the op descriptor) — `Io.zig:257`

- **Purpose:** tagged union of the four batchable streaming ops. Carries
  buffers + result type per variant.
- **Ownership:** caller-owned. The caller constructs the `Operation` value and
  hands it to `operate` or to `Batch.add`.
- **Lifetime:** the value lives as long as the caller needs; the buffers it
  references must outlive the outstanding op (cppio ADR §5 L1-L3c mirrors this).
- **Terminal states:** an op becomes a `Result` via `operate` or via a
  `Batch.Completion` (`Io.zig:538`).
- **Blocking points:** `operate` blocks the current task until the result is
  ready.
- **Suspension points:** on Evented, `operate` suspends the current fiber.
- **Wakeup rules:** the backend's completion mechanism (CQE / kevent / dispatch
  source / thread-finish) publishes the result and wakes the task.
- **Cancellation:** `operate`/`batchAwait*` return `Cancelable!…`; cancel
  propagation is through `cancel`/`batchCancel`.
- **Thread-affinity:** the op executes on the backend of the `Io` it was
  submitted through.
- **Scheduler assumptions:** a backend that can execute the op.
- **Platform:** file ops everywhere; net/device variants platform-gated.

### C.3 `Operation.Storage` (the caller-provided slot) — `Io.zig:400`

- **Purpose:** the four-state backing storage for one in-flight op: `unused →
  submission → pending → completion → unused`.
- **Ownership:** caller-owned (`[]Operation.Storage` array passed to `Batch`).
- **Lifetime:** slots are recycled by `Batch.next` (`Io.zig:551`) which moves a
  completed slot back to `unused`.
- **Terminal states:** `completion` (carrying `Result`) is the per-op terminal;
  `next()` recycles it.
- **Pending.Userdata = [7]usize** (`Io.zig:416`): per-op backend scratch.

### C.4 `Batch` (the awaitable group) — `Io.zig:474`

- **Purpose:** the lowest-level awaitable. Groups N ops over a
  `[]Operation.Storage` array; awaited as a whole.
- **Ownership:** caller owns the storage array; `Batch` owns four intrusive
  lists (`unused/submitted/pending/completed`) + a `userdata` backend-scratch
  pointer.
- **Lifetime:** bounded by the caller's scope; `cancel` (`Io.zig:601`) returns
  all slots to `unused` and asserts `userdata == null` on return (backend
  released all resources).
- **Ordering:** `next()` returns completions in **backend completion order**
  (the `completed` list is built by the backend). Not FIFO.
- **Cancellation:** `batchCancel` moves `submitted → unused`, requests backend
  cancel; ops that raced and completed anyway appear in `completed`; ops that
  were canceled are simply absent. **Exactly-once:** every op ends up in
  `completed` OR dropped, never both, never neither.
- **Concurrency tiers:** `awaitAsync` (may be concurrent) vs `awaitConcurrent`
  (must be concurrent, can fail `ConcurrencyUnavailable`).

### C.5 `Future(Result)` — `Io.zig:1176`

- **Purpose:** the awaitable for one async task. `{any_future: ?*AnyFuture,
  result: Result}`.
- **Ownership:** caller-owned storage; `result` IS the caller-provided result
  slot (`Io.async` passes `&future.result` to the backend).
- **Lifetime:** `any_future == null` ⇒ synchronous completion (result already
  materialized) OR already-awaited. `await`/`cancel` null it out → idempotent.
- **Terminal states:** `result` populated, `any_future == null`.
- **Not thread-safe** (documented): one awaiter.
- **Cancellation:** `cancel(io)` initiates cancel; `await(io)` returns the
  result regardless. Both idempotent.

### C.6 `Group` — `Io.zig:1218`

- **Purpose:** unordered set of tasks, awaitable only as a whole. Cancel-
  propagation boundary (group tasks swallow `error.Canceled`).
- **Ownership:** `{token: atomic ?*anyopaque, state: usize}`; token read with
  `.acquire`.
- **Resource guarantee** (`Io.zig:1211`): per-task resources freed when each
  task returns, not at group completion.
- **Idempotent** await/cancel; safe to call concurrently with `async`/`concurrent`.

### C.7 `Select(U)` — `Io.zig:1367`

- **Purpose:** higher-level than `Batch`. Spawns tasks whose results are
  `@unionInit`-wrapped into `U`; awaits one result at a time via an internal
  `Queue(U)`.
- **Cancellation:** `cancel()` cancels the group, closes the queue, drains one
  remaining result.

### C.8 Cancellation — `Io.zig:704, 1310, 1322`

- **`Cancelable = error{Canceled}`** (`Io.zig:704`): the single cancellation
  error.
- **Single-shot per point:** only the *next* cancellation point in a task
  returns `Canceled`; subsequent points do not re-signal unless `recancel`.
- **`recancel(io)`** (`Io.zig:1310`): re-arms so the next point fires again.
  Used by `Queue` to report partial progress before re-propagating.
- **`CancelProtection = enum(u1){unblocked, blocked}`** (`Io.zig:1322`):
  per-task state. `blocked` makes no Io function a cancellation point.
  **Delivery-blocking, not request-blocking** — the cancel request is recorded,
  delivery is deferred.
- **`checkCancel(io)`** (`Io.zig:1356`): pure cancel point for CPU-bound loops.

### C.9 Concurrency primitives on the futex substrate

All suspend the *task* on Evented (fiber yield) or block the *thread* on
Threaded (parking futex). None spawns threads.

- **futex** (`Io.zig:1552`): `futexWait`/`futexWaitTimeout`/`futexWaitUncancelable`/
  `futexWake`, all cancelable, T must be 4 bytes.
- **Mutex** (`Io.zig:1587`): three-state futex mutex (unlocked → locked_once →
  contended).
- **Condition** (`Io.zig:1653`): epoch-counter futex to defeat lost wakeups.
- **Event** (`Io.zig:1766`): the value IS the futex word (`unset/waiting/is_set`).
- **Queue/TypeErasedQueue** (`Io.zig:1872`): MPSC+MPSC ring with suspended
  putters/getters; partial-transfer-on-cancel via `recancel`.
- **RwLock** (`Io/RwLock.zig`): state + Mutex + Semaphore.
- **Semaphore** (`Io/Semaphore.zig`): Mutex + Condition + permits.

### C.10 Backend semantics — Threaded (`Io/Threaded.zig`)

- **Worker model:** no fixed pool. Workers spawned **lazily, one per task**,
  never exit until `deinit`. One global FIFO run-queue under one mutex+condvar.
  **No work stealing.** Detached threads; lifetime bounded by `wait_group`.
- **Task = thread:** a task IS the body a worker thread runs synchronously.
  **No task stack separate from the OS stack.** No preemption, no suspension,
  no continuation — once a worker picks a task it runs it to completion.
- **Operation execution:** `operate` (`Threaded.zig:2543`) routes only the four
  streaming ops, all of which block the calling thread in a syscall
  (`readv`/`preadv`/`fsync`/`openat`). `fileSync` and all other file ops are
  direct vtable methods that block the caller. **There is no asynchronous
  operation execution in Threaded.** Concurrency = number of OS threads.
- **Completion delivery:** futex wake on a stack-local atomic
  `num_completed`; result copied by the awaiter after wake.
- **Cancellation:** `cancelAwaitable` cmpxchg'es `Thread.status` to `.canceling`;
  `signalAllCanceledSyscalls` sends `pthread_kill(SIGIO)` / `tgkill(.IO)` /
  `NtCancelSynchronousIoFile` to interrupt the blocked syscall. Exponential
  backoff re-send until `num_completed` flips.
- **Shutdown:** set `join_requested`, broadcast, `wait_group.wait()`. In-flight
  syscalls finish naturally; queued tasks abandoned. No special drain.
- **Capacity:** `async_limit` defaults to `cpu_count - 1`; `concurrent_limit`
  unlimited. At limit, `async` runs inline; `concurrent` errors
  `ConcurrencyUnavailable`.

### C.11 Backend semantics — Uring Evented (`Io/Uring.zig`)

- **Worker model:** per-thread `Thread` (`Uring.zig:94`) with its own
  `io_uring`, two lock-free fiber queues (`ready_queue`/`free_queue`), three
  steal cursors. Cross-thread work stealing (idle-search, ready-steal,
  free-steal). Worker rings created `R_DISABLED` + `ATTACH_WQ`, enabled via
  `REGISTER_ENABLE_RINGS`; cross-thread wakeup via `MSG_RING` SQE with
  `IOSQE_CQE_SKIP_SUCCESS`.
- **Fiber pool:** per-thread LIFO free-queue, stolen across threads (up to 4
  attempts). `Fiber.create`/`destroy` (`Uring.zig:273`/`305`). Slab = header +
  512B result + **60 MiB stack** (`min_stack_size = 60*1024*1024`, `:256`) +
  closure trailer.
- **SQE submission:** `Thread.enqueue()` (`:128`) gets an SQE (submit+retry on
  full). `user_data` = `*Fiber` (LSB `0b00`) | batch slot (`0b10`) | timeout
  (`0b11`) | reserved small enum (`wakeup`/`futex_wake`/`close`/`cleanup`/
  `exit`). Cancel requests use `user_data = wakeup` + `addr = target fiber`
  (distinct identity from the target op).
- **CQE reap loop:** `idle(ev, thread)` (`:1147`). Inner: drain ready fibers.
  When idle: `io_uring.submit_and_wait(1)`, batch-copy up to 256 CQEs, dispatch
  by `user_data` LSBs. A fiber-pointer CQE writes the result into the fiber's
  result area and enqueues it on the ready list; the next pass resumes it.
- **Fiber suspension/resume:** `yield(ev, maybe_ready_fiber, pending_task)`
  (`:964`) is the single seam → `Io.fiber.contextSwitch`. Suspension in a
  blocking op = enqueue SQE then `yield(null, .nothing)`. Resume = the CQE
  reaper enqueues the fiber and the idle loop switches into it.
- **Cancellation:** per-fiber atomic `cancel_status = {requested, awaiting}`
  (`:167`). `CancelRegion` RAII (`:415`) erected around every cancelable op;
  if `requested` is observed at region entry, returns `Canceled` before
  submitting the SQE (fast-path cancel, no kernel round-trip). In-flight
  cancel = `IORING_OP_ASYNC_CANCEL` with `IOSQE_CQE_SKIP_SUCCESS` so a
  *successful* cancel produces no CQE — the target op's CQE (carrying
  `-ECANCELED`) is the single wake-up signal. **Exactly-once by construction.**
- **Short reads/writes, EOF, errno:** CQE `res` → byte count (non-negative) or
  `-errno` (negative). `INTR`/`CANCELED` retried in-loop; `AGAIN` →
  `WouldBlock`; EOF mapped by caller (`n==0` → `EndOfStream`). No looping
  inside `preadv`/`pwritev`.
- **Shutdown:** assert no pending async tasks; `yield(null, .exit)` posts
  `MSG_RING` (`off = exit`) to every thread; each worker's reaper hits the
  `exit` arm and returns; join workers; per-thread `deinit` frees fibers and
  the ring.

### C.12 fiber.zig (stack switching) — `Io/fiber.zig`

- **Architectures:** aarch64, riscv64, x86_64 only (`fiber.zig:1-4`).
- **Save area:** three words per arch (`sp, fp, pc`) — much smaller than
  `ucontext`. Treated as a **full ABI call boundary**: all GP/FP/vector
  registers declared clobbered; no lazy FP save.
- **Switch:** `contextSwitch(s: *const Switch) *const Switch` (`:29`). Saves
  `sp/fp` + resume-label into `*old`, loads `sp/fp/pc` from `*new`, jumps to
  `new.pc`. The `Switch*` arg is passed in `rsi`/`x1`/`a1` and returned in the
  same register, so the caller recovers the message that resumed it.
- **Initial entry:** a `naked` trampoline (`AsyncClosure.entry`) tail-calls a
  `noreturn` `call`; the user function never returns into the trampoline.

---

## D. Zig-to-cppio mapping

Classification per task §D: `MATCHED` / `PARTIAL` / `MISSING` /
`INTENTIONALLY DIFFERENT` / `NOT YET APPLICABLE`.

| Zig concept | cppio mapping | Classification | Evidence / note |
| ----------- | ------------- | -------------- | --------------- |
| `Io` context | `sluice::async::AsyncIoContext` | PARTIAL | cppio's context is the L1 foundation (submit/poll/wait_one/cancel) only; Zig `Io` also owns Future/Group/Select/Batch/cancel-protection/futex/sync-primitives. |
| `Io.VTable` | `sluice::async::AsyncBackend` (L0 interface) | PARTIAL | cppio's interface covers submit/poll/wait_one/cancel/outstanding only; Zig VTable spans ~50 file ops + async/concurrent/await/cancel/group/futex/operate/batchAwait. **The two are not the same boundary** — see doc 023B. |
| `Operation` union (4 variants) | `ReadOp`/`WriteOp`/`SyncDataOp`/`SyncAllOp` | INTENTIONALLY DIFFERENT | cppio models sync as an async op (018B); Zig keeps `fileSync` as a direct vtable method. cppio has no net/device variants (out of scope, ADR §A5). cppio's ops are single-buffer; Zig's are scatter/gather (`[]const []u8`). |
| `Operation.Storage` (4-state slot) | `Completion<T>` (3-state: idle/outstanding/ready) | PARTIAL | cppio collapses submission+pending into `outstanding`; no per-op `[7]usize` backend scratch; no intrusive list integration. Sufficient for current L1 but cannot host a real Batch. |
| `Batch` (grouped completions) | (none) | MISSING | cppio submits one op per `submit_*`; no L1 batch surface. Required for high-concurrency parity. |
| `operate` (sync one-shot) | `submit_*` + `poll`/`wait_one` (driver-driven) | INTENTIONALLY DIFFERENT | cppio's submit does not block; the caller drives completion via poll/wait_one. Zig's `operate` blocks the current task. Both are valid; cppio's shape is the ADR-accepted L1 foundation. |
| `Future` | (none) | MISSING | Single-task awaitable. Deferred to the L2/task-runtime layer. |
| `Group` | (none) | MISSING | Unordered task group; cancel-propagation boundary. Deferred. |
| `Select` | (none) | MISSING | Higher-level task selector. Deferred. |
| `Cancelable` (`error{Canceled}`) | `IoError::Code::canceled` | MATCHED | cppio reuses the existing error vocabulary (ADR §7 X1). |
| Single-shot-per-point + `recancel` | (none) | MISSING | cppio cancel is best-effort, no protection regions, no recancel. Required before faithful task runtime. |
| `CancelProtection` | (none) | MISSING | Delivery-blocking protection. Deferred. |
| futex substrate | (none — uses `std::mutex`/`std::condition_variable`) | INTENTIONALLY DIFFERENT | cppio's sync primitives are standard-library, OS-thread-blocking. Zig's suspend the task on Evented. The **semantic question** (does contention block a worker or suspend a task?) is open per task §12. |
| `Mutex`/`Condition`/`Event`/`Queue` | `std::mutex`/`std::condition_variable` (no Event/Queue) | INTENTIONALLY DIFFERENT | cppio does not have Io-aware sync primitives. Adding them is post-Evented (task §12). |
| `RwLock`/`Semaphore` | (none) | MISSING | Io-aware variants. Deferred. |
| `Threaded` backend | `ThreadPoolBackend` | PARTIAL | Both spawn one OS thread per task and run synchronous syscalls. **Gaps:** cppio has no `Syscall` cancel region, no `cancelAwaitable`, no signal-based interrupt, no `async_limit`. Header doc/impl drift on cancel (header claims remove-and-cancel; impl is no-op). |
| `Uring` Evented backend | `UringAsyncBackend` | PARTIAL | cppio reaps CQEs and maps errno (E3) and does exactly-once cancel structurally. **Gaps:** single-driver-thread (no fibers, no scheduler, no stealing), no L1 batch submit, no buffer/file registration, no flags/chains, cancel is O(outstanding) scan, no `CancelRegion`. **Cannot express the Evented model.** |
| `Kqueue` / `Dispatch` | (none) | NOT YET APPLICABLE | BSD / Apple only; not pursued on Linux host. |
| `fiber.zig` stack switching | (none) | MISSING | Required before any Evented/fiber model. Three arches upstream; x86_64/aarch64 are the cppio-relevant ones. |
| `fileSync` vtable method | `SyncDataOp`/`SyncAllOp` async ops | INTENTIONALLY DIFFERENT | cppio makes durability an overlappable async op (W4, 018B); Zig keeps it a direct vtable method. Documented divergence. |
| Networking / processes / timers / mmap / terminal / dir | (none) | INTENTIONALLY DIFFERENT | Out of scope (ADR §A5, 016B O1-O5). |

---

## E. Dependency graph (authoritative construction order)

Derived from source. Edges mean "depends on the semantic contract of". This is
the order in which cppio must migrate to preserve Zig semantics faithfully.

```text
                         [existing: L0 AsyncBackend + L1 Completion/AsyncIoContext/AsyncStats,
                          FakeAsyncBackend, ReadOp/WriteOp/SyncDataOp/SyncAllOp, ThreadPoolBackend,
                          UringAsyncBackend, cancellation spike, bench harness — jobs 017-022, GREEN]
                                                          │
                                                          ▼
┌────────────────────────────────────────────────────────────────────────────────┐
│ PHASE B  — backend substrate completeness (doc 023B governs A/B/C verdict)     │
│                                                                                  │
│  B1  AsyncBackend conformance suite (shared contract cases)                     │
│       └─ depends on: nothing new (exercises existing backends)                  │
│  B2  ThreadPoolBackend correctness repair                                       │
│       (Syscall-style cancel region, destroying_ gate, header/impl drift)        │
│       └─ depends on: B1 (proves the contract)                                   │
│  B3  UringAsyncBackend substrate hardening                                      │
│       (cancel O(1) via token, submit batching seam, feature gating)             │
│       └─ depends on: B1                                                         │
└────────────────────────────────────────────────────────────────────────────────┘
                                                          │
                                                          ▼
┌────────────────────────────────────────────────────────────────────────────────┐
│ PHASE T  — task/async model (the Zig Io.zig:1174-1538 surface)                 │
│                                                                                  │
│  T1  Cancel token + protection region + recancel                                │
│       (Cancelable single-shot semantics, CancelProtection, recancel)            │
│       └─ depends on: B1 (cancel contract defined)                               │
│  T2  Future<Result> (single-task awaitable)                                     │
│       └─ depends on: T1 (cancel flows through Future)                           │
│  T3  Group (unordered task set, cancel-propagation boundary)                    │
│       └─ depends on: T2                                                         │
│  T4  Batch (grouped completions over Operation.Storage)                         │
│       └─ depends on: T2, B3 (batch submit seam on Uring)                        │
│  T5  Select (higher-level task selector on Queue)                               │
│       └─ depends on: T3, T4, + Queue (T7)                                       │
└────────────────────────────────────────────────────────────────────────────────┘
                                                          │
                                                          ▼
┌────────────────────────────────────────────────────────────────────────────────┐
│ PHASE S  — synchronization on the Io substrate (Io.zig:1552-2312)              │
│                                                                                  │
│  S1  futex substrate (cancelable wait/wake, 4-byte word)                        │
│       └─ depends on: T1 (cancelable) — and on the execution model decision      │
│  S2  Io-Mutex / Condition / Event                                               │
│       └─ depends on: S1                                                         │
│  S3  Queue (MPSC ring with suspended putters/getters, recancel-on-partial)      │
│       └─ depends on: S2, T1                                                     │
│  S4  RwLock / Semaphore                                                         │
│       └─ depends on: S2                                                         │
└────────────────────────────────────────────────────────────────────────────────┘
                                                          │
                                                          ▼
┌────────────────────────────────────────────────────────────────────────────────┐
│ PHASE E  — Evented task runtime (the real Zig Uring/Dispatch/Kqueue model)     │
│                                                                                  │
│  E0  Execution-model ADR: Threaded-equivalent vs Evented-equivalent, and        │
│       whether cppio adopts fibers (the principal experiment, task §9)           │
│       └─ depends on: B, T, S complete (informs the decision with evidence)      │
│  E1  fiber.zig port: x86_64 (+ aarch64) contextSwitch, Context, trampoline      │
│       (gated; only the chosen arch(es))                                         │
│       └─ depends on: E0 (decision to adopt fibers)                              │
│  E2  Fiber + Thread (per-thread ring, ready/free queues, idle loop)             │
│       └─ depends on: E1, B3 (Uring substrate)                                   │
│  E3  yield + SwitchMessage (the suspension/resume seam)                         │
│       └─ depends on: E2                                                         │
│  E4  Work stealing (idle-search, ready-steal, free-steal, MSG_RING wakeup)      │
│       └─ depends on: E3                                                         │
│  E5  CancelRegion + IORING_OP_ASYNC_CANCEL race (exactly-once via SKIP_SUCCESS) │
│       └─ depends on: E3, T1                                                     │
│  E6  Io-aware sync rebase: Mutex/Condition/Event/Queue suspend the fiber        │
│       └─ depends on: E3, S* (the principal experiment's payoff, task §12)       │
└────────────────────────────────────────────────────────────────────────────────┘
```

### Why this order

- **B before T:** the task model (Future/Group/Batch) needs a correct,
  conformance-tested backend to execute on. Landing T before B closes would
  build a task runtime on a substrate with known correctness gaps
  (ThreadPoolBackend doc/impl drift, UringAsyncBackend O(outstanding) cancel).
- **T before S:** the sync primitives are cancelable (`futexWait` returns
  `Cancelable!void`), so they depend on the cancel-token model (T1). They also
  need the awaitable infrastructure (T2/T3) to express "suspend the current
  task".
- **T before E:** the Evented runtime's `yield`/`CancelRegion` integrate with
  the task model; you cannot build a fiber scheduler without knowing what a
  "task" is in cppio.
- **E0 (ADR) gates E1-E6:** whether cppio adopts fibers is the principal
  experiment (task §9). It is a public-contract decision that must not be made
  silently. The ADR lands after B/T/S give evidence about whether the
  completion-based L1 model can already preserve blocking-shaped control flow,
  or whether fibers are required.

### Hard blockers (per task §14)

- **fiber.zig is gated to 3 arches.** cppio on any other arch cannot host an
  Evented model without writing new asm. The ADR (E0) must record the arch set.
- **`Io.Operation` has no `file_sync` variant** — Zig's durability is a vtable
  method, not a batchable op. cppio's `SyncDataOp`/`SyncAllOp` divergence is
  intentional and documented; it does not block migration.
- **Networking/processes/timers/mmap/terminal/dir are out of scope** (ADR §A5).
  Their absence does not block the file-I/O async migration.

---

## F. Cross-links

- Backend parity audit (consumes §D, decides A/B/C): `docs/async-backend-parity.md` (023B).
- Job-card sequence (built from §E): `docs/zig-stdio-migration-jobs.md` (023C).
- Async ADR (the accepted L1 model): `docs/adr/ADR-async-io-model.md` (016D).
- Async source inventory (the design-level inventory this extends):
  `docs/async-source-inventory.md` (016A).
- Async next jobs (the 017-022 sequence, all GREEN): `docs/async-next-jobs.md` (016F).
- Earlier (MVP-era) Zig inventory/parity (blocking-only, superseded here for
  async): `docs/zig-std-io-source-inventory.md` (012B), `docs/zig-std-io-parity-audit.md` (012C).
