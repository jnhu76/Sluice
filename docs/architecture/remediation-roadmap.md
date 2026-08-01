# Remediation Roadmap

**Purpose:** Ordered sequence of design and implementation work derived from
the architecture audit findings. This roadmap does NOT prescribe solutions — it
prescribes the order in which decisions must be made.

**Baseline:** `d299fc0`. Derived from `current-architecture-findings.md`,
`zig-io-conformance-map.md`, and `divergence-registry.md`.

**Principle:** Each phase produces a design decision (ADR or design doc) BEFORE
implementation begins. No phase may skip the design compliance gate.

---

## Phase 0 — Authority Correction

**Goal:** Eliminate documentation/implementation authority conflicts. No
behavioral change. No new abstraction.

**Findings addressed:** P1-01, P1-02, P3-01, P3-02, P3-03.

### Work items

1. **Correct mark_outstanding authority documentation.**
   The header comment says context marks; implementation shows backend marks.
   Decision: confirm backend is the marking authority; update
   `async_io_context.hpp:68-70` comment.

2. **Document the two-phase completion model.**
   Name the states explicitly: backend-ready (result in backend structure) vs.
   completion-ready (Completion::complete_with called by poll/wait_one). Add to
   as-built doc and ADR-async-io-model.

3. **Qualify "worker" terminology.**
   Audit all documentation uses of "worker" and prefix with subsystem:
   "scheduler worker," "offload thread," "pool worker."

4. **Evaluate ThreadPoolBackend naming.**
   The name implies a bounded pool. Options: rename (breaking), add doc
   qualifier, or defer to Phase 3 when the implementation changes.

### Decision required

- Is `mark_outstanding` authority definitively the backend's? (Evidence says
  yes, but an explicit ADR statement is needed.)
- Is the two-phase model (backend-ready → completion-ready) the permanent
  contract, or an implementation detail that may change in Phase 2?

### Dependencies

None. This phase is pure documentation and comment correction.

### Risks

- Renaming a public type (`ThreadPoolBackend`) is an API break. Must be
  weighed against the naming clarity benefit.

### Tests

- No production behavior change; existing tests MUST pass unchanged.
- If comments in headers change, verify no doxygen/doc tooling breaks.

### Exit criteria

- [ ] Header comment matches implementation for mark_outstanding.
- [ ] Two-phase completion model documented in ADR.
- [ ] All "worker" references qualified in architecture docs.
- [ ] Naming decision recorded (rename now or defer).

### Out of scope

- Changing who calls mark_outstanding (implementation is correct).
- Modifying any production logic.
- Introducing new types or abstractions.

---

## Phase 1 — Explicit Operation Ownership and Submission Transaction

**Goal:** Design (not implement) an explicit accepted-operation ownership model
that makes the submission transaction auditable and the operation identity
queryable.

**Findings addressed:** P0-01 (root cause), P2-03, DIV-02, DIV-12.

### Work items

1. **Define the submission linearization point.**
   At what exact instruction does "submit succeeded" become true? Current:
   after `workers_.emplace_back()` returns without throwing. Document this.

2. **Evaluate caller-owned operation storage.**
   Zig's `Operation.Storage` eliminates backend allocation. Is a Sluice
   equivalent feasible? What would the caller-facing API look like? Does
   `Completion<T>` absorb this role, or is a new `OperationSlot` needed?

3. **Define bounded capacity and queue-full semantics.**
   What is the maximum outstanding operation count? What error is returned
   when full? Is it `would_block`, `busy`, or a new code? How does this
   interact with Runtime admission?

4. **Define backend scratch contract.**
   If the caller provides storage, what may the backend write into it? How
   many bytes? What alignment? Is it reusable after reap?

5. **Design allocation-free terminal publication.**
   The ready-queue push (P0-01 root cause) must not allocate. Options:
   intrusive list, pre-allocated ring, or caller-provided completion slot.

### Decision required

- Does Sluice adopt caller-owned operation storage (Zig-faithful), or keep
  Completion-only with bounded backend allocation?
- What is the capacity model: per-backend, per-context, or per-runtime?
- Is queue-full a synchronous error or a suspension point?

### Dependencies

- Phase 0 complete (authority model is unambiguous).
- As-built doc and conformance map current.

### Risks

- Caller-owned storage increases API complexity.
- Bounded capacity may break existing tests that assume unlimited submit.
- Intrusive list requires address-stable operation records (layout constraint).

