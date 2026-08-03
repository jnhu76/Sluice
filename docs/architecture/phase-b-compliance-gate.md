# Phase B Compliance Gate — Bounded RequestKey / RequestSlot Reference Lifecycle

**Design:** [`docs/design/phase-b-request-slot-reference.md`](../design/phase-b-request-slot-reference.md)
**Governing ADR:** [`ADR-explicit-io-request-contract`](../adr/ADR-explicit-io-request-contract.md) (Accepted)
**Branch:** `feat/bounded-request-slot-reference`
**Status:** Gate 0–3 complete at design time; Gate 4 evidence filled as each commit lands
(per `design-compliance-gate.md`: pre-filling PASS before execution is forbidden).

This document is the completed Gate 0–4 checklist for the Phase B PR, complementing the ADR's
own Gate 0–4 (which gives the contract-level classification; this file gives the PR-level
evidence ledger).

---

## Gate 0 — Architecture Classification

```text
Affected capability:    AsyncIoContext + AsyncBackend (reference backends FakeAsyncBackend, SyncBackend)
Affected layer:         L0 backend contract; L1 AsyncIoContext contract
Classification:         Corrective plus Intentional Divergence (DIV-02 activated for reference backends)
Governing ADR:          ADR-explicit-io-request-contract (Accepted)
Conformance map change: yes — DIV-02 Proposed -> Active transitional (Phase B); zig-io-conformance-map
                        Operation.Storage / Pending.Userdata / Resource bounds rows advance
Constitution rules:     AC-2, AC-3, AC-4, AC-5, AC-6, AC-7, AC-10, AC-11, AC-12, AC-13, AC-14, AC-15
Public API effect:      none to submit_* signatures; IoError::Code gains three enumerators;
                        Completion<T> gains a binding transient: the internal State enum
                        gains `binding`/`publishing`/`resetting` values and the class gains
                        the slot-release payload (`release_arena_`, `bound_slot_`) — a
                        public LAYOUT change, accepted as the Phase B cost of the
                        caller-owned Completion lifecycle (ADR Decision 7 / I2; no
                        signature or semantic change to existing public methods)
```

No field is "unknown" or "TBD". Coding may proceed.

---

## Gate 1 — Ownership and State Machine

Full state machine, transition authorities, lock domains, allocation rules, failure semantics,
wake obligations, and shutdown behavior: see design doc §9. Every transition has exactly one
authority, one lock domain (the leaf slot-lifecycle mutex — the single arbitration domain for
admission, waiter registration, reap publication, release, generation increment, and free-list
publication; ADR `:290-297`), an allocation classification, a failure behavior, a wake
obligation, and a shutdown behavior. The three competing transitions
(`pending->enqueued`, `pending->backend_ready(canceled)`, `pending->backend_ready(error)`)
share that one domain (I17 — no TOCTOU window).

The enqueue-in-flight pin bit lives in the same state word; reap acquire-checks it; release
cannot occur until it is acknowledged (I19).

---

## Gate 2 — Resource and Failure Model

```text
Construction-time resources:
  - RequestSlot[request_capacity]:  capacity=fixed-at-construction, allocation=preallocated,
                                    failure=no_space at construction (never a submit error)
  - per-slot terminal Result storage, ready linkage, pending linkage, backend scratch,
    waiter storage:                  preallocated at construction

Submit-time resources:
  - RequestSlot (reserve):           capacity=request_capacity, allocation=none (claims free),
                                    failure=would_block (full) / invalid_argument / invalid_state
                                    / not_supported
  - submit allocation-free after acceptance? YES (enqueue is noexcept; I9)

Completion-time resources:
  - terminal Result:                 capacity=1/slot preallocated, cannot be lost (I4/I9)

Capacity and backpressure:
  - Maximum outstanding:             request_capacity (bounded; AC-7)
  - Queue-full behavior:             synchronous would_block, Completion idle, no borrow
  - OOM at each stage:               construction -> no_space; submit post-reserve -> none

Reclamation:
  - Shrink under load?               NO (fixed arena; free-list reuse)
  - Bounded by:                      outstanding (slot_in_use: reserve -> release)
```

Distinct counters (ADR Decision 13): `slot_in_use` (reserve → release) vs `accepted_outstanding`
(commit → completion-ready publication). These are NOT merged.

Submit success cannot be followed by permanent operation loss due to allocation failure: the
accepted terminal path depends on no new allocation (I9).

---

## Gate 3 — Progress and Wake Model

