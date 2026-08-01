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

**Recommended next action:** Phase 3 roadmap — design persistent blocking-I/O
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

**Semantic consequence:** Memory leak proportional to total I/O operations over
the backend's lifetime. A long-running Runtime with millions of ops accumulates
millions of `std::thread` objects (each ~8-16 bytes after join, plus vector
capacity).

**Currently regression-tested:** No.

**Recommended next action:** Phase 3 roadmap — vector compaction or slot reuse.

**Do not fix in this audit PR.**

---

## P2-03: Hot-Path Heap Allocation (std::function + deque)

**Finding:** Every accepted operation allocates:
1. A `std::function` (heap, type-erased lambda) — `threadpool_backend.cpp:85`
2. A `std::thread` (kernel + heap) — same line
3. A deque node on ready push — `threadpool_backend.cpp:89`

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
pay 2ms p99 latency tax. CPU wakes 500 times/second per parked worker even
when idle.

**Currently regression-tested:** Deterministic causal tests verify liveness but
do not measure latency.

**Recommended next action:** Phase 2 roadmap — backend wake integration may
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
| P0-01 | P0 | Worker OOM → terminate | Open issue needed |
| P1-01 | P1 | mark_outstanding authority | Comment correction needed |
| P1-02 | P1 | Backend-ready vs completion-ready | Documentation needed |
| P2-01 | P2 | Per-op thread creation | Phase 3 roadmap |
| P2-02 | P2 | workers_ monotonic growth | Phase 3 roadmap |
| P2-03 | P2 | Hot-path allocation | Phase 1/3 roadmap |
| P2-04 | P2 | 2ms MIXED-WAKE latency | Phase 2 roadmap |
| P3-01 | P3 | ThreadPoolBackend naming | Phase 0 rename |
| P3-02 | P3 | Stale header comment | Small corrective PR |
| P3-03 | P3 | "workers" ambiguity | Phase 0 audit |
