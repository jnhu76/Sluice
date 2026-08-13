# Zig std.Io → Sluice Conformance Map

This map is a **comparator**, not a compatibility promise. A row classified
faithful (`F`) means a Sluice caller can express the Zig capability, not that
Sluice implements Zig's mechanism or ABI.

**Sluice baseline SHA:** `5e5ec3663d69f09b5b571ea01c9f4200c75aec98` (master,
2026-08-12, merge of PR #93).
**Zig baseline SHA:** vendored `zig/lib/std/Io.zig` is a gitignored Zig
**0.16.0-dev** snapshot ≈ 2026-04-14 (no SHA in-tree); "still current" claims
were verified against Codeberg `ziglang/zig` master `89e0881f` (2026-08-11).
The GitHub mirror is frozen at `738d2be9`; Zig now lives on Codeberg.
**Audit issue:** [#94](https://github.com/jnhu76/Sluice/issues/94)
(re-baseline scope: issue #95).
**Last reconciled:** 2026-08-13.

Classification key:
- **F** — Faithful enough: core semantic preserved, C++ expression differs
- **I** — Intentional Divergence: approved by ADR/design/test
- **M** — Missing within intended scope: Zig capability absent in Sluice
- **P** — Partial / evidence incomplete: protocol faithful, domain narrowed
- **O** — Obsolete comparison axis: no longer represents current Zig
- **U** — Unresolved: evidence insufficient or contradictory
- **X** — Outside current Sluice scope: deliberately not targeted

Two axes are used per row:

- **Semantic class** — can a Sluice caller express the Zig behavior?
- **Mechanism class** — does Sluice implement it via a similar mechanism?

The former `A` (Accidental Drift) class is retired: the three rows that carried
it (`Operation.Storage`, `Pending.Userdata`, resource bounds — all Uring) were
resolved by the Phase D migration (PR #78/#80/#83/#84) and now carry their
target classes with implementation evidence.

---

## Two-tier Zig taxonomy (verified against both baselines)

- **Low-level tier:** `Operation` (tagged union, **7 tags** at `Io.zig:246` on
  Codeberg master — it *grew* from 4 tags by folding `netSend`/`netRead`/
  `netWrite` into `Operation` tags) + `operate` + caller-owned
  `Operation.Storage` (`:490`, unused/submission/pending/completion) +
  `Pending.Userdata = [7]usize` backend scratch (`:506`) + `Batch` (`:565`,
  4 intrusive lists).
- **High-level tier:** `Io.async`/`concurrent`/`await`/`cancel`/
  `cancelRequested`; `Future`; `Group` (`groupAsync`/`groupConcurrent`/
  `groupWait`/`groupCancel`); `Select`; `CancelProtection`/`recancel`/
  `swapCancelProtection`/`checkCancel` + `*Uncancelable` sync-wait twins;
  `futexWait`/`futexWake` family.
- **Vtable surface (≈106 entries):** async/concurrency, group, select,
  cancelable + uncancelable mutex/condition/event/queue/semaphore/rwlock,
  clock (`now`/`sleep`/`Timeout`/duration/deadline), file (open/close/stat/
  access/create/read/write/seek, streaming + positional), dir ops, net ops,
  process ops, device ioctl.
- **Backends:** `Threaded` (thread pool, `async_limit` default `cpu_count-1`),
  `Uring` (struct `Evented`; work-stealing stackful fibers + per-thread
  io_uring, `IORING_SETUP_COOP_TASKRUN|SINGLE_ISSUER`), `Kqueue` (same shape),
  `Dispatch` (fibers + GCD).

---

## Conformance Matrix

| Zig concept | Zig purpose | Sluice equivalent | Sem | Mech | Evidence |
|---|---|---|---|---|---|
| `Io` (userdata + vtable) | Lightweight copyable capability; any holder can submit ops | `AsyncIoContext` (move-only owner, mutex-serialized) | I | I | DIV-01 Approved; `async_io_context.hpp:118-158` |
| `Operation` (tagged union) | Explicit op descriptor with typed result | `ReadOp`/`WriteOp`/`SyncDataOp`/`SyncAllOp` structs + `BatchOp` variant | F | I | `async_io_context.hpp:32-49` |
| `Operation.Storage` (caller-owned slot) | Caller allocates stable bounded storage for submission→pending→completion and identity-preserving reuse | Backend-owned `RequestSlot` arena (`detail::RequestArena`); Completion stays caller-owned (DIV-02 — **implemented on all four backends**, Phase B/E/D) | I | I | `request_arena.hpp`; `request_slot.hpp`; DIV-02 implemented |
| `Pending.Userdata` (7×usize backend scratch per op) | Backend-private bounded scratch in stable operation storage | Fixed per-slot `TerminalResult` / `PreparedBlockingOp` / Uring cookie+router scratch | I | I | `request_slot.hpp`; `uring_backend.cpp:101-119` (cookie-routed `user_data`) |
| `operate` (blocking-shaped I/O on current task) | Submit + await on the current concurrency unit; returns result inline | `op_helpers::read_all/write_all` (poll-loop) or `RuntimeTaskContext::submit_* + await_completion` (Fiber suspend) | F | I | `op_helpers.hpp:1-64`; `application_runtime.hpp:60-86` |
| `Batch` + `batchAwait` | N ops submitted together; await ≥1; iterate in completion order; cancel as a whole | `Batch` driver over `AsyncIoContext` (reap order via internal `reap_seq`) | I | I | `batch.hpp:1-137`; documented narrowing (no native batch vtable) |
| `Future` / `async` / `await` / `cancel` | Generic async computation | **Public `Future<T>`** (caller-owned value channel: producer `complete_with`, idempotent `await()`/`cancel()`, `cancel_token()` cooperative cancel) + `Group::async` task spawn + `RuntimeTaskContext::submit_* + await_completion` (op-level await); the physical wait delegates to a `WaitPolicy&` (Threaded = block thread, Evented = Fiber suspend via Scheduler) | F | I | `future.hpp` / `future_test.cpp` (public, sluice-CORE-028 T2); `group.hpp:58`; `wait_policy.hpp`; `fiber.hpp:60`; `scheduler.hpp:272`; `application_runtime.hpp:60-86` |
| `Group` (`groupAsync`/`Concurrent`/`Wait`/`Cancel`) | Structured concurrency, cancel propagation | `Group` (Threaded = thread-per-task, Evented = Fiber); await/cancel/size; tasks swallow cancellation | F | I | `group.hpp:58`; DIV-03 |
| `Select` | Multi-source exactly-one winner | `select()` over **Event + Timer cases only** — no I/O-Completion case | P | I | `select.hpp:40-43`; `select_fwd.hpp:34-36`; design issue [#99](https://github.com/jnhu76/Sluice/issues/99) |
| `CancelProtection` / `recancel` / `swapCancelProtection` / `checkCancel` | Delivery-blocking protection regions + re-arm | `CancelProtection` enum, `CancelState::swap_protection`, `CancelGuard` RAII, `CancelToken::rearm` (= Zig `recancel`; request-epoch re-arm, ADR-cancel-request-epoch), `check_cancel` | F | F | `cancel.hpp` / `cancel.cpp` (request-epoch representation); `tests/cancel_token_test.cpp` (T-CANCEL-REARM-1, T-CANCEL-PROTECTION-2, T-CANCEL-CLEAR-3, T-CANCEL-SHARED-4, T-CANCEL-FUTURE-5 — rearm, protection+rearm, clear+request reuse, shared-token, Future consumer); DIV-11 Resolved |
| I/O cancellation (pending/enqueued/running) | Cancel op, explicit disposition | Arena `cancel(SlotHandle)` Scheme-B: pending/enqueued → canceled terminal, running → intent only; reap publishes | F | F | `request_arena.hpp:655` |
| cancelable/uncancelable sync waits (`futexWait` + twin) | Wait cancelable by default + uncancelable twin | Wait-cancel via `WaitQueue::cancel_locked`; **no explicit uncancelable twin** | P | I | `wait_queue.hpp:119` |
| Io-aware sync primitives (Mutex/Condition/Event/Semaphore/RwLock/Queue) | Suspend fiber, not thread | `AsyncMutex`/`AsyncCondition`/`Event`/`Semaphore`/`AsyncRwLock`/`AsyncQueue` via WaitQueue+WaitNode | F | I | `async_mutex.hpp:86`; `condition.hpp:100`; `event.hpp:76`; `semaphore.hpp:80`; `async_rwlock.hpp:63` |
| clock / `now` / `sleep` / `Timeout` | Scheduler-integrated time | Monotonic `deadline_t`, `advance_clock`, `*_until` waits, `TimerRegistration`, `TimerSelectCase`; **no standalone public `sleep_for`/`Timeout` type** | F | I | `scheduler.hpp:325-371`; `timer_registration.hpp` |
| file open/close/stat/access/seek/streaming/dir | General filesystem I/O | **Not present** (positional read/write/sync_data/sync_all only) | X | X | DIV-08 Approved (file-only scope) |
| networking (net ops, incl. the new `Operation` tags) | Sockets | **Not present** | X | X | DIV-08 Approved |
| `Threaded` backend | Thread-per-task execution | `Group()` default mode (thread-per-task); **`ThreadPoolBackend` is a different concept** (bounded blocking-I/O offload) | I | I | DIV-03 Resolved |
| `Uring` backend | Fibers + per-thread io_uring | `UringAsyncBackend` (single private ring, cookie-routed `user_data`, Scheduler fibers) | I | I | `uring_backend.cpp:481-490`; Phase D (PR #78/#80/#83/#84) |
| `Kqueue` backend | BSD kevent | **Not present**; portable fallback = blocking ThreadPool path (macOS-validated, PR #93) | X | X | Intentional Linux-first portability choice |
| Registered buffers / files | Kernel-pinned zero-copy | **Not present** | M | M | DIV-09 Accepted (deferred, lifetime contract) |
| Signal-based blocking-syscall cancel | Interrupt in-flight syscall | **Not present**; running cancel = best-effort intent | M | M | DIV-10 Accepted |
| Durability ops (`fileSync`) | Durability as a property of write ops or explicit Io call | `SyncDataOp`/`SyncAllOp` as first-class async operations | I | I | DIV-06; `async_io_context.hpp:32-49` |
| Completion wake (backend-owned) | Backend completion directly makes the waiting task runnable | Scheduler polling bridge + 2ms MIXED-WAKE backstop | I | I | DIV-04/DIV-05; `scheduler.cpp:362`; Phase G |
| `AsyncBackend` (L0 seam) | Library-internal backend boundary | Public `AsyncBackend` extension point with trusted backend-author contract | I | I | DIV-13 (Accepted); protected `try_claim`/`publish`/`rollback_claim_before_accept`; negative-compile gate |

## Summary by Classification

Semantic axis (can a caller express the behavior):

| Class | Count | Areas |
|-------|-------|-------|
| F (Faithful) | 8 | Operation, operate, Future/async/await, Group, CancelProtection, I/O cancellation, Io-aware sync primitives, clock |
| I (Intentional) | 9 | Io shape, Operation.Storage, Pending.Userdata, Batch, Threaded naming, Uring topology, durability ops, completion wake bridge, AsyncBackend extension |
| M (Missing in scope) | 2 | Registered buffers, signal-based syscall cancel |
| P (Partial) | 2 | Select case domain, uncancelable wait twins |
| O (Obsolete) | 0 | — |
| U (Unresolved) | 0 | — |
| X (Out of scope) | 3 | file/dir/seek I/O, networking, Kqueue |

Mechanism axis: F = 2 (CancelProtection family, I/O cancellation), I = 17,
M = 2, X = 3, P = 0, O = 0, U = 0.

---

## Obsolete comparison axes from the previous map

**None.** The previous map's axes (`Operation`, `Operation.Storage`,
`Pending.Userdata`, `Batch`, `CancelProtection`, `futexWait`/`futexWake`,
`Io {userdata, vtable}`, `Future`/`Group`/`Select`) were verified against the
pinned snapshot AND Codeberg master `89e0881` — all remain present and
load-bearing in current Zig. See the two-tier taxonomy above.

**Only structural drift recorded:** `netSend`/`netRead`/`netWrite` moved out of
dedicated VTable slots into `Operation` tags (3 slots → 3 tags, plus
`net_receive` already there). This *strengthens* `Operation` as a comparison
axis; it does not obsolete anything. Sluice's networking rows are `X` by
DIV-08 regardless of how Zig encodes the op.

---

## Notes

1. **This map is a comparator, not a compatibility promise.** A faithful row
   does not mean Sluice replicates Zig's ABI, vtable, or storage layout; it
   means the caller-visible behavior is expressible.

2. **Old-map concepts are still current.** The seed assumption behind the
   original map (that `Operation`/`Operation.Storage`/`Pending.Userdata`/
   `CancelProtection` belonged to an older Zig design) is incorrect — all of
   them exist in Codeberg master today. Do not reclassify them `O`.

3. **`ThreadPoolBackend` is a blocking-I/O offload mechanism** — a fixed pool
   of persistent blocking-I/O workers + a construction-time bounded dispatch
   ring — NOT an implementation of Zig's `Threaded` execution strategy
   (thread-per-task). `Group` Threaded mode is the faithful Zig Threaded
   equivalent (DIV-03, Resolved). Conflating these leads to incorrect capacity
   reasoning.

4. **Uring topology diverges intentionally.** Zig `Uring` uses per-thread
   io_uring instances with work-stealing stackful fibers; Sluice uses a single
   private ring with cookie-routed `user_data` (`uring_backend.cpp:101-119`)
   under the Scheduler's fibers. Phase D (PR #78/#80/#83/#84) completed the
   bounded `RequestArena` migration, descriptor validation before commit
   (`uring_backend.cpp:378-433`), and `UringConfig{request_capacity=64,
   queue_depth=64}` with `would_block` admission. KernelIo is ELIGIBLE in real
   mode; stub builds honestly report INCOMPLETE.

5. **The completion wake bridge is still polling-based.** Backend readiness is
   observed via `poll()`/`wait_one()` and the 2ms MIXED-WAKE backstop
   (`scheduler.cpp:362`); backend completion does not directly make the
   Scheduler park domain runnable. Classified **I** (DIV-04/DIV-05, Accepted).
   Phase G owns any change.

6. **`P` rows are domain-narrowing, not mechanism gaps.** `Select` implements
   the exactly-one-winner protocol faithfully but over Event + Timer cases
   only (no I/O-Completion case — issue #99); sync waits have wait-cancellation
   but no `*Uncancelable` twin; the clock row lacks a standalone public
   `sleep_for`/`Timeout` convenience type.

7. **DIV-02 is fully implemented.** The backend-owned `RequestSlot` adaptation
   of Zig's caller-owned `Operation.Storage` now covers all four backends
   (Fake/Sync Phase B, ThreadPool Phase E, Uring Phase D); the revisit trigger
   (measured per-request overhead vs. public-API migration cost) is preserved
   in the divergence registry.
