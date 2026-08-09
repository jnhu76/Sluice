# Phase D2 Uring failure/no-allocation implementation plan

**Status:** EXECUTED — all Phase D2 command gates passed; D3/D4 remain pending.

> Execute this plan in order. Preserve Phase D1 production semantics; stop if a detector requires
> a new lifecycle authority, unbounded accepted-path storage, submit-prefix lifecycle semantics, or
> unsafe release of possibly consumed work.

**Goal:** Replace the mandatory `uring_c2d_failure_injection_not_implemented` gap with
real-liburing, command-backed failure/no-allocation evidence while leaving D3, D4, and the
KernelIo fail-closed verdict unchanged.

**Architecture:** Reuse the Phase D1 RequestArena/private-ring/P0-D implementation. Add a dedicated
real/stub-aware D2 test target, an always-throw userspace allocation probe, and only read-only
internal-testing observations of already-bounded Uring state. Existing D1 transport tests remain
the evidence for transient/zero/partial/Class-A/Class-C semantics. The conformance harness will
classify real-only evidence as incomplete in stub mode instead of converting a stub run into PASS.

**Toolchain:** C++20, xmake, liburing 2.14, Python manifest gate, Clang sanitizers, repository formal
and documentation scripts.

## Task 1: Add the focused D2 target and failing evidence skeleton

**Files:**

- Create: `tests/uring_d2_failure_noalloc_test.cpp`
- Modify: `xmake/experimental.lua`

1. Define `uring_d2_failure_noalloc_test` in both build modes.
2. In real mode, compile the authoritative `src/async/uring_backend.cpp` and
   `src/async/fail_fast.cpp` directly with `SLUICE_HAS_LIBURING` and
   `SLUICE_ASYNC_INTERNAL_TESTING`; do not link either async library variant.
3. In stub mode, link production `sluice_async` and compile only a stub build/API case.
4. Emit exactly one stable evidence-mode line per target run.
5. Build/run the target in both modes. At this step, the real target is expected to fail to compile
   where planned read-only D2 observations are still absent; record that failing-first result.

## Task 2: Add guarded read-only Uring observations

**Files:**

- Modify: `include/sluice/async/uring_backend.hpp`
- Modify: `src/async/uring_backend.cpp`
- Test: `tests/uring_d2_failure_noalloc_test.cpp`

1. Under `SLUICE_HAS_LIBURING && SLUICE_ASYNC_INTERNAL_TESTING`, declare read-only methods for:
   local dispatch count, physical-ledger count, SQ-ready count, and live prepared/submitted control
   count.
2. Define the methods against the real bounded production structures. Add no member data, no
   production branch, no public installed-build symbol, and no alternate state machine.
3. Run the focused target and prove the observations expose zero/unchanged residue and the exact
   one-control bound.
4. Build production `sluice_async` and inspect symbols/preprocessed guards as appropriate to prove
   the methods are absent from production.

## Task 3: Drive the natural pre-commit failure matrix

**Files:**

- Modify: `tests/uring_d2_failure_noalloc_test.cpp`

1. Size path: retain one accepted request to force natural capacity `would_block`; compare arena,
   dispatch, router, ledger, and SQ snapshots before/after the rejected submission; then complete,
   reap, reset, and prove capacity recycling.
2. Size descriptor rejection: use a real malformed descriptor and prove idle Completion plus zero
   residue.
3. Size binding-CAS loss: resubmit with an already-bound Completion while spare capacity exists;
   prove the original request and every backend-progress domain are unchanged.
4. Repeat the applicable descriptor and binding-CAS checks for a void sync operation.
5. Do not add injected prepare/commit errors: record those private, same-lock transitions as
   structural N/A in the gate, with RequestArena transition tests as supporting evidence.
6. Run the focused cases.

## Task 4: Prove accepted paths under an always-throw allocator

**Files:**

- Modify: `tests/uring_d2_failure_noalloc_test.cpp`

1. Reuse the repository's sanitizer-compatible replacement `operator new` pattern; compile it out
   under TSan and keep lifecycle assertions active there.
2. Construct ring, file/pipe, buffers, Completions, and scripted hook state before arming.
3. Ordinary size: arm before submit, drive real write -> submit -> CQE -> terminal -> reap -> reset,
   then prove allocation count zero.
4. Ordinary void: arm before `submit_sync_data`, drive real CQE/reap/reset, then prove zero.
5. Permanent recovery: arm before accepted size and void submissions, inject permanent `-EIO`
   (a scripted replacement of the submit return — only `kRealSubmit` calls liburing), drive the
   existing P0-D Class-A controller and reap/reset both defined backend errors, then prove zero.
   The injected result exercises the production recovery controller; it does NOT independently
   reproduce the real kernel negative-enter physical state (that claim stays with the D1
   liburing/kernel source proof).
6. Run the focused target in Debug, Release, and ASan+UBSan.

## Task 5: Prove cancel/recovery arbitration and bounded control work

**Files:**

- Modify: `tests/uring_d2_failure_noalloc_test.cpp`

1. Pending/enqueued cancel: hold an older SQ entry, leave a newer request in the bounded local FIFO,
   cancel the newer request (execution disarmed first), then inject permanent failure for the older
   Class-A batch. Prove canceled versus backend-error terminals remain distinct and each publishes
   exactly once.
2. Running/control path: positively submit a blocked pipe read as Class C, arm the allocator probe,
   call `cancel()` repeatedly, and inject `EINTR`, `EAGAIN`, `EBUSY`, then permanent `EIO` for the one
   control suffix.
