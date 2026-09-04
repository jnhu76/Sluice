# Phase C2d Compliance Gate — Failure Injection / Accepted-Terminal under Allocator Failure

**Roadmap:** [`remediation-roadmap.md`](remediation-roadmap.md) — Phase C (status PARTIAL; C2d COMPLETE)
**Governing ADR:** [`ADR-explicit-io-request-contract`](../adr/ADR-explicit-io-request-contract.md) (Accepted) — Decisions 5, 6, 8, 12, 13, 14, 15; invariants I7, I9, I17, I19
**Issue #68:** https://github.com/jnhu76/Sluice/issues/68 — C2d scope (rows 9–10)
**Branch:** test/phase-c2d-failure-injection-accepted-terminal
**Baseline:** `origin/master` @ `8b24ede5b54bc41fde67decb3b820c6385faf125` (PR #71, C2c merged)
**Scope:** Tests + test-only guarded seams (dispatch-failure injection, worker-spawn
failure injection) + manifest/gate records + docs. No public API change, no new
public request handle, no Uring migration, no Scheduler/Batch/wake-phase work.

This is the PR-level evidence ledger for Phase C2d, the fourth C2 semantic-coverage
slice: **failure injection** (row 9) and **accepted-terminal under allocator failure**
(row 10). C2d closes these rows for the Fake reference path and the real
ThreadPoolBackend, records Uring's Phase-D gap as a `not_implemented` manifest record
that enters Uring's verdict, and proves every detector case fails on deliberately
nonconforming code (mutants M1–M13).

---

## 1. Scope

| Requirement (Issue #68 row) | Evidence |
|---|---|
| 9 — failure injection (alloc / worker-spawn / dispatch failure surfaces a defined terminal, leaves no partial state) | `tp_c2d_reserve_failure_injection_zero_residue`, `tp_c2d_prepare_failure_injection_slot_rollback`, `tp_c2d_commit_failure_injection_rollback_binding_before_accept`, `tp_c2d_cas_loss_rejection_zero_side_effects`, `tp_c2d_partial_worker_startup_failure`, `tp_c2d_dispatch_failure_injection_size_op`, `tp_c2d_dispatch_failure_injection_void_op`, `tp_c2d_dispatch_failure_races_cancel_exactly_one`, `tp_c2d_cancel_after_dispatch_failure_terminal_no_overwrite` (ThreadPool); `fake_full_window_alloc_failure_defined_error_terminal` + existing `fake_cas_loss_rejection_zero_side_effects` (Fake) |
| 10 — post-commit allocation failure still reaches terminal (ADR Decision 14 / I9) | `tp_c2d_real_worker_post_commit_no_allocation`, `tp_c2d_dispatch_failure_post_commit_no_allocation` (ThreadPool, always-throw operator new); `fake_full_window_alloc_failure_defined_error_terminal` (Fake, full-window) |

**Out of scope (explicitly, unchanged from Issue #68):** rows 4b/12b/14b (Phase F),
row 15 (C2e), Scheduler/Batch identity consumption (Phase F), backend-ready wake
bridge (Phase G), Uring RequestArena migration (Phase D), public `RequestHandle`.

## 2. Authority

- **Issue #68** — the C2d design authority: failure injection (row 9) and
  accepted-terminal under allocator failure (row 10).
- **ADR-explicit-io-request-contract (Accepted)** — Decision 5 (five-stage
  admission transaction; pre-commit rollback), Decision 6 (synchronous error
  vocabulary), Decision 8 (borrow commit → completion-ready), Decision 12
  (terminal winner; a **post-commit dispatch failure after execution ownership
  is proven absent** is an explicit winner candidate), Decision 13 (bounded
  capacity; arena full → `would_block`), Decision 14 (the accepted terminal
  path — enqueue, backend-ready publication, **dispatch-failure recording**, and
  reap — cannot depend on new unbounded allocation), Decision 15 (admission
  close / drain / destruction; slot release allocation-free); invariants I7
  (borrow lifetime), I9 (post-commit no new allocation dependency), I17/I19
  (enqueue pin / reap eligibility); **ADR Gate 4** (injected failure at
  reserve/prepare/commit/dispatch — per-stage evidence in §3.1; the
  deterministic commit/enqueue pause in which pending cancellation wins —
  `tp_c2d_cancel_wins_before_enqueue_injection_armed`, §3.5; enqueue
  allocation-free with no ordinary recoverable failure).
- **AGENTS.md** §10.2–10.7 (transactional submission, dispatch ownership,
  terminal winner, reap authority, slot release), §10.5 (the explicit
  "ownership-safe post-commit dispatch failure" winner candidate), §12
  (resource bounds; the ring-full invariant MUST stay fail-fast), §14
  (quiescent destruction; a worker-pool teardown join is not I/O drain), §15
  (test-only controls), §16.3 (TSan for concurrency), §18 (conformance
  philosophy).
- **Architecture Constitution** — AC-3 (transactional admission), AC-4
  (accepted terminality), AC-7 (bounded resources), AC-12/AC-13 (identity /
  reap authority).
- **Finding P1-04** — "No test injects thread-creation failure": C2d adds the
  regression test the finding says is missing (Phase E resolution claimed the
  cleanup; C2d proves it).

## 3. Linearization of every failure class (the C2d semantics)

### 3.1 Pre-commit failure (reserve / prepare / binding / commit boundary)

Any failure before the submit-success linearization point is a **synchronous
rejection** with zero side effect: the Completion stays idle, no borrow exists,
`accepted_outstanding` / `slot_in_use` / dispatch / ready-ring are unchanged,
and the candidate slot is rolled back by `rollback_reserved_or_prepared` (the
lost binding CAS) or by `rollback_binding_before_accept` + rollback (a commit
failure). The rejected resources are immediately reusable by a fresh submit.

ADR Gate 4 requires injected failure at **reserve, prepare, commit boundary,
and dispatch**. Per stage, the C2d evidence is:

| Stage | Failure surface | C2d evidence | Status |
|---|---|---|---|
| reserve | capacity full → `would_block`; admission closed → `invalid_state` | Phase E `phase_e_capacity_full_returns_would_block` + the C2a shared capacity suite (deterministic, real backend), PLUS `tp_c2d_reserve_failure_injection_zero_residue` — the Gate-4 injected reserve failure: the submit path returns `would_block` BEFORE reserving, the Completion stays idle, zero slot/borrow/dispatch/ready residue, and the capacity is untouched (a fresh submit immediately succeeds) | DRIVEN (C2a suite + C2d injected stage failure) |
| reserve | allocation failure | `reserve()` performs NO allocation: fixed slot array + construction-time-pre-reserved free list (arena ctor); an empty free list is capacity pressure (`would_block`), not OOM | N/A (structural) |
| prepare | stale/invalid handle | natural failure unreachable for a well-formed submit (the submit path holds the current-generation `reserved` handle it just obtained); no allocation on the path. Gate-4 injected verification: `tp_c2d_prepare_failure_injection_slot_rollback` — injected prepare failure AFTER a successful reserve drives the SAME `rollback_reserved_or_prepared` rollback the natural path uses: slot_in_use → 0, capacity immediately recyclable (fresh submit runs a real syscall), Completion idle, zero dispatch/ready residue | DRIVEN (C2d, injected; natural surface N/A structural) |
| binding install | duplicate/null binding | unreachable for a well-formed submit (fresh slot, real Completion pointer); no allocation | N/A (structural) |
| begin_binding (Completion CAS) | non-idle Completion | `tp_c2d_cas_loss_rejection_zero_side_effects` — the one naturally triggerable pre-commit failure, driven deterministically on the real backend: `invalid_state`, both original Completions untouched, `slot_in_use`/`outstanding` unchanged at 2, dispatch holds exactly the two real ops, `backend_ready_count` 0 (no ghost), both accepted ops reach exactly one terminal, capacity immediately recycled by a third submit | DRIVEN (C2d, new) |
| commit | stale/invalid handle → `rollback_binding_before_accept` | natural failure unreachable for a well-formed submit (the slot is `prepared` at current generation immediately after a successful binding install); no allocation. Gate-4 injected verification: `tp_c2d_commit_failure_injection_rollback_binding_before_accept` — the binding CAS WINS (Completion in `binding`), then commit is injected to fail: the submit path executes the REAL `rollback_binding_before_accept` (binding → idle) + `rollback_reserved_or_prepared` — the ONLY executable instance of that branch in the corpus (a natural commit failure is unreachable after a same-thread reserve → prepare → begin_binding). The Completion returns to fully reusable idle and the SAME Completion + capacity are immediately reused by a fresh submit; accepted-outstanding/borrow/dispatch/ready residue all zero | DRIVEN (C2d, injected; natural surface N/A structural) |
| enqueue | no failure surface; `terminal_noop` outcome | allocation-free `noexcept` (ADR Gate 4: "enqueue is allocation-free and cannot produce an ordinary recoverable failure"); the `terminal_noop` outcome is driven deterministically by `tp_c2d_cancel_wins_before_enqueue_injection_armed` (§3.5, ADR Gate 4 pending-cancel scenario) | DRIVEN (C2d, new) |
| dispatch push | full ring | `BoundedDispatchQueue::push_back` is `noexcept`, capacity == request_capacity, allocation-free; a full push is the invariant fail-fast (Debug AND Release, §3.3) — never a recoverable failure | N/A (structural, fail-fast) |
| dispatch (post-commit terminal event) | ADR Decision-12 candidate | guarded one-shot injection probe (`SLUICE_ASYNC_INTERNAL_TESTING` only) driving the SHARED production arena's terminal-winner/reap machinery (§3.3) | PROBE (test-only, supplementary) |

The C2d `COMPLETE` claim is therefore scoped precisely: all four ADR Gate-4
stages are DRIVEN — reserve / prepare / commit-boundary by deterministic
injected stage failure on the real backend, the binding-CAS by its real
transactional residue proof, and the dispatch-failure terminal by a guarded
probe, not a claim that production ThreadPool handles an ordinary dispatch
failure (it cannot fail by construction). Where the natural failure surface is
structurally unreachable on a well-formed submit path (allocation-free,
bounded, fail-fast), it is marked N/A structural WITH the injected Gate-4
verification beside it — the injected verification is the executable instance
of the rollback code (for prepare/commit) or of the synchronous rejection
(for reserve). A CAS-loss case is never presented as coverage for
prepare/commit boundaries it does not drive.

### 3.2 Worker / backend construction failure (partial worker startup)

Thread creation is a **synchronous construction-time admission boundary**
(P1-04 Phase E resolution): the constructor stops and joins the already-started
workers (`stopping_ = true`, `work_cv_.notify_all()`, join, rethrow). C2d
proves it with an injected `std::system_error(errc::resource_unavailable_try_again)`
at a chosen worker index:

- construction propagates the injected failure synchronously (fail before
  worker 0 AND fail after worker 1 started);
- surviving the failed construction with no `std::terminate` IS the join proof
  (an unjoined joinable `std::thread` in the unwound member vector terminates
  the process);
- the RAII-restored seam leaves no process-global residue: a normal
  construction afterwards succeeds with the full worker count and destroys
  quiescently.

### 3.3 Post-commit permanent dispatch failure (ADR Decision 12 candidate)

The injected dispatch failure fires **after `arena_.enqueue(h)` won and
acknowledged the enqueue pin, before `dispatch_.push_back(h)`, inside
`work_mtx_`**. Ownership proof that no executor holds execution ownership:

- workers obtain a handle ONLY by popping the dispatch ring under `work_mtx_`;
  the handle was never pushed, and the injection holds `work_mtx_` across the
  decision — no worker, ring entry, kernel, or other executor can hold or have
  held the handle;
- the slot is `enqueued` at current generation with the pin acknowledged
  (`arena_.enqueue` outcome `enqueued` — the submit path's final slot access),
  so `arena_.record_terminal(h, err(backend_error))` is a legal terminal-winner
  transition from `enqueued` (the arena accepts pending/enqueued/running);
- the terminal is recorded EXACTLY ONCE through the arena's terminal-winner
  authority (first caller wins; a later cancel observes `already_terminal`);
- reap publishes Completion-ready exactly once through the slot binding; the
  borrow stays active from commit until reap (record_terminal does NOT end the
  borrow — only reap does, I7);
- submit still returns success — the dispatch failure is a terminal event, not
  a rejection;
- the request never enters a worker or a syscall (`syscall_count` unchanged,
  dispatch ring empty);
- the ready domain is signalled (`signal_ready_progress`) so a waiter parked
  BEFORE the submit observes the backend-ready (a lost wake would hang a
  blocked `wait_one` — the parked-waiter detector catches it with a bounded
  timeout).

The **ring-full fail-fast invariant path is NOT converted** into a recovery
path: `BoundedDispatchQueue::push_back` still fail-fasts when full
(`threadpool_dispatch_queue_invariant_fail_fast`, Debug AND Release). Production
ThreadPool dispatch therefore **cannot fail by construction**: the ring is sized
== request_capacity, the push is `noexcept` and allocation-free, and a full push
is an invariant violation, never a recoverable failure (ADR Gate 4: "enqueue is
allocation-free and cannot produce an ordinary recoverable failure"). There is
consequently no production dispatch-failure *handling* path to prove.

The injected branch is a **TEST-ONLY probe** (AGENTS.md §2/§15 — test seams are
non-authoritative): it simulates the ADR Decision-12 "post-commit dispatch
failure after execution ownership is proven absent" candidate by driving the
**shared PRODUCTION arena machinery** — `record_terminal`, the ready-ring,
`reap`, and the Completion publication thunk are all production code in
`request_arena.hpp` — proving that a post-commit terminal event with no
dispatch linkage produces exactly one terminal and exactly one publication.
The probe does not, and is not presented as, evidence of a production
failure-handling path.

Seam-absence: the entire injected block — branch, local, and symbol — is
preprocessor-removed from the production build; the production
`enqueue_after_commit` compiles to the original Phase E code (verified by
object scan: 0 seam symbols and 0 seam identifier strings in the production
object and `libsluice_async.a`; the internal-testing library carries them).
Symbol inspection alone cannot prove control-flow absence, which is why the
source-level exclusion (every reference sits inside
`SLUICE_ASYNC_INTERNAL_TESTING`) is the primary claim and the scan is
corroborating.

Both operation shapes go through different template paths (`submit_size` /
`submit_void`) and share the same `enqueue_after_commit` seam; both are
proven (`tp_c2d_dispatch_failure_injection_size_op` for read,
`tp_c2d_dispatch_failure_injection_void_op` for sync_data).

### 3.4 Post-commit allocator failure (ADR Decision 14 / I9)

The accepted path — submit → enqueue → terminal (worker `record_terminal` OR
the injected dispatch-failure `record_terminal`) → ready wake → poll/reap →
reset — runs entirely under an **always-throw operator new** (counting +
throwing probe, the same malloc-based pattern as
`reference_backend_no_alloc_test`; compiled out under TSan) and must still:
reach exactly one terminal, allocate ZERO bytes, and never let an exception
escape a worker. Both the ordinary real-syscall path and the injected
dispatch-failure path are proven on the real ThreadPoolBackend; the Fake
reference path is proven in ONE full window including a **defined error**
terminal (`complete_oldest_with_error` under always-throw) — a
completed-with-error op is not allocation-gated either.

### 3.5 Terminal winner vs cancel (row 9 interaction)

Cancel and the injected dispatch failure serialize on `work_mtx_`; whichever
reaches the work domain first wins. BOTH orderings now have deterministic
evidence plus the genuinely racy interleaving:

- **cancel-wins (ADR Gate 4)**: `tp_c2d_cancel_wins_before_enqueue_injection_armed`
  pauses the submit path after commit, before it takes `work_mtx_` (slot
  `pending`, enqueue pin live); the test's cancel wins the canceled terminal
  from `pending` (Scheme B); an intervening reap publishes nothing and the
  Completion stays non-ready (I17/I19); the resumed enqueue observes
  `backend_ready`, acknowledges the pin as a terminal no-op, and links
  NOTHING — submit still succeeds, no worker runs, and the injection (still
  armed) does not fire (`fired == 0`), because the seam is gated on the
  `enqueued` outcome. This is the ADR Gate-4 "deterministic commit/enqueue
  pause in which pending cancellation wins" obligation, previously uncovered.
- **injection-wins**: `tp_c2d_dispatch_failure_injection_size_op` /
  `tp_c2d_dispatch_failure_injection_void_op` (the terminal is recorded
  synchronously inside submit) and
  `tp_c2d_cancel_after_dispatch_failure_terminal_no_overwrite` (a later cancel
  is a pure no-op: no overwrite, no second tally, no double ready-ring push —
  reap publishes exactly once).
- **racy interleaving**: `tp_c2d_dispatch_failure_races_cancel_exactly_one`
  covers the genuine barrier race (submit vs cancel) and asserts the invariant
  that holds in EVERY interleaving: exactly one terminal, exactly one
  publication, no worker/syscall execution, and at most one tally —
  `canceled_ops == 1` iff the result is `canceled`. The injected
  `backend_error` terminal contributes NO tally: `AsyncStats::completion_errors`
  is not wired for ThreadPoolBackend (§10 residual gap), so an injection-wins
  interleaving records zero tallies.

## 4. Test-only seams (all `SLUICE_ASYNC_INTERNAL_TESTING`-guarded)

1. **`ThreadPoolBackend::DispatchFailureInjection`** — post-commit
   dispatch-failure control (`armed` / `fired`). The control object must be
   declared before the backend and outlive it (same lifetime rule as the
   pause gates). The production build carries no member, no branch, no local,
   and no symbol (verified via `nm` on `libsluice_async.a` plus a string scan
   of the production object; the entire injected block is preprocessor-removed,
   so production `enqueue_after_commit` is byte-for-byte the Phase E code).
2. **`ThreadPoolBackend::set_injected_worker_spawn_failure_index`** — static
   construction-time spawn-failure seam (value = zero-based worker index to
   fail; `SIZE_MAX` disarms). A static seam is REQUIRED because the injection
   point is the constructor, which runs before any instance exists
   (constructor-before-instance); the tests guarantee serial isolation (only
   the constructing thread reads it while armed; the harness runs cases
   sequentially in one process) and restore the disarmed sentinel via RAII
   (`ScopedSpawnFailureSeam`) even on failure. Compiled out of production
   builds (verified via `nm`).
3. **`ThreadPoolBackend::BeforeEnqueueLockPauseGate`** — deterministic
   commit/enqueue pause (ADR Gate 4). The submit path pauses after commit and
   before taking `work_mtx_`, so the test's pending cancellation wins
   (`tp_c2d_cancel_wins_before_enqueue_injection_armed`); the resumed enqueue
   then observes `backend_ready` and acknowledges the pin as a terminal no-op.
   Instance-owned, same lifetime rule as the other pause gates; compiled out of
   production builds.

No public API, no production behavior, no production branch, no symbol, and no
object-layout change outside the internal-testing variant (the guarded member
is compiled out of the production class).

## 5. Test case ledger

| Case (SLUICE_TEST_CASE) | Target | Status |
|---|---|---|
| `tp_c2d_cas_loss_rejection_zero_side_effects` | threadpool_backend_c2d_failure_test | PASS |
| `tp_c2d_partial_worker_startup_failure` | threadpool_backend_c2d_failure_test | PASS |
| `tp_c2d_dispatch_failure_injection_size_op` | threadpool_backend_c2d_failure_test | PASS |
| `tp_c2d_dispatch_failure_injection_void_op` | threadpool_backend_c2d_failure_test | PASS |
| `tp_c2d_real_worker_post_commit_no_allocation` | threadpool_backend_c2d_failure_test | PASS |
| `tp_c2d_dispatch_failure_post_commit_no_allocation` | threadpool_backend_c2d_failure_test | PASS |
| `tp_c2d_cancel_wins_before_enqueue_injection_armed` | threadpool_backend_c2d_failure_test | PASS |
| `tp_c2d_dispatch_failure_races_cancel_exactly_one` | threadpool_backend_c2d_failure_test | PASS |
| `tp_c2d_cancel_after_dispatch_failure_terminal_no_overwrite` | threadpool_backend_c2d_failure_test | PASS |
| `fake_full_window_alloc_failure_defined_error_terminal` | reference_backend_no_alloc_test | PASS |
| Python: `test_backend_conformance_manifest.py` (136 cases, incl. 16 C2d) | unittest | PASS |

## 6. Fake / ThreadPool eligibility, historical Uring gap

- **Fake = ELIGIBLE** — `c2d_fake_failure_injection_terminal=PASS` in the
  per-backend report (defined-error terminal under a full-window always-throw
  probe, plus the existing CAS-loss/would_block no-allocation cases).
- **ThreadPool = ELIGIBLE** — `c2d_threadpool_failure_injection=PASS` (all 9
  cases, real backend, deterministic seams).
- **Uring = NOT CONFORMING** — `uring_c2d_failure_injection_not_implemented=
  INCOMPLETE (not_implemented)` is the authoritative C2d gap record, surfaced
  in the lifecycle layer AND in the verdict reasons (alongside the C2a/C2b/C2c
  gaps). Uring's own `uring_submit_failure_test` drives the pre-RequestArena
  SQE model and does NOT satisfy the C2d contract; Uring stays NOT CONFORMING
  until Phase D.

### Phase D2 reconciliation (2026-08-09)

The paragraph above records the historical Phase C2d exit state; it is not the
current Uring classification. After PR #78 completed D1, Phase D2 added the
real-liburing `uring_d2_failure_noalloc_test` and replaced
`uring_c2d_failure_injection_not_implemented` with the implemented, real-mode-only
`uring_c2d_failure_injection` manifest record. The new target covers natural
pre-commit rollback, ordinary size/void accepted no-allocation windows, permanent
Class-A recovery, Class-C retention, bounded cancel/control recovery, and
cancel/failure one-winner behavior. The existing D1 target remains the evidence
for transient/zero/positive-prefix transport neutrality and the detailed P0-D
classification cases.

Stub mode remains build/API evidence and is mechanically INCOMPLETE for this
record. KernelIo remains NOT CONFORMING because D3's C2b/C2c and D4's C2e gaps
are unchanged. The D2 Gate 0–4 record and current command ledger are
[`phase-d2-uring-failure-noalloc-gate.md`](phase-d2-uring-failure-noalloc-gate.md).

## 7. Validity evidence (mutations)

Method: **local uncommitted single-point production mutation** (the
C2b/C2c-precedented alternative), applied by a harness that snapshots the
pristine file, applies one exact replacement, rebuilds, runs exactly the
detector case, records the RED exit + diagnostic, restores the pristine file,
and re-runs the case GREEN. See
[`docs/verification/phase-c2d-failure-injection-mutation-evidence.md`](../verification/phase-c2d-failure-injection-mutation-evidence.md)
for the full per-mutant ledger.

| Mutant | Deliberate defect | Expected failing case | Actual failing case / failure mode |
|---|---|---|---|
| M1 | dispatch failure is handled by pushing the handle anyway (worker executes) | `tp_c2d_dispatch_failure_injection_size_op` | same — result is the real success, `!c.result().has_value()` fails |
| M2 | wrong terminal error code on the injected path (`no_space` instead of `backend_error`) | same | same — `error().code == backend_error` fails |
| M3 | ready-domain wake skipped on the injected path | same | same — `wait_one lost the dispatch-failure ready wake` (bounded timeout) |
| M4 | spawn-failure catch does not join the started workers | `tp_c2d_partial_worker_startup_failure` | same — `std::terminate` (unjoined thread vector), process abort |
| M5 | spawn-failure catch swallows the failure (partial pool accepted silently) | same | same — `constructor must propagate the injected spawn failure` |
| M6 | binding-CAS-loss rollback removed (slot leaks) | `tp_c2d_cas_loss_rejection_zero_side_effects` | same — `slot_in_use` residue trips the assertion, then the non-quiescent destructor fail-fast aborts |
| M7 | post-commit heap allocation added to the enqueue path | `tp_c2d_real_worker_post_commit_no_allocation` | same — bad_alloc in the noexcept post-commit path aborts |
| M8 | Fake manual terminal path allocates | `fake_full_window_alloc_failure_defined_error_terminal` | same — bad_alloc under always-throw aborts |
| M9 | worker-spawn injection seam removed | `tp_c2d_partial_worker_startup_failure` | same — `constructor must propagate the injected spawn failure` |
| M10 | dispatch-failure injection fires on a terminal_noop enqueue (seam not gated on the enqueued outcome) | `tp_c2d_cancel_wins_before_enqueue_injection_armed` | same — `the injection must not fire on a terminal_noop enqueue` |
| M11 | injected commit-failure rollback drops `rollback_binding_before_accept` | `tp_c2d_commit_failure_injection_rollback_binding_before_accept` | same — the Completion is stuck in `binding`: the fresh submit loses the idle→binding CAS and the destructor fail-fast aborts |
| M12 | injected reserve check removed (submit succeeds instead of `would_block`) | `tp_c2d_reserve_failure_injection_zero_residue` | same — the armed injection is ignored; the rejection assertions fail |
| M13 | injected prepare check removed (submit succeeds instead of `invalid_state`) | `tp_c2d_prepare_failure_injection_slot_rollback` | same — the armed injection is ignored; the rejection assertions fail |

Every mutant ran RED (non-zero exit) and GREEN after restore; a marker scan
confirmed zero `MUTANT` residue.

## 8. Commands run (validation) — see section 9 for the full matrix

| Gate | Command | Result |
|---|---|---|
| Focused ThreadPool | `xmake build threadpool_backend_c2d_failure_test && xmake run threadpool_backend_c2d_failure_test` | PASS (12 cases) |
| Focused Fake | `xmake build reference_backend_no_alloc_test && xmake run reference_backend_no_alloc_test` | PASS (5 cases) |
| Stability | 10× repeated runs of `threadpool_backend_c2d_failure_test` | PASS (10/10, no flake) |
| Manifest self-test | `python3 scripts/tests/test_backend_conformance_manifest.py` | PASS (136 cases) |
| Aggregate gate | `python3 scripts/verify-backend-conformance.py` | PASS (Fake/TP ELIGIBLE with C2d records PASS; Uring NOT CONFORMING with the C2d gap in its reasons) |
| RED validity | 13 mutations (M1–M13), focused filtered runs | all RED; all restored GREEN (see §7 + mutation ledger) |

> **Gate-run note (honesty):** the FIRST aggregate-gate run was executed
> concurrently with the full `xmake test -v` suite and reported one isolated
> RUN_FAIL for the pre-existing `threadpool_scheme_b_race` evidence under CPU
> contention; the full Debug suite itself passed 147/147 in the same window,
> and the gate passed on the immediate clean rerun (recorded above). The flake
> is under-load contention in an existing race case, not a C2d regression; the
> rerun result is the gate's authoritative outcome. The review-fix round
> reproduced the same pattern once (first gate run overlapping the still-
> finishing full suite → `c2b_threadpool_identity_integration` RUN_FAIL; the
> full Debug suite passed 147/147 including the scheme-b target, and the gate
> passed on the clean rerun).
>
> **Test-hygiene note (ASan):** the first full ASan run exposed a RACY
> assertion in the C2d CAS-loss case — `dispatch_size_for_test() == 2`
> observed the dispatch ring while the single worker dequeues concurrently, so
> the ring occupancy was not deterministic (~20% abort under ASan timing:
> `~Completion` fail-fast after the case's early return). Fixed by pausing the
> worker at the existing `BeforeWorkerDequeuePauseGate` (Gate B) before its
> first dequeue, making the ring/syscall/ready observations deterministic, then
> disarming the gate (the worker's resume-acquire makes the disarm visible) and
> draining normally. Re-verified: Debug 8/8, ASan 30× repeated runs 0 failures,
> full ASan gate green. The failure was a test-observation race, not a
> production defect.

> **Review-fix round (post-CodeRabbit/second-review):** (1) the injected
> dispatch-failure block was restructured so production `enqueue_after_commit`
> compiles to the original Phase E code — no branch, no local, no symbol
> (object/archive scan: 0 seam symbols, 0 seam identifier strings); (2) a
> deterministic cancel-wins ordering was added (`tp_c2d_cancel_wins_before_enqueue_injection_armed`,
> ADR Gate 4) with the `BeforeEnqueueLockPauseGate` seam; (3) the per-stage
> pre-commit matrix (§3.1) re-scopes reserve/prepare/commit as structural N/A
> vs the driven binding-CAS failure; (4) the "exactly one tally" claim was
> corrected to "at most one tally" (§3.5/§10) to match the unwired
> `completion_errors`; (5) the two manifest self-tests were hardened per
> CodeRabbit (backend-agnostic arena PASS seeding; a synthetic ReferenceProfile
> backend that actually exercises the mandatory-slot priority-2 branch);
> (6) detectors collect-then-reset so mutation RED diagnostics are the case's
> own message (quiescent destructor). Re-validated: mutations M1/M2/M3/M7/M10
> RED→GREEN (M10 = injection firing on a terminal_noop enqueue, new), Debug
> full 147/147, 10× stability, manifest 136, aggregate gate PASS on the clean
> rerun (one contention RUN_FAIL of the pre-existing scheme-b evidence while
> the full suite was still finishing — see the gate-run note above), 5×
> negative-compile, doc-link/architecture checks PASS.
>
> **Second review round (P1 per-stage injection, fixed):** the reviewer's P1
> (ADR Gate 4 demands injected failure at reserve/prepare/commit-boundary, not
> only a structural-N/A argument) is closed by the `SubmitStageFailureInjection`
> seam: (1) `tp_c2d_reserve_failure_injection_zero_residue` — injected
> reserve failure returns `would_block` before reserving; Completion idle, zero
> residue, capacity untouched; (2) `tp_c2d_prepare_failure_injection_slot_rollback`
> — injected prepare failure after a successful reserve drives the SAME
> `rollback_reserved_or_prepared` rollback the natural path uses; slot_in_use
> → 0, capacity immediately recyclable; (3) `tp_c2d_commit_failure_injection_rollback_binding_before_accept`
> — the binding CAS wins (Completion in `binding`), then commit is injected to
> fail: the submit path executes the REAL `rollback_binding_before_accept`
> (binding → idle) + slot rollback, the ONLY executable instance of that
> branch in the corpus (a natural commit failure is unreachable after a
> same-thread reserve → prepare → begin_binding), and the Completion returns
> to fully reusable idle — the same Completion + capacity are immediately
> reused. §3.1's matrix now marks reserve/prepare/commit **DRIVEN (injected)**
> with the natural surface N/A structural. All seam blocks compile out of
> production builds (no branch, no local, no symbol; re-scanned). New
> mutations M11 (drop `rollback_binding_before_accept` from the injected
> commit rollback), M12 (drop the injected reserve check), M13 (drop the
> injected prepare check) all ran RED on their detector case and GREEN after
> restore. Focused target now 12 cases.

## 9. Validation matrix (full evidence)

All rows below were executed on the current branch head. `PASS` is recorded
only for commands that actually ran green.

| Gate | Command | Result |
| ---- | ------- | ------ |
| Debug / Clang full | `xmake f -m debug --toolchain=clang -y && xmake build -g test && xmake test -v` | PASS (147 targets, 0 failed) |
| Focused ThreadPool | `xmake run threadpool_backend_c2d_failure_test` | PASS (12 cases) |
| Focused Fake | `xmake run reference_backend_no_alloc_test` | PASS (5 cases) |
| Stability | 10× repeated runs of the C2d ThreadPool target | PASS (10/10) |
| Release / Clang | `xmake f -m release --toolchain=clang -y && xmake build -g test && xmake test -v` | PASS (147 targets, 0 failed; re-run after the test-hygiene fix) |
| ASan/UBSan | `xmake f -m asanubsan --toolchain=clang -y && xmake build -g test && xmake run -g test` | PASS (exit 0; zero ASan/UBSan reports; re-run after the test-hygiene fix — the initial run's racy-assertion abort is documented in section 8) |
| TSan | `xmake f -m tsan --toolchain=clang -y && xmake build -g test && xmake run -g test` | PASS (exit 0; zero race reports, incl. the dispatch-failure-vs-cancel and worker-vs-submit races) |
| Manifest self-test | `python3 scripts/tests/test_backend_conformance_manifest.py` | PASS (136 cases, incl. 16 C2d) |
| Aggregate gate | `python3 scripts/verify-backend-conformance.py` | PASS (Fake/TP ELIGIBLE; Uring NOT CONFORMING with the C2d gap) |
| Doc links | `python3 scripts/check-doc-links.py --self-test` + `python3 scripts/check-doc-links.py` | PASS (no broken links, no stale paths) |
| Architecture docs | `python3 scripts/verify-architecture-docs.py` | PASS |
| Negative-compile | `scripts/verify-completion-authority-negative-compile.sh` | PASS (12 cases) |
| Negative-compile | `scripts/verify-request-arena-negative-compile.sh` | PASS (6 cases) |
| Negative-compile | `scripts/verify-async-identity-negative-compile.sh` | PASS (3 cases) |
| Negative-compile | `scripts/verify-external-backend-authority-negative-compile.sh` | PASS (2 cases) |
| Negative-compile | `scripts/verify-async-api-negative-compile.sh` | PASS (9 cases; CI runs it at `.github/workflows/ci.yml` — included here for a complete negative-compile row set) |
| Seam absence | `nm -C` symbol scan of production `libsluice_async.a` for `injected_worker_spawn`, `dispatch_failure_injection`, and `submit_stage_failure_injection`/`injected_precommit_stage_failure` | PASS (0 matches); the internal-testing library carries all of them. Post-restructure the entire injected block is preprocessor-removed from the production build — no branch, no local, no symbol — so the scan result and the source-level exclusion agree (symbol inspection alone cannot prove control-flow absence, which is why the source-level claim is stated here too) |
| Diff hygiene | `git diff --check` | PASS (clean) |

## 10. Remaining gaps

- **C2e** — close/drain/reset (row 15; row 16 already FULL): PENDING, not closed.
- **Rows 4b/12b/14b** — Phase F scope (unchanged).
- **Phase D** — D1 RequestArena/private-ring/P0-D and D2 failure/no-allocation
  evidence are implemented; D3 identity/cancel/borrow/waiter and D4
  wait/close/drain/KernelIo-lift remain pending. The historical Uring C2d gap
  above is superseded by the Phase D2 reconciliation note.
- **Phase G** — backend-ready progress wake bridge: PENDING (out of C2d scope).
- **Formal models** — no TLA suite under `spec/tla/` binds the RequestArena /
  dispatch-failure lifecycle (manifest checked; the formal suites cover
  scheduler primitives e10–e16 and blocking-io-pool), so the C2d
  dispatch-failure terminal candidate and the worker-spawn construction
  boundary change no modeled transition and no model update is required
  (AGENTS.md §17 formal-coverage gap recorded here). A future RequestArena
  model should encode the post-commit dispatch-failure terminal-winner
  candidate and the partial-construction join protocol.
- **completion_errors counter** — ThreadPoolBackend does not tally
  `completion_errors` (only Fake auto-error and Uring do); C2d asserts the
  terminal ERROR CODE and exactly-once completion via reap counts/state, not
  the unwired counter. Consequently the terminal-vs-cancel evidence claims **at
  most one tally** (`canceled_ops == 1` iff canceled; the injected
  `backend_error` terminal contributes no tally) — never "exactly one tally in
  every interleaving". Wiring `completion_errors` for ThreadPool would be a
  stats-semantics production change beyond C2d's test-scope mandate and is
  recorded here as a deliberate non-goal.

## 11. Phase status

- Phase C remains **PARTIAL** (C1 IMPLEMENTED; C2a COMPLETE; C2b COMPLETE; C2c
  COMPLETE; C2d COMPLETE; C2e pending).
- **C2d: COMPLETE** — rows 9–10 have real-backend runtime evidence
  (ThreadPool: ADR Gate-4 per-stage pre-commit injection at reserve / prepare /
  commit-boundary — the commit-boundary arm being the ONLY executable instance
  of `rollback_binding_before_accept` — plus transactional rejection, partial
  worker-startup cleanup, post-commit dispatch-failure defined terminal on
  size + void paths, post-commit no-allocation on the real worker path and the
  injected path, deterministic cancel-wins before enqueue (ADR Gate 4),
  exactly-one winner vs cancel in the racy interleaving) and reference-path
  evidence (Fake defined-error terminal under a full-window always-throw
  probe), each detector proven mutation-RED (M1–M13), with Uring's Phase-D gap
  authoritatively recorded.
- Production change (guarded only): the C2d failure-injection seams in
  `include/sluice/async/threadpool_backend.hpp` / `src/async/threadpool_backend.cpp`
  compile out of production builds (verified by symbol inspection); no other production behavior change; no synchronous
  Reader/Writer change; no Phase D Uring implementation; no C2e/Phase F/Phase G
  scope creep.
