# M0 State Inventory & Allocator/Layout Audit — Sluice v2 (master `8fdcd99c`)

Every runtime state in `src/`+`include/` (async domain), classified per the
Mechanism Justification Gate vocabulary (authority / derived summary /
compatibility state / physical obligation / test-only evidence). Layout bytes
are **measured** with a layout probe (`sizeof` over the real definitions,
run once at BASE; method note at the end). "per-slot" columns multiply by
`request_capacity` (C) unless stated.

## 1. RequestCore (`request_core.hpp:257-296`)

| State | Type | Bytes | Class | Owner of fact |
|---|---|---|---|---|
| `Slot.phase` | enum u8 | (in Slot) | authority (REQ-03 lifecycle) | core |
| `Slot.generation` | u64 | authority (REQ-01 identity) | core |
| `Slot.accepted / binding_live / terminal_chosen / published / publication_inflight / execution_claimed / cancel_intent / zero_op` | 8 × bool | authority (lifecycle dimensions, REQ-03) | core |
| `Slot.observer_phase` | enum u8 | authority (OBS-01..03) | core |
| `Slot.execution_refs / control_refs` | 2 × u32 | authority (borrow/control pins, LIFE-01) | core |
| `Slot.descriptor` | `RequestDescriptor` = 32 | authority (op facts) | core |
| `Slot.borrow` | `BorrowFacts` = 24 | authority (borrow boundary, REQ-04) | core |
| `Slot.outcome` | `IoOutcome` = 32 | authority (terminal result) | core |
| `Slot.metadata` | `FileInfo` = 40 | authority (SEM-07) | core |
| **`Slot` total** | | **168** | | |
| `slots_` | `vector<Slot>` sized once | 168·C | storage of the above | |
| `free_slots_` | `vector<u32>`, reserved to C, LIFO | 4·C | **derived index** over `phase==free` (redundant with the table, kept for O(1) `reserve`) | core |
| `retired_count_` | usize | 8 | derived summary of exhausted slots (generation-max retirement) | core |
| `outstanding_` | usize | 8 | **derived summary** of `accepted_live − published`; comment at `request_core.hpp:180-183` explicitly keeps `occupancy()` as the settlement authority | core |
| `admission_open_ / health_failed_` | 2 × bool | 2 | authority (admission/health) | core |
| `mutex_` | `std::mutex` | 40 | physical obligation (REQ-02 serialization) | — |

`RequestCore` byte cost per admitted request: 168 B slot + 4 B free-list
entry = **172 B**. All lookups are direct slot indexing with a generation
equality check (`resolve_internal_`, `request_core.cpp:114-138`): O(1), no
scan, no hashing. The scan-shaped members (`occupancy()`, `collect_outstanding()`,
`snapshot()`) are settlement/diagnostics readers, not per-request state.

Cold-field observation: after the terminal is chosen, `descriptor`/`borrow`
(56 B) are dead but stay in the slot; the whole 168 B is re-initialized at
`reserve` and `release_slot_` (`request_core.cpp:158-190, 602-626`). This is
straight-line store work (~20 scalars) under the core lock — a plausible
micro-architectural candidate, but it buys REQ-05's "slot reuse must
reinitialize under the generation discipline" and costs no allocation.

## 2. ThreadPoolBackend (`threadpool_backend.hpp:116-239`)

| State | Type | Bytes/slot | Class |
|---|---|---|---|
| `prepared_ops_` | `vector<PreparedBlockingOp>` = 32·C | 32 | authority (backend op staging, REQ-02) |
| `delivery_` | `vector<DeliveryRecord>` = 32·C | 32 | authority per-record (owed-event handoff); **no count summary exists** |
| `dispatch_ / publication_pending_ / publish_batch_` | 3 × `BoundedHandleRing` (`vector<RequestReservation>` 16·C each) | 48 | authority (bounded queues, BOUND-01) |
| `owed_batch_` | `vector<OwedEvent>` = 32·C scratch, manual bound | 32 | pass-bounded scratch (BOUND-01 "storage established before acceptance") |
| `active_workers_ / stopping_` | usize + bool | — | authority (quiescence) |
| `work_mtx_ + work_cv_` | 40 + 48 | — | physical obligation |
| `syscall_count_` | atomic u64 | 8 | test-only evidence counter (relaxed) |
| `BoundedHandleRing::remove_exact` | — | — | cancel-path mechanism; FIFO-order-preserving shift |

