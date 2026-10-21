# M0 Complexity Ledger — Sluice v2 (master `8fdcd99c`)

Standard record per candidate mechanism. `Cardinality` names what N is;
`Trigger` separates hot/warm/cold phases (normal / cancel / recovery /
diagnostic / shutdown); `Decision` is the M0 verdict feeding M1 admission.
Line numbers are BASE `8fdcd99c`.

## Part 1 — Ledger

### L1. `BoundedHandleRing::remove_exact` (ThreadPool)

| Field | Record |
|---|---|
| Path | `src/async/threadpool_backend.cpp:64-87` |
| Function | `ThreadPoolBackend::BoundedHandleRing::remove_exact` |
| Call DAG | `cancel_key` (`:767-768`) only |
| Trigger | cancel (warm); never on the normal submit/completion path |
| Cardinality | D = live dispatch-queue length ≤ `request_capacity` |
| Container | custom ring (`vector<RequestReservation>`, head/size subtract-wrap) |
| Algorithm | linear scan for (slot,generation) match; FIFO-order-preserving shift of successors |
| Complexity | worst/avg O(D) compare + O(D−i) 16 B moves; no allocation |
| Physical cost | ≤ D cache lines touched in one ring (≤ 16 B/entry); zero allocs; under `work_mtx_` |
| Contract | BOUND-01 bounded dispatch; REQ-02 acceptance; CANCEL-01 dispositions; the shift carries the FIFO dispatch-order obligation (THREAD-02) |
| Evidence | source read; cancel frequency in workloads: W-01/02/03 exercise cancel only in test oracles and shutdown-drain paths; G0 matrix contains no cancel-heavy case |
| Alternative | swap-remove (rejected: breaks FIFO), reverse index map (adds per-push/pop maintenance to the hot path for a warm path win) |
| Decision | **KEEP** — O(D) on a rare path with a real ordering obligation; an index would tax every push/pop to accelerate a path the supported workloads rarely take |

### L2. `BoundedDispatchQueue::remove_exact` (io_uring)

| Field | Record |
|---|---|
| Path | `src/async/uring_internal.hpp:118-133`; called `uring_transport.cpp:201` (per dispatched op, post-claim) and `:583` (cancel) |
| Function | `UringAsyncBackend::BoundedDispatchQueue::remove_exact` |
| Call DAG | `dispatch_one_locked` ← drain loops in `dispatch_after_accept`/`poll`; `cancel_key` |
| Trigger | normal (every dispatched operation) + cancel |
| Cardinality | D = live dispatch length ≤ capacity |
| Container | custom ring, `%`-wrap |
| Algorithm | linear scan + FIFO shift |
| Complexity | O(D) per dispatched op |
| Physical cost | ≤ D lines; zero alloc; under `dispatch_mtx_` |
| Contract | REQ-02 dispatch discipline; the post-claim removal must find the exact reservation (fail-fast at `:202-206`) |
| Evidence | source read. D is small in practice for the single-submitter W-02 workloads this study covers: `dispatch_after_accept` drains immediately after each push, so the queue holds >1 entry only under concurrent submitters (THREAD-01) or SQE exhaustion (`dispatch_one_locked:142-148` submits on a full ring) |
| Alternative | pop_head-only variants don't apply (removal is exact, not FIFO-head); maintain per-reservation back-pointer (new state on hot path) |
| Decision | **KEEP** — D is ~1 in the normal cadence (push→drain in the same critical section); worst-case O(D) occurs only under ring-full backpressure |

### L3. `find_live_router_cookie_` (io_uring per-CQE route)

| Field | Record |
|---|---|
| Path | `src/async/uring_transport.cpp:551-557`; call sites `uring_completion.cpp:105,148`, `uring_transport.cpp:283,388` |
| Function | `UringAsyncBackend::find_live_router_cookie_` |
| Call DAG | `handle_one_cqe` (every operation CQE and control CQE), `mark_consumed_control_locked_` (every consumed control SQE), `poison_and_recover_locked` |
| Trigger | normal completion (hot), cancel control (warm), recovery (cold) |
| Cardinality | R = router capacity = `request_capacity` |
| Container | `vector<RouterEntry>` (32 B) dense array |
| Algorithm | reverse linear scan comparing `cookie` |
| Complexity | worst O(R), expected O(R/2) at high occupancy |
| Physical cost | R × 32 B walked; ~R/2 cache lines; every compare is a live branch |
| Contract | identity must survive cookie reuse — but cookies are never reused (`allocate_cookie_` monotonic, fail-fast at domain end), and `user_data` is fully owned by the backend |
| Evidence | source read; per-CQE frequency = 1:1 with completions. **First confirmed hot-path candidate**: the router slot index is chosen by this very code from a LIFO free list (`dispatch_one_locked:161-162`) and is not encoded in `user_data`, which the kernel returns verbatim |
| Alternative | encode router slot in `user_data` (direct O(1) index; cookie stays in the entry for exact identity re-check, preserving anti-ABA); hash map (heavier) |
| Decision | **INVESTIGATE → M1 candidate H1** |

