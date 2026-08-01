# Remediation Roadmap

**Purpose:** Ordered sequence of design and implementation work derived from
the architecture audit findings. This roadmap does NOT prescribe solutions — it
prescribes the order in which decisions must be made.

**Baseline:** `d299fc0`. Derived from `current-architecture-findings.md`,
`zig-io-conformance-map.md`, and `divergence-registry.md`.

**Principle:** Each phase produces a design decision (ADR or design doc) BEFORE
implementation begins. No phase may skip the design compliance gate.

**Core ordering principle (round-2 review):** The remediation main line is:

```text
unforgeable Completion authority
→ stable Request identity / provenance / generation
→ identity-preserving reap
→ transactional backend admission
→ wait/cancel concurrency
→ wake integration
→ blocking offload optimization
```

This replaces the earlier symptom-driven order (authority comment → operation
storage → wake → persistent pool).

---

## Phase 0 — Fact and Classification Correction (This PR)

**Goal:** Eliminate documentation/implementation authority conflicts and
classification errors. No behavioral change. No new abstraction.

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
   capacity pressure. Rename or split the stat. Define distinct error codes:
   invalid_state (lifecycle), would_block (capacity), no_space (OOM),
   backend_error (fatal).

5. **Qualify "worker" terminology.**
   Audit all documentation uses of "worker" and prefix with subsystem:
   "scheduler worker," "offload thread," "pool worker."

6. **Evaluate ThreadPoolBackend naming.**
   The name implies a bounded pool. Options: rename (breaking), add doc
   qualifier, or defer to Phase 7 when the implementation changes.

### Decision required

- Is `mark_outstanding` authority definitively the backend's? (Evidence says
  yes, but an explicit ADR statement is needed.)
- Is the two-phase model (backend-ready → completion-ready) the permanent
  contract, or an implementation detail that may change in Phase 4?
- Is SyncBackend a test-only backend (AC-5 exception acceptable) or must it
  conform fully?

### Dependencies

None. This phase is pure documentation and comment correction.

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

## Phase 1 — Completion Authority Hardening

**Goal:** Make Completion state transitions structurally unforgeable. This
precedes request pool design because the pool requires a trustworthy
Completion state machine.

**Findings addressed:** P0-03, P1-10 (partial), AC-13 enforcement.

### Work items

1. **Restrict mutation authority.**
   `mark_outstanding()`, `complete_with()`, `reset()` MUST NOT be callable
   by arbitrary application code. Options:
   - `friend class CompletionAccess` + capability token;
   - Private mutators + backend-accessible accessor;
   - Internal-only header not installed.
   The specific mechanism is a design decision, but the invariant is:
   caller code cannot compile against backend mutators.

2. **Prevent outstanding reset.**
   `reset()` on an outstanding Completion is a contract violation. Must
   fail-fast in Release (not just Debug assert).

3. **Prevent outstanding destruction.**
   Destructor of an outstanding Completion is use-after-free for the backend.
   Must fail-fast or trap in Release.

4. **Prevent double submit.**
   Submitting a non-idle Completion must be a synchronous error (already
   partially enforced by backend `idle()` check, but the check is not
   atomic — two contexts can race).

5. **Prevent cross-context submit.**
   A Completion submitted via Context A must not be submittable via Context B.
   Requires provenance binding (see Phase 2) or at minimum a context-identity
   stamp checked at submit.

6. **Atomic exactly-once transition.**
   `mark_outstanding()` must be CAS (compare_exchange), not
   load-assert-store. Two concurrent submitters must not both succeed.

7. **Negative-compile gate.**
   Add compile-fail tests proving that caller code cannot call
   mark_outstanding / complete_with / reset.

8. **Migrate existing tests.**
   Tests that directly call backend mutators must use the test-only access
   path (`SLUICE_ASYNC_INTERNAL_TESTING` guard).

### Decision required

