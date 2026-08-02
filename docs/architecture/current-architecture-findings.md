# Current Architecture Findings

**Baseline:** `d299fc0` (master). All findings are evidence-backed from code
inspection at this commit. No finding is based on speculation.

Severity:
- **P0** — correctness/liveness: accepted op may be permanently lost or process
  may terminate unexpectedly
- **P1** — architecture contract conflict: documentation, interface, and
  implementation disagree on authority
- **P2** — bounded-resource/performance structure: unbounded growth, hot-path
  allocation, excessive overhead
- **P3** — naming/documentation: misleading names, stale comments

---

## P0-01: Worker Thread Unhandled Exception → std::terminate

**Finding:** If `ready_size_.push_back()` or `ready_void_.push_back()` throws
`std::bad_alloc` inside a worker thread lambda, the exception is unhandled.
Per C++ standard, an unhandled exception in a `std::thread` callable invokes
`std::terminate()`. The accepted operation's result is permanently lost and the
process aborts.

**Evidence:**
- `src/async/threadpool_backend.cpp:85-92` — worker lambda for size ops:
  ```cpp
  workers_.emplace_back([this, cp, worker_idx, work = std::move(work)] {
      Result<std::size_t> r = work();
      {
          std::lock_guard<std::mutex> lk(mtx_);
          ready_size_.push_back(ReadySize{cp, std::move(r), worker_idx});
          // ^^^ can throw bad_alloc → unhandled in thread → terminate
      }
      cv_.notify_one();
  });
  ```
- Same pattern at lines 113-120 for void ops.
- `fail_spawn_size` (line 136) also does `push_back` in the catch handler —
  if THAT throws, it is a double-fault → immediate terminate.

**Violated constitution rule:** AC-4 (accepted operation must terminate),
AC-3 (transactional submission — post-acceptance failure path is unsound).

**Semantic consequence:** After `submit_*` returns success, the caller trusts
that the Completion will eventually reach ready. Under OOM at the ready-queue
push, the process terminates instead. The caller has no opportunity to handle
this.

**Currently regression-tested:** No. No OOM injection test exists for the
worker-internal push_back path.

**Recommended next action:** File a separate issue. Options:
1. Wrap worker lambda body in try/catch; on allocation failure, retry with
   exponential backoff or use a pre-allocated emergency slot.
2. Pre-allocate a bounded ready-entry pool so push_back never allocates.
3. Use an intrusive list for ready entries (no allocation at publication time).

**Do not fix in this audit PR.**

---

## P1-01: mark_outstanding() Authority Conflict

**Finding:** The `AsyncBackend` header comment states "marking the Completion
via mark_outstanding() is the context's job, not the backend's." In reality,
ALL backends call `c.mark_outstanding()` themselves, and `AsyncIoContext` does
NOT call it.

**Evidence:**
- `include/sluice/async/async_io_context.hpp:68-70`:
  ```
  // records the op outstanding (marking the Completion via mark_outstanding()
  // is the context's job, not the backend's)
  ```
- `src/async/async_io_context.cpp`: grep for `mark_outstanding` → 0 matches.
- `src/async/threadpool_backend.cpp:71,106`: `c.mark_outstanding()`
- `src/async/uring_backend.cpp:484`: `c.mark_outstanding()`
- `include/sluice/async/fake_backend.hpp:221,228`: `c.mark_outstanding()`
- `include/sluice/async/sync_backend.hpp:40,46,52,58`: `c.mark_outstanding()`

**Violated constitution rule:** AC-10 (documentation–interface–implementation
authority alignment).

**Semantic consequence:** A future implementer reading the header comment will
add `mark_outstanding()` in the context, causing a double-mark (assertion
failure in Debug, undefined state transition in Release). The contract is
untrustworthy.