Per-request backend bytes: **144 B** (32+32+48) plus 32 B owed scratch share.
`event_owed` is the only representation of "a delivery event is pending";
there is no derived count, which is why `poll_progress` sweeps `delivery_`
twice per pass (`threadpool_backend.cpp:569-577, 597-600`) and
`internal_work_retired` sweeps it again (`:826-833`).

## 3. UringAsyncBackend (`uring_backend.hpp:147-320`, `uring_internal.hpp`)

| State | Type | Bytes/slot | Class |
|---|---|---|---|
| `prepared_ops_` | `vector<PreparedUringOp>` = 48·C | 48 | authority (SQE staging) |
| `delivery_` | `vector<DeliveryRecord>` = 32·C | 32 | authority per-record (same design as ThreadPool) |
| `router_` | `vector<RouterEntry>` = 32·C | 32 | authority (cookie→handle route, the kernel-side identity map) |
| `cookie_free_list_` | `vector<SlotIndex>` = 4·C, LIFO | 4 | derived free index over `router_[].in_use` |
| `statx_buffers_` | `vector<statx>` = 256·C | 256 | authority (metadata payload buffer, SEM-07) |
| `dispatch_ / publication_pending_` | 2 × `BoundedDispatchQueue` (16·C each, `%`-wrap) | 32 | authority |
| `TransportLedger` (unique) | `vector<Entry>` ≈ 48·Q, Q = ring_entries | 48·Q | authority (SQ-side accounting vs kernel head; monotonic sequence + physical position) |
| `next_cookie_` | u64 monotonic | 8 | authority (cookie domain, anti-ABA by never reusing cookies) |
| `submit_flushes_ / live_cookies_ / live_control_sqes_` | 3 × atomic | 24 | `live_*` are authority counters for quiescence (relaxed), `submit_flushes_` test-only evidence |
| `fatal_error_` | `optional<IoError>` | 16 | authority (sticky health) |
| `dispatch_mtx_` | mutex | 40 | physical obligation |
| `have_ring_ / eventfd_registered_ / available_` | bools | — | authority |

Per-request backend bytes: **404 B + 256 B statx = 660 B** (plus 48 B/queue-depth
ledger). Cookie identity: a cookie is *never* reused (`allocate_cookie_`
fail-fasts at exhaustion, `uring_transport.cpp:526-538`), and `user_data`
carries only the cookie (control bit in the top bit). The router slot is
chosen from a LIFO free list and is **not** encoded in the cookie, so CQE
dispatch requires `find_live_router_cookie_` `[scan R]`
(`uring_transport.cpp:551-557`) under `dispatch_mtx_`. This is the single
clearest direct-indexing gap in the codebase — the slot index could ride in
`user_data` next to the cookie — and is admitted to M1 as candidate H1.

## 4. AsyncIoContext / ProgressSource / Request / Scope

| State | Class / note |
|---|---|
| `access_mtx_` (`async_io_context.hpp:336`) | physical obligation; serializes admission |
| `stop_policy_` (atomic, release/acquire) | authority (shutdown policy, THREAD-01 race) |
| `owner_thread_ / drive_active_ / owner_claimed_ / notification_interest_live_` | authority (single-driver discipline) |
| `ProgressSource` epochs (`progress/control_epoch_`, `*_exhaustion_`, acked pairs, all under `mtx_`) | authority — PROG-02 token/park handshake; saturation discipline documented at `progress_source.hpp:252-264` |
| eventfd + `notification_pending_` suppression bit | physical obligation (coalesced wake, no lost obligation) |
| `Request<T>` = 48 B (core ptr, backend ptr, `RequestId` 32 B) | authority handle |
| `RequestScope::Slot` = 64 B (`state` + `variant<monostate, Request<size_t>, Request<void>>`) | authority (scope tracking, BOUND-01 "Scope tracking") |
| `RequestScope` has **no free list**: `reserve_slot_` scans for `state==free` and `resolve_` scans for the ticket id (`request_scope.cpp:46-55`, `request_scope.hpp:114-129`) | the scope's two hot scans (M1 candidate H3) |
| `ScopeTicket` = 32 B (a `RequestId`) | carries no slot hint — an index hint would be a public ABI change (out of scope this round) |

