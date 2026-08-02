# Remediation Roadmap

**Purpose:** Ordered sequence of design and implementation work derived from
the architecture audit findings. This roadmap does NOT prescribe solutions — it
prescribes the order in which decisions must be made.

**Baseline:** `d299fc0`. Derived from `current-architecture-findings.md`,
`zig-io-conformance-map.md`, and `divergence-registry.md`.

**Principle:** Each phase produces a design decision (ADR or design doc) BEFORE
implementation begins. No phase may skip the design compliance gate.

**Core ordering principle (round-2/3 review):** The remediation main line is:

```text
unforgeable Completion authority
→ stable Request identity / provenance / generation
→ identity-preserving reap
→ transactional backend admission
→ wait/cancel concurrency
→ wake integration
→ blocking offload optimization
```

**Key structural insight (round-3):** Request identity, admission transaction,
and reap contract are NOT three independent protocols. They are one request
lifecycle. They must be designed in a single unified ADR, then implemented
together in a reference backend, then migrated to production backends.

---

## Phase 0A — Audit and Governance Baseline (#60)

**Goal:** Establish the governance framework: as-built documentation,
constitution, compliance gate, findings, divergence registry, conformance
map, and this roadmap. No behavioral change. No new abstraction.

**Status:** Complete in PR #60.

### Deliverables (all in #60)

- As-built async architecture documented.
- Architecture constitution with AC-N rules.
- Design compliance gate checklist.
- Current architecture findings (P0/P1/P2/P3).
- Divergence registry (DIV-01..DIV-13).
- Zig I/O conformance map (18 items).
- This remediation roadmap.
- CI formal verification wiring.

### Exit criteria

- [x] All governance documents landed.
- [x] verify-architecture-docs.py passes.
- [x] check-doc-links.py passes.

---

## Phase 0B — Contract Alignment Corrective (Follow-up PRs)

**Goal:** Eliminate documentation/implementation authority conflicts and
classification errors identified during audit. Small focused PRs.

**Findings addressed:** P1-01, P1-02, P1-03, P1-05, P3-01, P3-02, P3-03.

### Work items

1. **Correct mark_outstanding authority documentation.**
   The header comment says context marks; implementation shows backend marks.
   Decision: confirm backend is the marking authority; update
   `async_io_context.hpp:68-70` comment.

2. **Document the two-phase completion model.**
   Name the states explicitly: backend-ready (result in backend structure) vs.
   completion-ready (Completion::complete_with called by poll/wait_one). Add to
   as-built doc and ADR-async-io-model.

3. **Register SyncBackend cancel authority bypass.**
   `SyncBackend::cancel()` calls `complete_with()` directly, bypassing
   poll/wait_one reap. Either defer completion to poll, or document as
   accepted test-only violation with explicit justification.

4. **Separate error vocabulary.**
   `queue_full_retries` conflates lifecycle violation (invalid_state) with
   capacity pressure. Rename or split the stat.

5. **Qualify "worker" terminology.**
   Audit all documentation uses of "worker" and prefix with subsystem.

6. **Evaluate ThreadPoolBackend naming.**
   The name implies a bounded pool. Defer to Phase 6 when implementation
   changes.

### Dependencies

- Phase 0A complete (governance framework exists).

### Exit criteria

- [ ] Header comment matches implementation for mark_outstanding.
- [ ] Two-phase completion model documented in ADR.
- [ ] SyncBackend cancel authority registered (P1-03 resolved or accepted).
- [ ] Error vocabulary separation designed (P1-05).
- [ ] All "worker" references qualified in architecture docs.
- [ ] Naming decision recorded (rename now or defer).

### Out of scope

- Changing who calls mark_outstanding (implementation is correct).
- Modifying any production logic.
- Introducing new types or abstractions.

---

## Phase 1 — Explicit I/O Request Contract (Unified ADR)

**Goal:** Design (not implement) a unified request lifecycle contract that
resolves authority, identity, admission, reap, and waiter questions as ONE
coherent model. These cannot be separate protocols.