### Tests

- Design-phase: no implementation tests yet.
- Post-implementation: OOM injection at every allocation point; verify no
  terminate; verify Completion reaches terminal under all failure modes.
- Resource-bound test: submit to capacity, verify synchronous error.

### Exit criteria

- [ ] ADR or design doc accepted for operation ownership model.
- [ ] Submission linearization point defined and documented.
- [ ] Capacity model decided (bounded N with error code).
- [ ] Terminal publication path is allocation-free by design.
- [ ] Conformance map updated (DIV-02 status change if applicable).

### Out of scope

- Implementing the design (this phase produces the design only).
- Changing the Scheduler wake model (Phase 2).
- Changing thread management (Phase 3).

---

## Phase 2 — Unified Progress/Wake Integration

**Goal:** Design a backend-readiness wake capability that reduces or eliminates
the 2ms observation interval for backend-only waits, while preserving the
decoupled lock ordering.

**Findings addressed:** P2-04, DIV-04, DIV-05.

### Work items

1. **Evaluate backend → Scheduler wake signal.**
   Can the backend notify the Scheduler's wake_cv_ (or equivalent) when a
   result is ready, WITHOUT acquiring global_mtx_? What lock ordering is safe?

2. **Define the wake bridge contract.**
   If a wake signal is added, what is the API? A callback? A condition
   variable pointer injected at construction? An eventfd?

3. **Evaluate MIXED-WAKE simplification.**
   If backend wake is instant, does MIXED-WAKE mode still need the 2ms
   interval? Can the MW-S2 participant park on a unified signal?

4. **Preserve decoupled domains.**
   The current P3 decoupling exists to avoid backend_mtx → global_mtx
   ordering. Any wake bridge MUST preserve this or explicitly approve a new
   lock order via ADR.

5. **Unify across backends.**
   ThreadPoolBackend, UringAsyncBackend, FakeAsyncBackend, and SyncBackend
   must all conform to the same wake contract (even if some are no-ops).

### Decision required

- Is the 2ms interval reduced, eliminated, or kept as defense-in-depth?
- What is the wake bridge API shape?
- Does this change the AsyncBackend vtable?

### Dependencies

- Phase 1 design accepted (operation ownership may affect how wake is
  associated with an operation).
- Lock ordering analysis complete.

### Risks

- Introducing a wake callback into the backend interface couples backend to
  Scheduler lifetime.
- Uring backend has its own completion mechanism; a generic wake bridge may
  not fit cleanly.
- Removing 2ms may expose latent lost-wake bugs in edge cases.

### Tests

- Deterministic causal test: backend completes → Scheduler observes within
  bounded time (not 2ms).
- TSan: no data race on wake signal.
- Liveness: single-worker + backend wait + external wake coexist without
  starvation.
- Negative: remove wake bridge → verify 2ms fallback still provides liveness.

### Exit criteria

- [ ] Wake bridge contract documented and ADR-approved.
- [ ] Lock ordering preserved or explicitly changed via ADR.
- [ ] All backends conform to the wake contract.
- [ ] 2ms role reclassified (defense-in-depth or removed).
- [ ] Conformance map DIV-04/DIV-05 updated.

### Out of scope

- Implementing persistent workers (Phase 3).
- Changing Completion publication authority (remains poll/wait_one).
- Adding coroutine or P2300 integration.

---

## Phase 3 — Portable Blocking-I/O Offload

**Goal:** Design and implement a bounded, reusable blocking-I/O offload
mechanism to replace per-op thread creation.

**Findings addressed:** P2-01, P2-02, P2-03, DIV-03, DIV-12.

### Work items

1. **Evaluate persistent worker models.**
   Fixed-size worker pool with task queue? Work-stealing? Single dedicated
   I/O thread with batched submission? Each has different latency/throughput
   tradeoffs.

2. **Define queue capacity and backpressure.**
   Bounded queue with synchronous `would_block` on full? Or blocking submit
   with timeout? How does this interact with Runtime admission and Scheduler
   MW-S2?

3. **Define worker lifecycle.**
   When are workers created? At backend construction or on first submit?
   How are they joined at shutdown? What if a worker crashes?

4. **Define allocation-free accepted-op path.**
   With Phase 1's operation storage design, the offload mechanism should
   accept an operation without heap allocation. Pre-allocated request slots?
   Intrusive submission queue?