- Friend/capability vs. internal-header vs. passkey idiom?
- Is Release fail-fast via `std::terminate`, `abort()`, or `Result` return?
- What is the test-only access mechanism?

### Dependencies

- Phase 0 complete (authority model is unambiguous).

### Risks

- Breaking existing tests that use public mutators (migration cost).
- Custom backend authors lose direct access (see DIV-13 decision).
- ABI break if Completion layout changes.

### Tests

- Negative-compile: caller cannot call mark_outstanding/complete_with/reset.
- Death test: reset on outstanding → trap.
- Death test: destroy outstanding → trap.
- TSan: concurrent submit to same Completion from two contexts → exactly one
  succeeds.
- All existing tests pass via test-only access path.

### Exit criteria

- [ ] Public header no longer exposes backend mutators.
- [ ] Negative-compile gate in CI.
- [ ] Release fail-fast on invalid transitions.
- [ ] CAS-based mark_outstanding.
- [ ] All tests migrated to test-only access.
- [ ] AC-13 satisfied structurally.

### Out of scope

- Request identity/generation (Phase 2).
- Reap redesign (Phase 3).
- Backend admission (Phase 4).

---

## Phase 2 — RequestKey / RequestSlot Design

**Goal:** Define stable request identity with provenance and generation,
eliminating ABA and enabling explicit cancel/await targeting.

**Findings addressed:** P1-06, P1-10, AC-14 enforcement.

### Work items

1. **Define RequestKey.**
   ```text
   RequestKey:
       slot index (bounded arena position)
       generation (monotonic per-slot reuse counter)
       backend/context identity (provenance)
   ```
   Raw pointer may be a locating optimization but MUST NOT be the sole
   logical identity.

2. **Define RequestSlot state machine.**
   ```text
   free → reserved → pending → executing/kernel-owned → backend-ready → reaped → free
   ```
   Each transition is atomic and generation-stamped.

3. **Define operation binding.**
   Each accepted request binds: operation kind, fd, buffer pointer, buffer
   length, offset, Completion reference. This makes buffer lifetime
   structurally visible (not just "caller remembers").

4. **Define Completion binding.**
   RequestSlot → Completion is 1:1 while outstanding. Completion →
   RequestSlot back-reference enables provenance check at await.

5. **Define waiter cardinality policy.**
   Explicit: 0 or 1 waiter per outstanding request? Or multi-waiter with
   registration tokens? Current map-overwrite behavior is forbidden either
   way.

6. **Define bounded slot arena.**
   Internal bounded arena (per-context or per-backend). Capacity is
   configurable. Full → synchronous `would_block`.

7. **Provenance check at await.**
   `await_completion()` verifies: Completion is outstanding AND bound to
   THIS context/backend. Release fail-fast on violation.

### Decision required

- Caller-owned storage (Zig-faithful) or backend-owned bounded arena?
- Per-context or per-backend slot ownership?
- Single-waiter or multi-waiter policy?
- Is generation 32-bit or 64-bit?

### Dependencies

- Phase 1 complete (Completion authority is trustworthy).

### Risks

- Slot arena introduces capacity limit (existing tests may assume unlimited).
- Generation check adds per-op overhead (minimal: one atomic increment).
- API complexity increases if caller must manage slots.

### Tests

- ABA test: reuse Completion address, verify stale cancel hits nothing.
- Generation test: delayed event targets old generation → rejected.
- Provenance test: await with wrong-context Completion → fail-fast.
- Multi-waiter test: second waiter → explicit error or correct wake.
- Capacity test: submit to arena full → would_block.

### Exit criteria

- [ ] ADR/design doc accepted for RequestKey/RequestSlot.
- [ ] Generation counter prevents ABA.
- [ ] Provenance binding prevents cross-context await hang.
- [ ] Waiter cardinality policy documented and enforced.
- [ ] Bounded capacity with synchronous error.
- [ ] Conformance map DIV-02 resolved.

### Out of scope