3. At every retry prove at most one live prepared/submitted control, bounded ledger/SQ state, and no
   second control reference from repeated cancel.
4. Complete the original pipe read from its real CQE after poison, reap/reset exactly once, and prove
   the whole cancel/control/recovery window allocated zero userspace heap storage.
5. Run focused Debug and TSan cases.

## Task 6: Add honest real/stub manifest integration

**Files:**

- Modify: `scripts/backend_conformance_manifest.py`
- Modify: `scripts/verify-backend-conformance.py`
- Modify: `scripts/tests/test_backend_conformance_manifest.py`
- Test: `tests/uring_d2_failure_noalloc_test.cpp`

1. Add a closed, optional required-execution-mode field for evidence records.
2. Parse the D2 target's single evidence-mode line fail-closed: missing, duplicate, foreign, or
   disallowed mode is `INCOMPLETE`, not PASS.
3. Replace `uring_c2d_failure_injection_not_implemented` with mandatory implemented
   `uring_c2d_failure_injection`, target `uring_d2_failure_noalloc_test`, layer `lifecycle`, backend
   Uring, required mode `real`.
4. Notes must enumerate pre-commit rollback, transient/zero/partial transport, permanent Class-A
   recovery, Class-C retention, accepted-terminal no-allocation, and cancel/failure winner evidence.
5. Keep the C2b/C2c/C2e gap records and the KernelIo hard-coded NOT CONFORMING verdict unchanged.
6. Run manifest self-tests in both real and stub configurations and run the aggregate gate.

## Task 7: Reconcile D1/C2a capacity only if the exact shared suite already passes

**Files:**

- Modify if proven: `tests/backend_conformance_driver_test.cpp`
- Modify if proven: `scripts/backend_conformance_manifest.py`
- Modify if proven: `scripts/verify-backend-conformance.py`
- Modify if proven: `scripts/tests/test_backend_conformance_manifest.py`

1. Add only the real-liburing `make_backend_with_capacity` closure using existing `UringConfig`.
2. Register a real Uring capacity driver that calls the exact existing `run_capacity_cases`.
3. In stub mode, emit mode metadata but do not run kernel capacity semantics; the aggregate
   classifier must return `INCOMPLETE`, not PASS.
4. Run the exact case in real mode before changing the manifest.
5. If it passes with no production change, add Uring to `shared_capacity_suite` and remove the stale
   `uring_capacity_not_implemented` gap. If it fails or needs material implementation, revert this
   task's wiring and report the separate gap without expanding D2.

## Task 8: Focused mutation evidence

**Files:**

- Create: `docs/verification/phase-d2-uring-failure-noalloc-mutation-evidence.md`
- Temporarily mutate and restore: `src/async/uring_backend.cpp`,
  `include/sluice/async/uring_backend.hpp`, or the guarded D2 test seam as appropriate

For each M1–M11, make one minimal mutation, run the exact detector, capture the RED result, and
restore with `apply_patch` before proceeding:

1. M1 allocate after acceptance;
2. M2 terminalize on transient EINTR;
3. M3 terminalize on zero progress;
4. M4 make positive prefix alter lifecycle;
5. M5 include prior Class-C router work in permanent recovery;
6. M6 omit Class-A retirement;
7. M7 allow admission after poison;
8. M8 use a submitting wait after poison;
9. M9 allow cancel/recovery overwrite or distinguishable second publication;
10. M10 clear/bypass one-control idempotence or retirement;
11. M11 leave pre-commit backend residue.

After every restoration, rerun the detector green. Finish with `rg` for mutation markers and a
source diff review.

## Task 9: Reconcile roadmap and close Gate 4 evidence

**Files:**

- Modify: `docs/architecture/phase-d2-uring-failure-noalloc-gate.md`
- Modify: `docs/architecture/phase-d-uring-migration-plan.md`
- Modify: `docs/architecture/remediation-roadmap.md`
- Modify: `docs/architecture/phase-c2d-compliance-gate.md`
- Modify: `docs/verification/phase-d2-uring-failure-noalloc-mutation-evidence.md`

1. Update stale prose to D0/D0.5 complete, D1 complete via PR #78, D2 complete only on final passing
   head, D3 pending, D4 pending.
2. Record exact commands and final-head PASS/SKIP results in the D2 gate.
3. Keep historical Accepted ADR text historical and do not mark full Phase D complete.
4. Keep stub evidence separate and keep KernelIo fail-closed.

## Task 10: Final verification, review, and commits

1. Run focused D2, existing submit-failure/backend/death tests, real Debug/Release, stub Debug,
   ASan+UBSan, TSan, negative compile, manifest self-test, aggregate conformance, existing
   `d1-uring-poison` formal suite, doc links, architecture docs, and `git diff --check`.
2. Inspect `git status --short`, `git diff --stat`, and the full diff; prove no unrelated files or
   production test seams remain.
3. Request a final code review and address only validated findings.
4. Prefer three commits:
   - `test(async): add Uring D2 no-allocation and failure-conformance evidence`
   - `test(gate): wire Uring C2d evidence into backend conformance`
   - `docs(async): record Phase D2 Uring failure/no-allocation gate`
5. Report the baseline SHA, final SHA, branch, commits, D2 audit, allocator scope, mutation REDs,
   real/stub gates, C2a reconciliation, remaining D3/D4, and D2-only verdict.