## 5. Allocation & layout audit

- **No heap allocation after admission opens** in either backend: every
  collection is sized once in the constructor; the only runtime-path `reserve()` in `src/`
  is `free_slots_` (`request_core.cpp:41`; `workers_.reserve` at `threadpool_backend.cpp:102` is constructor-time setup); the ThreadPool documents the
  invariant at `threadpool_backend.cpp:552-557` (BOUND-01 pass-storage rule).
  `allocations/op` on the steady request path is **0** (verified by reading
  every submit/completion function; no `make_*`/`new` on those paths).
- `shared_ptr` appears nowhere in production code (only test support).
  `Request<T>` is a 48 B movable value; results move through
  `PublicationPayload` (120 B, stack) by copy — no allocation.
- No `std::pmr`, no custom allocators, no `alignas`, no placement new except
  the `Result<T>` union storage (`result.hpp:32-87`). Introducing a pool
  allocator would add management state for zero measured benefit (nothing
  allocates per-op).
- Layout: mixed hot/cold inside `RequestCore::Slot` (168 B = ~3 cache lines;
  the hot first line holds phase/generation/flags, the cold tail holds
  outcome/metadata). `delivery_` (32 B) is touched by both submit and the
  pass sweep — false sharing is not a concern: all `delivery_` access is
  under one mutex. `RouterEntry` (32 B) is touched per CQE under one mutex.
  The ring storages (16 B entries) are touched under one mutex each.
- Padding waste is bounded (e.g. `RequestKey` 24/20 used; `Slot` has
  natural alignment gaps) — no meaningful packing win is available without
  splitting hot/cold, which the ledger treats as INVESTIGATE-only.
- WSL2 note: glibc malloc per-thread arenas are irrelevant here (no per-op
  allocation).

## 6. Duplication / redundancy register

| Mechanism | Class | Verdict |
|---|---|---|
| `free_slots_` (core) | derived index over slot phases | justified: O(1) `reserve` is a per-submit hot path; a first-free scan would reintroduce the RequestScope problem |
| `outstanding_` (core) | derived summary, explicitly non-authoritative (`request_core.hpp:180-183`) | justified: O(1) `outstanding_summary` is read per pass; `occupancy()` stays the settlement authority |
| `cookie_free_list_` (uring) | derived index over router `in_use` | justified (same shape as `free_slots_`) |
| `retired_count_` | derived summary | used only for the exhaustion verdict in `reserve` |
| `syscall_count_`, `submit_flushes_`, eventfd test counters | test-only evidence | correctly confined to test builds or relaxed counters |
| `DeliveryRecord.event_owed` without a count | authority-per-record, no derived count | **candidate**: a count would remove the per-pass `[scan C]`; but it is new derived state that must be maintained in every critical section that touches `event_owed` — admitted to M1 as H2 for measurement, PROPOSE_UPSTREAM only if measured share justifies it |
| `RequestScope` lacking free list / index | absence of derived state | **candidate H3**: the scans are real per-op work; a free list mirrors `RequestCore`'s existing mechanism; a ticket→slot hint changes public ABI (excluded) |

## 7. Layout-probe method note

`sizeof` probe compiled at BASE against the real headers with access
relaxed (`#define private public` around the include block only, standard
headers pre-included; gcc 15.2, `-std=c++20`). Measured: `RequestCore::Slot`
168, `RequestDescriptor` 32, `BorrowFacts` 24, `IoOutcome` 32, `FileInfo` 40,
`RequestKey` 24, `RequestId` 32, `Request<size_t>` 48, `RequestReservation`
16, `PublicationPayload` 120, `PreparedUringOp` 48, `RouterEntry` 32,
`DeliveryRecord` 32, `PreparedBlockingOp` 32, `OwedEvent` 32,
`RequestScope::Slot` 64, `ScopeTicket<size_t>` 32; `struct statx` 256 (UAPI).