5. **Define cancellation of queued vs. running requests.**
   Queued request: remove from queue, complete with cancelled.
   Running request: best-effort (current behavior) or signal-based (DIV-10)?

6. **Shutdown drain.**
   All queued requests must be completed or cancelled before backend
   destruction returns. Running requests must be joined.

### Decision required

- Worker count: fixed, configurable, or adaptive?
- Queue model: bounded ring, bounded deque, or unbounded with backpressure?
- Is this a new backend implementation or a refactor of ThreadPoolBackend?
- Does the public name change?

### Dependencies

- Phase 1 design implemented (operation storage, bounded capacity).
- Phase 2 wake bridge available (so offload completion wakes Scheduler
  instantly).

### Risks

- Persistent workers introduce idle-thread resource cost.
- Bounded queue may reject ops that current code accepts (behavior change).
- Worker crash handling adds complexity.
- Migration from ThreadPoolBackend requires all tests to pass on new impl.

### Tests

- Resource-bound: submit to capacity → verify error.
- OOM: inject at every allocation point → verify no terminate.
- Shutdown: submit N, immediately destroy → verify all N reach terminal.
- TSan: concurrent submit + poll + cancel + shutdown.
- Benchmark: compare per-op thread vs. persistent pool (latency, throughput).
- Backend conformance: new implementation passes all existing conformance tests.

### Exit criteria

- [ ] Bounded capacity with documented default and maximum.
- [ ] Queue-full returns synchronous error.
- [ ] No per-op thread creation on the hot path.
- [ ] No per-op heap allocation on the hot path (if Phase 1 storage adopted).
- [ ] workers_ equivalent does not grow monotonically.
- [ ] All existing backend conformance tests pass.
- [ ] Benchmark evidence of improvement (or documented tradeoff).
- [ ] DIV-03 and DIV-12 status updated.

### Out of scope

- io_uring backend changes (separate concern).
- Scheduler internal changes (Phase 2 handles wake).
- Public API changes beyond capacity configuration.

---

## Phase 4 — Strategy Cleanup and Capability Façade

**Goal:** Evaluate whether to formally define a Threaded execution strategy,
provide a lightweight Io capability façade, and clean up remaining naming and
layering issues.

**Findings addressed:** DIV-01, DIV-03 (naming), AC-8 (long-term).

### Work items

1. **Evaluate lightweight Io capability façade.**
   Should Sluice provide a copyable, non-owning `IoRef` or `IoCapability`
   that delegates to `AsyncIoContext`? This would move toward Zig's model
   without breaking the owning context.

2. **Formalize Threaded strategy.**
   Group Threaded mode is thread-per-task. Is this a first-class execution
   strategy with its own ADR, or an implementation detail of Group?

3. **Separate naming definitively.**
   If Phase 3 renamed the backend, ensure all documentation, tests, and
   comments use consistent terminology.

4. **Evaluate owning Runtime vs. capability injection.**
   Is ApplicationRuntime the permanent top-level owner, or should the design
   allow standalone Scheduler + AsyncIoContext without Runtime?

### Decision required

- Is a lightweight façade worth the API surface increase?
- Is Threaded strategy formalized or left as Group implementation detail?
- Is Runtime the only supported entry point?

### Dependencies

- Phases 0-3 complete.
- Usage patterns from real applications (sluice-copy, benchmarks).

### Risks

- Façade may create confusion about which object to pass.
- Formalizing Threaded may over-constrain Group's flexibility.

### Tests

- API acceptance tests for any new public surface.
- Negative-compile tests for capability misuse.

### Exit criteria

- [ ] Decision recorded for each work item (implement or defer).
- [ ] If implemented: conformance map and divergence registry updated.
- [ ] Naming audit complete across all docs and code.

### Out of scope

- Networking, timers, or non-file I/O.
- Coroutine/P2300 integration.
- Multi-runtime or nested runtime topologies.

---

## Phase Dependency Graph

```text
Phase 0 (authority correction)
    ↓
Phase 1 (operation ownership design)
    ↓
Phase 2 (wake integration design)
    ↓
Phase 3 (blocking-I/O offload implementation)
    ↓
Phase 4 (strategy cleanup)
```

Phases 1 and 2 may overlap in design work, but Phase 3 implementation depends
on both being decided. Phase 4 is independent of Phase 3 implementation details
but benefits from Phase 3 being complete.
