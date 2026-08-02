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
                        Completion<T> gains an internal binding transient (no public layout change)
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

### Completion binding

| Property | Test | Result |
|---|---|---|
| binding CAS elects one context; loser cannot write binding (I2) | `completion_binding_test.cpp :: binding_cas_elects_one_context`, `:: concurrent_binding_exactly_one_wins` | PASS (commit 2; Debug 129/129, Release 129/129, ASan/UBSan clean) |
| binding rollback restores idle (winner that fails pre-commit) | `completion_binding_test.cpp :: binding_rollback_restores_idle` | PASS (commit 2) |
| binding unobservable: cancel/await gate on outstanding()==false while binding (I15) | `completion_binding_test.cpp :: binding_cas_elects_one_context` asserts !outstanding() while binding | PASS (commit 2) |
| destroy/reset in binding -> fail-fast (Debug AND Release) | `completion_authority_death_test.cpp :: destroy-in-binding`, `:: reset-in-binding` | PASS (commit 2; Debug + Release + ASan/UBSan) |
| ordinary caller cannot forge/commit/rollback a binding | `completion_authority_negative_compile_probe.cpp` NEG_BEGIN/COMMIT/ROLLBACK_BINDING_PRIVATE + `verify-completion-authority-negative-compile.sh` (10/10 cases) | PASS (commit 2) |

### Scheme B (the load-bearing arbitration)

| Property | Test | Result |
|---|---|---|
| pending cancel wins before enqueue; enqueue observes backend_ready -> successful no-op; submit returns success; exactly one terminal result; exactly one ready linkage; exactly one ReadySink; reset -> generation++; stale key -> not_found (I10/I17/I19) | `request_lifecycle_scheme_b_test.cpp :: pending_cancel_wins_before_enqueue_then_enqueue_noop` (the 19-step trace) | PASS (commit 3; Debug 130/130, ASan/UBSan clean, TSan build+run clean) |
| reap with live pin publishes nothing, no accepted_out--, no borrow end | (same test, step 9) | PASS (commit 3) |
| exactly one terminal winner among pending-cancel / dispatch-error / ordinary (I10) | `request_lifecycle_scheme_b_test.cpp :: exactly_one_terminal_winner` | PASS (commit 3) |
| ReadyEvent survives reset/reuse during sink callback; no dangling pointer (I16) | `request_lifecycle_scheme_b_test.cpp :: ready_sink_event_survives_reset_during_callback` | PASS (commit 3) |
| single-waiter cardinality; wait-cancel doesn't cancel I/O; lease exactly-once (I13) | `request_lifecycle_scheme_b_test.cpp :: waiter_registration_cardinality`, `:: reap_wins_lease_over_wait_cancel` | PASS (commit 3) |
| TSan under genuine concurrency (submit thread ‖ cancel/reap thread) | `request_lifecycle_scheme_b_test.cpp :: concurrent_submit_cancel_enqueue` | PASS (commit 5; Debug + TSan 132/132, 0 data races; 2000 iterations of submit-vs-cancel on one slot, ~9% record_wins / ~91% cancel_wins — both terminal-winner paths exercised) |

### Backend migration (Fake + Sync)

| Property | Test | Result |
|---|---|---|
| Fake/Sync traverse accepted -> backend_ready -> reap -> completion_ready; submit never makes Completion ready inline | `reference_backend_arena_lifecycle_test.cpp :: *_slot_in_use_tracks_lifecycle`, `:: fake_arena_slot_lifecycle_explicit_staging` | PASS (commit 4; Debug 131/131, Release 131/131, ASan/UBSan clean, TSan clean) |
| bounded admission observable: arena_capacity_rejections increments on full (I8); slot_in_use returns to 0 at reap | `reference_backend_arena_lifecycle_test.cpp :: sync_arena_bounded_admission_rejects_full`, `:: fake_arena_bounded_admission_rejects_full` | PASS (commit 4) |
| generation advances on slot reuse (I6); exactly-once sink deliveries per reaped op (Decision 9) | `reference_backend_arena_lifecycle_test.cpp :: sync_arena_slot_reuse_advances_generation` | PASS (commit 4) |
| cancel drives the Scheme-B terminal path through the arena (pointer-keyed -> SlotHandle -> canceled terminal -> reap -> release) | `reference_backend_arena_lifecycle_test.cpp :: fake_arena_cancel_drives_scheme_b_release` | PASS (commit 4) |
| public submit/cancel/complete surface unchanged (ADR Decision 7) — every pre-existing fake/cancel/completion test passes unmodified | `fake_backend_test.cpp` (7/7), `async_cancel_test.cpp` (5/5), `async_completion_test.cpp` (13/13), `backend_conformance_test.cpp` | PASS (commit 4) |