```text
Blocking/suspension:
  - Who may block?                   caller thread via wait_one(); poll() is non-blocking
  - Who may suspend?                 no Fiber in Phase B
  - What makes them continue?        caller-driven poll()/wait_one(); backend_ready observable
                                     on next reap

Backend -> Scheduler progress:
  - How does backend-ready reach?    observation via poll()/wait_one() (as-built, unchanged)
  - Signal or observation?           observation
  - Worst-case latency?              caller-defined; NO internal polling interval introduced

External wake coexistence:
  - External wake + backend progress? Phase B has no external wake (Scheduler integration is
                                     Phase F/G). The enqueue-pin protocol guarantees no lost
                                     progress: a backend_ready slot whose pin is still live
                                     remains ready and is re-reaped on the next poll() after ack
                                     (level-triggered for Fake/Sync by construction).
  - Commit-to-sleep race closed by?  the enqueue_in_flight_pin bit under the leaf domain,
                                     acquire-observed by reap

Polling dependency:
  - Periodic timeout?                NO. No polling interval is introduced.

Single-worker liveness:
  - N/A (Phase B has no Scheduler worker; Fake/Sync are single-threaded deterministic)
```

No answer is "periodic poll". AC-6 satisfied.

---

## Gate 4 — Evidence Plan (filled as commits land)

Each row lists the property, the test that proves it, and (once the relevant commit lands)
the actual command + result. **No row is pre-marked PASS.** Until a commit lands its tests,
the Result column reads "PENDING — commit N".

### Capacity / bounded admission

| Property | Test | Result |
|---|---|---|
| arena capacity bounded; full -> would_block; Completion idle (I3/I8) | `request_arena_test.cpp :: arena_capacity_bounded` | PENDING — commit 1 |
| generation advances on release; stale key rejected (I6) | `request_arena_test.cpp :: arena_generation_advances_on_release`, `:: arena_stale_key_rejected` | PENDING — commit 1 |
| slot_in_use vs accepted_outstanding are distinct counters | `request_arena_test.cpp :: arena_accounting_tracks_slot_in_use_vs_accepted_outstanding` | PENDING — commit 1 |
| accepted terminal path allocation-independent (I9) | `request_arena_test.cpp :: arena_no_post_accept_allocation` | PENDING — commit 1 |
| **explicit metric vocabulary (P1-05)**: `queue_full_retries` counts ONLY capacity pressure (would_block); caller lifecycle violations (invalid_state) count `invalid_state_rejections` — the two are never conflated | `async_completion_test.cpp :: async_stats_increment_on_submit_poll_wait` (invalid_state -> invalid_state_rejections, queue_full_retries stays 0), `reference_backend_no_alloc_test.cpp :: sync_full_arena_rejection_is_allocation_free` (would_block -> queue_full_retries), `uring_backend_test.cpp :: uring_sqe_pressure_increments_queue_full_retries` (backend_error SQE-pressure path, unchanged) | PASS (round-2 review-fix: `AsyncStats::invalid_state_rejections` added; tally_submit split) |

### Completion binding

| Property | Test | Result |
|---|---|---|
| binding CAS elects one context; loser cannot write binding (I2) | `completion_binding_test.cpp :: binding_cas_elects_one_context`, `:: concurrent_binding_exactly_one_wins` | PASS (commit 2; Debug 129/129, Release 129/129, ASan/UBSan clean) |
| binding rollback restores idle (winner that fails pre-commit) | `completion_binding_test.cpp :: binding_rollback_restores_idle` | PASS (commit 2) |
| binding unobservable: cancel/await gate on outstanding()==false while binding (I15) | `completion_binding_test.cpp :: binding_cas_elects_one_context` asserts !outstanding() while binding | PASS (commit 2) |
| destroy/reset in binding -> fail-fast (Debug AND Release) | `completion_authority_death_test.cpp :: destroy-in-binding`, `:: reset-in-binding` | PASS (commit 2; Debug + Release + ASan/UBSan) |
| ordinary caller cannot forge/commit/rollback a binding | `completion_authority_negative_compile_probe.cpp` NEG_BEGIN/COMMIT/ROLLBACK_BINDING_PRIVATE + `verify-completion-authority-negative-compile.sh` (12/12 cases) | PASS (commit 2 + round-2) |
| **the Completion publication binding lives IN the RequestSlot record** (installed before commit, validated by reap before any accounting change, published through inside the leaf domain); missing binding -> fail-fast, never a silent drop (review C2/C3, I4/I5/I11) | `request_lifecycle_scheme_b_test.cpp :: acquire_observer_of_ready_sees_all_effects` (real Completion + real thunk + acquire-load ready), `request_arena_death_test.cpp :: reap-without-binding` (death), `completion_binding_test.cpp` (real thunk publish), `reference_backend_arena_lifecycle_test.cpp` (backend lifecycle), `request_arena_negative_compile_probe.cpp` NEG_SLOT_BINDING_PRIVATE (6/6) | PASS (round-2 review-fix: the parallel `bindings_` unordered_map is GONE — the slot record is the single identity carrier; cancel resolves via a bounded O(capacity) scan) |

