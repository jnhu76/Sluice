# Phase D3 — Uring C2b / C2c Identity, Cancel, Borrow and Waiter Integration Gate

Status: COMPLETE (2026-08-09, branch branch test/phase-d3-uring-identity-waiter-conformance)

Governing authority chain (AGENTS.md §3):

```text
Accepted ADR (ADR-explicit-io-request-contract.md, Decisions 4, 5, 8, 9, 10, 11, 12, 18, 19)
>
Phase D frozen design (phase-d1-uring-frozen-design.md, phase-d2-uring-failure-noalloc-gate.md)
>
shared C2b / C2c contract gates (phase-c2b-compliance-gate.md rows 3-8, phase-c2c-compliance-gate.md rows 11-14a)
>
production implementation (include/sluice/async/uring_backend.hpp, src/async/uring_backend.cpp)
>
tests (uring_backend_c2b_identity_test, uring_backend_c2c_waiter_borrow_test)
>
docs / PR prose
```

This gate is the D3 slice only: it closes the Uring integration gaps for the
shared C2b (identity / generation / cancel matrix) and C2c (borrow / waiter /
lease) evidence rows on the REAL liburing backend. It does NOT implement
`BackendWaitSource`, `close_admission()`, or drain semantics (D4), and it does
NOT lift the KernelIo fail-closed verdict.

---

## 1. Gap audit (required before code)

| C2 requirement | Arena-level evidence already exists | ThreadPool integration analogue | Uring production mechanism already exists | Missing Uring evidence / seam | Production fix needed |
| -------------- | ----------------------------------- | ------------------------------- | ----------------------------------------- | ----------------------------- | --------------------- |
| C2b row 3 — state / generation validation | `arena_mainline_state_transition_matrix`, `arena_illegal_transition_contract_errors`, `arena_request_key_carries_context_provenance` | `tp_stale_generation_event_harmless` (stale handle through real cancel authority) | RequestArena leaf validates full `SlotHandle{slot, generation}` + context at `record_terminal` / `cancel` / `mark_running`; CqeRouter stores full handle and the arena re-validates it after cookie routing | Integration: drive a REAL accepted Uring op through `handle_for_completion_for_test` / `observe_for_test` and prove the full-handle chain `Completion -> arena binding -> SlotHandle(slot, full generation, context provenance)`; stale-handle cancel rejected `not_found` | None (test-only seams) |
| C2b row 4 — stale full SlotHandle cannot mutate a reused slot | `arena_stale_handle_leaves_live_occupant_untouched` | `tp_stale_generation_event_harmless` | Same arena generation validation; cookie never reused within backend lifetime (P0-B) | Integration: A(S,N) full lifecycle + release; B reuses S,N+1; captured A handle rejected with zero side effect on B; stale A cookie CQE dropped, B untouched | None |
| C2b row 5 — pending cancellation | `fake_cancel_disposition_counts_exactly_once`, `tp_canceled_ops_tallied_only_on_terminal_won` | `tp_enqueued_cancel_wins_no_syscall` (Scheme B) | Uring `cancel()`: `remove_exact` + `arena_.cancel` under `dispatch_mtx_`; `enqueue_after_commit` observes `terminal_noop` | Deterministic pending window: pause between `commit_binding` (accept LP) and `enqueue_after_commit`; cancel wins; prove no SQE / no router cookie / no ledger entry / no syscall; reap publishes once | Test-only `AfterCommitBeforeEnqueuePauseGate` |
| C2b row 6 — enqueued cancellation | (same rows) | Gate B (`BeforeWorkerDequeuePauseGate`: worker releases `work_mtx_` while paused; cancel wins pre-dequeue) | `cancel()` `remove_exact` + `arena_.cancel`; the dispatch drain re-reads the queue front after any cancel | Deterministic enqueued window: drain paused with `dispatch_mtx_` RELEASED while the request sits at the dispatch-ring front with no SQE installed; cancel wins; resumed drain installs nothing | Test-only `BeforeDispatchTransferPauseGate` (mirror of Gate B) |
| C2b row 7 — running cancellation | `tp_running_cancel_intent_does_not_tally`, `tp_running_cancel_intent_real_result_verbatim` | same (Gate C) | `intent_recorded` + `issue_running_cancel` (one fixed `cancel_queued` bit; tagged control SQE; control CQE informational) | Integration with a REAL kernel-blocked operation (pipe read): intent only, slot stays bound, borrow active, Completion not ready merely because cancel() was called; terminal comes only from the original operation CQE | None |
| C2b row 8 — identity-bearing reap / publication | `arena_reap_preserves_terminal_winner_order`, `fake_binding_identity_and_publication_boundary` | `tp_publication_boundary_reap_gates_ready`, `tp_terminal_publication_after_bookkeeping` | `arena_.reap(sink_)` is the sole publication authority; workers/CQE handler never publish | Integration: real op reaches `backend_ready`; Completion NOT ready until `poll()`/`wait_one()` reaps; exactly one publication | None |
| C2c row 11 — borrow lifetime | `c2c_arena_borrow_waiter_lease_matrix` (prepare inactive -> commit active -> backend_ready active -> reap ends) | `tp_backend_ready_borrow_still_active_before_reap`, `tp_running_borrow_cancel_intent_waiter_survives` | Submit commits `BorrowMetadata{fd,address,length}` through `arena_.prepare`; reap ends the borrow (I18) | Integration: borrow_for_test over real Uring op in pending / enqueued / running / backend_ready windows with EXACT fd/addr/len; sync op no-buffer shape; borrow does NOT end at SQE install / submit / CQE / record_terminal / cancel intent | None |
| C2c row 12a — waiter registration | `arena_waiter_registration_state_matrix` (registration orthogonal to execution state; only reap closes it) | `tp_running_window_waiter_registration`, `tp_backend_ready_window_waiter_registration` | `arena_.register_waiter` / `cancel_waiter` (public arena authorities, no Uring-specific waiter storage) | Integration: register while enqueued / running / backend_ready-before-reap; duplicate registration rejected per shared contract | None (test-only seams) |
| C2c row 13 — wait-cancel vs I/O-cancel independence | `arena_waiter_cancel_removes_only_the_waiter`, `arena_io_cancel_keeps_waiter_registration` | `tp_wait_cancel_keeps_io`, `tp_io_cancel_keeps_waiter` | Same arena authorities; Uring cancel path does not touch waiter state | Integration: wait-cancel removes only the waiter and returns/moves the RoutingLease (I/O continues, real result terminal); I/O cancel keeps the waiter (canceled terminal + waiter delivered at reap) | None |
| C2c row 14a — lease delivery / stale waiter authority | `arena_lease_transfer_chain_reap_path`, `arena_lease_transfer_chain_wait_cancel_path`, `arena_ready_event_waiter_survives_slot_reuse` | `tp_stale_waiter_authority_harmless` (gen N+1 occupant untouched) | `ReferenceReadySink` by-value delivery; arena validates generation on waiter seams | Integration: sink delivers token+lease exactly once; stale waiter handle (gen N) cannot register/cancel on the N+1 occupant | None |