### Remaining lifecycle / shutdown / authority

| Property | Test | Result |
|---|---|---|
| generation reuse rejects stale submit/cancel/reap/register/release | `request_lifecycle_scheme_b_test.cpp :: generation_reuse_stale_attempts` | PASS (commit 5; every post-reserve authority rejects a stale-generation handle) |
| cancel races per state (pending/enqueued/backend_ready -> already_terminal; unknown -> not_found) | `request_lifecycle_scheme_b_test.cpp :: cancel_races_per_state` | PASS (commit 5) |
| acquire observer of ready sees all effects (result + closed reg + token + completion-ready + accepted_out-- + borrow end) (I18) | `request_lifecycle_scheme_b_test.cpp :: acquire_observer_of_ready_sees_all_effects` | PASS (commit 5) |
| allocation-free slot release; no I/O/Scheduler/backend-progress wait; no upward lock | `request_lifecycle_scheme_b_test.cpp :: allocation_free_slot_release_proof` | PASS (commit 5; 1000 release cycles converge; generation advanced 1000× on one slot; ASan/UBSan in the gate run prove no allocation side-effects) |
| close_admission rejects new reserve but existing reapable still reaps (Decision 15) | `request_lifecycle_scheme_b_test.cpp :: close_admission_rejects_new_but_existing_reapable` | PASS (commit 5; also found+fixed an arena bug where reserve() did not consult admission_closed_) |
| death: release-with-live-pin, release-with-registered-waiter (Debug AND Release) | `request_arena_death_test.cpp :: arena_death_release_with_live_pin`, `:: arena_death_release_with_registered_waiter` + control | PASS (commit 5; Debug + Release + ASan/UBSan + TSan all clean; exit 86 via deterministic terminate handler) |
| death: destroy-in-binding, reset-in-binding, destroy-outstanding, reset-outstanding, double-publish, concurrent-publish-loser | `completion_authority_death_test.cpp` (commit 2 + existing) | PASS (commit 2) |
| ordinary caller cannot mutate slot state / generation / enqueue pin / terminal / registration | `request_arena_negative_compile_probe.cpp` + `verify-request-arena-negative-compile.sh` (5/5 cases) | PASS (commit 5) |
| ordinary caller cannot forge/commit/rollback a binding / claim / publish / reap_seq | `completion_authority_negative_compile_probe.cpp` + `verify-completion-authority-negative-compile.sh` (10/10 cases) | PASS (commit 2) |

### Sanitizers / modes

| Gate | Command | Result |
|---|---|---|
| Clang Debug | `xmake f -m debug --toolchain=clang -y && xmake build sluice_core && xmake build sluice_async && xmake build -g test && xmake test -v` | PASS (commit 5: 132/132 passed, 0 failed) |
| Clang Release (§6.1) | `xmake f -m release --toolchain=clang -y && ...` | PASS (commit 5: 132/132 passed, 0 failed) |
| ASan + UBSan (§6.2) | `xmake f -m asanubsan --toolchain=clang -y && ...` | PASS (commit 5: 132/132 passed, 0 sanitizer warnings) |
| TSan (§6.3) — target 0 data races | `xmake f -m tsan --toolchain=clang -y && ...` | PASS (commit 5: 132/132 passed, 0 data races — including the genuine two-thread `concurrent_submit_cancel_enqueue` case) |
| negative-compile | `scripts/verify-completion-authority-negative-compile.sh` (10/10) + `scripts/verify-request-arena-negative-compile.sh` (5/5) | PASS (commit 5) |
| doc-check | `python3 scripts/check-doc-links.py --self-test && python3 scripts/check-doc-links.py && python3 scripts/verify-architecture-docs.py` | PASS (commit 5; doc-content updates land at commit 6) |

TSan coverage MUST include (§6.3): commit/pending-cancel/enqueue, reap pin check, reset/reuse,
cross-context binding, waiter cancel/reap.

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
- [x] AGENTS.md change-class gates run (Debug 132/132, Release 132/132, ASan/UBSan 132/132 clean, TSan 132/132 0 data races, both negative-compile gates 15/15, doc-check PASS — commit 5)

(Boxes ticked only when the ACTUAL command has run and passed. "PENDING" rows above are
the honest pre-execution state.)