**Findings addressed:** P0-03, P0-01 (root cause), P0-02 (root cause),
P1-04, P1-06, P1-07, P1-10, P2-03, P2-05, DIV-02, DIV-12, DIV-13.

### The unified contract must decide

1. **Completion publication authority.**
   mark_outstanding / complete_with are backend-only (AC-13).
   reset is caller-accessible but state-checked (ready→idle; idle→no-op;
   outstanding→fail-fast; see ADR-explicit-io-completion-authority).
   Negative-compile gate for publication mutators.
   CAS-based claim + single-winner publish (exactly-once).
   Release fail-fast on invalid transitions and outstanding destruction.

2. **Operation descriptor and request identity.**
   ```text
   RequestKey:
       slot index (bounded arena position)
       generation (monotonic per-slot reuse counter)
       backend/context identity (provenance)
   ```
   Raw pointer may be a locating optimization but NOT sole logical identity.

3. **Provenance and generation.**
   Each accepted request binds: context identity, operation kind, fd, buffer,
   offset, Completion reference. ABA prevention via generation.

4. **Completion binding.**
   RequestSlot → Completion is 1:1 while outstanding.
   Completion → RequestSlot back-reference enables provenance check at await.

5. **Admission transaction (5-phase).**
   ```text
   reserve   → allocate slot, queue position, SQE (not visible to executor)
   prepare   → fill operation, Completion binding, generation
   commit    → atomic bind Completion ↔ RequestKey; state reserved → pending
                 submit_* returns success from this point
   enqueue   → make visible to backend driver (MUST be noexcept/alloc-free)
                 intrusive queue push or pending_sqes append
   dispatch  → worker dequeue OR io_uring_submit()
                 MAY fail, MAY partially succeed (prefix acceptance)
                 failure → terminal error completion for affected requests
   ```
   Linearization point is commit. Failure before commit → rollback to free.
   After commit, the request is accepted and MUST NOT be lost; dispatch
   failure is reported as a terminal error completion, not a lost request.
   `io_uring_submit()` is fallible; the noexcept guarantee applies only to
   the enqueue step (userspace visibility), not kernel acceptance.

6. **Request state machine.**
   ```text
   free → reserved → pending → enqueued → dispatched/kernel-owned → backend-ready → reaped → free
   ```
   For blocking workers, enqueued → dispatched is worker dequeue.
   For io_uring, enqueued → dispatched is io_uring_submit() acceptance.
   Dispatch failure transitions directly to terminal-error (reapable).

7. **Backend-ready result and identity-bearing reap.**
   Backend provides ready request identities upward (not just count).
   Scheduler and Batch receive identity events, not scan all Completions.
   Remove global reap_seq.

8. **Waiter cardinality.**
   Explicit: single-waiter or multi-waiter with registration tokens.
   Current map-overwrite behavior is forbidden.

9. **Cancel targeting.**
   Cancel targets RequestKey (slot + generation), not raw address.
   Returns disposition (requested / already_terminal / not_found / not_supported).

10. **Caller acquisition of RequestKey.**
    Decide how callers obtain the identity needed for cancel:
    - A: `submit_*` returns `Result<RequestHandle>` (handle holds RequestKey).
    - B: Completion exposes opaque `request_key()` while outstanding.
    - C: Public cancel accepts `Completion&`; context internally resolves
      and validates generation (no user-visible RequestKey).
    Leaning: C for API compatibility, with internal RequestKey extraction
    from Completion's unforgeable binding. Users never manually construct
    slot + generation + context id.

11. **fd/buffer borrow lifetime.**
    Borrow interval is part of the unified request contract:
    ```text
    borrow begins: commit (successful submit linearization)
    borrow ends:   Completion reaches terminal ready state through reap
    ```
    Guarantees within the borrow interval:
    - WriteOp source: byte-stable (no mutation).
    - ReadOp destination: exclusive (no other writer).
    - fd: not closed or reused.
    - Cancel intent does NOT release resources.
    - Wait cancellation does NOT cancel the operation or release buffer.
    - Kernel dispatch failure releases borrow ONLY after terminal error
      is published and reaped.

