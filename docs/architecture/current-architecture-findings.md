# Current Architecture Findings

**Baseline:** `b20bcc7` (master, including PR #60 and PR #61). Findings retain
their original audit evidence where useful and include explicit resolved notes
for PR #61, the Phase B reference layer (PR #63), the Phase E blocking
backend (PR #64), and the Phase D Uring migration (PR #78/#80/#83/#84,
2026-08-09..11). No target contract is treated as implementation evidence.

Severity:
- **P0** — correctness/liveness: accepted op may be permanently lost or process
  may terminate unexpectedly
- **P1** — architecture contract conflict: documentation, interface, and
  implementation disagree on authority
- **P2** — bounded-resource/performance structure: unbounded growth, hot-path
  allocation, excessive overhead
- **P3** — naming/documentation: misleading names, stale comments

## Proposed request-contract impact (not resolution evidence)

[ADR-explicit-io-request-contract](../adr/ADR-explicit-io-request-contract.md)
now supplies a concrete target for the remaining request-lifecycle findings.
The ADR is Accepted (2026-08-02); the reference layer (Phase B, PR #63),
`ThreadPoolBackend` (Phase E, PR #64), and `UringAsyncBackend` (Phase D,
PR #78/#80/#83/#84) implement it, and findings resolved by those merges carry
resolution notes below (see the Summary table). Open findings remain open.

| Open finding | Target decision | Required follow-up evidence |
|---|---|---|
| P0-01, P2-03 | Pre-reserved result/ready linkage; no post-accept unbounded-allocation dependency | RESOLVED — Phase B/C reference terminal-path proof + Phase E (PR #64): ThreadPool post-commit path is allocation-free (see P0-01/P2-03 notes) |
| P0-02, P1-04 | Five-stage admission; pre-commit failure rejects, post-commit dispatch failure completes | RESOLVED at the reference layer (Phase B), for ThreadPool (Phase E, PR #64), and for Uring (Phase D, PR #78/#80/#83/#84) — see P0-02/P1-04 notes |
| P1-02, P1-07 | Distinct backend-ready/completion-ready plus synchronous pointer-free `ReadyEvent`/`ReadySink` delivery | Phase B/C reset/reuse-during-sink proof, Phase F Scheduler/Batch consumption |
| P1-05 | `invalid_state`, `would_block`, and `no_space` are distinct; capacity rejects have their own metric | Phase B implementation and stats contract tests |
| P1-06, P1-10 | `(context, slot, generation)` identity, private Completion `binding` transient, RequestSlot-owned stable waiter token/routing lease | Phase B/C cross-context binding/generation/fake-lease tests; Phase F Runtime/Scheduler lifetime and routing |
| P1-08, P1-09 | RequestKey-targeted cancellation and explicit disposition; exact wait/cancel lock design remains open | Phase C contract tests plus a focused later concurrency design before L1 lock changes |
| P2-05 | Batch outcome origin distinguishes rejection from accepted completion | RESOLVED — Phase F2: `BatchResultOrigin` (`rejected` vs `accepted_and_completed`) on `BatchResult` (`tests/batch_result_origin_test.cpp`; ADR Decision 9) |
| P2-01, P2-02 | Fixed persistent blocking workers and bounded queue | RESOLVED — Phase E (PR #64): persistent construction-time workers + bounded dispatch ring + directed stress evidence (see P2-01/P2-02 notes) |
| P2-04 | Backend-ready progress signal remains a separate wake contract | RESOLVED — Phase G (2026-08-15): split-wait production backends park the MW-S2 participant in the backend domain for both wake kinds (progress epoch + Scheduler interrupt bridge); the 2ms interval is condition-driven-only there; reference poll-driven backends retain it intentionally (DIV-05 amended). See the P2-04 section and ADR §9.4.7.2 |

The Accepted ADR is decision evidence only. It does not make an unchecked box,
missing regression, or current backend defect resolved.

---

## P0-01: Worker Thread Unhandled Exception → std::terminate

> **Phase E resolution (PR #64):** RESOLVED. The per-op worker lambdas that
> pushed to `ready_size_`/`ready_void_` are gone; workers are a fixed
> construction-time pool whose post-commit terminal path records into
> preallocated slot storage (`record_terminal`) and allocates nothing (Phase E
> gate Slice 12), so no accepted-request worker path can throw
> `std::bad_alloc` and terminate the process. The finding text below describes
> the pre-Phase-E legacy model and is retained as the historical audit record.
> Evidence: `docs/architecture/phase-e-compliance-gate.md` (Slices 5/12);
> `tests/threadpool_backend_reap_test.cpp`.

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

## P1-01: Completion Claim Authority — Resolved by PR #61

**Current fact:** The backend is the explicit claim authority. Derived backends
use protected `AsyncBackend::try_claim()`, which performs an atomic
`idle -> outstanding` CAS. `AsyncIoContext` serializes/routes submission but does
not claim the Completion. Claim failure returns synchronous `invalid_state`
without backend tracking or outstanding-count mutation.

**Current evidence:**
- `include/sluice/async/async_io_context.hpp` names the backend as claim authority;
- `include/sluice/async/completion.hpp` keeps the claim mutator private and friends
  only `AsyncBackend`;
- `include/sluice/async/async_io_context.hpp` exposes protected
  `AsyncBackend::try_claim()` to trusted derived backends; and
- `tests/completion_authority_death_test.cpp` proves concurrent claims have one
  winner.

**Historical PR #60 finding (not current behavior):** the public header formerly
named context-side `mark_outstanding()` authority while backends performed the
mutation. PR #61 removed that public mutator and corrected the comment. There is
no remaining P1-01 implementation action.

---

## P1-02: No Unified Backend-Ready vs. Completion-Ready Distinction

**Finding:** The current design conflates "backend has a result available"
(backend-ready) with "Completion has transitioned to ready" (completion-ready).
In ThreadPoolBackend, the worker pushes to `ready_size_` (backend-ready), and
`poll()` calls protected `publish()` (completion-ready). But the current
common API does not consistently distinguish these two states, and the
`outstanding()` counter
is decremented at poll time, not at backend-ready time.

**Evidence:**
- `src/async/threadpool_backend.cpp:86-93`: worker pushes to `ready_size_`
  (backend-ready)
- `src/async/threadpool_backend.cpp:225-248`: `poll()` publishes each local ready
  entry, decrements `outstanding_`, takes its worker, and joins outside the lock.
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

**Recommended next action:** Phase B/C must implement and test the two states and
the synchronous pointer-free ReadySink selected by the Proposed request-contract
ADR, including reset/reuse during callback delivery; Phase F migrates
Scheduler/Batch consumption. The ADR decision alone is not resolution evidence.

**Do not fix in this audit PR.**

---

## P0-02: Cross-Backend Transactional Submit Defect (No Unified Admission Contract)

> **Phase D resolution (PR #78/#80/#83/#84):** RESOLVED for Uring. The
> post-claim `register_op` allocation window is gone: Uring now runs the same
> five-stage `RequestArena` admission as the other backends (reserve → prepare →
> Stage 1.5 descriptor validation → two-stage binding → commit → enqueue →
> dispatch) with generation-safe cookie-routed `user_data` on a single private
> ring, a `TransportLedger` preserving unsubmitted suffixes after partial submit
> (P0-D recovery), and accepted-terminal paths that allocate nothing. The
> finding text below describes the pre-Phase-D legacy model and is retained as
> the historical audit record. Evidence: `docs/architecture/phase-d2-uring-failure-noalloc-gate.md`,
> `docs/history/closeout/phase-d4-uring-wait-close-drain-gate.md`; real-liburing conformance run
> (audit issue #94): KernelIo ELIGIBLE.

**Current finding:** PR #61 made claim authority atomic and added a narrow
pre-accept rollback capability, but the backend family still lacks one bounded
reserve/prepare/commit/enqueue/dispatch transaction. Fake and Sync allocate
tracking containers after `try_claim()`. ThreadPool performs task/thread/ready
storage work after claim. Uring prepares an SQE before fallible registration of
the operation in all parallel maps and `pending_sqes`.

**Current evidence (per backend):**

- **FakeAsyncBackend** (`include/sluice/async/fake_backend.hpp`):
  ```cpp
  if (!try_claim(c)) ...;
  ready_size_.push_back(&c);    // can throw bad_alloc
  pending_size_.push_back(op.len); // can throw bad_alloc
  ```
  Either push_back failure leaves Completion outstanding with incomplete
  backend records.

- **SyncBackend** (`include/sluice/async/sync_backend.hpp`):
  ```cpp
  if (!try_claim(c)) ...;
  entries_.push_back(Entry{op, &c, false}); // can throw bad_alloc
  ```
  Allocation failure leaves Completion outstanding with no entry to poll.

- **UringAsyncBackend** (`src/async/uring_backend.cpp:499+`): claim happens
  before SQE acquisition. A null SQE rolls the claim back, but after an SQE is
  prepared `register_op()` still performs fallible insertions:
  ```cpp
  io_uring_prep_read(sqe, ...);
  comp_to_op.emplace(...);      // can throw
  ops.emplace(...);             // can throw
  pending_sqes.push_back(...);  // can throw
  ```
  An exception can leave a claimed Completion, a live prepared SQE, and partial
  identity maps with no safe rollback.

- **ThreadPoolBackend** — see P0-01 (worker OOM) and P1-04 (spawn failure).

**Violated constitution rule:** AC-3 (transactional submission — failed submit
MUST leave Completion idle), AC-4 (accepted operation must terminate).

**Semantic consequence:** The root problem is not any single backend's OOM
path. After claim, an allocation can strand identity; for Uring, a prepared
userspace SQE can also outlive attempted compensation and later execute against
released request resources.

**Currently regression-tested:** PR #61 tests claim races and null-SQE rollback.
There is no cross-backend OOM/admission suite, and the post-SQE `register_op()`
allocation window remains untested and open.

**Recommended next action:** Phases B/C — implement and prove the five-stage
transactional admission selected by the Proposed request-contract ADR. Phase D
must acquire/fill SQEs after commit, retain partial-submit suffixes for retry,
and forbid terminal publication or slot reuse until no SQE/kernel/CQE reference
can remain. Migrate blocking offload in Phase E.

**Do not fix in this audit PR.**

---

## P1-03: SyncBackend Cancellation Publication — Resolved by PR #61

**Current fact:** `SyncBackend::cancel()` only marks the matching buffered entry
as canceled. `poll()`/`wait_one()` later derive the canceled terminal result and
publish it through protected `AsyncBackend::publish()`; cancellation does not
make the Completion ready inline.

**Current evidence:** `include/sluice/async/sync_backend.hpp` and
`tests/async_completion_test.cpp`
(`cancel_outstanding_op_completes_canceled`).

**Historical PR #60 finding (not current behavior):** SyncBackend formerly
published cancellation directly. PR #61 moved that transition into the reap
path. There is no remaining P1-03 implementation action; the separate lack of
an explicit cancel disposition remains P1-09.

---

## P1-04: ThreadPoolBackend Spawn Failure Has No Explicit Admission Boundary

> **Phase E resolution (PR #64):** RESOLVED. Worker threads are created only at
> construction (`ThreadPoolConfig::worker_count`); a construction failure stops
> and joins the started workers and rethrows, so thread-creation failure is a
> synchronous construction-time admission boundary — no submit path can hit a
> `std::thread` construction throw after the Completion claim. The finding text
> below describes the pre-Phase-E legacy model and is retained as the historical
> audit record. Evidence: `docs/architecture/phase-e-compliance-gate.md` (Slice
> 11: partial worker-construction cleanup) and the Phase E contract suite.
>
> **C2d resolution (Phase C):** the missing regression test is now
> closed. `tp_c2d_partial_worker_startup_failure`
> (`tests/threadpool_backend_c2d_failure_test.cpp`) injects a
> `std::system_error(errc::resource_unavailable_try_again)` worker-spawn
> failure at a chosen index via a `SLUICE_ASYNC_INTERNAL_TESTING`-guarded
> static seam (constructor-before-instance; RAII-restored) and proves the
> constructor propagates the failure synchronously (both before the first
> worker and after one started), that the already-started workers exit and are
> joined (surviving the failed construction with no `std::terminate` IS the
> join proof — an unjoined joinable thread vector aborts), and that a normal
> construction afterwards succeeds with the full worker count. See
> `docs/architecture/phase-c2d-compliance-gate.md` §3.2 and
> `docs/verification/phase-c2d-failure-injection-mutation-evidence.md`
> (mutants M4/M5/M9).

**Finding:** When `std::thread` construction throws in
`ThreadPoolBackend::enqueue_size`, the catch handler (`fail_spawn_size`)
pushes an error entry to the ready queue and the Completion is eventually
completed with an error via poll(); `submit_read()` returns success. The code
has already claimed the Completion, but no explicit commit separates rejection
from an accepted dispatch failure and no terminal ready entry was reserved.

Under the Proposed request contract, thread creation would be post-commit
dispatch and an ownership-safe spawn failure would correctly become an accepted
terminal error. The current implementation is still non-conforming because the
commit boundary is implicit and `fail_spawn_size()` / `fail_spawn_void()` may
allocate while trying to preserve terminality.

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

**Violated constitution rule:** AC-3 (no explicit transactional admission),
AC-4 (terminal error staging can allocate), and AC-7 (unbounded per-op spawn).

**Semantic consequence:** The accepted/rejected classification depends on the
incidental location of `try_claim()`, while OOM in the compensating ready-queue
push can still terminate or strand the accepted request.

**Currently regression-tested:** No test injects thread-creation failure.

**Recommended next action:** Phase E — replace per-op spawning with bounded
persistent workers under the five-stage admission contract. Reserve queue and
terminal linkage before commit; reject earlier failures synchronously and make
ownership-safe failures after commit terminal.

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
slots are added (Phase B), operators cannot distinguish:
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

**Recommended next action:** Phase B/C — rename the stat or split into
`lifecycle_violations` and `capacity_rejects`. Introduce distinct error codes
for capacity pressure vs. lifecycle errors.

**Do not fix in this audit PR.**

---

## P0-03: Completion Publication Authority — Resolved by PR #61

**Current fact:** Completion publication mutators are private. `AsyncBackend`
alone is friended and exposes protected `try_claim()`/`publish()`/
`rollback_claim_before_accept()` capabilities to trusted backend authors.
Claim and publication use CAS winner transitions; invalid reset or destruction
while outstanding/publishing/resetting fail-fast in Debug and Release. Destroying
idle or ready remains allowed.

**Current evidence:**
- `include/sluice/async/completion.hpp` and
  `include/sluice/async/async_io_context.hpp`;
- `scripts/verify-completion-authority-negative-compile.sh`, wired into CI; and
- `tests/completion_authority_death_test.cpp` for invalid lifecycle, concurrent
  claim, and concurrent publication cases.

**Historical PR #60 finding (not current behavior):** publication and claim
mutators were public, claim was not a CAS, and outstanding reset/destruction was
not Release fail-fast. PR #61 closed those authority and lifecycle defects.

**Residual tracked elsewhere:** P0-02 remains open because backend admission
containers can still allocate after claim; Uring can prepare an SQE before
fallible `register_op()` bookkeeping. That is not a remaining publication
authority leak.

---

## P1-06: No Request Generation — ABA on Completion Reuse

> **Phase D resolution (PR #83):** RESOLVED for Uring. Generation safety now
> covers all four backends: `SQE.user_data` carries a 64-bit op cookie
> (generation-safe; `uring_backend.cpp:101-119`), and the arena's per-slot
> `Generation` increment-on-release rejects stale keys across every post-reserve
> authority (C2b real-mode evidence, `docs/history/closeout/phase-d3-uring-identity-waiter-gate.md`).
> The finding text below describes the pre-Phase-B/D state and is retained as
> the historical audit record.

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

**Recommended next action:** Phase B/C RequestKey/RequestSlot implementation —
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

**Recommended next action:** Phase B/C reference reap, then Phase F integration — backend
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

**Recommended next action:** Focused wait/cancel concurrency design before Phase F —
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
- SyncBackend cancel: records a cancel bit; the next reap publishes canceled.
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

**Recommended next action:** Phase B/C internal mechanics plus a focused API
review before a public change — redesign cancel to return
`Result<CancelDisposition>` targeting a RequestKey.

**Do not fix in this audit PR.**

---

## P1-10: Runtime await Has No Request Provenance Check

**Finding:** `RuntimeTaskContext::await_completion()` only checks
`assert(!c.idle())` (vanishes in Release). It cannot distinguish:
- Completion submitted via THIS Runtime's context;
- Completion submitted via ANOTHER context;
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

**Recommended next action:** Phase B/C proves the Completion
`idle -> binding -> outstanding` protocol plus RequestSlot-owned opaque waiter
tokens and duplicate rejection, followed by Phase F Runtime/Scheduler routing.

**Do not fix in this audit PR.**

---

## P2-05: Batch Conflates Submit Rejection with Terminal Completion

> **Phase F resolution (branch `feat/phase-f-remaining`, Phase F2, commit
> `d096f1f`):** RESOLVED. `BatchResult` now carries an explicit admission-origin
> discriminator — `BatchResultOrigin` (`rejected` vs `accepted_and_completed`)
> on `BatchResult` (ADR-explicit-io-request-contract's batch admission-origin
> requirement; `tests/batch_result_origin_test.cpp`). Production sets
> `submit_rejected = true` on the ONLY synchronous submit-failure path; an
> accepted op — success, error, or canceled — reaches the caller through the
> ordinary Completion/reap path with `origin == accepted_and_completed`, so
> admission origin stays orthogonal to success/error and `reap_seq` remains a
> pure completion-ordering discriminator. The summary table at the top of this
> document is updated; the detail below is the historical pre-F2 audit record.

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

**Recommended next action:** Phase F — add explicit `BatchOutcomeKind`
(rejected/completed); do not mix rejections into the completion stream.

**Do not fix in this audit PR.**

---

## P2-01: Per-Op Thread Creation (Unbounded)

> **Phase E resolution (branch `feat/phase-e-bounded-threadpool-explicit-io`):**
> RESOLVED. ThreadPoolBackend now uses a fixed pool of persistent blocking-I/O
> workers created only at construction (`ThreadPoolConfig::worker_count`); no
> thread is created per op and worker storage never grows. The line references
> below describe the pre-Phase-E legacy model and are retained as the historical
> audit record. Regression: `tests/threadpool_backend_reap_test.cpp`
> (`workers_spawned_for_test == worker_count` for the backend's whole life).
> See DIV-03 (Resolved).

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

**Recommended next action:** Phase E roadmap — design persistent blocking-I/O
offload workers with bounded capacity.

**Do not fix in this audit PR.**

---

## P2-02: workers_ Vector Monotonic Growth

> **Phase E resolution:** RESOLVED. The historical-growth `workers_` vector is
> gone; the worker pool is a fixed `std::vector<std::thread>` of size
> `worker_count` created once. The line references below are the legacy audit
> record. See DIV-12 (Resolved).

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

**Recommended next action:** Phase E roadmap — persistent workers and bounded
RequestSlot reuse remove the historical per-op vector model.

**Do not fix in this audit PR.**

---

## P2-03: Hot-Path Heap Allocation (std::function + deque)

> **Phase E resolution:** RESOLVED for ThreadPoolBackend. The `std::function`
> payload and the per-op `ready_size_`/`ready_void_` deques are gone: the
> payload is a fixed `PreparedBlockingOp` per slot, the dispatch ring is a
> construction-time bounded `BoundedDispatchQueue`, and the terminal path goes
> through the `RequestArena` ready-ring (no per-op allocation). The accepted
> terminal path is allocation-independent (ADR Decision 14 / I9). The line
> references below are the legacy audit record.

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

**Recommended next action:** Phase B/C reference and Phase D/E backends — use pre-allocated request
slots and intrusive ready list.

**Do not fix in this audit PR.**

---

## P2-04: 2ms Polling Latency/CPU Tax in MIXED-WAKE — Resolved by Phase G

**Status: RESOLVED (Phase G, 2026-08-15).** On split-wait production
backends (ThreadPool, real io_uring), MIXED-WAKE now parks the MW-S2
participant in the BACKEND domain for both wake kinds: backend progress
arrives through the wait source's own epoch (prompt), and external wakes
arrive through the Scheduler interrupt bridge (`signal_wake_locked` ->
`backend_wait_active_` -> `interrupt_backend_waiters`, control-epoch bump,
one-shot per invocation). The 2ms interval survives there only as a
condition-driven park cap (active E11 deadline, or E5-A2 ready-flag poll
resolution); with neither present the park is unbounded and event-driven —
no fixed-interval latency or periodic CPU wake. Reference poll-driven
backends (Fake, Sync/Synthetic) intentionally retain the bounded
observation interval (DIV-05 reference exemption): their readiness cannot
self-notify. See ADR-execution-model §9.4.7.2, `docs/architecture/
phase-g-compliance-gate.md`, and the closeout causal tests
(`tests/phase_g_closeout_test.cpp` Cases A–D).

**Original finding (pre-Phase G, retained for history):**

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

**Recommended next action:** Phase G roadmap — backend wake integration may
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

**Recommended next action:** Phase E — select the production blocking-offload
name as part of the public migration (a public rename requires API review).

**Do not fix in this audit PR.**

---

## P3-02: Header Claim-Authority Comment — Resolved by PR #61

**Current fact:** `include/sluice/async/async_io_context.hpp` names the backend
as Completion claim authority and exposes the protected `try_claim()` capability.

**Historical PR #60 finding (not current behavior):** the header formerly named
context-side claim authority. PR #61 corrected it; no follow-up remains.

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

**Recommended next action:** Phases E/G — audit all "worker" references and qualify
with subsystem prefix in documentation.

**Do not fix in this audit PR.**

---

## Summary

| ID | Severity | Area | Status |
|----|----------|------|--------|
| P0-01 | P0 | Worker OOM → terminate | RESOLVED — Phase E (PR #64): fixed construction-time worker pool; worker post-commit terminal path allocates nothing (see section note; phase-e gate Slices 5/12) |
| P0-02 | P0 | Cross-backend transactional submit | Reference-layer CLOSED (Phase B round 2: bounded RequestArena + five-stage admission on Fake/Sync; the Completion publication binding lives IN the RequestSlot record (no parallel map), the fake's submission-order FIFO is a construction-time bounded ring, and the Completion-ready release-store happens INSIDE the leaf domain (single-domain reap) — pre-commit bookkeeping is transactional and the accepted path performs ZERO allocations (proven by counting + always-throw operator new in `reference_backend_no_alloc_test.cpp`); a lost binding CAS leaves Completion/slot/FIFO/counters untouched with no result contamination); ThreadPool transactional admission RESOLVED (Phase E, PR #64 — `would_block` at capacity, pre-commit rollback, no borrow); Uring RESOLVED (Phase D, PR #78/#80/#83/#84 — five-stage arena admission, cookie-routed `user_data`, TransportLedger partial-submit preservation, allocation-free accepted terminal; real-mode evidence, KernelIo ELIGIBLE) |
| P0-03 | P0 | Completion publication authority | RESOLVED — ADR-explicit-io-completion-authority / PR #61 |
| P1-01 | P1 | Completion claim authority | RESOLVED — ADR-explicit-io-completion-authority / PR #61 (backend is claim authority) |
| P1-02 | P1 | Backend-ready vs completion-ready | RESOLVED — reference-layer CLOSED (Phase B); Phase F1 closed the Scheduler consumption (ReadyRoutingSink consumes identity-bearing reap; the O(N) `Completion::ready()` re-scan removed from the arena path) |
| P1-03 | P1 | SyncBackend cancel publication | RESOLVED — ADR-explicit-io-completion-authority / PR #61 (cancel records intent; reap publishes) |
| P1-04 | P1 | Spawn failure lacks explicit admission boundary | RESOLVED — Phase E (PR #64): workers created only at construction; ctor failure stops/joins started workers and rethrows (see section note; phase-e gate Slice 11) |
| P1-05 | P1 | queue_full_retries semantic conflation | Reference-layer CLOSED (Phase B round 2: distinct `slot_in_use` vs `accepted_outstanding` vs `capacity_rejections` counters on the arena; capacity pressure propagates as `would_block` — never `invalid_state` — and tallies `queue_full_retries`, while caller lifecycle violations (`invalid_state`) tally the new `AsyncStats::invalid_state_rejections` — the two are never conflated); Phase C metrics integration pending |
| P1-06 | P1 | No request generation (ABA) | Reference-layer CLOSED (Phase B: per-slot Generation incremented on release before reuse; stale-key rejection proven across every post-reserve authority); ThreadPool generation-safe as of Phase E (PR #64); Uring RESOLVED (Phase D, PR #83 — C2b real-mode evidence; cookie-routed `user_data`) |
| P1-07 | P1 | Reap API discards completion identity | RESOLVED — reference-layer CLOSED (Phase B: identity-bearing SynchronousReadySink delivers by-value ReadyEvent{RequestKey, OperationKind, OptionalWaiterDelivery}); Phase F1 closed the Scheduler consumption (the re-scan that discarded identity is gone) |
| P1-08 | P1 | wait_one holds lock, blocks cancel | RESOLVED — the focused wait/cancel concurrency design landed in Phase F1 (split-phase `wait_one` under issue #67 + the `wait_registry_mtx_` leaf lock; lock-order table in the F1 design) |
| P1-09 | P1 | Cancel API not explicit | Reference-layer CLOSED at arena (Phase B: RequestKey-targeted cancel returns CancelDisposition); public-API change deferred to a later ADR |
| P1-10 | P1 | Runtime await no provenance check | RESOLVED — reference-layer CLOSED (Phase B: RequestKey provenance is the cancel/reap identity); Phase F1 enforced provenance at the Scheduler registration (cross-context / duplicate-waiter → `invalid_state`); Phase F3 exposed it publicly via `RequestHandle` + `request_state` (cross-context → `not_found`) |
| P2-01 | P2 | Per-op thread creation | RESOLVED — Phase E (PR #64): fixed persistent workers, no per-op thread creation (see section note; DIV-03) |
| P2-02 | P2 | workers_ monotonic growth | RESOLVED — Phase E (PR #64): fixed worker vector, no historical growth (see section note; DIV-12) |
| P2-03 | P2 | Hot-path allocation | Reference-layer CLOSED (Phase B round 2: allocation-independent accepted terminal path — fixed RequestSlot array, pre-reserved terminal storage, publication binding in the slot record, construction-time bounded FIFO ring; the accepted submit → poll → reset path performs ZERO allocations under a counting + always-throw operator new); Phase E backend migration RESOLVED (PR #64: ThreadPool post-commit path allocation-free — phase-e gate Slice 12; see section note) |
| P2-04 | P2 | 2ms MIXED-WAKE latency | RESOLVED — Phase G: backend-domain MIXED-WAKE park + interrupt bridge on split-wait backends; condition-driven park cap only (see section note; ADR §9.4.7.2; DIV-04/DIV-05 amended) |
| P2-05 | P2 | Batch conflates rejection with completion | RESOLVED — Phase F2 (`BatchResultOrigin`; `tests/batch_result_origin_test.cpp`) |
| P3-01 | P3 | ThreadPoolBackend naming | Phase E decision (PR #64): name retained for API continuity (DIV-03); the backend is now genuinely bounded, so the misleading resource-model semantic is gone; a rename would require a separate API ADR |
| P3-02 | P3 | Header claim-authority comment | RESOLVED — PR #61 corrected backend claim authority |
| P3-03 | P3 | "workers" ambiguity | Phase E/G documentation audit |
