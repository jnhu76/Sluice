# Remediation Roadmap

**Purpose:** Order the design and implementation work derived from the async
architecture audit. A phase may not claim completion without its named evidence.

**Baseline:** `b20bcc7` (master, including PR #60 and PR #61). This roadmap is
governed by the current findings, divergence registry, Zig conformance map, and
the Accepted
[Unified Explicit I/O Request Contract](../adr/ADR-explicit-io-request-contract.md).
Statuses below reflect the Phase-G closeout (branch
closeout-phase-g-foundation-freeze). **Phases A–G are COMPLETE (2026-08-15).
The async foundation is FROZEN — see
[foundation-freeze.md](foundation-freeze.md); no Phase H is planned.** Future
foundation work is application-triggered only, through the freeze policy's
entry conditions and a normal `AGENTS.md` §8 gate.

**Ordering rule:** Stabilize bottom-layer request identity, admission, and reap
before higher-layer Scheduler/Batch/wake migration. Do not make a persistent
thread pool or Runtime rewrite the center of the request contract.

```text
unforgeable Completion authority                         complete (#61)
    -> explicit request identity + bounded reference lifecycle
    -> backend-agnostic conformance
    -> production backend migration
    -> Scheduler/Batch identity consumption
    -> backend-ready wake integration
```

## Completed governance prerequisites

### Phase 0A — audit and governance baseline (#60)

**Status:** Complete.

- As-built architecture, Constitution, compliance gate, findings, divergence
  registry, conformance map, roadmap, and documentation verification landed.
- Architecture and documentation validation passed in PR #60.

### Phase 0B — Completion authority corrective (#61)

**Status:** Completion-authority portion complete; unrelated vocabulary,
identity, capacity, and backend work remains in the phases below.

PR #61 established:

- private Completion publication mutators;
- protected backend `try_claim` / `publish` / pre-accept rollback capability;
- CAS-based claim and single-winner publish;
- Release fail-fast lifecycle boundaries;
- reap-only Sync/Synthetic cancellation publication; and
- a CI negative-compile authority gate.

This prerequisite does not resolve P0-01, residual P0-02, P1-02, P1-04 through
P1-10, P2-01 through P2-05, DIV-02 implementation, or DIV-12.

## Phase A — unified request-contract ADR

**Change:**

```text
docs(adr): define unified Explicit I/O Request Contract
```

**Status:** Complete — `ADR-explicit-io-request-contract.md` is Accepted
(2026-08-02). Documentation-only — no production behavior was changed by the ADR
itself. Its target contract is implemented for the reference backends by Phase B
(PR #63), for `ThreadPoolBackend` by Phase E (PR #64), and for
`UringAsyncBackend` by Phase D (PR #78/#80/#83/#84); Scheduler/Batch identity
consumption remains Phase F and wake integration remains Phase G.

**Decisions made:**

- descriptors remain separate from `(context, slot, generation)` request identity;
- caller owns Completion; context/backend owns a bounded RequestSlot arena;
- Completion uses a private `idle -> binding -> outstanding` protocol so one
  CAS winner atomically installs key, provenance, and release capability;
- reserve/prepare/commit/enqueue/dispatch form one admission transaction;
- commit is successful-submit linearization;
- backend-ready and Completion-ready are distinct;
- reap synchronously carries pointer-free identity and an optional stable waiter
  token/routing lease through a non-escaping ReadySink;
- borrow lasts from commit through Completion-ready publication;
- RequestSlot owns one waiter registration/token while Scheduler owns Fiber
  routing; a second waiter is synchronous `invalid_state`;
- cancel targets RequestKey and returns an explicit disposition;
- request capacity full is synchronous `would_block`;
- accepted terminal progress does not depend on new unbounded allocation;
- close admission/drain/reset/release precede quiescent destruction; slot
  release is allocation-free, uses only a leaf bounded synchronization domain,
  and waits for no asynchronous progress; and
- public AsyncBackend authors are trusted and must pass conformance.

**Exit criteria:**

- [x] Proposed ADR covers state, resources, failure, wake, lifecycle, Zig
  classification, alternatives, and backend mappings.
- [x] DIV-02 Proposed target decision and revisit trigger recorded without
  claiming acceptance.
- [x] Conformance map distinguishes selected target from current implementation.
- [x] Architecture Gate 0–4 completed without claiming future tests passed.
- [x] Maintainer review accepts the ADR.

**Out of scope:** Any C++ implementation or public API change.

## Phase B — bounded reference request lifecycle

**Status:** COMPLETE — merged to master via PR #63 (merge commit `7f434f0`).
The bounded `RequestArena` / `RequestSlot` reference lifecycle landed with
`FakeAsyncBackend` and `SyncBackend` migrated, along with the Phase B evidence
ledger in `docs/architecture/phase-b-compliance-gate.md`.

```text
feat(async): add bounded RequestKey / RequestSlot reference lifecycle
```

**Scope—only:**

- internal `RequestKey` and context identity;
- bounded construction-time RequestSlot arena;
- complete reference state machine and five-stage admission;
- private Completion `binding` transient plus the shared allocation-free
  reset/ready-destruction release handshake;
- RequestSlot-owned single-waiter registration state with fake stable tokens
  and delivery leases;
- FakeAsyncBackend migration;
- Sync/Synthetic migration and explicit reference-backend positioning;
- synchronous non-escaping identity-bearing `ReadySink`; and
- focused conformance cases needed to prove the reference lifecycle.

**Must prove:**

- full arena returns `would_block` with idle Completion;
- every pre-commit failure rolls back with no borrow or background work;
- two contexts racing one Completion have one `idle -> binding` winner, and no
  path observes or overwrites a half-initialized binding;
- every ownership-safe, irrevocable post-commit dispatch failure becomes one
  reaped terminal error, while transient/partial dispatch remains enqueued;
- slot reuse increments generation and stale keys cannot act;
- backend-ready result storage and linkage require no new allocation;
- Completion-ready alone ends the fd/buffer borrow;
- duplicate waiter registration is rejected without overwriting the first;
- reap and waiter cancellation race to consume a token/routing lease exactly once;
- reap closes registration, takes any delivery, and publishes Completion-ready
  in the shared slot-lifecycle domain, so no waiter can register into a
  lost-wake window and release/reuse cannot overtake old-generation reap;
- the delivery lease pins the Scheduler routing record until the winning sink or
  cancellation path routes and acknowledges it; Phase B proves fake transfer,
  while Phase F proves actual Scheduler cancel/drain/shutdown lifetime;
- reset/destruction with a still-registered waiter fails fast, while reset,
  destruction, and slot reuse during synchronous ReadySink delivery cannot
  dangle a Completion/slot pointer or lose the extracted waiter delivery;
- close admission rejects new work while progress/reap/cancel remain legal; and
- ready reset and ready Completion destruction both release the slot without
  allocation, cancellation, drain, I/O/Scheduler/backend-progress waiting, user
  callbacks, or upward lock acquisition; and
- all slots are released before clean destruction.

**Required gates:** Clang Debug and Release; ASan/UBSan for slot/buffer lifetime;
TSan for any concurrent arena/cancel/reap path; negative-compile and death tests;
architecture documentation validation.

**Dependencies:** Phase A Accepted. Completion authority from #61.

**Out of scope:** Scheduler, Batch, Runtime wake, Uring, persistent workers,
public RequestHandle, and public submit-signature changes.

## Phase C — backend conformance framework

**Change:**

```text
test(async): enforce explicit request lifecycle across backends
```

**Status:** PARTIAL. Phase C is split into C1 (infrastructure) and C2 (semantic
coverage).

- **C1 — infrastructure: IMPLEMENTED.** A reliable, extensible, auditable
  backend-conformance "ruler" now exists: a closed profile model
  (`ReferenceProfile` → Fake, `BlockingIoProfile` → ThreadPool, `KernelIoProfile`
  → Uring), a three-layer evidence classification (shared observable semantics /
  lifecycle protocol / backend-specific mechanism), a single-source-of-truth
  manifest (`scripts/backend_conformance_manifest.py`) with a pure-data
  self-test, an aggregate gate (`scripts/verify-backend-conformance.py`) that
  preflights via `xmake show`/`build`/`run` and classifies backends from stable
  `[conformance-meta]` lines (never display-name/skip-text/`PASS`-string
  parsing), an external-backend admission probe built from the public extension
  surface only, and a narrow negative-compile gate for the `AsyncBackend`
  protected-helper authority gap. The full C1 validation matrix (Debug, Release,
  ASan/UBSan, TSan, all negative-compile gates, aggregate gate, doc checks) is
  recorded in [`phase-c1-conformance-gate.md`](phase-c1-conformance-gate.md),
  bound to the validated implementation head. The gate reports honestly:
  Fake = ELIGIBLE, ThreadPool = ELIGIBLE, **Uring = NOT CONFORMING**
  (KernelIo lifecycle/backend-specific INCOMPLETE — Phase D migration not
  implemented), external probe = admission PASS / conformance NOT ASSESSED.
  `Uring` is never marked conforming. *(C1-era report — superseded: D3/D4
  closed the KernelIo records and lifted the fail-closed gate; see the C2d
  text below and Phase D.)*

  - **C1 corrective (PR #66 review):** the first C1 push had two latent defects
    surfaced by GitHub CI (run `30972306135`): (1) the manifest self-test called
    `sys.exit(0)` at module top level, which `unittest discover` surfaced as a
    `_FailedTest` ERROR; and (2) the aggregate gate drove all three backends'
    shared suite in one process, so one backend's shared-case failure
    contaminated the others' verdicts (the in-binary harness breaks on first
    failure). The corrective removes the top-level `sys.exit`, drives the shared
    suite **once per registered backend in a separate subprocess** (via
    `SLUICE_TEST_FILTER=<driver_case>`), adds a 36-case manifest + attribution
    isolation regression suite, and wires three explicit Phase C1 CI steps
    (aggregate gate, manifest self-test, external-backend authority
    negative-compile). See `phase-c1-conformance-gate.md` § "C1 corrective".
    Scope is still `scripts/` + `.github/` + docs only; no production/public-API
    change. Phase C remains PARTIAL; READY requires a green CI run on the final
    PR head.

- **C2 — semantic coverage: PARTIAL.** C2 is split into C2a (capacity /
  admission / rejection / accounting), C2b (generation / stale / cancel matrix),
  C2c (waiter / borrow / delivery lease), C2d (failure injection), and C2e
  (close / drain / destruction).

  - **C2a — capacity / admission / rejection / accounting: COMPLETE.** The
    shared observable capacity suite (`run_capacity_cases`, 5 cases) runs
    identically against Fake, ThreadPool, and real-liburing Uring via the
    `make_backend_with_capacity`
    factory seam and asserts ONLY `AsyncIoContext`-observable state: accepts
    exact capacity, (N+1)th rejects with `would_block` (rejected Completion
    stays idle; no async from a reject), exact stats split (`submitted_ops`
    committed-only; `queue_full_retries` vs `invalid_state_rejections`),
    `max_outstanding <= capacity`, and recycle after cancel→reap→reset. The
    aggregate gate drives the capacity suite per-backend in isolated
    subprocesses (`shared_capacity_suite` evidence). The D1 post-merge audit
    confirmed that the production Uring capacity contract already satisfies
    the exact shared suite, so the stale `uring_capacity_not_implemented`
    record was removed without production changes. Uring stub mode remains
    INCOMPLETE for this real execution requirement — never skip-as-pass.
    `NonConformingCapacityBackend` (test-only, `SLUICE_ASYNC_INTERNAL_TESTING`-
    guarded, NOT in the manifest) proves `run_capacity_cases()` returns the
    SPECIFIC failing case name for six injected violations (over-accept,
    bind-rejected, late-complete, misclassified `invalid_state`, inflated
    `outstanding`, no-recycle). See
    [`phase-c2a-compliance-gate.md`](phase-c2a-compliance-gate.md).

  - **C2b — generation / stale / cancel matrix: COMPLETE.** The arena-level
    state-transition and identity matrix (rows 3 and 4a — slot-generation
    authority: generation +1 before reuse, stale-handle rejection, live-N+1
    stale-event harmless) is extended with Fake and ThreadPool integration
    evidence for cancel-winner and publication-boundary semantics (rows 5–8).
    Uring's Phase-D identity gap is recorded as a `not_implemented` manifest
    record (`uring_c2b_identity_not_implemented`), which enters Uring's verdict
    via `applicable_evidence_for_backend()`. *(Superseded: D3 closed this
    record with real-liburing evidence — see C2d below and Phase D.)* Eight
    single-point production
    mutations (A, B1, B2, C, D, E, F, G) prove each detector case fails on
    deliberately nonconforming identity behavior. **Row 4b — cross-context
    `RequestKey` authority rejection — is Phase F scope**, not a C2b gap: the
    RequestArena mutable authorities take a context-less `SlotHandle`, so the
    foreign-`RequestKey` authority path does not exist until the Phase F public
    RequestHandle consumer does. See
    [`phase-c2b-compliance-gate.md`](phase-c2b-compliance-gate.md).

  - **C2c — waiter / borrow / delivery lease: COMPLETE.** Rows 11 (fd/buffer
    borrow lifetime), 12a (RequestSlot-level single-waiter registration),
    13 (waiter-cancel independence), and 14a (abstract move-only delivery
    lease) now have arena-level, per-backend, concurrency-proven,
    mutation-valid evidence: a focused arena matrix target
    (`request_waiter_borrow_lease_test`, 14 cases) and Fake/ThreadPool
    integration targets (`backend_c2c_waiter_borrow_test`,
    `threadpool_backend_c2c_waiter_borrow_test`) that route real accepted
    Completions through the REAL arena waiter/borrow authorities. The
    ThreadPool evidence includes the running window (borrow active with exact
    fd/addr/len, waiter survives enqueued → running → backend_ready, running
    cancel intent ends neither), the backend_ready-before-reap window (a
    worker finishing its syscall is NOT the borrow lifetime end), and waiter
    registration inside the running/backend_ready windows (registration is
    orthogonal to execution state — ADR Decision 10). Nine
    single-point production mutations (A–I) prove each detector case fails on
    deliberately nonconforming borrow/waiter/lease behavior. **Rows 12b
    (public waiter / RequestHandle / Scheduler registration consumer) and 14b
    (real Scheduler routing-record lifetime / lease acknowledgement) are
    Phase F scope** — the Accepted ADR's own Decision 10 boundary (Phase B
    proves abstract transfer; Phase F proves real Scheduler lifetime); C2c
    adds no public waiter API. Uring's Phase-D gap is the
    `uring_c2c_borrow_waiter_not_implemented` record. *(Superseded: D3 closed
    this record with real-liburing evidence — see C2d below and Phase D.)* See
    [`phase-c2c-compliance-gate.md`](phase-c2c-compliance-gate.md).

  - **C2d — failure injection / accepted-terminal under allocator failure:
    COMPLETE.** Rows 9–10 now have real-backend runtime evidence on
    `ThreadPoolBackend` (`threadpool_backend_c2d_failure_test`, 12 cases,
    `SLUICE_ASYNC_INTERNAL_TESTING`-guarded deterministic seams) and
    reference-path evidence on Fake (`reference_backend_no_alloc_test`
    full-window defined-error case): ADR Gate-4 per-stage pre-commit injection
    at reserve (injected would_block — Completion idle, zero residue), prepare
    (candidate slot rolled back, capacity immediately recyclable), and the
    COMMIT-BOUNDARY (the binding CAS wins, then commit is injected to fail —
    the submit path executes the REAL `rollback_binding_before_accept` + slot
    rollback, the only executable instance of that branch in the corpus, and
    the Completion returns to fully reusable idle); transactional pre-commit
    rejection on the real backend (binding-CAS loss → `invalid_state`, zero
    residue, capacity recyclable); partial worker-startup failure stops and
    joins the already-started workers and rethrows synchronously (the finding
    P1-04 regression test that was missing); a post-commit permanent dispatch
    failure (injected between enqueue and dispatch push, inside `work_mtx_`,
    with no worker ever able to see the handle — the ADR Decision-12
    "post-commit dispatch failure after execution ownership is proven
    absent" winner candidate) leaves submit successful, drives the request to
    exactly ONE defined `backend_error` terminal, publishes once via reap,
    keeps the borrow active until reap, and never executes a worker or syscall
    — for both the size and void operation paths; the accepted
    submit → enqueue/terminal → reap → reset path performs ZERO heap
    allocations under an always-throw operator new (ADR Decision 14 / I9) on
    the real worker path and on the injected failure path; and the
    dispatch-failure terminal vs cancel has exactly one winner, no overwrite,
    no double publication, and at most one tally in every interleaving
    (`canceled_ops == 1` iff cancel won — the injected `backend_error`
    terminal contributes no tally because `completion_errors` is unwired for
    ThreadPool). The two orderings are proven deterministically: cancel-wins
    before enqueue (ADR Gate 4 commit/enqueue pause) and injection-wins with
    a cancel-after no-op. Thirteen single-point mutations (M1–M13) prove each
    detector case fails on deliberately nonconforming behavior
    (`docs/verification/phase-c2d-failure-injection-mutation-evidence.md`).
    The ring-full invariant fail-fast path is untouched (never converted to a
    recovery path). Phase D2 replaces the former Uring known-gap record with
    real-command-backed `uring_c2d_failure_injection` evidence covering
    pre-commit zero residue, D1 transient/zero/partial transport preservation,
    P0-D Class-A recovery and Class-C retention, ordinary/permanent/control
    no-allocation windows, and cancel/recovery terminal arbitration. Stub mode
    is explicitly INCOMPLETE. The former NOT CONFORMING tail is now closed:
    D3 (branch test/phase-d3-uring-identity-waiter-conformance) filled the
    C2b/C2c integration records and D4 (branch
    `feat/phase-d4-uring-wait-close-drain`) filled the C2e record and lifted
    the KernelIo fail-closed gate only after the complete real-mode evidence
    set passed. See
    [`phase-c2d-compliance-gate.md`](phase-c2d-compliance-gate.md),
    [`phase-d2-uring-failure-noalloc-gate.md`](phase-d2-uring-failure-noalloc-gate.md)
    and [`phase-d4-uring-wait-close-drain-gate.md`](../history/closeout/phase-d4-uring-wait-close-drain-gate.md).

  - **C2e — close / drain / destruction: COMPLETE.** Row 15 (close/drain/reset
    sequence) is now FULL and row 16 (quiescent destruction) is FULL
    (re-audited at C2e start, then strengthened). The shared close/drain suite
    (`run_close_drain_cases`, 4 cases) runs identically against Fake and
    ThreadPool through the new `FakeAsyncBackend::close_admission()` reference
    method and the driver-wired `close` / `slot_in_use` closures, and asserts
    ONLY the shared boundary: close rejects future submit with `invalid_state`
    (Completion idle, zero residue), accepted-before-close still reaches
    exactly ONE defined terminal with cancel/poll/reap legal after close,
    drained != releasable (`accepted_outstanding == 0` and Completion ready
    but `slot_in_use == 1` until the caller resets, then `== 0`), and
    slot-release vs admission-close orthogonality (a released slot does not
    re-open admission). The deterministic ThreadPool target
    (`threadpool_backend_c2e_close_drain_test`, 12 cases) pins every window
    with the existing guarded pause gates: close while `pending` / `enqueued` /
    `running` (real result verbatim, size + void), close then pending cancel
    still WINS canceled (Scheme B; no dispatch linkage; no syscall), close
    then running cancel records intent only, close wakes a parked wait_one as
    a ONE-SHOT control wake (0, no fabricated completion; a future wait parks
    normally — no busy-spin), close ‖ final `record_terminal` in BOTH
    orderings (the control interrupt never swallows the final ready), an
    invariant-only close-vs-workers race drain, and the submit ‖ close
    concurrent linearization invariant (never half-accepted). Row 16 keeps its
    FULL status with the added `pending`-state death case
    (`tp_death_destroy_with_pending`) and a new Fake-type death target
    (`fake_backend_death_test`: unreaped-bound, ready-unreset, quiescent
    control — the reference path fail-fasts through the arena destructor in
    Debug AND Release). Ten single-point production mutations (M1–M10) prove
    each detector case fails on deliberately nonconforming behavior
    (`docs/verification/phase-c2e-close-drain-destruction-mutation-evidence.md`).
    Uring's Phase-D gap is the `uring_c2e_close_drain_not_implemented` record,
    which enters Uring's verdict — Uring stays NOT CONFORMING and is never
    skip-as-pass for close/drain/destruction. *(Superseded: D4 closed this
    record with real-liburing evidence — see C2d below and Phase D.)* See
    [`phase-c2e-compliance-gate.md`](phase-c2e-compliance-gate.md).

  The "must cover" scope below is broader than the 8-case shared suite: injected
  allocation/startup/dispatch failure, accepted-terminal under allocator
  failure, single-waiter enforcement, generation/stale-event cases,
  borrow-lifetime cases, the full capacity/high-water/rejection matrix (C2a
  done), the full stale cancel/wait/reap matrix, the full
  waiter/borrow/delivery-lease interleave, and all shutdown/reset/non-quiescent
  destruction cases. Some are already covered out-of-suite by Phase B arena
  tests, death tests, and the negative-compile scripts; C2 closes the remaining
  matrix into the aggregate gate.

The shared backend-agnostic suite is implemented and wired into the `test` group
(`tests/backend_conformance.hpp` + `backend_conformance_test.cpp` + driver
`backend_conformance_driver_test.cpp`; target `backend_conformance_test`) and
runs against Fake, ThreadPool, and Uring (stub without liburing; real path with
liburing). It currently contains 8 shared-semantic cases (submit→reap
exactly-once, positional independence, EOF after partial, short-completion
retry, exactly-once terminal, cancel yields defined terminal, stats accounting,
clean shutdown). ThreadPool passes the suite as of Phase E (PR #64); Fake
passes; Uring passes only the stub subset (non-`real_mode` skips). Per the scope
text below, Uring must not be marked conforming before the Phase D migration.

Build a backend-agnostic suite rather than backend-specific happy-path copies.
The framework must cover:

- capacity, high-water, and capacity-reject accounting;
- rejection vs accepted completion;
- every state transition and invalid transition;
- generation/provenance and stale cancel/wait/reap events;
- pending, running, kernel-style, backend-ready, and completion-ready cancel cases;
- cancel winner vs ordinary result and dispatch/shutdown terminal candidates;
- exactly one backend-ready winner and one Completion publication;
- identity-bearing reap order without global sequence authority;
- injected allocation/startup/dispatch failure;
- accepted-terminal path under allocator failure;
- borrow lifetime and fd identity obligations;
- single-waiter enforcement and waiter-cancel/I/O independence;
- close admission, graceful drain, reset/release, clean destruction, and
  fail-fast non-quiescent destruction; and
- public-backend negative compile and conformance entry requirements.

Gate 4 items in the ADR remain pending until command-backed tests exist. Fake
and Sync/Synthetic are the first passing instances; this phase must not mark
Uring or blocking offload conforming before their migrations.

**Dependencies:** Phase B reference types and ReadySink. It may be developed in
the same implementation PR only if the review surface remains reference-only;
the logical dependency remains B -> C.

## Phase D — Uring migration

**Change:**

```text
refactor(async): migrate UringBackend to RequestSlot identity
```

**Status:** COMPLETE — D0/D0.5 complete; D1 complete via PR #78; D2 complete
via PR #80 with command-backed real-liburing evidence; D3 complete via PR #83
(branch test/phase-d3-uring-identity-waiter-conformance) closing the C2b/C2c
integration matrix; D4 complete via PR #84 (branch
`feat/phase-d4-uring-wait-close-drain`) implementing the wait source,
close/drain/destruction proof and lifting the KernelIo fail-closed gate only
after the complete mandatory real-mode evidence set passed
([`phase-d4-uring-wait-close-drain-gate.md`](../history/closeout/phase-d4-uring-wait-close-drain-gate.md)).
Full Phase D is complete; the KernelIo profile is ELIGIBLE in real mode and
honestly INCOMPLETE in stub builds.

**D0 audit / PR decomposition (2026-08-08):** COMPLETE. The complete audit and
the D1–D4 decomposition are in
[`docs/architecture/phase-d-uring-migration-plan.md`](phase-d-uring-migration-plan.md)
(historical baseline `1349a6f`). PR #78 completed D1, including the private-ring
RequestArena migration and permanent-submit P0-D recovery. D2 closed the
C2d failure/no-allocation record and reconciled the already-satisfied C2a
capacity evidence; D3 closed `uring_c2b_identity_not_implemented` and
`uring_c2c_borrow_waiter_not_implemented`; D4 closed
`uring_c2e_close_drain_not_implemented` and lifted the KernelIo hard-code.

**Remaining work:** none. D3 (PR #83) and D4 (PR #84) were merged on
2026-08-10/11; the stub/build-vs-real evidence separation is maintained by the
per-suite KernelIo real-mode attribution (unchanged requirement).

**Dependencies:** Phase C. No Scheduler dependency.

**Out of scope:** Scheduler identity consumption and wake bridge.

## Phase E — bounded portable blocking offload

**Change:**

```text
refactor(async): replace per-op threads with bounded blocking-I/O workers
```

**Status:** COMPLETE — merged to master via PR #64 (merge commit `a8178d8`).
The full evidence ledger is `docs/architecture/phase-e-compliance-gate.md`
(validated implementation head `9f91bd3`, evidence-recording head `4af082b`,
master merge commit `a8178d8`).

**Work:**

- replace thread-per-operation with a fixed persistent worker set;
- use explicit operation kinds/parameters rather than arbitrary `std::function`
  as the core request;
- use a bounded, pre-reserved pending queue over RequestSlots;
- have workers perform only syscall work and backend-ready publication;
- make reap the only Completion-ready publication path;
- define queued removal and running-syscall best-effort cancellation;
- close admission, drain the queue/running work, join workers, then destroy; and
- add benchmark evidence before claiming performance improvement.

**Exit criteria:** Phase C suite passes; no per-op thread creation; no unbounded
hot-path allocation; worker/storage bounds and defaults documented; P0-01,
P1-04, P2-01, P2-02, P2-03, DIV-03, and DIV-12 receive command-backed resolution
evidence. **All exit criteria are met** — see `phase-e-compliance-gate.md` for the
command-backed evidence and the resolution notes in `current-architecture-findings.md`
(P0-01, P1-04, P2-01, P2-02, P2-03) and `divergence-registry.md` (DIV-03, DIV-12).

**Dependencies:** Phase C. It does not depend on Scheduler wake.

## Phase F — Scheduler and Batch consume identity-bearing reap — COMPLETE

**Change:**

```text
refactor(runtime): consume identity-bearing reap events
```

**Status: COMPLETE.** F1 (PR #105), F2, and F3 are all implemented. The only
remaining identity-related work is the backend-ready wake bridge (Phase G),
which is a separate, untouched phase.

**Work (re-baselined by audit issue #94; runtime decomposition in issue #98):**

- **F2 — COMPLETE.** `BatchResult` now carries a typed `BatchResultOrigin`
  (`rejected` vs `accepted_and_completed`), orthogonal to success/error. A
  submit-time rejection is `rejected`; any accepted request (success, terminal
  error, or canceled winner) is `accepted_and_completed`. Tracked by an explicit
  per-slot `submit_rejected` flag (not inferred from the result, and independent
  of the internal `reap_seq` ordering mechanism). Evidence:
  `tests/batch_result_origin_test.cpp` (ADR Decision 9 — Batch consumes outcome
  origin explicitly);
- **F3 — COMPLETE.** Public `RequestHandle` identity surface (ADR:
  `docs/adr/ADR-public-request-handle.md`; gate:
  `docs/architecture/phase-f3-compliance-gate.md`). Additive
  `submit_*_request -> Result<RequestHandle>` (success ⇒ exactly one valid
  handle; rejection ⇒ no handle; non-identity backend ⇒ `not_supported` with no
  side effect); the read-only `request_state` identity consumer (outstanding /
  backend_ready / completion_ready / not_found); non-forgeable construction
  (private ctor, friend `AsyncBackend`, negative-compile gate). Closes C2b row
  4b (cross-context) and the C2c row 12b/14b public-API residual.

**Implemented by F1 (issue #98):**

- production Scheduler consumes identity-bearing reap: the Scheduler-owned
  `ReadyRoutingSink` routes the by-value `ReadyEvent{key, token, lease}` from
  every arena-backed backend reap to the pinned Scheduler wait record; the
  drain routes the resumed fiber exactly once under `global_mtx_` (design:
  `docs/history/implementation-plans/phase-f1-scheduler-ready-sink.md`; gate:
  `docs/architecture/phase-f1-compliance-gate.md`);
- the O(N) `Completion::ready()` re-scan is removed from the
  completion-progress path for arena backends. The legacy
  `Completion*`-keyed `waiting_size_`/`waiting_void_` maps remain ONLY as a
  disjoint fallback for non-arena backends whose `register_waiter` returns
  `not_supported` — never a second authority on the identity path;
- context provenance and duplicate-waiter enforcement on the Scheduler side:
  a second waiter on one Completion, or a Completion not bound to this
  context, surfaces as synchronous `invalid_state` without touching the first
  waiter (C2c rows 12b/14b Scheduler-side enforcement);
- waiter cancellation: `Scheduler::cancel_waiter` plus the production
  `RuntimeTaskContext::cancel_waiter` caller remove only the waiter; the I/O,
  borrow, and terminal result are untouched (arena `cancel_waiter` moved from
  seam-only to a production caller);
- the F1 concurrency lock design: the wait registry adds the leaf
  `wait_registry_mtx_` (R); lock order is G→A and G→R (R never precedes
  G/A in the opposite direction); the sink takes only R; no join/alloc/
  syscall under R (full table in the design doc).

**Already implemented (removed from Phase F scope):** the backend half of
identity-bearing reap — all four backends reap through the arena's
identity-bearing `SynchronousReadySink` with by-value `ReadyEvent`; and the
process-global `reap_seq` ordering authority was removed/demoted (F-02
closeout, `completion.hpp:113` — internal ordering mechanism consumed only by
`Batch::next()`, not public API).

**Dependencies:** Phases C, D, and E for the backends selected into the unified
production path. No wake-bridge dependency.

## Phase G — backend-ready progress wake integration — COMPLETE

**Status:** COMPLETE (2026-08-15, closeout branch
closeout-phase-g-foundation-freeze). Design
`docs/design/phase-g-backend-progress-wake.md` implemented (R1–R4 park
protocol, split-wait bridge); compliance gate
`docs/architecture/phase-g-compliance-gate.md`; causal closeout matrices
`tests/phase_g_closeout_test.cpp` (Cases A–D, TP-G1..G7) and
`tests/phase_g_closeout_uring_test.cpp` (UR-G1..G7, real liburing); formal
model `spec/tla/e9_park_wake/` (bridge/control-epoch, 4 positive + 4 negative
TLC gates). P2-04 RESOLVED; DIV-04/DIV-05 amended; ADR-execution-model
§9.4.7.2 records the amendment.

**Change:**

```text
feat(runtime): bridge backend-ready progress to Scheduler wake
```

Design a narrow progress-notification capability after stable identity events
exist. It must:

- preserve Scheduler as the only Fiber-routing authority;
- avoid backend-lock -> Scheduler-global-lock inversion;
- define notification coalescing and lost-wake prevention;
- state whether the 2ms mixed-wake interval is removed or retained only as
  defense in depth;
- work consistently across Fake, Sync/Synthetic, Uring, and blocking offload;
  and
- use causal/barrier tests rather than sleeps as correctness proof.

**Dependencies:** Phase F identity consumption. This is the only phase that may
reclassify DIV-04/DIV-05.

## Dependency graph

```text
0A governance (#60)
    -> 0B Completion authority (#61)
        -> A proposed request-contract ADR
            -> B bounded reference lifecycle
                -> C conformance framework
                    -> D Uring migration
                    -> E blocking-offload migration
                        -> F Scheduler/Batch identity consumption
                            -> G backend-ready wake integration
```

D and E can be prepared independently after C, but F must not switch the common
integration contract until the backends it supports are identity-bearing. B/C
must not depend on Scheduler. E must not be pulled forward merely because the
current type is named `ThreadPoolBackend`.

## Findings-to-phase summary

| Finding/divergence | Target phase |
|---|---|
| P0-01, P1-04, P2-01, P2-02, P2-03, DIV-03, DIV-12 | E, after B/C — **resolved in Phase E (PR #64)** |
| residual P0-02 | B/C reference (closed); E ThreadPool — resolved (PR #64); D Uring — resolved (PR #78/#80/#83/#84) |
| P1-02, P1-06, P1-07 | B/C reference CLOSED; D/E production backends RESOLVED; F Scheduler/Batch consumption |
| P1-05 | B/C vocabulary and metrics |
| P1-08, P1-09, P1-10 | B/C target semantics; focused lock design and F integration |
| P2-05 | F (Batch origin flag — F2) |
| P2-04, DIV-04, DIV-05 | G — **RESOLVED/AMENDED in Phase G (2026-08-15): split-wait bridge + condition-driven park cap; reference exemption retained** |
| DIV-02 target ownership | A; implementation B–E + D; revisit only on measured trigger |
| DIV-13 public backend conformance | C, enforced for D/E and future backends |

## Global rules for every implementation phase

- Apply the Architecture Constitution and complete the compliance gate before
  production implementation.
- Preserve synchronous public Reader/Writer semantics and target boundaries.
- Add failure evidence that can fail on the pre-fix implementation.
- Do not mark a finding resolved because only the ADR or reference backend
  exists.
- Do not claim real-liburing, sanitizer, TSan, formal-model, or benchmark
  evidence that was not run.
- Update the as-built architecture only after code lands; keep future designs in
  ADR/roadmap language until then.