**Currently regression-tested:** No test verifies WHO marks outstanding. The
behavior is correct today (backend marks, context doesn't), but the
documentation invites a future break.

**Recommended next action:** Correct the header comment to state: "marking the
Completion via mark_outstanding() is the backend's job, performed inside
submit_* before returning success." Update the as-built doc authority table.

**Do not fix in this audit PR** (comment-only fix is borderline; recommend
separate small PR to keep audit PR documentation-only).

---

## P1-02: No Unified Backend-Ready vs. Completion-Ready Distinction

**Finding:** The current design conflates "backend has a result available"
(backend-ready) with "Completion has transitioned to ready" (completion-ready).
In ThreadPoolBackend, the worker pushes to `ready_size_` (backend-ready), and
`poll()` calls `complete_with()` (completion-ready). But the documentation does
not consistently distinguish these two states, and the `outstanding()` counter
is decremented at poll time, not at backend-ready time.

**Evidence:**
- `threadpool_backend.cpp:89`: worker pushes to `ready_size_` (backend-ready)
- `threadpool_backend.cpp:160-163` (poll drain): `c->complete_with(r); --outstanding_`
- ADR-async-io-model §6 A3: "poll()/wait_one() is the sole Completion
  publication authority" — this is correct but the two-phase nature is not
  named.

**Violated constitution rule:** AC-2 (explicit operation identity — the
backend-ready state is implicit, not queryable), AC-5 (single publication
authority — correct but undocumented two-phase).

**Semantic consequence:** If a future backend completes the Completion directly
(e.g., a "fast-path" optimization), it would violate the publication authority
contract. The two-phase model needs explicit documentation to prevent this.

**Currently regression-tested:** Backend conformance tests verify
exactly-once completion but do not test the intermediate backend-ready state.

**Recommended next action:** Document the two-phase model explicitly:
1. Backend-ready: result available in backend-internal structure.
2. Completion-ready: `complete_with()` called by poll/wait_one.
Add to as-built doc and ADR.

**Do not fix in this audit PR.**

---

## P0-02: Cross-Backend Transactional Submit Defect (No Unified Admission Contract)

**Finding:** ALL backends lack a unified transactional admission contract. The
mark_outstanding → resource-acquisition sequence is not atomic in any backend.
If any allocation between mark_outstanding and backend acceptance fails, the
Completion is left outstanding with no path to terminal.

**Evidence (per backend):**

- **FakeAsyncBackend** (`fake_backend.hpp:221-228`):
  ```cpp
  c.mark_outstanding();
  ready_size_.push_back(&c);    // can throw bad_alloc
  pending_size_.push_back(op.len); // can throw bad_alloc
  ```
  Either push_back failure leaves Completion outstanding with incomplete
  backend records.

- **SyncBackend** (`sync_backend.hpp:40-41`):
  ```cpp
  c.mark_outstanding();
  entries_.push_back(Entry{op, &c}); // can throw bad_alloc
  ```
  Allocation failure leaves Completion outstanding with no entry to poll.

- **UringAsyncBackend** (`uring_backend.cpp:484+`):
  ```cpp
  c.mark_outstanding();
  comp_to_op.emplace(...);   // can throw
  ops.emplace(...);          // can throw
  pending_sqes.push_back(...); // can throw
  ```
  Multiple allocation points; any failure leaves partial submission state.

- **ThreadPoolBackend** — see P0-01 (worker OOM) and P1-04 (spawn failure).

**Violated constitution rule:** AC-3 (transactional submission — failed submit
MUST leave Completion idle), AC-4 (accepted operation must terminate).

**Semantic consequence:** The root problem is not any single backend's OOM
path — it is the absence of a unified transactional admission contract across
the L0 backend family. Each backend independently attempts mark-then-allocate
with no rollback.

**Currently regression-tested:** No. No OOM injection test exists for any
backend's submit path between mark_outstanding and acceptance.

**Recommended next action:** Phase 1 roadmap — design unified transactional
admission: either allocation-free submit path, or mark_outstanding AFTER
resource acquisition succeeds (rollback-free by construction).

**Do not fix in this audit PR.**

---

## P1-03: SyncBackend cancel() Bypasses Reap Authority

**Finding:** `SyncBackend::cancel()` calls `c.complete_with()` directly,
bypassing the poll()/wait_one() reap path. The interface contract (AC-5)
requires that Completion publication to ready happens ONLY through the
designated reap authority (poll/wait_one drain).

**Evidence:**
- `include/sluice/async/sync_backend.hpp:87`:
  ```cpp
  void cancel(Completion<std::size_t>& c) override {
      auto it = std::find_if(entries_.begin(), entries_.end(), ...);
      if (it != entries_.end()) {
          c.complete_with(make_unexpected<std::size_t>(...)); // DIRECT
          entries_.erase(it);
      }
  }
  ```
- Same pattern at line 95 for void Completions.
- Contrast with ThreadPoolBackend where cancel does NOT call complete_with;
  the op completes with its real result via poll().

**Violated constitution rule:** AC-5 (single Completion publication authority).

**Semantic consequence:** Bypasses the unified reap/publication authority.
Completion ordering and statistics may be inconsistent. Backend conformance
is non-uniform (ThreadPoolBackend defers to poll; SyncBackend does not). If
external code directly instantiates SyncBackend (bypassing AsyncIoContext's
access_mtx_), the interface itself provides no thread-safety guarantee.
Note: when accessed normally through AsyncIoContext, cancel() and poll() are
serialized by access_mtx_, so a proven double-completion race does NOT exist
on the normal path. The violation is structural authority bypass, not a
demonstrated concurrent race.

**Currently regression-tested:** No concurrent cancel + poll test for
SyncBackend.

**Recommended next action:** Either:
1. Defer cancel completion to poll() (mark entry as cancelled, let poll drain
   complete it), or
2. Document SyncBackend as a test-only synthetic backend where the AC-5
   violation is accepted with explicit justification.

**Do not fix in this audit PR.**

**RESOLVED (ADR-explicit-io-completion-authority, branch
fix/explicit-io-completion-authority):** SyncBackend::cancel() now marks the
entry as cancelled; publication happens through the unified reap path
(poll/wait_one) via the protected `publish()` helper. The direct
`complete_with()` call has been removed. Regression-tested in
`async_completion_test` (cancel_outstanding_op_completes_canceled).

---

## P1-04: ThreadPoolBackend Spawn Failure Incorrectly Asyncized

**Finding:** When `std::thread` construction throws in
`ThreadPoolBackend::enqueue_size`, the catch handler (`fail_spawn_size`)
pushes an error entry to the ready queue and the Completion is eventually
completed with an error via poll(). However, `submit_read()` still returns
SUCCESS to the caller.

This violates the public contract: submit-time errors (queue full, invalid
operation, Completion non-idle) should be returned synchronously. The caller
sees `submit_read()` return `{}` (success) but the operation will "complete"
asynchronously with an error — a semantic that does not exist in the
documented contract.

**Evidence:**
- `src/async/threadpool_backend.cpp:93-101` (catch handlers):
  ```cpp
  } catch (const std::bad_alloc&) {
      fail_spawn_size(cp, IoError{IoError::Code::no_space});
  } catch (const std::system_error& e) {
      IoError err{IoError::Code::backend_error};
      if (e.code().value() > 0) err.os_errno = e.code().value();
      fail_spawn_size(cp, err);
  } catch (...) {
      fail_spawn_size(cp, IoError{IoError::Code::backend_error});
  }
  ```
- `fail_spawn_size` pushes to `ready_size_` with an error result.
- `submit_read()` returns `{}` (success) regardless.

**Violated constitution rule:** AC-3 (transactional submission — failed submit
MUST leave Completion idle and return error synchronously).

**Semantic consequence:** The caller cannot distinguish "submit succeeded, op
will complete" from "submit failed, error will appear asynchronously." This
breaks the submit return-value contract.

**Currently regression-tested:** No test injects thread-creation failure.

**Recommended next action:** Phase 1/3 roadmap — if thread creation fails,
roll back mark_outstanding and return a synchronous error. Alternatively,
pre-allocate thread resources before marking outstanding.

**Do not fix in this audit PR.**

---

## P1-05: queue_full_retries Conflates Lifecycle Violation with Capacity Pressure

**Finding:** `AsyncIoContext::tally_submit()` counts
`IoError::Code::invalid_state` into `AsyncStats::queue_full_retries`. But
`invalid_state` means "submit into a non-idle Completion" — a caller
lifecycle violation. This is semantically distinct from capacity pressure
(queue full, ring depth exhausted, OOM).

**Evidence:**
- `src/async/async_io_context.cpp:87-88`:
  ```cpp
  } else if (r.error().code == IoError::Code::invalid_state) {
      ++s->queue_full_retries;
  }
  ```
- Code comment (line 73-81) acknowledges `invalid_state` represents "submit
  into a non-idle Completion" but still counts it as `queue_full_retries`.

**Violated constitution rule:** AC-10 (documentation–interface–implementation
alignment — the stat name misrepresents what is counted).

**Semantic consequence:** Observability is broken. Even if bounded request
slots are added (Phase 1), operators cannot distinguish:
- Caller reusing a Completion (lifecycle bug)
- Backend queue full (capacity pressure, retryable)
- Ring depth exhausted (Uring-specific)
- OOM (resource exhaustion)
- Backend fatal (unrecoverable)

Correct error vocabulary requires at minimum:
```text
invalid_state     → Completion lifecycle error (caller bug, not retryable)
would_block       → capacity full (retryable)
no_space          → cannot allocate required resources
backend_error     → backend unrecoverable
```

**Currently regression-tested:** No test verifies stat categorization.

**Recommended next action:** Phase 0/1 — rename the stat or split into
`lifecycle_violations` and `capacity_rejects`. Introduce distinct error codes
for capacity pressure vs. lifecycle errors.

**Do not fix in this audit PR.**

---

## P0-03: Completion Mutation Authority Is Forgeable

**Finding:** Two distinct defects:

**A. Publication authority leak:**
`Completion<T>` exposes `mark_outstanding()` and `complete_with()` as public
methods. Any application code can forge publication state transitions. The
"backend-only" comment is not an authority boundary. Debug assertions are not
an authority boundary. Release builds have NO protection.

**B. Caller lifecycle validation defect:**
`reset()` is correctly caller-accessible (AC-13), but permits
idle/outstanding → idle without runtime state check. A caller resetting an
outstanding Completion is a contract violation that goes undetected.

**Evidence:**
- `include/sluice/async/completion.hpp`: all three methods are `public`.
- `mark_outstanding()` is load-assert-store, NOT atomic CAS.
- `reset()` does not reject outstanding state (should trap).
- Destructor is default (does not fail-fast on outstanding).
- Tests call `c.mark_outstanding(); c.complete_with(...);` directly
  (publication forgery). Tests calling `ready → reset` are legitimate.
- Two different AsyncIoContext instances can both submit to the same
  Completion concurrently (access_mtx_ is per-context, not per-Completion).

**Attack scenario:**
```cpp
ctx.submit_read(op, c);  // backend holds &c
c.reset();               // caller resets outstanding (undetected violation)
ctx.submit_write(op2, c); // second backend also holds &c
// Both backends will eventually complete_with → double publication,
// stale result overwrite, or use-after-free if c is destroyed.
```

**Violated constitution rule:** AC-13 (unforgeable publication authority;
state-checked caller lifecycle).

**Semantic consequence:** Use-after-free, double completion, permanent
Completion/backend state divergence. The type system provides zero protection
for publication; runtime provides zero protection for outstanding reset.

**Currently regression-tested:** No negative-compile test prevents caller
calls to publication mutators. Tests actively USE them. No death test for
outstanding reset.

**Recommended next action:** Phase 1 (Completion authority hardening) —
friend/capability pattern for publication mutators; negative-compile gate;
Release fail-fast on invalid transitions; outstanding reset → trap;
outstanding destructor check.

**Do not fix in this audit PR.**

**RESOLVED (ADR-explicit-io-completion-authority, branch
fix/explicit-io-completion-authority):**
- Part A: `mark_outstanding()` and `complete_with()` removed from public API.
  Publication mutators are now private (friend AsyncBackend). Derived backends
  use protected `try_claim()`/`publish()`/`rollback_claim_before_accept()`
  helpers. Negative-compile gate:
  `scripts/verify-completion-authority-negative-compile.sh` (wired into CI).
- Part B: `reset()` now fail-fasts (std::terminate) on outstanding/publishing.
  Destructor fail-fasts on outstanding/publishing. Both enforced in Debug AND
  Release. Death tests: `completion_authority_death_test`. `reset()` from idle
  is a registered idempotent no-op (AC-13 amended; op_helpers depends on it).
- CAS-based claim: `try_claim_for_backend()` uses atomic
  compare_exchange_strong (exactly-once under concurrent submission).
- Single-winner publish: `publish_from_reap()` CASes `outstanding → publishing`
  (transient state) before building the result, so a concurrent publisher
  loses the CAS and fail-fasts instead of racing the storage write.
  Death test: two-thread concurrent publish → loser fail-fasts. Concurrency
  test: two-thread concurrent claim → exactly one wins.
- Transactional submit (P0-02 partial): io_uring claims BEFORE SQE acquisition;
  a failed SQE acquisition rolls the claim back via
  `rollback_claim_before_accept()` (no untracked SQE can run I/O after a
  failed submit). Residual: `register_op` container allocation after SQE prep
  is still non-transactional — explicitly deferred to the RequestSlot PR.

---

## P1-06: No Request Generation — ABA on Completion Reuse

**Finding:** Completion address is the sole logical identity for an operation
across all async phases. Completions can be reset and reused. The same address
may represent different operations at different times (classic ABA). No
generation counter exists.

**Evidence:**
- Backend maps keyed by `Completion*` (threadpool ready queue, uring
  comp_to_op map, Scheduler waiting_completion_ map).
- `reset()` returns Completion to idle; same address can be resubmitted.
- Cancel targets `Completion&` with no generation check.
- Scheduler waiter map: `waiting_size_[&c] = {fiber, worker}` — if
  Completion is reused, stale waiter registration points to new operation.

**Violated constitution rule:** AC-14 (request provenance and generation).

**Semantic consequence:** Delayed events (cancel, CQE, shutdown cleanup)
cannot distinguish which generation of a request they target. Stale cancel
may hit a new operation. Stale waiter may never wake.

**Currently regression-tested:** No generation/ABA test exists.

**Recommended next action:** Phase 1 ADR + Phase 2 implementation
(RequestKey/RequestSlot design) —
add generation counter; cancel targets generation; Scheduler registration
includes generation.

**Do not fix in this audit PR.**

---

## P1-07: Reap API Discards Completion Identity

**Finding:** `poll()`/`wait_one()` return only a count. The backend KNOWS
which operations completed, but this identity is discarded at the L0/L1
boundary. Higher layers must recover it by O(N) scanning.

**Evidence:**
- `AsyncIoContext::poll()` returns `std::size_t` (count).
- Scheduler: after `ctx_.poll()`, scans entire `waiting_completion_` map
  checking `c.ready()` on each entry — O(N) per progress iteration.
- Batch: after `wait_one()`, scans all slots checking `ready()`.
- `Completion::reap_seq_` + process-wide static `next_reap_seq()` exist
  solely to let Batch reconstruct completion order that the backend already
  knew.

**Violated constitution rule:** AC-15 (completion identity preservation).

**Semantic consequence:** O(N) overhead per progress cycle; hidden global
static state in Completion; Scheduler and Batch do redundant work.

**Currently regression-tested:** N/A (design issue, not a bug per se).

**Recommended next action:** Phase 3 (reap contract redesign) — backend
provides `reap_ready(ReadySink&)` or equivalent; remove global reap_seq;
Scheduler uses identity-bearing reap.

**Do not fix in this audit PR.**

---

## P1-08: wait_one() Holds Context Lock, Blocks Cancel and Submit

**Finding:** `AsyncIoContext::wait_one()` holds `access_mtx_` for the entire
duration of the backend blocking wait. During this time, `submit_*`,
`cancel()`, `poll()`, and `outstanding()` are ALL blocked on the same mutex.

**Evidence:**
- `src/async/async_io_context.cpp:133-138`:
  ```cpp
  Result<std::size_t> AsyncIoContext::wait_one() {
      std::lock_guard<std::mutex> lk(access_mtx_);
      auto r = backend_->wait_one(); // may block indefinitely
      ...
  }
  ```
- All other methods (`submit_*`, `cancel`, `poll`, `outstanding`) also
  acquire `access_mtx_`.

**Violated constitution rule:** AC-1 (explicit capability — the context does
not express whether it is single-driver or concurrent-capable), AC-9
(cancel cannot reach the backend while wait_one holds the lock).

**Semantic consequence:**
1. Cancel cannot interrupt a blocking wait (it cannot acquire the lock).
2. "Submit does not block" is false in shared-context scenarios.
3. The API does not express whether AsyncIoContext is single-driver or
   concurrent-submit capable.

**Currently regression-tested:** Scheduler avoids this via single-admission
rules, but L1 public API has no such protection for direct users.

**Recommended next action:** Phase 4 (wait/cancel concurrency redesign) —
split capabilities or use interruptible wait; document single-driver
contract if that is the intent.

**Do not fix in this audit PR.**

---

## P1-09: Cancel API Is Not Explicit

**Finding:** `void cancel(Completion&)` provides no information about what
happened. It does not distinguish: request found and cancel submitted,
request already terminal, request not found, cancel not supported, target
belongs to another context.

**Evidence:**
- `async_io_context.hpp`: `void cancel(Completion<std::size_t>& c)` — void
  return.
- ThreadPoolBackend cancel: best-effort, op completes with real result.
- SyncBackend cancel: directly completes with cancelled (bypasses reap).
- UringBackend cancel: may or may not produce a cancel CQE.
- No disposition enum; no Result return; no request identity beyond address.

**Violated constitution rule:** AC-9 (layered cancellation — each cancel API
MUST state possible outcomes), AC-14 (cancel should target request identity,
not reusable address).

**Semantic consequence:** Caller cannot distinguish "cancel accepted" from
"nothing happened" from "already done." Cannot build reliable cancellation
protocols on a void-return fire-and-forget API.

**Currently regression-tested:** Tests verify eventual Completion state but
not cancel disposition reporting.

**Recommended next action:** Phase 4 — redesign cancel to return
`Result<CancelDisposition>` targeting a RequestKey.

**Do not fix in this audit PR.**

---

## P1-10: Runtime await Has No Request Provenance Check

**Finding:** `RuntimeTaskContext::await_completion()` only checks
`assert(!c.idle())` (vanishes in Release). It cannot distinguish:
- Completion submitted via THIS Runtime's context;
- Completion submitted via ANOTHER context;
- Completion manually mark_outstanding'd by the caller;
- Completion already reset/resubmitted.

**Evidence:**
- `application_runtime.hpp`: await checks only idle state via assert.
- Scheduler registers waiter by raw `Completion*` in waiting map.
- If wrong-context Completion is awaited: current Scheduler polls its own
  backend, the real operation is on another backend → permanent hang.
- If idle Completion is awaited in Release: registered in waiting map,
  no backend will complete it → Fiber permanently parked.
- Second Fiber awaiting same Completion overwrites first waiter in map
  (`waiting_size_[&c] = ...`) → first Fiber never wakes.

**Violated constitution rule:** AC-14 (request provenance), AC-13
(structural authority — idle check is assert-only).

**Semantic consequence:** Permanent Fiber hang (unrecoverable) for
wrong-context or idle Completions in Release. Silent waiter loss for
multi-waiter on same Completion.

**Currently regression-tested:** No test for wrong-context await. No test
for multi-waiter on same Completion. No Release-mode idle-await test.

**Recommended next action:** Phase 1/2 — provenance token binding
Completion to submitting context; Release fail-fast on idle await; explicit
single-waiter or multi-waiter policy.

**Do not fix in this audit PR.**

---

## P2-05: Batch Conflates Submit Rejection with Terminal Completion

**Finding:** When Batch submit fails, it fabricates a ready result in the
slot (with `reap_seq == 0`) and returns it via `next()` as if it were a
completion. This conflates admission rejection with terminal completion —
two fundamentally different events.

**Evidence:**
- `batch.hpp`: on submit failure, slot is marked ready with error result
  and reap_seq = 0.
- `next()` returns both rejected and completed slots in the same iteration.
- `reap_seq == 0` is the implicit discriminator (internal encoding, not
  explicit semantic).
- No `BatchOutcomeKind` enum distinguishes rejected vs. completed.

**Violated constitution rule:** AC-3 (transactional submission — failed
submit is NOT a completion), AC-15 (completion identity — rejected ops
should not enter the completion stream).

**Semantic consequence:** The public BatchResult does not preserve outcome
origin, so callers cannot distinguish admission rejection (operation never
executed) from execution failure (operation ran and failed). This matters for
retry logic, write idempotency, durability operations, and metrics. The
`reap_seq == 0` discriminator is a Batch-internal encoding not visible to
callers through the public result type.

**Currently regression-tested:** Tests verify error propagation but do not
test the semantic distinction.

**Recommended next action:** Phase 3 — add explicit `BatchOutcomeKind`
(rejected/completed); do not mix rejections into the completion stream.

**Do not fix in this audit PR.**

---

## P2-01: Per-Op Thread Creation (Unbounded)

**Finding:** ThreadPoolBackend spawns one `std::thread` per submitted operation.
Thread creation is expensive (~10-50μs + kernel resources). Under sustained
load, this creates and destroys threads at I/O rate.

**Evidence:**
- `threadpool_backend.hpp:8-9`: "one worker thread per outstanding op (simple,
  correct)"
