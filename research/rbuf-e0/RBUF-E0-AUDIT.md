# RBUF-E0-AUDIT — pre-implementation audit (#272)

Audited on `research/rbuf-e0` branched from master `67b208c7` (PR #271 merge).
Purpose: answer, from code evidence only, (a) what the current ordinary
io_uring path does, (b) whether any registered/fixed-buffer support exists,
(c) which experimental topology RBUF-E0 must therefore use. No guessing.

## 1. Current ordinary uring path (production `UringAsyncBackend`)

| Aspect | Location (as of `67b208c7`) | Fact |
| --- | --- | --- |
| Class | `include/sluice/async/uring_backend.hpp:114` | `UringAsyncBackend : AsyncBackend` |
| Impl | `src/async/uring_backend.cpp` (1895 lines) | real path compiled only under `SLUICE_HAS_LIBURING`; otherwise stub |
| Ring setup | `src/async/uring_backend.cpp:525` | `io_uring_queue_init(config.queue_depth, &ring, /*flags=*/0)`; request capacity and ring depth are separate bounded resources (`UringConfig` validation at `:494`) |
| SQE prep | `src/async/uring_backend.cpp:852-867` | `io_uring_prep_read` / `io_uring_prep_write` (ORDINARY opcodes) + `io_uring_prep_fsync` (two variants). **No `*_fixed` prep anywhere.** |
| Request identity | `src/async/uring_backend.cpp:868-887` | monotonically allocated `op_cookie` in `user_data`; router ARRAY slot maps cookie → `SlotHandle`; cookie never reused (stale-CQE ABA fix) |
| Buffer address flow | `PreparedUringOp` (`prep.buffer`, `prep.native_length`, `prep.offset`) | caller-owned borrowed buffer recorded at accept; address must be stable for the request lifetime; backend performs no registration or pinning of it |
| Submission | `src/async/uring_backend.cpp:934`, `:1564` | `io_uring_submit` = transport only; `io_uring_submit_and_wait(&ring, 1)` on the reap path |
| Completion | `wait_cqe_without_submit()` at `:1461`; peek drains in `poll()/wait_one()` | only the reap path publishes terminal results / Completion-ready |
| Dispatch queue | `BoundedDispatchQueue` (`:205`), capacity == request capacity | bounded; no per-op dynamic growth |

Opcode selection is `OperationKind::{read, write, sync_data, sync_all}` —
there is no code path that could select `IORING_OP_READ_FIXED` /
`IORING_OP_WRITE_FIXED`, and no `buf_index` is ever set on an SQE.

## 2. Existing registered-buffer support

Search terms (case-insensitive, over `src/ include/ apps/ bench/ tests/`):
`IORING_REGISTER_BUFFERS`, `io_uring_register_buffers`, `IORING_OP_READ_FIXED`,
`IORING_OP_WRITE_FIXED`, `read_fixed`, `write_fixed`, `buf_index`,
`registered buffer`, `fixed buffer`.

Findings:

- **Zero call sites** for `io_uring_register_buffers` /
  `io_uring_unregister_buffers` / `io_uring_prep_read_fixed` /
  `io_uring_prep_write_fixed` in the entire repo.
- `xmake/experimental.lua:26-30,55-59` declares option
  `with-uring-registered-buffers` (default OFF) which threads the define
  `SLUICE_URING_REGISTERED_BUFFERS` onto `sluice_async` — but **no source
  file contains `#ifdef SLUICE_URING_REGISTERED_BUFFERS`** (or the
  registered-files twin). The gate is plumbing inherited from the Zig
  reference design (`docs/history/implementation-plans/zig-stdio-migration-jobs.md`)
  with no consumer. Docs/history references only; no implementation.
- The only "fixed buffer" hits are the sluice-copy pipeline slot concept
  (`apps/sluice-copy/copy_task.cpp:39`, README) and a test comment —
  fixed-size application buffers, unrelated to io_uring fixed-buffer I/O.

### Classification

```text
REGISTERED-BUFFER PRODUCTION SUPPORT: NO SUPPORT EXISTS
```

(Not "partial": an unconsumed build gate is not support. Per the campaign
mandate this also means: do NOT add the feature to the production backend.)

## 3. Experimental topology decision

Because the production backend has no registered-buffer capability and no
research seam that could flip opcode selection without production changes,
RBUF-E0 uses the sanctioned fallback:

```text
RESEARCH-ONLY MECHANISM BENCH: bench/rbuf_e0_bench.cpp (direct liburing)
```

The bench implements the three arms itself with liburing 2.13 (host package;
the repo-pinned `add_requires("liburing 2.14")` feeds production targets
only and is not linked by this bench):

- **U0** — ordinary-natural reference: per-slot `std::vector<std::byte>`
  heap buffers (natural allocation, no explicit alignment), ordinary
  `IORING_OP_READ`/`IORING_OP_WRITE`. Contextual baseline only.
- **U1** — causal ordinary control: ONE `posix_memalign(4096, chunk*depth)`
  backing block, `depth` slots of `chunk` bytes at page-aligned strides,
  ordinary READ/WRITE opcodes, buffers reused across all transfers.
- **U2** — registered/fixed treatment: byte-identical storage construction
  as U1, then `io_uring_register_buffers(ring, iovec[depth], depth)` with
  one iovec per slot; `io_uring_prep_read_fixed`/`write_fixed` with
  `buf_index = slot`; `io_uring_unregister_buffers` at end of the measured
  lifecycle.

Arms share (single source of truth in the bench): ring entry count
(`max(8, 2*depth)`, actual `sq.ring_entries` recorded), single
submission/completion thread, identical slot state machine
(read slot's chunk → write it → advance to slot's next chunk), identical
file offsets, identical completion loop, identical process lifecycle,
identical fixture (CHUNK-E0 splitmix64 tile, seed `0xE1E1E1E121212121`).
The ONLY U1→U2 delta is registration + fixed opcode selection.

U1/U2 causal-isolation checklist to re-verify mechanically before the formal
sweep (prereg §): same allocation primitive, size, base alignment, slot
stride, slot count, lifetime, request order, chunk/depth, queue setup,
workload; treatment = registration/fixed opcode only. The bench additionally
emits the slot base addresses and registration errno so the driver can gate
on observed (not asserted) equality.

## 4. #262 relevance and the Q0 gate

#262 (intermittent multi-worker write canceled terminal / teardown abort) is
NOT root-caused; its reproductions are on a WSL2 host and involve the
production async runtime's cancel/multi-worker machinery. The RBUF-E0 bench
uses none of that (single-threaded direct liburing, no cancel API), so Q0
qualifies exactly the regime RBUF-E0 measures:

```text
Q0 = 50 x bench(U1, 2 MiB x d2, 1 GiB copy) with full same-work gates
     (exit 0, bytes exact, dst sha256 == src sha256, zero canceled CQEs,
      zero short I/O, exact CQE accounting, clean ring teardown)
```

Q0 PASS means "#262 did not reproduce in this restricted regime" — it does
NOT close #262. Q0 FAIL (any unexpected canceled / teardown abort / hash or
semantic mismatch) stops RBUF-E0 and makes #262 blocking for this campaign.

## 5. Host capability facts (captured by `rbuf_e0.py probe` at session start)

Collected into each session's `environment.json`: kernel, liburing version,
page size, `RLIMIT_MEMLOCK`, registration probe result (errno on failure),
`perf` availability. No automatic `ulimit`/`sysctl`/governor changes.