- Implementing the full slot arena (design phase; implementation in Phase 4).
- Reap contract changes (Phase 3).
- Wake integration (Phase 6).

---

## Phase 3 — Reap Contract Redesign

**Goal:** Backend provides ready request identities upward; higher layers
do NOT scan all Completions to recover information the backend already knew.

**Findings addressed:** P1-07, P2-05, AC-15 enforcement.

### Work items

1. **Define identity-bearing reap interface.**
   ```text
   AsyncBackend::reap_ready(ReadySink&)
   ```
   or equivalent visitor/callback. Each ready event carries:
   - RequestKey (slot + generation);
   - Completion pointer (for backward compat);
   - Operation kind.

2. **Preserve poll() count API as compatibility wrapper.**
   `AsyncIoContext::poll() -> count` remains, but internally uses
   identity-bearing reap. No public API break.

3. **Remove Scheduler O(N) Completion scan.**
   Scheduler receives ready identities from reap and routes only those
   Fibers. No full-map scan.

4. **Remove Batch global reap_seq.**
   Batch receives completion order from reap events. The process-wide
   static `next_reap_seq()` and per-Completion `reap_seq_` field are
   removed.

5. **Separate Batch rejection from completion.**
   Submit-time rejection produces `BatchOutcomeKind::rejected`, NOT a
   fake ready result with `reap_seq == 0`.

6. **Remove Completion Batch-specific metadata.**
   `reap_seq_` and `next_reap_seq()` are Batch-specific ordering hacks.
   They do not belong in the Completion primitive.

### Decision required

- ReadySink shape: callback, intrusive list, or small-vector return?
- Does this change the AsyncBackend vtable (breaking for custom backends)?
- Is reap ordering guaranteed (FIFO) or backend-defined?

### Dependencies

- Phase 2 design accepted (RequestKey exists for identity).

### Risks

- Vtable change breaks custom backends (coordinate with DIV-13 decision).
- Removing reap_seq may break Batch ordering guarantees if reap is unordered.
- Scheduler refactor is non-trivial (waiting_completion_ map redesign).

### Tests

- Identity test: submit 3 ops, complete in order 2-0-1, verify reap events
  carry correct identities in that order.
- Scheduler test: ready Fiber is routed without full scan (instrumentation
  or complexity assertion).
- Batch test: rejection and completion are distinct outcome kinds.
- Regression: all existing Scheduler/Batch tests pass.

### Exit criteria

- [ ] Backend provides identity-bearing reap.
- [ ] Scheduler does not scan all Completions.
- [ ] Batch does not use global reap_seq.
- [ ] Batch rejection ≠ completion (explicit enum).
- [ ] AC-15 satisfied.

### Out of scope

- Backend admission transaction (Phase 4).
- Wait/cancel concurrency (Phase 5).

---

## Phase 4 — Unified Backend Transactional Admission

**Goal:** All backends conform to a unified transactional submit contract:
either allocation-free submit, or mark_outstanding AFTER resource
acquisition. No backend may leave Completion outstanding on failure.

**Findings addressed:** P0-01 (root cause), P0-02 (root cause), P1-04,
P2-03, DIV-02, DIV-12.

### Work items

1. **Implement reference in FakeBackend first.**
   Bounded slot arena with Phase 2 state machine. Transactional admission.
   Capacity exhaustion. Allocation-free terminal publication. Conformance
   test suite.

2. **Migrate SyncBackend.**
   Apply transactional contract. Fix P1-03 (cancel defers to poll).
   Decide: test-only or conforming production backend.

3. **Migrate UringBackend.**
   Replace multi-container identity with unified RequestSlot.
   SQE user_data = RequestSlot index + generation.

4. **Migrate ThreadPoolBackend (interim).**
   Apply transactional contract to current per-op model. Full replacement
   deferred to Phase 7.

5. **Build backend-agnostic conformance suite.**
   Parameterized test suite verifying: transactional submit, capacity
   exhaustion, exactly-once publication, allocation-free terminal path,
   shutdown with outstanding ops.