- `threadpool_backend.cpp:85`: `workers_.emplace_back(...)` per op
- No capacity parameter; no reuse of terminated threads.

**Violated constitution rule:** AC-7 (bounded resources), AC-8 (execution
strategy ≠ I/O mechanism — this is not a "pool").

**Semantic consequence:** Under high I/O concurrency, thread creation overhead
dominates. System may hit OS thread limits (RLIMIT_NPROC) or memory exhaustion
from thread stacks.

**Currently regression-tested:** No resource-bound test. No test verifies
behavior under thread creation failure (beyond bad_alloc → op error).

**Recommended next action:** Phase 6 roadmap — design persistent blocking-I/O
offload workers with bounded capacity.

**Do not fix in this audit PR.**

---

## P2-02: workers_ Vector Monotonic Growth

**Finding:** The `workers_` vector grows by one entry per submitted operation
and never reclaims entries. Joined threads leave non-joinable placeholders.
Over a long-running process, this vector grows O(total_ops_submitted).

**Evidence:**
- `threadpool_backend.hpp:51-57`: documents the risk explicitly.
- `threadpool_backend.cpp:84`: `worker_idx = workers_.size()` — index grows
  monotonically.
- Poll drain joins threads but does not erase vector entries.

**Violated constitution rule:** AC-7 (no container may grow by historical
total without reclamation).