### Scheme B (the load-bearing arbitration)

| Property | Test | Result |
|---|---|---|
| pending cancel wins before enqueue; enqueue observes backend_ready -> successful no-op; submit returns success; exactly one terminal result; exactly one ready linkage; exactly one ReadySink; reset -> generation++; stale key -> not_found (I10/I17/I19) | `request_lifecycle_scheme_b_test.cpp :: pending_cancel_wins_before_enqueue_then_enqueue_noop` (the 19-step trace) | PASS (commit 3; Debug 130/130, ASan/UBSan clean, TSan build+run clean) |
| reap with live pin publishes nothing, no accepted_out--, no borrow end | (same test, step 9) | PASS (commit 3) |
| exactly one terminal winner among pending-cancel / dispatch-error / ordinary (I10) | `request_lifecycle_scheme_b_test.cpp :: exactly_one_terminal_winner` | PASS (commit 3) |
| ReadyEvent survives reset/reuse during sink callback; no dangling pointer (I16) | `request_lifecycle_scheme_b_test.cpp :: ready_sink_event_survives_reset_during_callback` | PASS (commit 3) |
| single-waiter cardinality; wait-cancel doesn't cancel I/O; lease exactly-once (I13) | `request_lifecycle_scheme_b_test.cpp :: waiter_registration_cardinality`, `:: reap_wins_lease_over_wait_cancel` | PASS (commit 3) |
| TSan under genuine concurrency (submit thread ‖ cancel/reap thread) | `request_lifecycle_scheme_b_test.cpp :: concurrent_submit_cancel_enqueue` | PASS (commit 5; Debug + TSan 132/132, 0 data races; 2000 iterations of submit-vs-cancel on one slot, ~9% record_wins / ~91% cancel_wins — both terminal-winner paths exercised; review-fix: the test now asserts the enqueue pin is ALREADY acknowledged after join — no extra acknowledge that could mask a pin bug) |
| **backend-level Scheme-B race** (real FakeAsyncBackend submit thread + Completion binding + deterministic barrier pause between commit and enqueue; cancel wins the pending terminal; resumed enqueue no-ops and acks the pin; poll reaps canceled) | `backend_scheme_b_race_test.cpp :: backend_scheme_b_cancel_wins_between_commit_and_enqueue` (links sluice_async_internal_testing for the SubmitPauseGate seam) | PASS (round-2 review-fix: closes review test-gap 1 — the arena trace alone did not prove the real backend integration) |

### Backend migration (Fake + Sync)

| Property | Test | Result |
|---|---|---|
| Fake/Sync traverse accepted -> backend_ready -> reap -> completion_ready; submit never makes Completion ready inline | `reference_backend_arena_lifecycle_test.cpp :: *_slot_in_use_tracks_lifecycle`, `:: fake_arena_slot_lifecycle_explicit_staging` | PASS (commit 4; Debug 131/131, Release 131/131, ASan/UBSan clean, TSan clean) |
| bounded admission observable: arena_capacity_rejections increments on full; capacity pressure returns synchronous **would_block** (never invalid_state — ADR Decision 6/13); rejected Completion stays idle | `reference_backend_arena_lifecycle_test.cpp :: sync_arena_bounded_admission_rejects_full`, `:: fake_arena_bounded_admission_rejects_full` | PASS (commit 4 + review-fix: error-code propagation + idle-Completion assertion) |
| generation advances on slot reuse (I6); exactly-once sink deliveries per reaped op (Decision 9) | `reference_backend_arena_lifecycle_test.cpp :: sync_arena_slot_reuse_advances_generation` | PASS (commit 4) |
| cancel drives the Scheme-B terminal path through the arena (pointer-keyed -> SlotHandle -> canceled terminal at cancel() time -> reap -> Completion-ready) | `reference_backend_arena_lifecycle_test.cpp :: fake_arena_cancel_drives_scheme_b_terminal` | PASS (commit 4 + review-fix: cancel wins via `arena.cancel` directly; no poll-time drop) |
| **slot release is the caller's reset/ready-destruction handshake**: slot stays bound (slot_in_use == 1) through reap; reset()/ready destruction returns it (generation++); the completed-binding release authority fails fast on ANY release failure (review I1); pre-commit rollback is a separate recoverable authority | `reference_backend_arena_lifecycle_test.cpp :: *_slot_in_use_tracks_lifecycle` (post-review), `completion_binding_test.cpp :: binding_release_capability_reset_releases_slot`, `:: binding_release_capability_ready_destruction_releases_slot`, `request_arena_death_test.cpp :: release-with-live-pin`, `:: release-with-registered-waiter` (death) | PASS (round-2 review-fix: `release_completed_binding` vs `rollback_reserved_or_prepared` — the caller handshake can no longer ignore a failed release) |
| **transactional pre-commit rejection (review C1)**: a lost binding CAS leaves Completion untouched, slot_in_use / accepted_outstanding / submission-FIFO unchanged, and produces no future result contamination — all under always-throw operator new (zero allocation) | `reference_backend_no_alloc_test.cpp :: fake_cas_loss_rejection_zero_side_effects`, `:: sync_full_arena_rejection_is_allocation_free` | PASS (round-2 review-fix: the parallel `bindings_` map and the unbounded FIFO deque are gone — binding in the slot record + construction-time bounded ring; every pre-commit failure rolls the reservation back) |
| public submit/cancel/complete surface unchanged (ADR Decision 7) — every pre-existing fake/cancel/completion test passes unmodified | `fake_backend_test.cpp` (7/7), `async_cancel_test.cpp` (5/5), `async_completion_test.cpp` (13/13), `backend_conformance_test.cpp` | PASS (commit 4) |