Rows deferred by the ADR to Phase F remain deferred (C2b row 4b — cross-context
RequestKey authority; C2c rows 12b / 14b — real public waiter / Scheduler
consumer). D3 claims no Scheduler / public RequestHandle coverage.

**D3 production fixes: none semantic.** The only production-source changes are
(1) a behavior-preserving refactor of `cancel()` so the Completion-keyed public
entry and the guarded handle-keyed seam share ONE production cancel core
(`dispatch remove_exact` + `arena_.cancel` + terminal_won tally/signal), and
(2) `SLUICE_ASYNC_INTERNAL_TESTING`-guarded pause gates and observation
delegates, compiled out of production builds.

## 2. Frozen constraints preserved (D1/D2 — DO NOT CHANGE)

```text
RequestArena          = logical lifecycle / identity / generation / capacity /
                        terminal-winner / borrow / waiter / lease authority
private io_uring ring = execution ownership domain
SQE installation      = enqueued -> running ownership transfer (mark_running)
io_uring_submit()     = transport progress ONLY (no RequestState transition)
original op CQE       = execution retirement / terminal candidate
cancel CQE            = informational control completion ONLY
RequestArena::reap()  = SOLE Completion-ready publication authority
```

Preserved: no `RequestState` transition on submit return; no `Completion*`
kernel identity; no `SlotIndex`-only kernel identity; no unbounded map/deque;
no post-accept unbounded allocation; no local release of possibly kernel-owned
work; no cancel-CQE terminal authority; no shared multi-producer ring; no
SQPOLL; no SINGLE_ISSUER / DEFER_TASKRUN / ATTACH_WQ; no registered
buffers/files; no Scheduler/Batch integration; no topology/sharding work.

## 3. Test-only seams added (SLUICE_ASYNC_INTERNAL_TESTING only)

Mirror the approved ThreadPool observation style; every seam delegates to REAL
production authority (`RequestArena`, `ReferenceReadySink`, the production
cancel core). No test-side state-machine implementation, no side-band identity
map, no second backend generation counter.

```text
handle_for_completion_for_test(Completion&)        -> arena_.resolve_completion
observe_for_test(SlotHandle)                       -> arena_.observe_for_test
cancel_handle_for_test(SlotHandle)                 -> production cancel core
register_waiter_for_test(Completion&, token, lease) / cancel_waiter_for_test(Completion&)
register_waiter_handle_for_test(SlotHandle, ...) / cancel_waiter_handle_for_test(SlotHandle)
borrow_for_test(SlotHandle)                        -> arena_.borrow_for_test
waiter_for_test(SlotHandle)                        -> arena_.waiter_for_test
sink_deliveries() / sink_last_has_waiter() / sink_last_token() / sink_last_lease_id()
AfterCommitBeforeEnqueuePauseGate                  (pending window; fires OUTSIDE dispatch_mtx_)
BeforeDispatchTransferPauseGate                    (enqueued window; releases dispatch_mtx_ while paused)
```

