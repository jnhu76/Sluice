# Architecture Constitution

**Authority:** Highest-level engineering principles for Sluice async I/O.
All designs, implementations, and reviews MUST satisfy these rules.
Violations require an approved ADR or an entry in the divergence registry.

**Baseline:** `b20bcc7` (master, including PR #60 and PR #61). Derived from
as-built code audit; proposed target contracts are labeled as such.

---

## AC-1. Explicit Capability Boundary

**Rule:** All I/O, wait, cancellation, and synchronization behavior MUST enter
through an explicit capability object held by the caller. No hidden global
runtime, no implicit default thread pool, no ambient executor.

**Rationale:** Zig `std.Io` is a lightweight copyable capability (userdata +
vtable). Sluice's `AsyncIoContext` is move-only and owning, but still explicit:
the caller must hold and pass it. This preserves auditability — every I/O path
is traceable to a capability holder.

**Required evidence:**
- Every `submit_*` call site holds a reference/pointer to `AsyncIoContext` or
  `RuntimeTaskContext`.
- No free function or static method performs I/O without a capability parameter.
- Backend construction requires explicit injection (no service locator).

**Allowed exceptions:**
- `BlockingIoPool` in the synchronous core is a bounded, explicitly-constructed
  pool. It is not an ambient executor.
- Test fixtures may construct capabilities inline.

**Common violations:**
- Adding a static/global `AsyncIoContext` singleton.
- Letting a backend spawn work without the caller holding a context reference.
- Introducing a "default backend" that activates without explicit construction.

**Review questions:**
1. What capability does the caller hold?
2. What does that capability permit?
3. What does it NOT permit?
4. Who owns the underlying resources?

---

## AC-2. Explicit Operation Identity

**Rule:** Every accepted I/O operation MUST have a stable identity from
admission to reap. It MUST be possible to answer: where is the operation, who
owns it, what state is it in, which Completion does it relate to, and who can
terminate it.

**Rationale:** Sluice separates caller-owned `Completion<T>` from per-request
storage. The Proposed ADR-explicit-io-request-contract selects a transitional
C++ adaptation in which the context/backend owns a bounded `RequestSlot` arena
and fixes logical identity as `(ContextIdentity, SlotIndex, Generation)`.
DIV-02 records that proposal as pending acceptance; neither document makes the
current pointer/container implementation conforming.

**Required evidence:**
- Each backend documents how an accepted op is tracked from submit to reap.
- The relationship between RequestKey, RequestSlot, Completion, operation, and
  borrowed resources is 1:1 and verifiable.
- Slot reuse changes generation before the next accepted request becomes
  visible; stale identity cannot affect the new occupant.
- No operation exists only as a closure with no queryable identity.

**Allowed exceptions:**
- During the explicitly staged migration, Fake and Sync/Synthetic may retain
  pointer-based tracking only while the roadmap names the removal phase and
  tests do not claim generation/provenance conformance.
- A raw pointer may remain a validated locating optimization after migration;
  it is never the sole logical identity.

**Common violations:**
- Relying on deque index as identity (invalidated by concurrent drain).
- Lambda captures that cannot be inspected or cancelled.
- Two parallel containers whose size agreement is an untested invariant.

**Review questions:**
1. After `submit_*` returns success, can I point to the operation?
2. Can I determine its current state without racing?
3. If the backend is destroyed, what happens to in-flight identity?

---

## AC-3. Transactional Submission

**Rule:** A successful `submit_*` return MUST mean: the backend has acquired all
resources needed to complete the operation, the Completion is outstanding, and
a reliable completion-or-failure path exists. A failed `submit_*` MUST leave the
Completion idle, outstanding unchanged, and no background work in progress.

The target admission transaction is `reserve -> prepare -> commit/accept ->
enqueue -> dispatch`. Commit is successful-submit linearization. Enqueue is
allocation-free and non-throwing. A dispatch failure after commit is a terminal
result only after the backend proves that no worker, userspace submission entry,
kernel request, or future completion event can still execute or reference the
request. Transient pressure and partial submission retain the request for retry;
neither is a retroactive rejection.

**Rationale:** The caller uses the submit return value to decide control flow.
If submit succeeds but the operation later vanishes (e.g., allocation failure
after the backend's claim), the caller cannot recover. Zig's `operate` is
inherently transactional because the caller owns the storage.

**Required evidence:**
- Reserve obtains every userspace resource required by the accepted terminal
  path before commit; prepare remains invisible to progress engines.
- Code path from `submit_*` entry to backend acceptance is allocation-free OR
  allocation failure is caught and rolled back to idle.
- No sequence: `try_claim` → allocate → fail → "compensate" with an error
  publication. A claim won but not accepted into backend tracking MUST be
  rolled back via `rollback_claim_before_accept()` (e.g. the io_uring
  SQE-acquisition-after-claim gap is closed this way). Residual:
  `register_op` container allocation AFTER the SQE is prepared is still
  non-transactional — explicitly tracked as P0-02 (open), deferred to the
  RequestSlot PR.
- An io_uring mapping acquires/fills SQEs after commit. A partial submit keeps
  its unsubmitted suffix bound and enqueued until allocation-free retry or an
  ownership-safe permanent failure; it does not publish a terminal result while
  an SQE can still be submitted.
- Test: inject `bad_alloc` at each allocation point; verify Completion returns
  to idle.

**Allowed exceptions:**
- If an ADR explicitly approves a two-phase submit with documented rollback.

**Common violations:**
- Claiming a Completion before resource acquisition, with no rollback on
  failure.
- `std::thread` constructor throws after Completion is already outstanding.
- Ready-queue `push_back` allocates; if it throws, the result is lost.

**Review questions:**
1. What is the linearization point of "submit succeeded"?
2. Between the claim and backend acceptance, what can fail?
3. If it fails, is the Completion provably idle?

---

## AC-4. Accepted Operation Must Terminate

**Rule:** Once submit succeeds, the operation MUST eventually reach a terminal
state (success, failure, or cancellation) that is reapable. Completion
publication MUST NOT depend on unbounded future allocation. No accepted
operation may become permanently outstanding due to worker spawn failure,
ready-queue allocation, or shutdown race.

**Rationale:** Zig's model guarantees this structurally: the caller owns the
storage, the backend writes into it, and completion is a state transition in
caller memory. Sluice's heap-mediated path must provide the same guarantee by
contract.

**Required evidence:**
- Every backend documents its "accepted → terminal" path.
- Terminal result storage and ready linkage are bounded or pre-allocated before
  acceptance; workers/CQE handlers stop at backend-ready and reap publishes the
  Completion.
- Explicit shutdown/drain terminates or reaps every accepted operation before
  destruction is permitted. Destruction with outstanding operations is a
  contract violation (fail-fast), not an implicit drain.
- Test: submit N ops → close admission → explicit drain/reap → verify
  outstanding == 0 → destroy cleanly.

**Allowed exceptions:**
- Cancellation may be best-effort (op completes with real result); this is
  documented in `threadpool_backend.hpp`. The op still terminates.

**Common violations:**
- Worker thread fails to spawn and error path itself allocates (double fault).
- Ready deque `push_back` throws `bad_alloc` in the worker — result lost.
- Explicit shutdown reports success while accepted requests remain non-terminal.

**Review questions:**
1. After submit succeeds, is there ANY path where the Completion stays
   outstanding forever?
2. Does the terminal publication require a dynamic allocation?
3. What happens if the terminal allocation fails?

---

## AC-5. Single Completion Publication Authority

**Rule:** A Completion reaches `ready` ONLY through the designated backend reap
publication path. The authorized backend calls `AsyncBackend::publish()`
(from the protected helper that drives `Completion::publish_from_reap()`) from
`poll()` / `wait_one()`. Backend workers stage backend-ready results into a
backend-internal structure; they do NOT publish the Completion directly. Cancel
records cancel intent; it does NOT publish a terminal result. Exactly one
terminal publication per operation is a structural invariant (single-winner CAS
`outstanding → publishing → ready`).

**Rationale:** ADR-async-io-model §6 A3/O1. If multiple paths can complete a
Completion, exactly-once becomes a race. The current ThreadPoolBackend worker
pushes to `ready_size_`/`ready_void_` (backend-ready); `poll()` drains and
publishes the Completion (Completion-ready). This two-phase split is correct.
Any change that lets a worker, timer, or cancel path publish a Completion
directly MUST be approved by ADR.

**Required evidence:**
- Production `publish()` call sites are confined to the `poll()`/`wait_one()`
  drain paths (backend-ready → Completion-ready publication).
- No worker lambda, timer callback, or cancel handler calls `publish()` /
  `publish_from_reap()`.
- `publish_from_reap()` uses a single-winner CAS (`outstanding → publishing`)
  before building the result, so a concurrent publisher cannot race a
  half-built storage write.
- Test: concurrent submit + cancel + poll; verify exactly-once. Concurrent
  publication death test proves a losing publisher fail-fasts before any
  storage mutation.

**Allowed exceptions:**
- Future ADR may introduce a direct-wake path with explicit exactly-once CAS.

**Resolved historical violation (P1-03, ADR-explicit-io-completion-authority):**
previously `SyncBackend::cancel()` called the legacy `complete_with()`
mutator directly, bypassing the `poll()`/`wait_one()` reap path. PR #61 changed
cancel to record cancel intent and lets `poll()`/`wait_one()` publish the
terminal canceled result through the unified reap path. The direct publication
call has been removed.

**Common violations:**
- Adding a "fast path" that publishes the Completion in the worker thread.
- Cancel handler that publishes a canceled result while `poll()` may also
  publish the real result.
- A `publish_from_reap()` that mutates storage before winning the CAS.

**Review questions:**
1. How many code paths can transition this Completion to ready?
2. Is exactly-once guaranteed by structure (single-winner CAS) or by timing?
3. If two paths race, who wins and how is the loser suppressed?

---

## AC-6. Explicit Wake Obligation

**Rule:** Every state change that may enable progress MUST document: who
publishes persistent state, who sends the wake signal, who may be sleeping, and
how the commit-to-sleep race is closed. Periodic polling MUST NOT be the default
architecture glue without explicit justification.

**Rationale:** E9 P3 decoupled wake domains are an approved design. The 2ms
bounded observation interval is protocol authority for MIXED-WAKE backend
progress (ADR §9.4.7.1). This is acceptable ONLY because it is explicit,
bounded, and documented. An undocumented or "defensive" timeout that is actually
the sole progress mechanism is a violation.

**Required evidence:**
- Each wake source (backend cv, wake_cv_, timer expiry) has a documented
  producer/consumer contract.
- The 2ms backstop's role (protocol authority vs. defense-in-depth) is stated
  per usage site.
- No new polling loop is added without stating why a signal is insufficient.

**Allowed exceptions:**
- The 2ms backstop in MIXED-WAKE mode (approved, documented).
- `op_helpers` poll-loop for non-Runtime callers (documented as Threaded-mode
  equivalent).

**Common violations:**
- Adding `sleep_for(1ms)` + poll as the only wake mechanism for a new wait type.
- Describing a timeout as "defensive" when removing it causes lost progress.
- Backend completion that cannot wake ANY sleeping entity.

**Review questions:**
1. If all producers are silent, does the system still make progress? If yes,
   what mechanism? Is it documented?
2. What is the worst-case latency from state-change to observer wake?
3. Is the commit-to-sleep race closed by lock/epoch/condition, or by timeout?

---

## AC-7. Bounded or Caller-Owned Resources

**Rule:** Every significant resource (threads, queues, maps, buffers, slots)
MUST have an explicit capacity OR be documented as caller-owned with the caller
responsible for bounding. No long-lived container may grow by cumulative
historical submissions without reclamation.

**Rationale:** Zig `Threaded` has `async_limit` and `concurrent_limit` with
backpressure (`ConcurrentError`). Sluice's ThreadPoolBackend is currently
UNBOUNDED (no ADR approves this — accidental drift). The `workers_` vector
grows monotonically by cumulative ops. This violates the principle.

**Required evidence:**
- Resource inventory table (see as-built doc §6) is current.
- Each resource row answers: capacity, bounded?, hot-path alloc?, failure mode.
- New resources require a capacity decision before implementation.
- No container grows O(total_ops_submitted) without reclamation.

**Allowed exceptions:**
- Caller-owned Completions (caller decides count).
- Fiber stacks (bounded by admitted tasks, which Runtime controls).

**Common violations:**
- `workers_` vector that never reclaims joined thread slots.
- `std::function` heap allocation per accepted op with no capacity limit.
- Adding an `unordered_map` that grows per-op with no shrink.

**Review questions:**
1. What is the maximum size of this resource under sustained load?
2. What happens when it is full?
3. Is the failure synchronous and reportable to the caller?
4. Does this resource shrink when load decreases?

---

## AC-8. Execution Strategy ≠ I/O Mechanism

**Rule:** Logical task execution strategy (Threaded/Evented), scheduler workers,
blocking-I/O offload workers, kernel async backends, and application pipeline
concurrency are DISTINCT concepts. They MUST NOT be conflated in naming,
configuration, or documentation.

**Rationale:** Zig `Threaded` = thread-per-TASK (execution strategy).
`ThreadPoolBackend` = thread-per-OP (blocking I/O offload mechanism). These are
different layers. Conflating them leads to incorrect capacity reasoning and
misleading API names.

**Required evidence:**
- Any config named `workers` or `threads` states which resource domain it
  belongs to.
- Documentation distinguishes: scheduler worker count, backend offload
  concurrency, pipeline depth, BlockingIoPool size.
- No comment or doc says "ThreadPoolBackend implements Zig Threaded strategy."

**Allowed exceptions:**
- `Group` Threaded mode IS thread-per-task (faithful to Zig). This is distinct
  from `ThreadPoolBackend`.

**Common violations:**
- Calling ThreadPoolBackend a "thread pool" (it is a per-op thread spawner).
- Documenting `worker_count` without saying which subsystem.
- Treating BlockingIoPool and ThreadPoolBackend as interchangeable.

**Review questions:**
1. Does this name/config refer to task execution or I/O offload?
2. Can a reader confuse scheduler workers with backend workers?
3. Is the Zig concept being referenced actually the same layer?

---

## AC-9. Layered Cancellation

**Rule:** Task cancellation, wait cancellation, I/O operation cancellation,
backend syscall interruption, and shutdown admission closure are DISTINCT
layers. Each cancel API MUST state: what is cancelled (intent/wait/operation),
who is the winner authority, possible terminal results, exactly-once guarantee,
whether it interrupts a syscall, and whether it is best-effort.

**Rationale:** Zig has `CancelProtection` regions with `recancel`. Sluice has
`CancelToken` (cooperative, single-shot) + E10 wait cancellation + backend
best-effort cancel. These are different mechanisms at different layers. Mixing
them creates ambiguous semantics.

**Required evidence:**
- Each cancel entry point documents its layer.
- E10 `cancel_wait` is distinct from `CancelToken::cancel()` is distinct from
  `AsyncBackend::cancel()`.
- No single "cancel everything" API that obscures layer boundaries.

**Allowed exceptions:**
- `Group::cancel()` propagates to children — this is documented as
  cancel-propagation boundary (faithful to Zig Group).

**Common violations:**
- A cancel that claims to cancel "the operation" but only cancels the wait.
- Shutdown that cancels admission but claims to cancel in-flight ops.
- Cancel that is not exactly-once but doesn't document this.

**Review questions:**
1. What exactly does this cancel? Intent, wait, or syscall?
2. If the op is already complete, does cancel still "succeed"?
3. Is the terminal result exactly-once or may the caller see both?

---

## AC-10. Documentation–Interface–Implementation Authority Alignment

**Rule:** For every critical state transition, exactly ONE authority layer is
designated. The interface comment, implementation, and ADR MUST agree. If they
disagree, the discrepancy MUST be flagged as `U — Unresolved` and resolved by
ADR before the next release. Silent divergence is forbidden.

**Rationale:** A prior P1 finding (P1-01, resolved by
ADR-explicit-io-completion-authority) recorded a documentation-versus-
implementation conflict: `async_io_context.hpp` once designated the context as
the marking authority ("marking the Completion via mark_outstanding() is the
context's job, not the backend's"), while every backend performed the claim
internally and the context never did. The conflict was documentation-only (no
as-built double transition). PR #61 corrected the header comment so the
interface names the backend as the claim authority (via the protected
`try_claim()` helper), matching the implementation. The rule itself stands:
whenever a critical state transition exists, the header comment, the
implementation, and the ADR MUST agree, or the discrepancy MUST be tracked as
`U — Unresolved` until resolved by ADR.

**Required evidence:**
- Each state transition (idle→outstanding, outstanding→ready) names its
  authority in the header comment.
- Implementation matches the header comment.
- ADR matches both.
- Discrepancies are tracked in the findings document with resolution plan.

**Allowed exceptions:**
- During an active corrective PR, temporary discrepancy is allowed if tracked.

**Common violations:**
- Header comment names the wrong authority for a transition (e.g. a stale
  "context claims outstanding" comment while the backend is the claim
  authority).
- ADR says "poll is sole publication authority" but a fast-path bypasses poll.
- Comment says "bounded" but code has no bound.

**Review questions:**
1. Does the header comment match the implementation for this transition?
2. Does the ADR match both?
3. If not, is there a tracked finding with a resolution timeline?

---

## AC-11. Tests Prove Semantics, Not Implementation Preference

**Rule:** Correctness tests MUST prove semantic properties: scheduler liveness,
exactly-once publication, no lost wake, bounded resources, transactional submit,
shutdown convergence, backend conformance. Tests MUST NOT use timing, thread
counts, container internals, or syscall names as the SOLE proof of a semantic
property.

**Rationale:** Implementation mechanism tests (e.g., "thread count increased")
are brittle and do not prove the contract. Semantic tests use deterministic
phase seams, barriers, controlled clocks, or explicit state observation.

**Required evidence:**
- Each concurrency test states which semantic property it proves.
- `sleep_for` is not the ordering proof (may be used for diagnosis only).
- Backend conformance tests run against all backends uniformly.

**Allowed exceptions:**
- Performance benchmarks may measure timing (they are not correctness proofs).
- Smoke tests may use timing as a sanity check if labeled as such.

**Common violations:**
- "Test passes with 10ms sleep" as proof of no lost wake.
- Asserting `workers_.size() == N` as proof of bounded concurrency.
- Testing only ThreadPoolBackend and claiming backend conformance.

**Review questions:**
1. What semantic property does this test prove?
2. Would this test still be valid if the container/thread/timing changed?
3. Does it run against all backends or just one?

---

## AC-12. Zig Divergence Must Be Registered

**Rule:** Any design that differs from the Zig source-derived model MUST be
registered in the divergence registry with: divergence description, reason,
benefit, cost, compatibility impact, and revisit trigger. "C++ is different" is
not a sufficient reason alone.

**Rationale:** Zig is a design reference, not a specification to copy. But
unregistered divergence becomes invisible drift. The registry makes divergence
auditable and prevents accidental accumulation.

**Required evidence:**
- Every `I` (Intentional) classification in the conformance map has a
  corresponding divergence registry entry.
- Every new `A` (Accidental) finding triggers either a corrective plan or a
  retroactive registry entry with status `Pending decision`.
- PR template asks whether the conformance map changes.

**Allowed exceptions:**
- Pure C++ idiom differences (e.g., virtual dispatch vs. vtable struct) that do
  not change semantics need only a conformance map note, not a full registry
  entry.

**Common violations:**
- Introducing a new backend without updating the conformance map.
- Changing resource bounds without registering the divergence.
- Removing a Zig-equivalent capability without marking it `O` (Obsolete).

**Review questions:**
1. Does this change alter the Zig conformance map?
2. If it diverges, is the divergence registered?
3. What is the revisit trigger for this decision?

---

## AC-13. Unforgeable Publication Authority; State-Checked Caller Lifecycle

**Rule:** Internal publication transitions (`try_claim_for_backend`,
`publish_from_reap`, `rollback_claim_before_accept`) MUST be structurally
unforgeable by ordinary application code (via type system, capability token, or
access-control pattern). Caller lifecycle transitions (`reset` / `rearm`) remain
caller-accessible but MUST be state-checked:

```text
- `ready → resetting → idle`: successful reuse;
- `idle → idle`: idempotent no-op;
- `outstanding` or `publishing` → reset: checked contract violation.
```

The internal `resetting` transient prevents a new claim from observing idle
before prior-result cleanup (storage_ / reap_seq_) is complete: reset CASes
`ready → resetting`, clears the result, then release-stores `idle`; a new
`try_claim` can only observe `idle` AFTER cleanup has happened. A comment saying
"backend-only" is NOT an authority boundary. Debug assertions are NOT an
authority boundary. Release builds MUST also detect or structurally prevent
invalid transitions.

**Authority separation:**
```text
Backend/system authority (structurally forbidden to ordinary callers):
  claim                      idle → outstanding   (CAS, exactly-once)
  publish                    outstanding → ready  (CAS via transient publishing,
                                                   exactly-once, single winner)
  rollback_claim_before_accept  outstanding → idle (backend-only, pre-tracking)

Caller lifecycle authority (permitted, state-checked):
  reset() / rearm()      ready → resetting → idle (success)
                         idle → no-op (idempotent; registered decision)
                         outstanding → invalid_state error / fail-fast
                         publishing / resetting → fail-fast
```

**Caller-lifecycle decision (registered, ADR-explicit-io-completion-authority):**
`reset()` from `idle` is an **idempotent no-op**, NOT an error and NOT a
fail-fast. `op_helpers` (`one_step` / `sync_step`) reset before their first
submit, when the Completion is already idle; fail-fast on idle would break
that legitimate reuse pattern. This amends the earlier "ready → idle ONLY"
reading of this rule. Reset from `outstanding`/`publishing` remains a checked
contract violation (fail-fast).

**Rationale:** Historically `Completion<T>` exposed `mark_outstanding()` and
`complete_with()` as public methods, so any application code could forge
publication transitions (double-complete, or mark outstanding without a backend)
— enabling use-after-free (backend holds pointer to reset/destroyed Completion),
double publication, and permanent state divergence. That public API was removed
(ADR-explicit-io-completion-authority; see the Resolved historical violation
note below). `reset()` remains the caller's reuse interface — the Completion
contract requires callers to reset after reading the result. Making reset
backend-only would destroy the caller-owned reusable model, so reset stays
caller-accessible but state-checked through the `ready → resetting → idle`
transition.

**Required evidence:**
- `try_claim_for_backend()`, `publish_from_reap()`,
  `rollback_claim_before_accept()`, and `reap_seq()` are NOT callable from
  ordinary application code (negative-compile proof).
- `reset()` IS callable by the caller. It CASes `ready → resetting`, clears
  storage/reap_seq, then release-stores `idle`; from idle it is an idempotent
  no-op; reset on outstanding/publishing/resetting is a checked contract
  violation (fail-fast in Release).
- Destruction of an outstanding/publishing/resetting Completion is a checked
  contract violation in BOTH Debug and Release.
- If the Proposed RequestSlot contract is accepted and implemented, Completion
  claim gains a private `idle → binding → outstanding` protocol. The first CAS
  elects one context; only its winner initializes the private RequestKey,
  context provenance, and slot-release capability. Acquire observation of
  `outstanding` sees the complete binding. Cancel and waiter registration do
  not read fields while `binding`; reset/destruction in `binding` fail fast.
- Under that proposal, `reset()` and destruction of a ready Completion also
  perform the same allocation-free release of the bound slot. Release does not
  wait for I/O, workers, cancellation, drain, backend progress, or Scheduler
  activity. It may use a bounded internal critical section in the leaf
  slot-lifecycle domain shared with waiter registration and reap-ready
  publication, but calls no user code, ReadySink, Scheduler, or backend progress
  path. Release cannot reuse the slot until old-generation reap has left that
  domain and requires registration to be closed with no stored token/lease.
  Destroying ready remains allowed; it discards terminal storage but does not
  cancel or drain I/O.
- No test uses backend-only publication mutators as if they were public API.
- Two different contexts cannot both bind the same Completion: the private
  `idle -> binding` CAS is the structural cross-context exclusion point, not a
  context-local lock or comment convention.

**Allowed exceptions:**
- During the corrective phase, temporary public access is allowed if tracked
  and guarded by a negative-compile deadline.
- Test-only access via friend or `SLUICE_ASYNC_INTERNAL_TESTING` guard.

**Resolved historical violation (P0-03, ADR-explicit-io-completion-authority):**
the legacy public `mark_outstanding()` and `complete_with()` were removed;
publication mutators are now private (friend `AsyncBackend`), and derived
backends use the protected `try_claim()` / `publish()` /
`rollback_claim_before_accept()` helpers. A negative-compile gate
(`scripts/verify-completion-authority-negative-compile.sh`, wired into CI)
proves non-backend code cannot publish.

**Common violations:**
- `try_claim_for_backend()` / `publish_from_reap()` left public, or reachable
  through a non-backend seam, with only an assert(idle) guard.
- `reset()` that silently succeeds from outstanding state, or that publishes
  `idle` before prior-result cleanup completes (missing the `resetting`
  transient).
- Default destructor that does not fail-fast on outstanding.
- Tests calling publication mutators directly as "convenience."

**Review questions:**
1. Can application code forge a publication transition?
2. If yes, what is the worst-case consequence?
3. Is the boundary structural or merely conventional?
4. Does reset enforce `ready → resetting → idle` with an `idle → no-op`,
   `outstanding/publishing/resetting → fail-fast` contract (AC-13 as amended)?

---

## AC-14. Request Provenance and Generation

**Rule:** Every accepted operation MUST have a stable identity that includes
context/backend provenance, reusable-slot identity, a monotonically increasing
per-slot generation (or equivalent ABA guard), operation kind, Completion
binding, and resource/buffer binding. A raw pointer (e.g., `Completion*`) MUST
NOT be the sole logical identity across asynchronous phases.

**Rationale:** Completion address is currently the only backend identity for
an operation. But Completions can be reset and reused. The same address may
represent different operations at different times (classic ABA). Any delayed
event (backend result, cancel request, cancel CQE, Scheduler waiter, Batch
reap, shutdown cleanup) that references only `&c` cannot distinguish which
generation of the request it targets. Uring internally uses op-id but the
public cancellation and Scheduler registration still target Completion address.

**Required evidence:**
- Each accepted op has a generation or epoch that monotonically increases per
  RequestSlot reuse and is bound opaquely to the outstanding Completion.
- Cancel targets a specific generation, not just an address.
- RequestSlot owns single-waiter registration state, a stable value token, and
  the token's routing lease; Scheduler owns the referenced Fiber/runnable
  routing record. The lease prevents cancel/drain/shutdown from retiring or
  reusing that record until exactly one winning delivery path acknowledges it.
  Registration records provenance and a second waiter cannot overwrite the
  first.
- Cross-context Completion submission is structurally prevented or detected.

**Allowed exceptions:**
- Internal backend scratch may use raw pointers for hot-path optimization,
  but the logical identity MUST include generation.
- Test-only backends may simplify if documented.

**Common violations:**
- Using `Completion*` as map key with no generation guard.
- Cancel that targets an address that may have been reused.
- Scheduler waiter map keyed by raw pointer with no context check.
- `await_completion` that only asserts `!c.idle()` (vanishes in Release).

**Review questions:**
1. If this Completion is reset and resubmitted, can a delayed event tell?
2. Can a Completion from context A be awaited on context B's Scheduler?
3. What happens if two Fibers await the same Completion?

---

## AC-15. Completion Identity Preservation Across Reap

**Rule:** When a backend knows WHICH operations completed (it always does),
that identity MUST NOT be discarded at the L0/L1 boundary. The reap interface
MUST provide completed operation identities to higher layers. Higher layers
MUST NOT be forced to recover completion identity by scanning all outstanding
Completions.

The Proposed request contract selects a synchronous, callback-scoped event
semantically equivalent to
`ReadyEvent{RequestKey, OperationKind, OptionalWaiterDelivery}`. A waiter
delivery contains a stable Scheduler identity/slot/generation token plus a
move-only routing lease; it is not a raw Fiber pointer. The event deliberately
contains no `Completion*` or `RequestSlot*`: under the same leaf slot-lifecycle
domain used by release/reuse, reap closes registration, takes any delivery, and
publishes ready before releasing that domain. It then invokes the sink with no
backend/slot lock held. The sink may copy value identity and must consume or
acknowledge the routing lease before returning, but cannot retain the event
reference or reacquire mutable slot storage. Exact C++ syntax is a lower-level
design choice; returning only a count is not sufficient as the authoritative
integration contract.

**Rationale:** Current `poll()`/`wait_one()` return only a count. The
Scheduler then scans its entire `waiting_completion_` map checking
`c.ready()` on each — O(N) work to recover information the backend already
had. Batch similarly scans all slots and uses a process-wide global
`reap_seq` to reconstruct completion order. This global sequence is hidden
static state that Completion is forced to carry for Batch's benefit.

**Required evidence:**
- Backend provides a reap-ready iteration or callback that yields completed
  request identities (not just a count).
- Ready delivery cannot outlive a referenced Completion/slot without an
  explicit event lease/ack design; the Proposed contract avoids that ownership
  by synchronous pointer-free delivery.
- A Scheduler routing record referenced by a waiter delivery remains pinned
  through sink/cancel acknowledgement; fake-token evidence is not proof of the
  actual Scheduler cancel/drain/shutdown integration.
- Scheduler does NOT scan all registered Completions to find ready ones.
- No process-wide global ordering state embedded in per-Completion objects.
- Batch obtains completion order from the reap interface, not from scanning.

**Allowed exceptions:**
- Small-scale test backends (≤8 outstanding) may use scan if documented.
- During corrective phases, scan is acceptable if tracked with a removal
  deadline.

**Common violations:**
- `poll()` returns `size_t` count; caller scans everything.
- Global atomic `next_reap_seq()` embedded in Completion for Batch ordering.
- Scheduler O(N) scan of waiting map on every progress iteration.

**Review questions:**
1. After `poll()` returns, does the caller know WHO completed?
2. If not, how does it find out? Is that O(1) or O(N)?
3. Is there hidden global state used to reconstruct ordering?