**Semantic consequence:** Retained memory grows with cumulative submissions
and is reclaimed only when the backend is destroyed. A long-running Runtime
with millions of ops accumulates millions of `std::thread` objects (each
~8-16 bytes after join, plus vector capacity). This is unbounded retained
container storage, not a strict resource leak (joined threads release OS
resources), but it is historical-growth memory retention with no reclamation.

**Currently regression-tested:** No.

**Recommended next action:** Phase 6 roadmap — vector compaction or slot reuse.

**Do not fix in this audit PR.**

---

## P2-03: Hot-Path Heap Allocation (std::function + deque)

**Finding:** Every accepted operation traverses multiple potentially
allocating hot-path operations:
1. A `std::function` (type-erased lambda — may use small-object optimization
   for small captures, but heap allocation for larger ones)
2. A `std::thread` (kernel + heap — always allocates)
3. A deque node on ready push (`deque::push_back` allocates new blocks
   periodically, not on every call)

All three are on the I/O submission/completion hot path.

**Evidence:**
- `std::function<Result<std::size_t>()>` parameter in `enqueue_size` signature.
- `workers_.emplace_back(lambda)` — thread creation.
- `ready_size_.push_back(...)` — deque allocation.

**Violated constitution rule:** AC-7 (hot-path allocation should be bounded or
pre-allocated).