### Remaining lifecycle / shutdown / authority

| Property | Test | Result |
|---|---|---|
| generation reuse rejects stale submit/cancel/reap/register/release | `request_lifecycle_scheme_b_test.cpp :: generation_reuse_stale_attempts` | PASS (commit 5; every post-reserve authority rejects a stale-generation handle) |
| cancel races per state (pending/enqueued/backend_ready -> already_terminal; unknown -> not_found) | `request_lifecycle_scheme_b_test.cpp :: cancel_races_per_state` | PASS (commit 5) |
| acquire observer of ready sees all effects (result + closed reg + token + completion-ready + accepted_out-- + **borrow end**) (I18) | `request_lifecycle_scheme_b_test.cpp :: acquire_observer_of_ready_sees_all_effects` (round-2: drives a REAL Completion through ProbeBackend::publish_size_ready and acquire-loads Completion::ready() — the real linearization point), `:: pending_cancel_wins_before_enqueue_then_enqueue_noop` (borrow assertions), `request_arena_test.cpp :: arena_borrow_lifecycle` | PASS (commit 5 + round-2 review-fix: borrow metadata implemented — borrow begins at commit, ends at completion-ready publication; the I18 proof now uses a real Completion-ready acquire) |
| allocation-free slot release; no I/O/Scheduler/backend-progress wait; no upward lock | `request_lifecycle_scheme_b_test.cpp :: allocation_free_slot_release_proof` | PASS (commit 5; 1000 release cycles converge; generation advanced 1000× on one slot; ASan/UBSan in the gate run prove no allocation side-effects) |
| **allocation-free completion-ready publication (I9 / Decision 14)**: reap is a SINGLE-DOMAIN protocol over the fixed slot array (no ready-record vector, no per-slot publish flag — the backend_ready -> completion_ready transition is the exactly-once authority); the Completion-ready release-store happens INSIDE the leaf domain (review C3); submit does not allocate after the commit LP (publication binding lives in the slot record; the fake's FIFO is a construction-time bounded ring) | `request_lifecycle_scheme_b_test.cpp` (all reap cases), `reference_backend_no_alloc_test.cpp` (counting + always-throw operator new: the accepted submit -> poll -> reset path performs ZERO allocations), `request_arena_test.cpp :: arena_no_post_accept_allocation` | PASS (round-2 review-fix: binding-in-slot + single-domain reap + bounded ring; P0-02's failure mode is closed at the reference layer) |
| close_admission rejects new reserve but existing reapable still reaps (Decision 15) | `request_lifecycle_scheme_b_test.cpp :: close_admission_rejects_new_but_existing_reapable` | PASS (commit 5; also found+fixed an arena bug where reserve() did not consult admission_closed_) |
| death: release-with-live-pin, release-with-registered-waiter, **enqueue-before-commit, destroy-arena-with-slot-in-use, reap-without-binding, record-terminal-on-prepared** (Debug AND Release) | `request_arena_death_test.cpp` (7 death/control cases) | PASS (commit 5 + round-2 review-fix: two new fail-fast boundaries — illegal enqueue state; non-quiescent arena destruction; missing publication binding; terminal on a non-accepted slot) |
| death: destroy-in-binding, reset-in-binding, destroy-outstanding, reset-outstanding, double-publish, concurrent-publish-loser | `completion_authority_death_test.cpp` (commit 2 + existing) | PASS (commit 2) |
| ordinary caller cannot mutate slot state / generation / enqueue pin / terminal / registration / **publication binding** | `request_arena_negative_compile_probe.cpp` + `verify-request-arena-negative-compile.sh` (6/6 cases) | PASS (commit 5 + round-2: NEG_SLOT_BINDING_PRIVATE added) |
| ordinary caller cannot forge/commit/rollback a binding / claim / publish / reap_seq / **install or clear the slot-release capability** | `completion_authority_negative_compile_probe.cpp` + `verify-completion-authority-negative-compile.sh` (12/12 cases) | PASS (commit 2 + review-fix) |