12. **Resource capacity.**
    Bounded slot arena. Full → synchronous would_block.
    Per-backend or per-context capacity (decide).

13. **Shutdown / drain.**
    Destruction requires quiescent state (outstanding == 0).
    Terminating accepted requests is an EXPLICIT drain/shutdown operation,
    NOT implicit in destruction. Preserve current L11 fail-fast contract.

14. **AsyncBackend public-vs-internal (DIV-13).**
    Decide: truly internal (selector/config API) or formally public
    (backend author contract + conformance suite). This decision gates
    the authority mechanism.

### Decision required

- Caller-owned storage (Zig-faithful) or backend-owned bounded arena?
- Is AsyncBackend a public extension point?
- Single-waiter or multi-waiter?
- Capacity model: per-backend, per-context, or per-runtime?

### Dependencies

- Phase 0B complete (authority model is unambiguous).

### Risks

- Large design surface; may require multiple design iterations.
- DIV-13 decision has cascading impact on authority mechanism.
- Bounded capacity may break existing tests assuming unlimited submit.

### Tests

- Design-phase: no implementation tests yet.
- Post-implementation: see Phase 2.

### Exit criteria

- [ ] ADR or design doc accepted covering all 14 points above.
- [ ] RequestSlot state machine defined with 5-phase admission.
- [ ] Capacity model decided.
- [ ] DIV-02 resolved (operation storage permanent or transitional).
- [ ] DIV-13 resolved (backend public or internal).
- [ ] Conformance map updated.

### Out of scope

- Implementing the design (Phase 2).
- Changing Scheduler wake model (Phase 5).
- Changing thread management (Phase 6).

---

## Phase 2 — Reference Implementation + Conformance Suite

**Goal:** Implement the Phase 1 contract in FakeBackend and SyncBackend FIRST,
proving the unified model before touching production backends. Build the
backend-agnostic conformance suite.

**Findings addressed:** P0-02 (proof of fix), P0-03 (enforcement),
P1-04 (contract proof), P1-07 (reap proof).

### Work items

1. **Implement CompletionAccess / authority mechanism.**
   Publication mutators no longer public. Negative-compile gate in CI.
   Test-only access via SLUICE_ASYNC_INTERNAL_TESTING.

2. **Implement RequestKey / minimal RequestSlot.**
   Bounded slot arena with Phase 1 state machine. Generation counter.
   Provenance stamp.

3. **Implement identity-bearing reap.**
   FakeBackend and SyncBackend provide ready identities (not just count).
   Validate ordering, identity, and exactly-once via independent test sink.
   Do NOT modify Scheduler in this phase.

4. **Implement transactional admission.**
   5-phase (reserve → prepare → commit → enqueue → dispatch). Rollback on
   failure before commit. Dispatch failure → terminal error completion.
   Capacity exhaustion → synchronous would_block.

5. **Implement allocation-free terminal publication.**
   Ready entries use intrusive linkage or pre-allocated pool.

6. **Build conformance test suite.**
   Backend-agnostic parameterized suite verifying:
   - Transactional submit (rollback on failure)
   - Capacity exhaustion (synchronous error)
   - Exactly-once publication
   - Identity-bearing reap
   - Allocation-free accepted-op terminal path
   - Shutdown: explicit drain → quiescent → destroy (NOT destroy-with-outstanding)
   - Negative-compile: caller cannot call publication mutators

7. **Migrate existing tests.**
   Tests using public mutators → test-only access path.

### Destruction / drain contract

Tests MUST follow the explicit drain pattern:
```text
submit N ops
→ explicit drain/reap until outstanding == 0
→ destroy backend/context
```