All observation seams are read-only by-value; the gates are paused/resume
atomic handshakes with a bounded-deadline test helper (no sleep-based
correctness). Gate state is member data ONLY in the internal-testing target.

## 4. Evidence records (manifest)

```python
uring_c2b_identity_integration
    backends=("Uring",), required_modes=("real",), mandatory=True
    cases=(exact pinned uring_backend_c2b_identity_test case-set)

uring_c2c_borrow_waiter_integration
    backends=("Uring",), required_modes=("real",), mandatory=True
    cases=(exact pinned uring_backend_c2c_waiter_borrow_test case-set)
```

G2 discipline (PR #80): every pinned case must run exactly once, no missing /
unexpected / duplicate case; source registrations == manifest pinned cases
(anchored macro regex); a zero-case or filtered run must never masquerade as
PASS.

Expected manifest after D3:

```text
Uring C2a PASS real
Uring C2b PASS real
Uring C2c PASS real
Uring C2d PASS real
Uring C2e still NOT_IMPLEMENTED
KernelIo overall still NOT CONFORMING        <- NOT lifted in D3
```

## 5. Mutations executed (RED -> GREEN)

See `docs/verification/phase-d3-uring-identity-waiter-mutation-evidence.md` for
the per-mutant commands and results:

```text
D3-M1  route CQE by raw SlotIndex instead of full cookie/handle   -> stale identity detector RED
D3-M2  allow stale SlotHandle cancel to hit the new occupant      -> generation reuse detector RED
D3-M3  pending cancel does not disarm enqueue                     -> later SQE/execution or double terminal RED
D3-M4  enqueued cancel does not remove the dispatch linkage       -> later SQE/execution or invariant RED
D3-M5  running cancel locally terminalizes / releases the slot    -> delayed original CQE / generation detector RED
D3-M6  cancel CQE chooses / overwrites the terminal               -> original-vs-control ordering RED
D3-M7  borrow ends at original CQE / record_terminal              -> backend_ready borrow detector RED
D3-M8  wait-cancel cancels the I/O                                -> independence detector RED
D3-M9  I/O cancel removes the waiter                              -> waiter-survival detector RED
D3-M10 reap drops or duplicates the waiter RoutingLease           -> sink exactly-once detector RED
D3-M11 stale waiter handle mutates the N+1 occupant               -> stale waiter detector RED
```

## 6. Gate evidence (actual commands)

Actual results on the final D3 head (branch branch test/phase-d3-uring-identity-waiter-conformance,
base SHA `126612a`, kernel 6.18.33.2-microsoft-standard-WSL2, liburing 2.14,
Clang Debug with `--with-liburing`):

```text
pre-push gate              : PASS (bash scripts/gates/pre-push.sh — "ALL CHECKS PASSED")
focused C2b real target    : PASS 11/11 (uring_backend_c2b_identity_test)
focused C2c real target    : PASS 13/13 (uring_backend_c2c_waiter_borrow_test)
existing D1 submit-failure : PASS (uring_submit_failure_test)
existing D2 target         : PASS 10/10 (uring_d2_failure_noalloc_test; evidence-meta mode=real)
backend_conformance_test   : PASS (per-backend isolated subprocess runs)
aggregate conformance      : PASS — uring_c2b_identity_integration=PASS,
                             uring_c2c_borrow_waiter_integration=PASS,
                             uring_c2d_failure_injection=PASS,
                             uring_c2e_close_drain_not_implemented=INCOMPLETE,
                             KernelIo overall NOT CONFORMING (fail-closed retained)
manifest self-tests        : PASS 178/178 (scripts/tests/test_backend_conformance_manifest.py,
                             incl. D3 source<->pin drift detectors)
real liburing Debug full   : PASS (xmake test -v — full group)
real liburing Release full : PASS (xmake f -m release --toolchain=clang -y; xmake test -v)
stub/off Debug suite       : PASS (xmake f --with-liburing=false -m debug; xmake test -v)
ASan+UBSan                 : PASS (xmake f -m asanubsan --toolchain=clang -y; xmake run -g test)
TSan                       : PASS (xmake f -m tsan --toolchain=clang -y; xmake run -g test)
negative compile           : PASS (12 + 6 + 9 + 3 + 2 authority probes)
formal d1-uring-poison     : PASS (python3 scripts/formal/verify.py)
docs / git diff --check    : PASS (check-doc-links --self-test, check-doc-links,
                             verify-architecture-docs, git diff --check)
```