**Semantic consequence:** Per-op allocation overhead; fragmentation under
sustained load; allocation failure on hot path (see P0-01).

**Currently regression-tested:** No allocation-count or allocation-free test.

**Recommended next action:** Phase 1/3 roadmap — evaluate pre-allocated request
slots and intrusive ready list.

**Do not fix in this audit PR.**

---

## P2-04: 2ms Polling Latency/CPU Tax in MIXED-WAKE

**Finding:** In MIXED-WAKE mode, backend progress is observed only when the 2ms
timed wait expires. This adds up to 2ms latency to every backend completion in
this mode, and causes periodic CPU wakes even when no progress occurred.

**Evidence:**
- ADR-execution-model §9.4.7.1: 2ms is protocol authority for MIXED-WAKE.
- Scheduler worker loop: `wake_cv_.wait_for(2ms)`.

**Violated constitution rule:** AC-6 (polling must be explicitly justified).
This IS justified and documented, but the cost is real.

**Semantic consequence:** Workloads with frequent small I/O in MIXED-WAKE mode
pay up to one observation interval (~2ms upper bound) of additional latency per
backend completion, and may cause periodic timed wakeups while this mode
remains active. Actual latency impact and wakeup frequency depend on workload
and require benchmark evidence to quantify precisely.

**Currently regression-tested:** Deterministic causal tests verify liveness but
do not measure latency.