### L4. `service_pending_controls_locked_` (io_uring per-pass control sweep)

| Field | Record |
|---|---|
| Path | `uring_transport.cpp:666-676`; called `uring_transport.cpp:88`, `uring_backend.cpp:610` |
| Trigger | every submit drain and every `poll()` pass (normal phase) |
| Cardinality | R |
| Algorithm | full router scan for `control_pending` entries |
| Physical cost | R × 32 B walk per pass even with zero cancels outstanding |
| Contract | sticky-intent retention (CANCEL-01, `control_pending` must never be silently dropped) |
| Evidence | `control_pending` is set only when an SQE could not be allocated (`issue_running_cancel_locked_:644-646`) — a rare ring-full corner |
| Alternative | maintain a count/list of pending-control router slots (derived summary updated at the two set/clear sites, both under `dispatch_mtx_`) |
| Decision | **INVESTIGATE (low)** — same scan family as L6; bundle with H2 measurement. Expected share: small |

### L5. ThreadPool owed-event sweeps

| Field | Record |
|---|---|
| Path | `src/async/threadpool_backend.cpp:569-577` (pass sweep) and `:826-833` via `:597-600` (per-pass `any_event_owed_locked_`), `:838` (settlement) |
| Trigger | every progress pass (normal phase) |
| Cardinality | C = `request_capacity` |
| Container | `vector<DeliveryRecord>` (32 B) |
| Algorithm | full capacity scan; pass sweep runs under one lock hold; verdict scan re-walks |
| Complexity | 2 × O(C) per pass |
| Physical cost | ~C/4 cache lines per sweep (32 B records, early-exit only on first owed); zero alloc |
| Contract | owed-event delivery must be honest residue (PROG-01, V29; `uring_owed_residue_test`); event_owed pairs with a core control ref |
| Evidence | W-02 performs one pass per wait (`waits` counter ≈ ops); zero-op submissions set owed events inline (`publish_zero_op_inline`), the common read path does not — so sweeps usually find nothing |
| Alternative | owed counter (derived summary maintained beside `event_owed` writes, all under `work_mtx_`) |
| Decision | **INVESTIGATE → M1 candidate H2** (shared with L6) |

### L6. io_uring owed sweep with per-slot locking

| Field | Record |
|---|---|
| Path | `src/async/uring_backend.cpp:636-654` (sweep, **one lock/unlock pair per slot index**), `any_event_owed_locked_` `:295-302` via `poll_progress:312` |
| Trigger | every `poll()`/progress pass |
| Cardinality | C lock acquisitions + C comparisons per pass, even when idle |
| Physical cost | C uncontended mutex round-trips — measured ≈5.7 ns/pair wall in M1 (Layer A, `m1-results.md`) → ≈370 ns per pass at C=64 |
| Contract | same owed-delivery obligations as L5; `DeliveryRecord` handoff discipline documented at `uring_backend.hpp:181-192` |
| Evidence | source read; the ThreadPool already implements the snapshot-under-one-lock shape (`threadpool_backend.cpp:559-577` with `owed_batch_`), so a mechanism precedent exists in-tree |
| Alternative | snapshot sweep under one lock (mirror ThreadPool `owed_batch_`), or owed counter (H2) |
| Decision | **INVESTIGATE → M1 candidate H2** (primary shape) |

### L7. `RequestScope::reserve_slot_` / `resolve_` scans

| Field | Record |
|---|---|
| Path | `src/async/request_scope.cpp:46-55`; `include/sluice/async/request_scope.hpp:114-129` (via `take_`, `wait_for`) |
| Function | first-free reserve scan; ticket-id resolve scan |
| Call DAG | every `submit_read/write/sync_*` (reserve); every `take`/`wait_for` (resolve, once each per op in W-02) |
| Trigger | normal (hot) |
| Cardinality | S = scope capacity (chosen by the caller before acceptance, BOUND-01) |
| Container | `unique_ptr<Slot[]>` (64 B slots) + `variant` requests |
| Algorithm | linear scan; resolve re-validates `owning && valid && id==` per slot |
| Complexity | O(S) per submit; O(S) per resolve (avg S/2 for random victims; short for FIFO-oldest because freed low slots are reused first) |
| Physical cost | ~S × 64 B worst; the `variant` visit on owning slots adds a branch; zero alloc |
| Contract | Scope tracking bound (BOUND-01); reserve-before-acceptance ordering (§16/D1 record); the id equality keeps stale-ticket resolution exact (REQ-01 identity) |
| Evidence | bench W-02 ships a dedicated isolation case (`-scope64`, `bench_w02_pipeline.cpp:140-145,640-663`): scope capacity 64 with core capacity 4 isolates exactly this scan |
| Alternative | LIFO free-slot vector (mirrors `RequestCore::free_slots_`, no API change); ticket→slot hint would change the public `ScopeTicket` ABI — excluded this round |
| Decision | **INVESTIGATE → M1 candidate H3** |

