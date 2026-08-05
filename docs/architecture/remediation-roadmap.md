# Remediation Roadmap

**Purpose:** Order the design and implementation work derived from the async
architecture audit. A phase may not claim completion without its named evidence.

**Baseline:** `b20bcc7` (master, including PR #60 and PR #61). This roadmap is
governed by the current findings, divergence registry, Zig conformance map, and
the Accepted
[Unified Explicit I/O Request Contract](../adr/ADR-explicit-io-request-contract.md).
Statuses below reflect master as of PR #64 (merge commit `a8178d8`).

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
and for `ThreadPoolBackend` by Phase E; Uring migration remains Phase D, while
Scheduler/Batch identity consumption and wake integration remain Phases F and G.

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

**Status:** PARTIAL — the shared backend-agnostic suite is implemented and wired
into the `test` group (`tests/backend_conformance.hpp` + `backend_conformance_test.cpp`
+ driver `backend_conformance_driver_test.cpp`; target `backend_conformance_test`) and
runs against Fake, ThreadPool, and Uring (stub without liburing; real path with
liburing). It currently contains 8 shared-semantic cases (submit→reap exactly-once,
positional independence, EOF after partial, short-completion retry, exactly-once
terminal, cancel yields defined terminal, stats accounting, clean shutdown).
ThreadPool passes the suite as of Phase E (PR #64); Fake passes; Uring passes only the
stub subset (non-`real_mode` skips). NOT complete: the "must cover" scope below is
broader than the suite — injected allocation/startup/dispatch failure, accepted-terminal
under allocator failure, single-waiter enforcement, generation/stale-event cases,
borrow-lifetime cases, and the public-backend negative-compile/conformance entry
requirements are not yet in the suite (some are covered out-of-suite by Phase B arena
tests, death tests, and the negative-compile scripts). Per the scope text below, Uring
must not be marked conforming before the Phase D migration.

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

**Status:** NOT IMPLEMENTED — `UringAsyncBackend` remains on the legacy maps/deques
identity path (DIV-02 and DIV-14 remain open for Uring; the conformance map keeps
its rows/notes at the pre-migration classification). This is the remaining
incomplete backend prerequisite ahead of Phase F: F must not switch the common
Scheduler/Batch identity consumption while Uring still reconstructs identity from
side-band containers. The D → F dependency below is unchanged.

**Work:**

- map RequestKey to SQE `user_data` and CQE back to the same validated slot;
- reserve RequestSlot and bounded userspace dispatch-queue bookkeeping before
  commit, then acquire/fill SQEs only during post-commit dispatch;
- remove Completion reverse maps and parallel identity reconstruction where
  RequestSlot suffices;
- keep a partial `io_uring_submit` suffix bound and enqueued for allocation-free
  retry rather than terminalizing it;
- store dispatch failure in the same slot only after proving that no SQE,
  kernel request, or future CQE can still reference the request;
- target cancel SQEs by key/generation and reject stale CQEs;
- keep ring queue depth distinct from request capacity; and
- pass the Phase C suite on both default stub/off and real liburing paths where
  available.

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

## Phase F — Scheduler and Batch consume identity-bearing reap

**Change:**

```text
refactor(runtime): consume identity-bearing reap events
```

**Work:**

- migrate all selected backends to identity-bearing reap before switching the
  common Scheduler path; do not run a mixed scan/event authority indefinitely;
- route a ReadyEvent/RequestKey to the one registered waiter while preserving
  Scheduler-owned runnable routing;
- remove O(N) `Completion::ready()` scans from completion progress;
- enforce context provenance and synchronous duplicate-waiter `invalid_state`;
- make waiter cancellation remove only the waiter, not cancel the I/O;
- make Batch distinguish `rejected` from `accepted_and_completed` explicitly;
- remove or demote process-global `reap_seq` from ordering authority; and
- preserve Runtime ownership and current shutdown sequencing unless a separate
  approved design changes them.

The exact `wait_one`/cancel concurrency lock design must be completed before
changing L1 synchronization. The request contract fixes target identity and
disposition, but does not authorize an incidental lock redesign.

**Dependencies:** Phases C, D, and E for the backends selected into the unified
production path. No wake-bridge dependency.

## Phase G — backend-ready progress wake integration

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
| residual P0-02 | B/C reference (closed); D Uring — open; E ThreadPool — resolved (PR #64) |
| P1-02, P1-06, P1-07 | B/C, then D/E/F |
| P1-05 | B/C vocabulary and metrics |
| P1-08, P1-09, P1-10 | B/C target semantics; focused lock design and F integration |
| P2-05 | F |
| P2-04, DIV-04, DIV-05 | G |
| DIV-02 target ownership | A; implementation B–E; revisit only on measured trigger |
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