### Sanitizers / modes

| Gate | Command | Result |
|---|---|---|
| Clang Debug | `xmake f -m debug --toolchain=clang -y && xmake build sluice_core && xmake build sluice_async && xmake build -g test && xmake test -v` | PASS (round-2: 134/134 passed, 0 failed — includes the two new targets `reference_backend_no_alloc_test`, `backend_scheme_b_race_test`) |
| Clang Release (§6.1) | `xmake f -m release --toolchain=clang -y && ...` | PASS (round-2: 134/134 passed, 0 failed) |
| ASan + UBSan (§6.2) | `xmake f -m asanubsan --toolchain=clang -y && ...` | PASS (round-2: 134/134 passed, 0 sanitizer warnings — the counting/always-throw operator new composes with ASan interposition; sized/aligned delete variants route through plain free) |
| TSan (§6.3) — target 0 data races | `xmake f -m tsan --toolchain=clang -y && ...` | PASS (round-2: 134/134 passed, 0 data races — including the genuine two-thread `concurrent_submit_cancel_enqueue` case and the backend-level `backend_scheme_b_race_test`; the allocation probe is compiled out under TSan because the TSan C++ runtime owns the new/delete symbols — the lifecycle/zero-side-effect assertions remain active) |
| negative-compile | `scripts/verify-completion-authority-negative-compile.sh` (12/12) + `scripts/verify-request-arena-negative-compile.sh` (6/6) | PASS (round-2: 18/18 — NEG_SLOT_BINDING_PRIVATE added) |
| doc-check | `python3 scripts/check-doc-links.py --self-test && python3 scripts/check-doc-links.py && python3 scripts/verify-architecture-docs.py` | PASS (round-2) |

TSan coverage MUST include (§6.3): commit/pending-cancel/enqueue, reap pin check, reset/reuse,
cross-context binding, waiter cancel/reap.

> **TSan scope note:** the 0-data-race TSan run covers the `RequestArena` protocol (the
> leaf slot-lifecycle domain) under genuine two-thread races (`concurrent_submit_cancel_enqueue`)
> and the backend-level Scheme-B race (`backend_scheme_b_race_test` — submit thread paused
> between commit and enqueue via the internal-testing seam, cancel thread interleaved).
> The reference backends' identity resolution is now the arena's bounded slot scan (the
> pointer-keyed `bindings_` map was removed in round 2), and the submit paths are
> single-threaded by design — `AsyncIoContext::access_mtx_` serializes all backend entry
> points — so the TSan evidence covers the slot-domain protocol under concurrency and the
> backend bookkeeping under the serialized domain. Multi-threaded backend usage is a
> later-phase concern (Phase D/E backends drive their own domains).

---

## Gate Completion Checklist

- [x] Gate 0 classification complete and accurate (above)
- [x] Gate 1 state machine covers all new/modified lifecycles (design §9)
- [x] Gate 2 resource model has no unbounded growth without ADR approval (above; AC-7)
- [x] Gate 3 wake model has no undocumented polling dependency (above; AC-6)
- [x] Gate 4 evidence filled with ACTUAL results (every row PASS as of commit 5)
- [x] Conformance map updated (DIV-02 activated; zig-map rows advanced — commit 6)
- [x] Divergence registry updated (DIV-02 — commit 0)
- [x] Constitution rules satisfied (design §19)
- [x] AGENTS.md change-class gates run (Debug 134/134, Release 134/134, ASan/UBSan 134/134 clean, TSan 134/134 0 data races, both negative-compile gates 18/18, doc-check PASS — round-2/round-3)

(Boxes ticked only when the ACTUAL command has run and passed. "PENDING" rows above are
the honest pre-execution state.)