### L8. `RequestCore` reserve/reclaim field stores

| Field | Record |
|---|---|
| Path | `request_core.cpp:158-190` (reserve re-init), `:602-626` (release re-init + generation bump) |
| Trigger | every submit / every reclaim (hot) |
| Cardinality | O(1) — ~20 scalar stores, 168 B slot, under core mutex |
| Contract | REQ-05 "slot reuse must reinitialize the publication state under the generation discipline" |
| Alternative | hot/cold slot split (descriptor+borrow+outcome moved to a side table) — reduces re-init stores but adds indirection to every access and splits a structure the formal model mirrors as one entity |
| Decision | **KEEP** — ~20 stores ≈ tens of ns under a contended mutex; splitting layout for that is complexity without measured cause (no hotspot evidence) |

### L9. `occupancy()` / `collect_outstanding` / `snapshot` scans

| Field | Record |
|---|---|
| Path | `request_core.cpp:59-84, 91-101, 657-687` |
| Trigger | settlement convergence (shutdown), diagnostics only |
| Cardinality | N = request capacity |
| Decision | **KEEP** — cold phase; `settlement_converged_` needs the full multi-dimension verdict and a maintained equivalent would be seven more derived counters crossing every transition the TLA model covers (`RequestCore.tla` correspondence would need re-derivation) |

### L10. W-02 harness window (`std::deque` + erase)

| Field | Record |
|---|---|
| Path | `bench/bench_w02_pipeline.cpp:323,437`; scan at `:371-377` |
| Trigger | harness only, per completion |
| Complexity | default mode: `victim == 0` always (`:395`), so `erase(begin())` is O(1); `-anyready` diagnostic mode: O(depth) `ctx.lookup` scan per entry + O(depth) middle erase |
| Decision | **KEEP** with documented caveat — the primary W-02 mode adds no O(depth) harness work per op (verified above); the diagnostic mode's scans are labeled diagnostics and excluded from throughput claims (see harness audit file) |

### L11. STL containers in production: negative findings

The container audit (agent-assisted exhaustive inventory, spot-verified by
direct reads) establishes: production `src/`+`include/` contains **no**
`std::deque/list/map/set/unordered_*/queue/priority_queue/bitset`, **no STL
algorithms on collections** (no `find/sort/lower_bound/remove_if/...`), no
`std::pmr`, no custom allocators, no `shared_ptr`, and no allocation on any
per-request path. Every collection is a constructor-sized `std::vector` used
as a fixed-capacity array, or one of three hand-written bounded rings. The
only `std::sort` in the tree is bench statistics (`bench_common.hpp:98`).
Consequently the "replace STL container X with Y" search space is empty by
construction; the real search space is *indexing and scan mechanisms* (L1-L7).

## Part 2 — STL algorithm-shaped findings (hand-written loops)

| Site | Shape | Verdict |
|---|---|---|
| `remove_exact` ×2 (L1, L2) | `std::find` + shift (`std::rotate`-like) | KEEP/KEEP |
| router scans (L3, L4) | `std::find_if` over dense array | H1/H2 |
| owed sweeps (L5, L6) | `std::count_if`-shaped full walk | H2 |
| scope scans (L7) | `std::find_if` over slot array | H3 |
| `occupancy/collect/snapshot` (L9) | full walks | KEEP (cold) |
| poison ledger×router nested scans (`uring_transport.cpp:377-443`) | O(L×R) nested find | KEEP — recovery-only; correctness-dominated path |
| `canonical_error_code` linear scan over 8-entry table (`error.hpp:83-86`) | O(1)-bounded find | KEEP (failure paths only) |
| `compose` retry loops (`blocking_file.cpp:286-299`) | short-I/O completion loops | KEEP — semantics (SEM-05), not overhead |
| repeated sort / re-sort findings | none exist | — |

## Part 3 — First-round candidate ranking

| Rank | Candidate | Expected steady-state share (to verify in M1) | Admitted |
|---|---|---|---|
| 1 | H1 router direct index (L3) | per-CQE O(R) walk under mutex; R=64 → up to ~1 KB walked per completion | yes |
| 2 | H2 owed-sweep shape (L5/L6) | per-pass O(C) ×2 (+ C lock pairs on uring) | yes |
| 3 | H3 scope free-list (L7) | per-op O(S) submit-side; resolve-side depends on consume order | yes |
| 4 | L4 controls sweep | per-pass O(R), rare pending set | bundle into H2 measurement, no separate experiment |
| 5 | L8 slot hot/cold split | unquantified, likely tens of ns | no — no measured cause |
| 6 | L1/L2/L9/L10 | warm/cold or O(1) in primary modes | no |

M1 proceeds with exactly three hypotheses (H1-H3) per the study charter;
`NO_ADMITTED_CANDIDATE` is not invoked because M0 confirmed three hot-phase
scan mechanisms with traceable per-op frequency.
