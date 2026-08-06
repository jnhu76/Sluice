# Phase C2a Compliance Gate — Shared Capacity / Admission / Rejection / Accounting Conformance

**Roadmap:** [`remediation-roadmap.md`](remediation-roadmap.md) — Phase C (status PARTIAL; C2a COMPLETE)
**Governing ADR:** [`ADR-explicit-io-request-contract`](../adr/ADR-explicit-io-request-contract.md) (Accepted) — Decisions 5, 6, 13; invariants I3, I8
**Issue #68:** https://github.com/jnhu76/Sluice/issues/68 — Revision 3 (PLAN READY), C2a scope (rows 1–2)
**Branch:** test/phase-c2-capacity-admission-rejection (see the PR)
**Scope:** Tests + gate scripts + docs only. **No `src/` or `include/sluice/` production change.**

This is the PR-level evidence ledger for Phase C2a, the first C2 semantic-coverage slice: the
shared capacity/admission/rejection/accounting conformance evidence. C2a closes rows 1–2 of the
C2 requirement-to-evidence matrix (Issue #68 CORRECTION 1) by adding a SHARED-OBSERVABLE capacity
suite that runs identically against Fake and ThreadPool, wiring it into the aggregate gate
per-backend, recording Uring's Phase-D capacity gap as a `not_implemented` manifest record, and
proving the cases catch deliberately nonconforming capacity behavior.

---

## 1. Scope

| Requirement (Issue #68 row) | Evidence |
|---|---|
| 1 — bounded backend accepts exactly `capacity`; (N+1)th rejects with `would_block`; high-water ≤ capacity; capacity-vs-invalid split | `capacity_accepts_exact_limit`, `capacity_rejects_with_idle_completion`, `capacity_stats_are_exact` (shared, Fake + ThreadPool) |
| 2 — rejected-vs-accepted isolation (rejected Completion stays idle; `submitted_ops` counts committed only; no async from a reject) | `capacity_rejects_with_idle_completion`, `capacity_rejection_never_completes`, `capacity_stats_are_exact` (shared, Fake + ThreadPool) |

Explicitly **out of scope** (later slices): C2b generation/stale/cancel matrix, C2c
waiter/borrow/delivery lease, C2d failure injection, C2e close/drain/destruction, and the entire
Phase D Uring RequestArena migration.

## 2. Authority

- **Issue #68 Revision 3 (PLAN READY)** — the C2a design authority: the capacity seam
  (CORRECTION 2), the shared-observable boundary + `CapacityFixture` cleanup model
  (CORRECTIONS 3a/3b, 7a/7b), the manifest/gate model (CORRECTION 4, no driver-side marker per
  7c), exact stats assertions (CORRECTION 5), and the isolated capacity runner + validity
  fixture (CORRECTION 6).
- **ADR-explicit-io-request-contract (Accepted)** — Decisions 5 (five-stage admission), 6 (error
  vocabulary: `would_block` vs `invalid_state`), 13 (bounded capacity + observability); invariants
  I3 (transactional rejection) and I8 (bounded admission).
- **AGENTS.md** §16.1 (test-header change), §16.6 (gate change), §18 (conformance philosophy).
- **Architecture Constitution** — AC-4 (accepted terminality), AC-7 (bounded resources),
  AC-12/AC-13 (admission/lifecycle observability).

## 3. What C2a produces

### 3.1 Capacity-aware factory seam (tests/backend_conformance.hpp)

`BackendFactory` gains an OPTIONAL `make_backend_with_capacity` (`std::function<unique_ptr<AsyncBackend>(size_t)>`)
alongside the preserved zero-arg `make_backend`. `factory_supports_capacity(f)` reports whether a
backend can be constructed at a chosen capacity. Driver wiring:
`Fake -> FakeAsyncBackend(cap)`, `ThreadPool -> ThreadPoolBackend({cap, worker_count=1})`,
`Uring -> nullptr` (no arena before Phase D). The seam is test-header-only; it is not a production
API and there is no fake `request_capacity` field pretending to be construction capability.

### 3.2 Shared capacity cases (tests/backend_conformance_test.cpp)

`run_capacity_cases(factory)` drives FIVE cases; it returns the empty string on full pass or the
**stable name of the FIRST failing case**. Cases assert ONLY `AsyncIoContext`-observable state
(submit/cancel/poll/wait_one/outstanding/stats + Completion public state/reset) — no downcast, no
`complete_*`, no `arena_*`/`dispatch_size_for_test` (those stay in the Phase B/E mechanism tests).

| Case | cap | What it pins |
|---|---|---|
| `capacity_accepts_exact_limit` | 2 | accepts exactly 2; `outstanding()==2`; `submit_calls==2`; `submitted_ops==2`; `max_outstanding==2`; `queue_full_retries==0`; `invalid_state_rejections==0` |
| `capacity_rejects_with_idle_completion` | 2 | 3rd submit → `would_block`; c3 `idle()`/not-`outstanding()`/not-`ready()`; `outstanding()==2`; `submit_calls==3`; `submitted_ops==2`; `queue_full_retries==1`; `invalid_state_rejections==0`; `max_outstanding==2` |
| `capacity_rejection_never_completes` | 1 | after driving progress + cancel→reap of the accepted op, the rejected c2 stays idle throughout and never produces a completion; `submitted_ops==1` |
| `capacity_stats_are_exact` | 2 | deterministic sequence (accept; resubmit-on-non-idle → `invalid_state`; accept; → `would_block`) with EXACT stats: `submit_calls==4`, `submitted_ops==2`, `invalid_state_rejections==1`, `queue_full_retries==1`, `max_outstanding==2`, `outstanding==2`, `canceled_ops==0`, `completion_errors==0` (no `>= 1` — CORRECTION 5) |
| `capacity_recycles_after_reset` | 1 | accepted op cancel→reap→reset, then a fresh submit succeeds (capacity recycled) |

### 3.3 CapacityFixture cleanup model (CORRECTIONS 3a/7a)

Cleanup is an **explicit method `cleanup_or_abort()`**, NOT a destructor and NOT built on
`SLUICE_CHECK` (which expands to `record_failure(); return;` — returning out of a destructor lets
the `AsyncIoContext` member destruct and fire its own L11 fail-fast, masking the real cause).
It is time-bounded with a real 10-second deadline; on expiry it prints a precise diagnostic
(backend, case, `outstanding`, per-Completion state, stats) and `std::abort()`s so the failure
cause is the capacity case, not a context-destructor violation.

- `submit_and_track()` registers a Completion into `accepted` BEFORE the case inspects the result,
  so a wrongly-accepted (N+1)th op is still cleaned up even if the case throws right after
  (CORRECTION 7b).
- Each case is a self-contained function whose `CapacityFixture` + `Completions` live in the SAME
  frame; `run_capacity_case()` runs `cleanup_or_abort()` on BOTH the success and exception paths
  while that frame is alive. (The Completions must NOT live inside the wrapper's try block: a
  `case_bail` exception would destruct the outstanding Completions — fail-fast — before the catch
  could run cleanup. C++ stack-unwinding order makes the wrapper-in-the-outer-frame the only
  scope-correct shape.)
- `Fake` and `ThreadPool` run the same cases. Fake's cancel wins under Scheme B; ThreadPool's
  cancel may yield the real syscall result — either is a valid terminal, and C2a does not care
  WHICH terminal wins, only that accepted ops reach exactly one and rejected Completions stay idle.

### 3.4 Manifest / gate model (CORRECTION 4)

`scripts/backend_conformance_manifest.py`:
- `implemented_evidence_for_backend(name)` — only implemented records (target selection,
  command execution, mandatory implemented-coverage).
- `applicable_evidence_for_backend(name)` — implemented + `not_implemented` + `not_applicable`
  (verdict, report, known-gap display). `evidence_for_backend` is kept as an alias for the
  implemented helper.
- New `shared_capacity_suite` evidence (implemented, mandatory, layer `shared`,
  backends Fake+ThreadPool) and `uring_capacity_not_implemented` (STATUS_NOT_IMPLEMENTED,
  mandatory, layer `shared`, backends Uring).

`scripts/verify-backend-conformance.py`:
- `_backend_verdict` iterates APPLICABLE mandatory evidence, so a `not_implemented` mandatory
  record forces INCOMPLETE in the backend's OWN verdict (not just a global results dict).
- `_run_capacity_suite` drives each capacity-capable backend's capacity driver case
  (`conformance_capacity_fake` / `conformance_capacity_threadpool`) in its own isolated
  subprocess; `_classify_shared_run` gained `expected_case` so the capacity run is classified
  against the capacity driver case name (fail-closed, same shape as the shared suite).
- The per-backend report lists applicable evidence per layer, so Uring renders:
  `shared: shared_suite=PASS (stub subset) / uring_capacity_not_implemented=INCOMPLETE
  (not_implemented)`.

There is NO driver-side `[conformance-incomplete]` marker protocol (CORRECTION 7c): the manifest
record is the SOLE authoritative capacity-gap surface, and Uring is never skip-as-pass.

### 3.5 Validity evidence (CORRECTION 6; tests/capacity_validity_test.cpp)

`NonConformingCapacityBackend` — a MINIMAL AsyncBackend (no RequestArena; a validity fixture, not
a lifecycle-conforming backend) that misbehaves ONLY on capacity accounting. Guarded by
`SLUICE_ASYNC_INTERNAL_TESTING`; NOT registered in the conformance manifest; never affects any
normal backend verdict. Each injected violation makes `run_capacity_cases()` return the SPECIFIC
failing case name:

| Violation | Failing case (asserted) |
|---|---|
| `over_accept` — accepts the (N+1)th op | `capacity_rejects_with_idle_completion` |
| `bind_rejected` — claims the rejected Completion | `capacity_rejects_with_idle_completion` |
| `late_complete` — completes a rejected op on progress | `capacity_rejection_never_completes` |
| `misclassify_invalid` — non-idle submit returns `would_block` | `capacity_stats_are_exact` |
| `inflate_outstanding` — `outstanding()` over-reports +1 | `capacity_accepts_exact_limit` |
| `no_recycle` — capacity never recycles after reap | `capacity_recycles_after_reset` |
| `none` (control) | all five cases PASS |

The claimed Completion is published through the protected two-stage binding WITHOUT an arena
release capability, so `reset()` of a published Completion is safe (probe-driven Completion).

## 4. Test case ledger (issue #68 C2a cases)

| Case (SLUICE_TEST_CASE / capacity case) | Target | Status |
|---|---|---|
| `conformance_fake` (base 8-case suite) | backend_conformance_test | PASS (unchanged) |
| `conformance_threadpool` (base 8-case suite) | backend_conformance_test | PASS (unchanged) |
| `conformance_uring` (base stub subset) | backend_conformance_test | PASS (stub subset) |
| `conformance_capacity_fake` → `run_capacity_cases` (5 cases) | backend_conformance_test | PASS |
| `conformance_capacity_threadpool` → `run_capacity_cases` (5 cases) | backend_conformance_test | PASS |
| `capacity_validity_over_accept` | capacity_validity_test | PASS |
| `capacity_validity_bind_rejected` | capacity_validity_test | PASS |
| `capacity_validity_late_complete` | capacity_validity_test | PASS |
| `capacity_validity_late_bind_only` (PR #69: pre-submit tracking pin) | capacity_validity_test | PASS |
| `capacity_validity_misclassify_invalid` | capacity_validity_test | PASS |
| `capacity_validity_inflate_outstanding` | capacity_validity_test | PASS |
| `capacity_validity_no_recycle` | capacity_validity_test | PASS |
| `capacity_validity_conforming_backend_passes` | capacity_validity_test | PASS |
| `capacity_regression_bound_over_accept_cleanup` (PR #69 A) | capacity_validity_test | PASS |
| `capacity_regression_bound_but_error_cleanup` (PR #69 B, fixture-level tracking assert) | capacity_validity_test | PASS |
| `capacity_regression_catch_all_exception_cleanup` (PR #69 C) | capacity_validity_test | PASS |
| Python: `test_backend_conformance_manifest.py` (86 cases, incl. 26 C2a/PR#69) | unittest | PASS |

## 5. Fake / ThreadPool eligibility, Uring known gap

- **Fake = ELIGIBLE** — `shared_capacity_suite=PASS` in the per-backend report.
- **ThreadPool = ELIGIBLE** — `shared_capacity_suite=PASS`.
- **Uring = NOT CONFORMING** — `shared_suite=PASS (stub subset)`; capacity coverage is the
  `not_implemented` manifest record, surfaced in the shared layer AND in the verdict reasons.
  Uring stays NOT CONFORMING until Phase D (RequestArena migration). This record reinforces
  (does not replace) the existing KernelIoProfile-stays-NOT-CONFORMING rule.

## 6. Shared observable vs backend mechanism evidence

C2a adds the SHARED-OBSERVABLE side (what a caller sees through `AsyncIoContext`). The
MECHANISM side (`arena_capacity_rejections()`, `arena_slot_in_use()`, `dispatch_size_for_test()`,
worker counts) remains in the existing Phase B/E tests (`reference_backend_arena_lifecycle_test`,
`request_arena_test`, `threadpool_backend_phase_e_test`, `threadpool_backend_reap_test`) — C2a
does not duplicate them.

## 7. Commands run (validation)

| Gate | Command | Result |
|---|---|---|
| Debug build | `xmake f -m debug --toolchain=clang -y` | PASS |
| Focused conformance | `xmake build backend_conformance_test && xmake run backend_conformance_test` | PASS (10 driver cases) |
| Focused validity | `xmake build capacity_validity_test && xmake run capacity_validity_test` | PASS (11 cases) |
| Manifest self-test | `python3 scripts/tests/test_backend_conformance_manifest.py` | PASS (86 cases) |
| Aggregate gate | `python3 scripts/verify-backend-conformance.py` | PASS (Fake/TP ELIGIBLE; Uring NOT CONFORMING with capacity gap) |
| Full test group | `xmake test -v` | see section 8 |
| Release | `xmake f -m release --toolchain=clang -y` | see section 8 |
| ASan/UBSan | `xmake f -m asanubsan --toolchain=clang -y` | see section 8 |
| TSan | `xmake f -m tsan --toolchain=clang -y` | see section 8 |

## 8. Validation matrix (full evidence)

All rows below were executed on the current branch head (`b4299cf`). `PASS` is recorded only for
commands that actually ran green. The PR #69 review-fix iteration (tracked cleanup, bind_rejected
without self-cleanup, pre-submit registration, late_bind_only mutant, cleanup re-cancel) landed in
the same head; the row counts reflect the post-fix binaries.

| Gate | Command | Result |
| ---- | ------- | ------ |
| Debug / Clang full | `xmake f -m debug --toolchain=clang -y && xmake build -g test && xmake test -v` | PASS (143 targets, 0 failed) |
| Focused conformance | `xmake run backend_conformance_test` | PASS (10 driver cases) |
| Focused validity | `xmake run capacity_validity_test` | PASS (11 cases) |
| Release / Clang | `xmake f -m release --toolchain=clang -y && xmake build -g test && xmake test -v` | PASS (143 targets, 0 failed) |
| ASan/UBSan | `xmake f -m asanubsan --toolchain=clang -y && xmake build -g test && xmake run -g test` | PASS (exit 0; validity-backend lifetime fix landed in 965ae6f) |
| TSan | `xmake f -m tsan --toolchain=clang -y && xmake build -g test && xmake run -g test` | PASS (exit 0; 134 targets, zero race reports) |
| Manifest self-test | `python3 scripts/tests/test_backend_conformance_manifest.py` | PASS (86) |
| Aggregate gate | `python3 scripts/verify-backend-conformance.py` | PASS (Fake/TP ELIGIBLE; Uring NOT CONFORMING) |
| Doc links | `python3 scripts/check-doc-links.py` | PASS (after branch-name link fix) |
| Architecture docs | `python3 scripts/verify-architecture-docs.py` | PASS |
| Negative-compile | `scripts/verify-completion-authority-negative-compile.sh` | PASS (12 cases) |
| Negative-compile | `scripts/verify-request-arena-negative-compile.sh` | PASS (6 cases) |
| Negative-compile | `scripts/verify-async-identity-negative-compile.sh` | PASS (3 cases) |
| Negative-compile | `scripts/verify-external-backend-authority-negative-compile.sh` | PASS (2 cases) |
| Diff hygiene | `git diff --check` | PASS |

## 9. Remaining gaps

- **C2b** — generation/provenance/stale-key/cancel-winner matrix (rows 3–8): PARTIAL, not closed.
- **C2c** — waiter/borrow/delivery lease (rows 11–14): PARTIAL, not closed.
- **C2d** — failure injection + post-commit allocator terminal (rows 9–10): MISSING, not closed.
- **C2e** — close/drain/reset (row 15; row 16 already FULL): PARTIAL, not closed.
- **Phase D** — Uring RequestArena migration: PENDING; Uring capacity conformance is the
  `uring_capacity_not_implemented` record, never skip-as-pass.

## 10. Phase status

- Phase C remains **PARTIAL** (C1 IMPLEMENTED; C2a COMPLETE; C2b–C2e pending).
- **C2a: COMPLETE** — rows 1–2 of the C2 matrix have shared-observable, per-backend, validity-
  proven evidence for Fake and ThreadPool; Uring's gap is authoritatively recorded.
- No `src/` or `include/sluice/` change; no synchronous Reader/Writer behavior change; no Phase D
  Uring implementation; no C2b–C2e scope creep.
