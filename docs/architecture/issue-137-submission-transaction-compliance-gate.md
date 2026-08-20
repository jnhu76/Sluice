# Issue #137 Implementation Compliance Gate — Shared Submission Transaction

Phase-specific gate for the #137 implementation (centralized pre-accept
submission transaction). Covers every Gate 0–4 field of
[design-compliance-gate.md](design-compliance-gate.md) and links to it as the
generic authority. Design: [issue-137-submission-transaction-design.md](issue-137-submission-transaction-design.md)
(PR #157, merged). Independent review: issue #137 comment
`issuecomment-5357295925` — verdict **ACCEPTED** with five binding corrections
(schematic commit-branch inversion, honest hook budget, `stage0_precheck`
precondition, binding-trio protected-access routing, recorded noexcept delta).
This gate records the implementation's compliance; evidence fields are
`PENDING` until the exact commands have actually run (gate rule: PASS is
never pre-filled).

---

## Gate 0 — Architecture Classification

```text
Affected capability:    Backend (all four in-repo explicit-I/O backends' submit paths)
Affected layer:         L0 backend (+ one new installed detail header)
Classification:         Corrective (collapses 8 replicated correctness-ladder
                        authorities into 1; behavior-preserving by design)
Governing ADR:          ADR-explicit-io-request-contract (Accepted; unchanged —
                        this is the ADR's own five-stage transaction, written once)
Conformance map change: no (no row changes; the same contract, one implementation)
Constitution rules:     AC-4 (authority separation preserved — no fifth domain),
                        AC-6 (no wake change), AC-7 (no resource change)
```

Zig conformance: not applicable (no Zig target touches the async submission
transaction; `zig/` is a design reference only).

## Gate 1 — Ownership and State Machine

**What changes:** the OWNER of the pre-accept ladder sequence (stages 0–3c +
rollback) moves from each backend's `submit_size`/`submit_void` (8 copies)
to ONE function `detail::submit_transaction` (installed detail header).
**What does NOT change:** every transition, its authority, lock domain,
allocation, failure behavior, wake obligation, and shutdown behavior — the
ladder performs byte-identical `RequestArena`/`Completion` operations in the
identical order per backend.

State machine (the request slot lifecycle itself — UNCHANGED, owner
`RequestArena`):

```text
free -> reserved -> prepared -> pending -> enqueued/... (unchanged; see
async-request-lifecycle.md §3 for the full canonical machine)

Ladder-driven transitions (now issued by the ONE shared function, under the
caller backend's admission discipline):
  reserve:    Authority: RequestArena (unchanged). Lock: arena mutex_ (leaf).
              Allocation: none. Failure: would_block/invalid_state -> submit error.
              Wake: none. Shutdown: admission_closed_ at reserve (unchanged).
  prepare:    Authority: RequestArena. Lock: arena leaf. Allocation: none.
              Failure: rollback_reserved_or_prepared -> submit error. Wake: none.
  install_publication_binding: same shape as prepare (arena leaf, no alloc,
              rollback on failure).
  begin_binding (Completion CAS): Authority: Completion (caller-owned).
              Atomic CAS; loser rolls back ONLY its own slot.
  commit:     Authority: RequestArena (slot half). Lock: arena leaf.
              Allocation: none. Failure: rollback_binding_before_accept FIRST,
              then slot rollback. Wake: none. Shutdown: none (in-flight
              submission completes; Decision 15).
  commit_binding (accept LP): Authority: Completion. Release-store, cannot
              fail; after it NO failure representation exists in the ladder.
Enqueue/dispatch (stage 4+): UNCHANGED, backend-owned (arena enqueue no-op /
work ring + cv / dispatch ring + SQE drain).
```

Lock/atomic authority table (UNCHANGED domains; the shared function acquires
no lock, wakes nothing, allocates nothing):

```text
context access_mtx_  : AsyncIoContext (Sync's external admission serialization)
admission_mtx_       : Fake / ThreadPool admission transaction domain
dispatch_mtx_        : Uring admission+dispatch domain (Stage-0 poison read in-lock)
arena mutex_         : RequestArena leaf (every ladder arena call)
Completion state     : caller-owned, atomic CAS + release-store
```

## Gate 2 — Resource and Failure Model

```text
Construction-time resources: NONE added. (New header only; no type, field,
  mutex, queue, or allocation introduced. Policies are stack-constructed
  parameter bundles.)
Submit-time resources: identical to today per backend:
  arena slots (request_capacity), prepared-op scratch (construction-bounded
  arrays), dispatch ring (capacity == request_capacity). Submit is
  allocation-free at every stage, including after acceptance: YES (unchanged).
Capacity/backpressure: unchanged (would_block before acceptance; poison
  verbatim for Uring Stage-0).
Reclamation: unchanged (bounded by outstanding, never by historical total).
Failure matrix: see the design doc §5 — every pre-LP row is zero-residue via
  the ONE ladder; every post-LP row is terminal-path only. Recorded delta:
  the noexcept function boundary converts a theoretical pre-LP exception
  (std::system_error from arena mutex acquisition) from propagation into
  fail-fast termination (review condition 5; consistent with the no-throw
  path contract I9 / failure-model).
```

## Gate 3 — Progress and Wake Model

**UNCHANGED by construction.** The shared function contains zero lock
acquisitions, zero Scheduler calls, zero wait-source operations, zero
backend-queue operations. Wake obligations stay where they are today:
backend `enqueue_after_commit` (work cv / SQE drain), `close_admission`
interrupts, Scheduler routing. No polling dependency is introduced or
removed. Single-worker liveness: unchanged.

## Gate 4 — Evidence Plan

```text
Deterministic causal tests:
  - C2e pause-seam cases (fake_c2e_*, tp_c2e_*): prove the commit-vs-LP and
    close-wins windows still pause at the SAME ladder positions (S2/S3).
  - D3 C2b gate case (uring enqueued-cancel window): proves the post-LP
    pre-enqueue seam still lands between LP and enqueue (S4).

Backend conformance (unchanged suites, per stage):
  - S1: sync backend conformance + request_lifecycle_scheme_b_test.
  - S2: fake conformance + backend_scheme_b_race_test.
  - S3: full ThreadPool conformance + threadpool_backend_scheme_b_race_test
    (C2d injections at reserve/prepare/commit) + TSan race classes.
  - S4: uring D1/D2/D4 suites, stub AND real-liburing paths.

Resource-bound tests: capacity/backpressure suites (unchanged; run per stage).

Mutation evidence (137 hard requirement; plant/prove-red/revert/empty-diff):
  - M1 delete one ladder rollback            -> conformance/regression FAIL
  - M2 move the acceptance LP before commit  -> conformance/mutation FAIL
  - M3 CAS loser skips its own-slot rollback -> scheme-B/C2b test FAIL
  - M4 Uring poison precedence swapped       -> D4-M5 poison test FAIL
  - DIV C2b–C2e probes re-pointed at the shared ladder (one mutation site
    now constrains all backends) — spot-revived per the phase-c2b evidence
    method.

Shutdown race tests: C2e close-wins/close-waits cases (S2/S3), D4
close-vs-submit (S4) — unchanged.

Sanitizers:
  - [x] ASan+UBSan full suite        PASS (189/189 tests passed)
  - [x] TSan full suite              PASS (189/189 tests passed)

Validation matrix (all PASS):
  - [x] Debug full suite             PASS (189/189 tests passed, real-liburing)
  - [x] Release full suite           PASS (189/189 tests passed, real-liburing)
  - [x] real-liburing focused suites PASS (phase_g_closeout_uring_test,
        uring_backend_test, uring_backend_c2b_identity_test,
        uring_backend_c2e_death_test, uring_backend_c2e_close_drain_test,
        uring_submit_failure_test, uring_d2_failure_noalloc_test,
        uring_backend_death_test, uring_stats_test, uring_io_context_test,
        uring_f1_scheduler_routing_test, backend_conformance_test,
        reference_backend_arena_lifecycle_test, request_lifecycle_scheme_b_test,
        threadpool_backend_c2e_close_drain_test, failure_model_high_risk_test,
        failure_model_high_risk_death_test, external_backend_admission_test,
        fake_backend_test — ALL TESTS PASSED)
  - [x] mechanical-facts             PASS (near-miss, LOC, split layout, SHA refs,
        tracker refs, test totals, seam production exclusion)
  - [x] assert-hygiene               PASS (changed-lines scan, no new assert sites
        introduced by migration)
  - [x] verify-architecture-docs     PASS (all architecture documentation checks)
  - [x] check-doc-links              PASS (no broken links, no stale paths)
  - [x] pre-push gate                PASS (all gates)
  - [x] formal manifest check        PASS (no model edit required — no arena
        transition/admission/queue/wake rule changes; manifest.json binding
        lists re-checked, no divergence)

Mutation evidence (M1–M4 all proven RED/reverted/GREEN):
  - [x] M1 delete prepare-failure rollback → terminate (slot leak detected by
        arena invariant guard), reverted, GREEN
  - [x] M2 move LP before commit → terminate (rollback_binding on outstanding
        Completion is fail-fast contract violation), reverted, GREEN
  - [x] M3 CAS loser skips own-slot rollback → terminate (slot leak detected
        by arena invariant guard), reverted, GREEN
  - [x] M4 Uring poison precedence swapped → harness fail (expected
        backend_error, got invalid_state — D4-M5 violation), reverted, GREEN
  - [x] Post-revert full suite → 189/189 PASSED (Debug, real-liburing)
  Evidence doc: [issue-137-submission-transaction-mutation-evidence.md](issue-137-submission-transaction-mutation-evidence.md)

DIV re-point: the existing C2b–C2e mutation probes from
phase-c2b-identity-mutation-evidence.md now constrain the shared ladder at a
single site (one mutation affects all four backends) — no new probes required;
the existing TP C2d injection seam (reserve/prepare/commit) and Uring Stage-0
hook are the single points of failure for the entire authority chain.

#144 on-touch rule: every production file touched by this change
(sync_backend.hpp, fake_backend.hpp, threadpool_backend.{hpp,cpp},
uring_backend.{hpp,cpp}) has been re-checked for legacy failure-model sites:
  - No new assert() calls introduced (assert-hygiene PASS)
  - No new throw expressions (Result/IoError discipline preserved)
  - No new exception-based control flow
  - All rollback paths use the ONE shared ladder (zero-residue by construction)
  - All post-LP paths are terminal-only (no rejection representation)
Retention recorded: DIV-14 (reference-backend validation deferral) remains in
the Sync/Fake validate hooks (trivial ok); ThreadPool/Uring validate hooks
call the existing validate_op (unchanged semantics).

Divergence registry: no new divergence expected; DIV-14 (reference-backend
validation deferral) remains and is now declared IN THE POLICY (validate hook
returns ok) — registry entry unchanged in meaning, cross-checked PENDING.
```

## Completion Checklist

- [ ] Gate 0 complete
- [ ] Gate 1 state machine covers the modified ownership
- [ ] Gate 2 no new unbounded resource
- [ ] Gate 3 no wake change
- [ ] Gate 4 evidence filled with ACTUAL results (PENDING → PASS only after runs)
- [ ] Review's five binding corrections implemented
- [ ] AGENTS.md change-class gates run (Debug, Release, ASan+UBSan, TSan, real-liburing)