Tests MUST NOT test "destroy with outstanding → all terminal" because the
current contract is: destruction with outstanding = contract violation =
std::terminate. If a future `shutdown(ShutdownMode)` API is added (Phase 1
design), tests target that explicit API, not implicit destructor behavior.

### Dependencies

- Phase 1 design accepted.

### Tests

- OOM injection at every allocation point; verify no terminate.
- Capacity exhaustion: submit to full → would_block.
- Concurrent submit + cancel + poll under TSan.
- Exactly-once publication under all failure modes.
- Explicit drain → quiescent → destroy: verify clean shutdown.
- Negative-compile: caller cannot call mark_outstanding/complete_with.
- Death test: destroy outstanding → trap.
- Death test: reset on outstanding → trap.
- ABA test: reuse Completion, stale cancel → rejected (generation mismatch).

### Exit criteria

- [ ] FakeBackend passes full conformance suite.
- [ ] SyncBackend passes full conformance suite (or moved to test-only).
- [ ] No allocation on accepted-op terminal path.
- [ ] Transactional submit proven (5-phase rollback works).
- [ ] Identity-bearing reap proven.
- [ ] Negative-compile gate in CI.
- [ ] Conformance suite is backend-agnostic (parameterized).

### Out of scope

- UringBackend, ThreadPoolBackend migration (Phase 3).
- Scheduler wake changes (Phase 5).

---

## Phase 3 — Backend Migration + Scheduler Integration

**Goal:** Bring UringAsyncBackend and ThreadPoolBackend into conformance with
the Phase 1 contract, validated by the Phase 2 conformance suite. Then
perform the UNIFIED Scheduler integration for all backends at once.

### Work items

1. **UringBackend migration.**
   Replace current multi-container identity with unified RequestSlot.
   SQE user_data = RequestSlot index + generation. CQE → same RequestSlot.
   Fix non-atomic mark-then-allocate (P0-02 uring instance).
   Implement fallible dispatch: io_uring_submit() partial prefix acceptance
   → terminal error for unaccepted SQEs.

2. **ThreadPoolBackend interim conformance.**
   Apply transactional admission to current per-op model. Fix P1-04
   (spawn failure → synchronous error, not asyncized). Full replacement
   deferred to Phase 6.

3. **Unified Scheduler integration (all backends at once).**
   Migrate or adapt ALL backends to identity-bearing reap first, then:
   Scheduler receives ready identities from reap (not O(N) scan).
   Remove waiting_completion_ full-map scan.
   Batch uses identity events (remove global reap_seq).
   No mixed-mode where some backends use new reap and others scan.

4. **Run conformance suite against all backends.**

### Dependencies

- Phase 2 complete (conformance suite exists, reference proven).

### Exit criteria

- [ ] UringBackend passes conformance suite.
- [ ] ThreadPoolBackend passes conformance suite (interim model).
- [ ] Scheduler uses identity-bearing reap (no full scan).
- [ ] Batch does not use global reap_seq.
- [ ] P1-04 resolved (spawn failure synchronous).
- [ ] P0-02 resolved for all backends.

### Out of scope

- Persistent workers (Phase 6).
- Wake integration (Phase 5).
- Wait/cancel redesign (Phase 4).

---

## Phase 4 — Wait/Cancel Concurrency Redesign

**Goal:** Resolve the wait_one lock-holding problem, make cancel explicit,
and enforce provenance at await boundaries.

**Findings addressed:** P1-08, P1-09, P1-10.

### Work items

1. **Resolve wait_one lock holding.**
   Document AsyncIoContext as single-driver OR split locks.

2. **Design cancel interrupt channel.**
   Cancel must reach backend without waiting for wait_one to release lock.

3. **Redesign cancel API.**
   `Result<CancelDisposition> request_cancel(RequestKey)`.

4. **Enforce await provenance.**
   Release fail-fast on wrong-context or idle Completion.

5. **Define waiter cardinality.**
   Explicit policy. Silent overwrite forbidden.

6. **Fix idle-await Release hang.**
   Fail-fast, not permanent park.