### Decision required

- Is the conformance suite a public backend-author requirement (DIV-13)?
- What is the capacity model: per-backend, per-context, or per-runtime?
- Is queue-full a synchronous error or a suspension point?

### Dependencies

- Phase 2 design accepted (RequestSlot state machine).
- Phase 3 complete (reap provides identities for conformance checks).

### Risks

- OOM injection may reveal latent terminate paths.
- Bounded capacity may break existing tests assuming unlimited submit.
- Uring migration requires careful SQE/CQE identity mapping.

### Tests

- OOM injection at every allocation point; verify no terminate.
- Capacity exhaustion: submit to full → synchronous error.
- Concurrent submit + cancel + poll under TSan.
- Exactly-once publication under all failure modes.
- Shutdown with outstanding ops: all reach terminal.
- Conformance suite passes for all 4 backends.

### Exit criteria

- [ ] All backends pass conformance suite.
- [ ] No allocation on accepted-op terminal path.
- [ ] Transactional submit proven (rollback works).
- [ ] P0-01 root cause eliminated.
- [ ] P0-02 root cause eliminated.
- [ ] DIV-12 resolved (bounded capacity).

### Out of scope

- Persistent workers (Phase 7).
- Wake integration (Phase 6).
- Wait/cancel redesign (Phase 5).

---

## Phase 5 — Wait/Cancel Concurrency Redesign

**Goal:** Resolve the wait_one lock-holding problem, make cancel explicit,
and enforce provenance at await boundaries.

**Findings addressed:** P1-08, P1-09, P1-10, AC-1/AC-9 enforcement.

### Work items

1. **Resolve wait_one lock holding.**
   Options:
   - Document AsyncIoContext as single-driver (only one thread calls
     wait_one/poll; submit from other threads uses a separate lock);
   - Split access_mtx_ into submit-lock and wait-lock;
   - Interruptible wait (eventfd/pipe wakeup for cancel).
   The choice depends on whether shared-context concurrent submit is a
   supported use case.

2. **Design cancel interrupt channel.**
   If wait_one can block indefinitely, cancel must be able to reach the
   backend WITHOUT waiting for wait_one to release a lock. Options:
   - eventfd/pipe wakeup;
   - Separate cancel mutex;
   - Lock-free cancel submission.

3. **Redesign cancel API.**
   ```cpp
   enum class CancelDisposition {
       requested,
       already_terminal,
       not_found,
       not_supported,
   };
   Result<CancelDisposition> request_cancel(RequestKey);
   ```
   Cancel targets RequestKey (slot + generation), not raw Completion
   address.

4. **Enforce await provenance.**
   `await_completion()` verifies Completion is bound to this context.
   Release fail-fast on wrong-context or idle Completion.

5. **Define waiter cardinality.**
   Explicit single-waiter or multi-waiter policy. Current silent overwrite
   is forbidden. If single-waiter: second await returns error. If
   multi-waiter: registration token + wake-all.

6. **Fix idle-await Release hang.**
   In Release, awaiting an idle Completion must fail-fast (not silently
   park forever).

### Decision required

- Is AsyncIoContext single-driver or concurrent-submit?
- Cancel disposition: Result<enum> or void + out-param?
- Single-waiter or multi-waiter?
- Interrupt mechanism: eventfd, pipe, or condition_variable?

### Dependencies

- Phase 2 (RequestKey for cancel targeting).
- Phase 4 (backends conform to transactional contract).

### Risks

- Lock split may introduce new race conditions.
- Cancel disposition changes public API.
- Single-waiter enforcement may break existing patterns.

### Tests

- TSan: concurrent wait_one + cancel + submit.
- Cancel returns correct disposition for all cases.
- Wrong-context await → fail-fast in Release.
- Idle await → fail-fast in Release.
- Second waiter → explicit error (or correct multi-wake).
- Liveness: cancel interrupts blocking wait within bounded time.

