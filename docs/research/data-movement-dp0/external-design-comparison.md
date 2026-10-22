# DP-0 external design comparison (R3)

Open-world comparison of per-request data movement in five independent
designs, read from actual source at pinned commits (no API-doc-only claims).
Full per-project notes with every file:line citation are preserved in the
session working notes summarized here; the matrix is the deliverable.

## Pinned sources

| project | commit | date | version evidence |
|---|---|---|---|
| libuv | `e36964a094fadba7926e88fe333dd1d6a6baf13c` (master) | 2026-10-21 | `include/uv/version.h` 1.53.0-dev |
| Boost.Asio (chriskohlhoff/asio) | `026dce530a6c39a8aa6d98e5177482310c4a01e5` (master) | 2026-10-21 | `asio/include/asio/version.hpp` 103802 (1.38.2) |
| tokio-uring | `5fb1a4f65b8c471ba6fab8d12e42e129231d865f` | — | Cargo tree at pin |
| Seastar | `587e3b9` (pinned) | — | `include/seastar/core/` at pin |
| liburing | `f33bc2f24cf3b38df8553aab5f9714fc5cf0aff4` | 2026-10-22 | `src/include/liburing.h` at pin |

## Unified matrix

| dimension | libuv | asio | tokio-uring | Seastar | liburing | Sluice base (this audit) |
|---|---|---|---|---|---|---|
| per-op library state storage | caller-owned request; `uv__work` (~40 B) embedded (`include/uv/threadpool.h:30-35`); loop keeps only a counter (`src/uv-common.h:263-274`) | handler+storage in an op node allocated from `recycling_allocator` (`asio/detail/recycling_allocator.hpp`); node freed **before** handler upcall | `Slab<Lifecycle>`+`Slab<Completion>`; `user_data` = slab index (`src/runtime/driver/mod.rs:30-37,151-157`) | promise/future share one `future_state` by pointer retargeting (`future.hh:462-466`) | none — descriptor IS the shared SQE slot | 168 B Slot in a mutex-guarded arena; index+generation token |
| per-op construction/zero-init | `uv__req_init` stamps type + counter bump only | op node default-init; recycling reuses memory without clearing (documented hazard in the header) | slab insert returns zeroed slot | `future_state` init varies (ready-optimized) | `io_uring_initialize_sqe` zeroes 8 fields **per fetched SQE** (`liburing.h:585`, `:1975`) — the same "re-init at acquire" choice Sluice makes | reserve() re-zeroed ~132 B after release already zeroed (double-init; dp3a removes one side) |
| copies across the lifecycle | pointer-link only: queue splice, no object moves; bufs array memcpy'd with 4-entry inline fallback (`src/unix/fs.c:2029-2036`) | completion → handler move; per-op allocation from thread-local pool | payload bytes never copied; ~4 small control moves (16 B CQE, waker, handle) | producer writes result directly where consumer reads it (state pointer retarget); `temporary_buffer` moves are 24 B + deleter | SQE written once in place; completion read in place; ownership = two 4-byte indices (`queue.c:247-272`, `liburing.h:494-500`) | **payload 120 B init + 96 B copy per publication; result 72 B + 40 B metadata copies** (DP-1/DP-2) |
| completion → result materialization | `uv__work_done` → caller's `req` in place | handler invoked with per-op result; no intermediate snapshot struct | `BufResult` move-out; buffer never touched by driver | placement-new into consumer's storage | CQE read in shared memory; `user_data` cookie echoes (kernel-side echo: IMPLEMENTATION_UNVERIFIED — kernel source not read) | begin_publication snapshot (120 B) consumed for 24 B; then observe/consume copies |
| buffer ownership | caller | caller | library (moved in/out; `Ignored(Box)` keeps abandoned ops alive) | library/co-owned (`deleter` refcount) | caller (raw addresses in SQE) | strictly borrowed |
| teardown discipline | loop drain counters | node recycle on handler return | driver drop must cancel+drain (`driver/mod.rs:496-503` assert) | deleter refcount must hit 0 exactly once | khead/kfoot advance discipline; batch CQEs read before advance | release_slot_ full re-init + generation ABA guard |

## What the open world says about Sluice's specific findings

1. **DP-1 (dead 120 B publication snapshot)** — every subject routes only the
   consumed datum through its completion machinery (libuv: in-place req;
   tokio-uring: 16 B CQE + waker; Seastar: write-where-consumed; liburing:
   16 B CQE). None builds a supersized snapshot to feed a 24 B consumer.
   Sluice's payload is the outlier shape; dp1's `nullptr` form matches the
   field.
2. **Per-acquire re-initialization** — liburing re-inits the fetched SQE slot
   (`io_uring_initialize_sqe`) for the same reason Sluice zeroes slots
   (reused slots, kernel reads stale fields). The consensus is re-init at
   **one** site per recycle; Sluice had two (reserve + release). dp3a
   converges toward the consensus shape; release-side re-init remains, which
   matches liburing's single-site discipline only if the acquire side stays
   trivial — consistent with dp3a's direction.
3. **user_data = slot index** — tokio-uring and liburing both carry
   ownership as an opaque 64-bit cookie echoed by the kernel. Sluice's
   RequestKey (context+index+generation) is the same shape with an ABA
   guard; nothing to change.
4. **Borrow facts** — libuv stores borrow descriptors in the caller's
   request; Sluice duplicates them (Slot.borrow) behind the backend's live
   `prepared_ops_`. None of the subjects keeps a write-only copy of borrow
   facts in a second authority — supporting the authority-contraction
   candidate recorded in field-liveness-matrix.md.
5. **Out of scope for Sluice without an API/root change** — tokio-uring's
   owned-buffer `BufResult` and Seastar's refcounted `temporary_buffer`
   views require library buffer ownership, which contradicts Sluice's
   strictly-borrowed contract. Recorded as **not recommended** (root
   constraint, not preference).

## IMPLEMENTATION_UNVERIFIED register (honesty items)

- Kernel-side io_uring behavior (SQE consumption, `user_data` echo, CQE
  publication ordering): verified only against liburing/uapi comments and
  the mmap contract — kernel source not read in this study.
- asio recycling-allocator hazard documentation: read from the header text;
  the runtime reuse pattern was not exercised.
- Seastar `future_state` retargeting under contention: source-read only; no
  execution measurement (out of scope for a source-comparison task).
