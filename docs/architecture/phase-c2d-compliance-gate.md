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
nonconforming code (mutants M1–M9).

---

## 1. Scope

| Requirement (Issue #68 row) | Evidence |
|---|---|
| 9 — failure injection (alloc / worker-spawn / dispatch failure surfaces a defined terminal, leaves no partial state) | `tp_c2d_cas_loss_rejection_zero_side_effects`, `tp_c2d_partial_worker_startup_failure`, `tp_c2d_dispatch_failure_injection_size_op`, `tp_c2d_dispatch_failure_injection_void_op`, `tp_c2d_dispatch_failure_races_cancel_exactly_one`, `tp_c2d_cancel_after_dispatch_failure_terminal_no_overwrite` (ThreadPool); `fake_full_window_alloc_failure_defined_error_terminal` + existing `fake_cas_loss_rejection_zero_side_effects` (Fake) |
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
  (enqueue pin / reap eligibility).
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
lost binding CAS additionally runs `rollback_binding_before_accept`). The
rejected resources are immediately reusable by a fresh submit.

Evidence: `tp_c2d_cas_loss_rejection_zero_side_effects` drives the REAL
ThreadPool binding-CAS loss (submit into a non-idle Completion with a free
slot): `invalid_state`, both original Completions untouched, `slot_in_use` and
`outstanding` unchanged at 2, dispatch holds exactly the two real ops,
`backend_ready_count` is 0 (no ghost), the two accepted ops reach exactly one
terminal each with the real result, and the capacity is immediately recycled
by a third submit. (Reserve-full `would_block` and admission-closed
`invalid_state` were already proven for ThreadPool in Phase E / C2a —
`phase_e_capacity_full_returns_would_block` and the shared capacity suite — and
are not duplicated here; the C2d gap was the *real-backend transactional
residue* proof, which this case closes.)

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
(`threadpool_dispatch_queue_invariant_fail_fast`, Debug AND Release); the
injection is a separate `SLUICE_ASYNC_INTERNAL_TESTING`-guarded branch that is
compiled out of production builds (verified: production `libsluice_async.a`
contains no injection symbol; the internal-testing library contains them).

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
reaches the work domain first wins. BOTH legal outcomes (cancel wins →
`canceled` terminal; injection wins → `backend_error` terminal) are covered by
`tp_c2d_dispatch_failure_races_cancel_exactly_one`, which asserts the
INVARIANT: exactly one terminal, exactly one publication, no worker/syscall
execution, and exactly one tally (`canceled_ops == 1` iff the result is
`canceled`). `tp_c2d_cancel_after_dispatch_failure_terminal_no_overwrite`
proves a cancel after the injected terminal is a pure no-op (no overwrite, no
second tally, no double ready-ring push — reap publishes exactly once).

## 4. Test-only seams (all `SLUICE_ASYNC_INTERNAL_TESTING`-guarded)

1. **`ThreadPoolBackend::DispatchFailureInjection`** — post-commit
   dispatch-failure control (`armed` / `fired`). The control object must be
   declared before the backend and outlive it (same lifetime rule as the
   pause gates). The production build carries no member, no branch, no symbol
   (verified via `nm` on `libsluice_async.a`).
2. **`ThreadPoolBackend::set_injected_worker_spawn_failure_index`** — static
   construction-time spawn-failure seam (value = zero-based worker index to
   fail; `SIZE_MAX` disarms). A static seam is REQUIRED because the injection
   point is the constructor, which runs before any instance exists
   (constructor-before-instance); the tests guarantee serial isolation (only
   the constructing thread reads it while armed; the harness runs cases
   sequentially in one process) and restore the disarmed sentinel via RAII
   (`ScopedSpawnFailureSeam`) even on failure. Compiled out of production
   builds (verified via `nm`).

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
| `tp_c2d_dispatch_failure_races_cancel_exactly_one` | threadpool_backend_c2d_failure_test | PASS |
| `tp_c2d_cancel_after_dispatch_failure_terminal_no_overwrite` | threadpool_backend_c2d_failure_test | PASS |
| `fake_full_window_alloc_failure_defined_error_terminal` | reference_backend_no_alloc_test | PASS |
| Python: `test_backend_conformance_manifest.py` (136 cases, incl. 16 C2d) | unittest | PASS |

## 6. Fake / ThreadPool eligibility, Uring known gap

- **Fake = ELIGIBLE** — `c2d_fake_failure_injection_terminal=PASS` in the
  per-backend report (defined-error terminal under a full-window always-throw
  probe, plus the existing CAS-loss/would_block no-allocation cases).
- **ThreadPool = ELIGIBLE** — `c2d_threadpool_failure_injection=PASS` (all 8
  cases, real backend, deterministic seams).
- **Uring = NOT CONFORMING** — `uring_c2d_failure_injection_not_implemented=
  INCOMPLETE (not_implemented)` is the authoritative C2d gap record, surfaced
  in the lifecycle layer AND in the verdict reasons (alongside the C2a/C2b/C2c
  gaps). Uring's own `uring_submit_failure_test` drives the pre-RequestArena
  SQE model and does NOT satisfy the C2d contract; Uring stays NOT CONFORMING
  until Phase D.

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

Every mutant ran RED (non-zero exit) and GREEN after restore; a marker scan
confirmed zero `MUTANT` residue.

## 8. Commands run (validation) — see section 9 for the full matrix