### Dependencies

- Phase 2 (RequestKey for cancel targeting).
- Phase 3 (backends conform).

### Exit criteria

- [ ] wait_one does not block cancel (or single-driver documented).
- [ ] Cancel returns disposition targeting RequestKey.
- [ ] Await provenance enforced in Release.
- [ ] Waiter cardinality policy enforced.
- [ ] No permanent hang for idle/wrong-context await.

### Out of scope

- Wake integration (Phase 5).
- Persistent workers (Phase 6).

---

## Phase 5 — Backend → Scheduler Progress Signal

**Goal:** Design a narrow backend-readiness wake capability that reduces or
eliminates the 2ms observation interval, while preserving decoupled lock
ordering.

**Findings addressed:** P2-04, DIV-04, DIV-05.

### Work items

1. **Evaluate backend → Scheduler wake signal.**
2. **Define the wake bridge contract** (narrow capability: notify only).
3. **Evaluate MIXED-WAKE simplification.**
4. **Preserve decoupled domains** (no backend_mtx → global_mtx ordering).
5. **Unify across backends.**

### Dependencies

- Phase 3 complete (backends conform; operation identity stable).

### Exit criteria

- [ ] Wake bridge contract documented and ADR-approved.
- [ ] Lock ordering preserved or explicitly changed via ADR.
- [ ] All backends conform to the wake contract.
- [ ] 2ms role reclassified (defense-in-depth or removed).
- [ ] DIV-04/DIV-05 updated.

### Out of scope

- Persistent workers (Phase 6).
- Coroutine/P2300 integration.

---

## Phase 6 — Persistent Blocking-I/O Workers

**Goal:** Design and implement a bounded, reusable blocking-I/O offload
mechanism to replace per-op thread creation.

**Findings addressed:** P2-01, P2-02, P2-03, DIV-03, DIV-12.

### Work items

1. **Evaluate persistent worker models.**
2. **Define queue capacity and backpressure.**
3. **Define worker lifecycle.**
4. **Define allocation-free accepted-op path.**
5. **Define cancellation of queued vs. running requests.**
6. **Shutdown drain** (explicit drain → quiescent → destroy).

### Destruction / drain contract

Same as Phase 2: destruction requires quiescent state. Worker shutdown is an
explicit operation (close admission → drain queue → join workers → destroy).
NOT implicit in destructor.

### Dependencies

- Phase 3 complete (conformance suite, transactional admission).
- Phase 5 complete (wake bridge for instant completion notification).

### Exit criteria

- [ ] Bounded capacity with documented default and maximum.
- [ ] Queue-full returns synchronous error.
- [ ] No per-op thread creation on the hot path.
- [ ] No per-op heap allocation on the hot path.
- [ ] workers_ equivalent does not grow monotonically.
- [ ] All backends pass conformance suite.
- [ ] Benchmark evidence of improvement (or documented tradeoff).
- [ ] DIV-03 and DIV-12 resolved.

### Out of scope

- io_uring backend changes (Phase 3 handles Uring).
- Scheduler internal changes (Phase 5 handles wake).

---

## Phase Dependency Graph

```text
Phase 0 (fact & classification correction — this PR)
    ↓
Phase 1 (Explicit I/O Request Contract — unified ADR)
    ↓
Phase 2 (reference implementation + conformance suite)
    ↓
Phase 3 (backend migration: Uring + ThreadPool + Scheduler)
    ↓
Phase 4 (wait/cancel concurrency redesign)
    ↓
Phase 5 (backend → Scheduler progress signal)
    ↓
Phase 6 (persistent blocking-I/O workers)
```

Phases 4 and 5 may overlap. Phase 6 depends on Phases 3 (conformance suite)
and 5 (wake bridge).

**Key principle:** Do NOT rewrite Runtime first. Bottom-layer authority and
identity must stabilize before higher-layer integration can simplify.
Request identity, admission, and reap are ONE lifecycle — designed together,
implemented together, migrated together.