**Recommended next action:** Phase 5 roadmap — backend wake integration may
allow reducing or eliminating the 2ms interval for backend-only waits.

**Do not fix in this audit PR.**

---

## P3-01: "ThreadPoolBackend" Name Implies Bounded Pool

**Finding:** The name "ThreadPoolBackend" suggests a fixed-size pool of reusable
workers. The implementation is a per-operation thread spawner with no bound and
no reuse. The name misleads readers about the resource model.

**Evidence:**
- `threadpool_backend.hpp:8-9`: "one worker thread per outstanding op"
- No pool size parameter; no worker reuse.
- Contrast with `BlockingIoPool` which IS a bounded pool.

**Violated constitution rule:** AC-8 (execution strategy ≠ I/O mechanism;
naming must not conflate concepts).

**Semantic consequence:** Developers may assume bounded concurrency and skip
capacity planning. Code reviewers may not question thread creation because the
name implies a pool manages it.

**Currently regression-tested:** N/A (naming issue).

**Recommended next action:** Phase 0 — rename to `PerOpThreadBackend` or
`BlockingOffloadBackend` (requires ADR for public name change).

**Do not fix in this audit PR.**

---

## P3-02: Header Comment Stale Authority Description

**Finding:** `async_io_context.hpp:68-70` contains a stale comment claiming
context-side mark_outstanding authority. This is documented in P1-01 but the
comment itself is a P3 documentation defect.