| Gate | Command | Result |
|---|---|---|
| Focused ThreadPool | `xmake build threadpool_backend_c2d_failure_test && xmake run threadpool_backend_c2d_failure_test` | PASS (8 cases) |
| Focused Fake | `xmake build reference_backend_no_alloc_test && xmake run reference_backend_no_alloc_test` | PASS (5 cases) |
| Stability | 10× repeated runs of `threadpool_backend_c2d_failure_test` | PASS (10/10, no flake) |
| Manifest self-test | `python3 scripts/tests/test_backend_conformance_manifest.py` | PASS (136 cases) |
| Aggregate gate | `python3 scripts/verify-backend-conformance.py` | PASS (Fake/TP ELIGIBLE with C2d records PASS; Uring NOT CONFORMING with the C2d gap in its reasons) |
| RED validity | 9 mutations (M1–M9), focused filtered runs | all RED; all restored GREEN (see §7 + mutation ledger) |

> **Gate-run note (honesty):** the FIRST aggregate-gate run was executed
> concurrently with the full `xmake test -v` suite and reported one isolated
> RUN_FAIL for the pre-existing `threadpool_scheme_b_race` evidence under CPU
> contention; the full Debug suite itself passed 147/147 in the same window,
> and the gate passed on the immediate clean rerun (recorded above). The flake
> is under-load contention in an existing race case, not a C2d regression; the
> rerun result is the gate's authoritative outcome.
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

## 9. Validation matrix (full evidence)

All rows below were executed on the current branch head. `PASS` is recorded
only for commands that actually ran green.

| Gate | Command | Result |
| ---- | ------- | ------ |
| Debug / Clang full | `xmake f -m debug --toolchain=clang -y && xmake build -g test && xmake test -v` | PASS (147 targets, 0 failed) |
| Focused ThreadPool | `xmake run threadpool_backend_c2d_failure_test` | PASS (8 cases) |
| Focused Fake | `xmake run reference_backend_no_alloc_test` | PASS (5 cases) |
| Stability | 10× repeated runs of the C2d ThreadPool target | PASS (10/10) |
| Release / Clang | `xmake f -m release --toolchain=clang -y && xmake build -g test && xmake test -v` | PASS (147 targets, 0 failed; re-run after the test-hygiene fix) |
| ASan/UBSan | `xmake f -m asanubsan --toolchain=clang -y && xmake build -g test && xmake run -g test` | PASS (exit 0; zero ASan/UBSan reports; re-run after the test-hygiene fix — the initial run's racy-assertion abort is documented in section 8) |
| TSan | `xmake f -m tsan --toolchain=clang -y && xmake build -g test && xmake run -g test` | PASS (exit 0; zero race reports, incl. the dispatch-failure-vs-cancel and worker-vs-submit races) |
| Manifest self-test | `python3 scripts/tests/test_backend_conformance_manifest.py` | PASS (136 cases, incl. 16 C2d) |
| Aggregate gate | `python3 scripts/verify-backend-conformance.py` | PASS (Fake/TP ELIGIBLE; Uring NOT CONFORMING with the C2d gap) |
| Doc links | `python3 scripts/check-doc-links.py --self-test` + `python3 scripts/check-doc-links.py` | PASS (no broken links, no stale paths) |
| Architecture docs | `python3 scripts/verify-architecture-docs.py` | PASS |
| Negative-compile | `scripts/verify-completion-authority-negative-compile.sh` | PASS |
| Negative-compile | `scripts/verify-request-arena-negative-compile.sh` | PASS |
| Negative-compile | `scripts/verify-async-identity-negative-compile.sh` | PASS |
| Negative-compile | `scripts/verify-external-backend-authority-negative-compile.sh` | PASS |
| Seam absence | `nm -C libsluice_async.a | grep -i injected_worker_spawn\|dispatch_failure_injection` | PASS (no match); internal-testing lib has them |
| Diff hygiene | `git diff --check` | PASS (clean) |

## 10. Remaining gaps

- **C2e** — close/drain/reset (row 15; row 16 already FULL): PENDING, not closed.
- **Rows 4b/12b/14b** — Phase F scope (unchanged).
- **Phase D** — Uring RequestArena migration: PENDING; Uring C2d conformance is
  the `uring_c2d_failure_injection_not_implemented` record, never skip-as-pass.
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
  the unwired counter. Wiring `completion_errors` for ThreadPool would be a
  stats-semantics production change beyond C2d's test-scope mandate and is
  recorded here as a deliberate non-goal.

## 11. Phase status

- Phase C remains **PARTIAL** (C1 IMPLEMENTED; C2a COMPLETE; C2b COMPLETE; C2c
  COMPLETE; C2d COMPLETE; C2e pending).
- **C2d: COMPLETE** — rows 9–10 have real-backend runtime evidence
  (ThreadPool: transactional rejection, partial worker-startup cleanup,
  post-commit dispatch-failure defined terminal on size + void paths,
  post-commit no-allocation on the real worker path and the injected path,
  exactly-one winner vs cancel) and reference-path evidence (Fake defined-error
  terminal under a full-window always-throw probe), each detector proven
  mutation-RED, with Uring's Phase-D gap authoritatively recorded.
- Production change (guarded only): the C2d failure-injection seams in
  `include/sluice/async/threadpool_backend.hpp` / `src/async/threadpool_backend.cpp`
  compile out of production builds (verified by symbol inspection); no other production behavior change; no synchronous
  Reader/Writer change; no Phase D Uring implementation; no C2e/Phase F/Phase G
  scope creep.