### Exit criteria

- [ ] wait_one does not block cancel/submit (or single-driver documented).
- [ ] Cancel returns disposition.
- [ ] Cancel targets RequestKey, not raw address.
- [ ] Await provenance enforced in Release.
- [ ] Waiter cardinality policy documented and enforced.
- [ ] No permanent hang possible for idle/wrong-context await.

### Out of scope

- Wake integration (Phase 6).
- Persistent workers (Phase 7).

---

## Phase 6 — Backend → Scheduler Progress Signal

**Goal:** Design a narrow backend-readiness wake capability that reduces or
eliminates the 2ms observation interval, while preserving decoupled lock
ordering.

**Findings addressed:** P2-04, DIV-04, DIV-05.

### Work items

1. **Evaluate backend → Scheduler wake signal.**
   Can the backend notify the Scheduler's wake_cv_ (or equivalent) when a
   result is ready, WITHOUT acquiring global_mtx_? What lock ordering is safe?

2. **Define the wake bridge contract.**
   A narrow capability (e.g., `ProgressSignal::notify()`) that can ONLY:
   - Update persistent wake state;
   - Notify;
   - NOT acquire Scheduler global lock;
   - NOT route Fiber;
   - NOT call Completion.

3. **Evaluate MIXED-WAKE simplification.**
   If backend wake is instant, does MIXED-WAKE mode still need the 2ms
   interval? Can the MW-S2 participant park on a unified signal?

4. **Preserve decoupled domains.**
   The current P3 decoupling exists to avoid backend_mtx → global_mtx
   ordering. Any wake bridge MUST preserve this or explicitly approve a new
   lock order via ADR.

5. **Unify across backends.**
   All backends must conform to the same wake contract (even if some are
   no-ops).

### Decision required

- Is the 2ms interval reduced, eliminated, or kept as defense-in-depth?
- What is the wake bridge API shape?
- Does this change the AsyncBackend vtable?

### Dependencies

- Phase 4 complete (backends conform; operation identity stable).
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

- Implementing persistent workers (Phase 7).
- Changing Completion publication authority (remains poll/wait_one).
- Adding coroutine or P2300 integration.

---

## Phase 7 — Persistent Blocking-I/O Workers

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
   With Phase 2/4's operation storage design, the offload mechanism should
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

- Phase 4 complete (conformance suite, transactional admission).
- Phase 6 complete (wake bridge for instant completion notification).

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
- Backend conformance: new implementation passes Phase 4 conformance suite.

### Exit criteria

- [ ] Bounded capacity with documented default and maximum.
- [ ] Queue-full returns synchronous error.
- [ ] No per-op thread creation on the hot path.
- [ ] No per-op heap allocation on the hot path.
- [ ] workers_ equivalent does not grow monotonically.
- [ ] All existing backend conformance tests pass.
- [ ] Benchmark evidence of improvement (or documented tradeoff).
- [ ] DIV-03 and DIV-12 status updated.

### Out of scope

- io_uring backend changes (Phase 4 handles Uring).
- Scheduler internal changes (Phase 6 handles wake).
- Public API changes beyond capacity configuration.

---

## Phase Dependency Graph

```text
Phase 0 (fact & classification correction — this PR)
    ↓
Phase 1 (Completion authority hardening)
    ↓
Phase 2 (RequestKey / RequestSlot design)
    ↓
Phase 3 (reap contract redesign)
    ↓
Phase 4 (unified backend transactional admission)
    ↓
Phase 5 (wait/cancel concurrency redesign)
    ↓
Phase 6 (backend → Scheduler progress signal)
    ↓
Phase 7 (persistent blocking-I/O workers)
```

Phases 5 and 6 may overlap. Phase 7 depends on Phases 4 (conformance suite)
and 6 (wake bridge).

**Key principle:** Do NOT rewrite Runtime first. Bottom-layer authority and
identity must stabilize before higher-layer integration can simplify.