**Evidence:** See P1-01.

**Violated constitution rule:** AC-10.

**Semantic consequence:** Misleads implementers.

**Currently regression-tested:** N/A.

**Recommended next action:** Fix comment in a small corrective PR.

**Do not fix in this audit PR.**

---

## P3-03: "workers" Ambiguity Across Subsystems

**Finding:** Multiple subsystems use "worker" terminology:
- Scheduler workers (fiber-executing threads)
- ThreadPoolBackend workers (per-op I/O threads)
- BlockingIoPool workers (bounded sync pool threads)

Documentation and config do not always distinguish which "worker" is meant.

**Evidence:**
- `scheduler.hpp`: `worker_count` parameter
- `threadpool_backend.hpp`: `workers_` vector
- `blocking_io_pool.hpp`: pool size

**Violated constitution rule:** AC-8.

**Semantic consequence:** Capacity discussions may conflate independent resource
domains.

**Currently regression-tested:** N/A.

**Recommended next action:** Phase 0 — audit all "worker" references and qualify
with subsystem prefix in documentation.

**Do not fix in this audit PR.**

---

## Summary

| ID | Severity | Area | Status |
|----|----------|------|--------|
| P0-01 | P0 | Worker OOM → terminate | Phase 1 ADR + Phase 2 impl |
| P0-02 | P0 | Cross-backend transactional submit | Phase 1 ADR + Phase 2/3 impl |
| P0-03 | P0 | Completion publication authority forgeable | Phase 1 ADR + Phase 2 impl |
| P1-01 | P1 | mark_outstanding stale comment | Comment correction needed |
| P1-02 | P1 | Backend-ready vs completion-ready | Documentation needed |
| P1-03 | P1 | SyncBackend cancel bypasses reap | Phase 3 (backend migration) |
| P1-04 | P1 | Spawn failure incorrectly asyncized | Phase 3 (backend migration) |
| P1-05 | P1 | queue_full_retries semantic conflation | Phase 0/1 error vocabulary |
| P1-06 | P1 | No request generation (ABA) | Phase 1 (unified ADR) |
| P1-07 | P1 | Reap API discards completion identity | Phase 1 ADR + Phase 2 impl |
| P1-08 | P1 | wait_one holds lock, blocks cancel | Phase 4 (wait/cancel) |
| P1-09 | P1 | Cancel API not explicit | Phase 4 (wait/cancel) |
| P1-10 | P1 | Runtime await no provenance check | Phase 1 ADR + Phase 2 impl |
| P2-01 | P2 | Per-op thread creation | Phase 6 (persistent workers) |
| P2-02 | P2 | workers_ monotonic growth | Phase 6 (persistent workers) |
| P2-03 | P2 | Hot-path allocation | Phase 1 design / Phase 6 |
| P2-04 | P2 | 2ms MIXED-WAKE latency | Phase 5 (wake) |
| P2-05 | P2 | Batch conflates rejection with completion | Phase 1 ADR + Phase 2 |
| P3-01 | P3 | ThreadPoolBackend naming | Phase 0 rename |
| P3-02 | P3 | Stale header comment | Small corrective PR |
| P3-03 | P3 | "workers" ambiguity | Phase 0 audit |
