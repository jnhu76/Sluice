# Design of Phase E — Bounded Blocking-I/O Backend (ThreadPoolBackend → RequestArena)

> **Archived 2026-08-25 (issue #167 Step 5).** Moved from `docs/design/`;
> classification at move: PINNED-EVIDENCE → CLOSED-HISTORY (implemented, frozen
> design record; all consumers updated atomically). Body preserved as-written;
> see `docs/history/README.md`.

**Author:** jnhu
**Date:** 2026-08-04
**Status:** Design frozen (governs the `feat/phase-e-bounded-threadpool-explicit-io` implementation)
**Governing ADR:** [ADR-explicit-io-request-contract](../../adr/ADR-explicit-io-request-contract.md) (Accepted)
**Compliance gate:** [phase-e-compliance-gate.md](../closeout/phase-e-compliance-gate.md)
**Constitution rules touched:** AC-1, AC-2, AC-3, AC-4, AC-5, AC-6, AC-7, AC-8, AC-9, AC-10,
AC-11, AC-12, AC-13

This design fills the design-compliance gate (Gate 0–4) for the Phase E backend migration.
It is binding for the implementation; divergence requires a superseding ADR or closeout note
(AGENTS.md §2). Production backend changes MUST NOT land until this design is frozen.

The work proceeds in **vertical TDD slices** (one failing test → minimal code to pass → repeat),
not as a single batch of pre-written tests. The slice order below is the implementation order.

---

## 1. Problem

`ThreadPoolBackend` is the last production blocking-I/O path still on the legacy model. The
as-built architecture (audit baseline `7f434f0`, post-Phase-B) is:

```text
submit
-> try_claim(c)                      # legacy single-step claim, not the 5-stage admission
-> enqueue_size(work = std::function<...>)
-> spawn one std::thread per op
-> worker runs the syscall, pushes ReadySize{c*, result, worker_idx} into a ready deque
-> poll()/wait_one() publish the Completion and join the per-op thread
```

This is NOT a small performance issue. It violates the Accepted explicit-I/O contract on four
counts:

| Violation | What the as-built code does | Contract it breaks |
|---|---|---|
| **No stable identity** | identity is `Completion*` + `std::function` capture + a `workers_` slot index | AC-2, AC-14 (no `RequestKey`/generation) |
| **Non-transactional submit** | the worker thread is spawned AFTER `try_claim` but the spawn can throw after the claim is won | AC-3 / I3 |
| **Terminal path depends on allocation** | the worker's `ready_size_.push_back(...)` can throw `bad_alloc` and terminate the process, losing the accepted result | AC-4 / I4 / I9 |
| **Unbounded resources** | `workers_` grows by one entry per submitted op and is never reclaimed; there is no `request_capacity`, no `would_block` | AC-7, DIV-03, DIV-12 |

It is the backend named in DIV-03 ("thread-per-op, not thread-per-task") and DIV-12 ("unbounded,
accidental drift"). Phase E is the roadmap phase that resolves both.

---

## 2. Target architecture

`ThreadPoolBackend` becomes the first production backend that runs **real POSIX syscalls** through
the bounded `RequestArena` / `RequestSlot` lifecycle:

```text
fixed count of persistent blocking-I/O workers
+ construction-time bounded dispatch queue (fixed ring, capacity == request_capacity)
+ RequestArena / RequestSlot as the ONE request lifecycle authority
+ fixed tagged operation payload (no std::function)
+ worker consumes SlotHandle / RequestKey, runs the syscall, records backend-ready ONLY
+ reap (poll/wait_one) is the ONLY Completion-ready publication authority
```

After Phase E:

```text
Fake / Sync :  explicit-I/O reference backends (Phase B)
ThreadPool  :  explicit-I/O real portable blocking backend (Phase E)
Uring       :  remaining production backend migration (Phase D)
Scheduler / Runtime / Batch : remaining upper-layer integration (Phase F/G)
```

Phase E does NOT migrate Uring, Scheduler routing, Runtime wake, Batch, or introduce a public
`RequestHandle` (ADR "Required implementation sequence"; AGENTS.md §23 forbidden list).

---

## 3. Configuration and resource model

```cpp
struct ThreadPoolConfig {
    std::size_t request_capacity = 64;   // == arena capacity; == dispatch ring capacity
    std::size_t worker_count = 4;        // persistent blocking-I/O workers
};
```

Both must be `> 0`. `worker_count` may be derived from `std::thread::hardware_concurrency()` but
MUST handle a `0` return with a documented non-zero fallback. The default constructor stays
available (`ThreadPoolBackend()` uses the defaults above). This is a **public API addition**
(header + contract tests + api-reference), subject to the §6.1 change-class gate.

These are DISTINCT resources (ADR Decision 13, AC-7, AC-8) and MUST NOT be conflated:

| Resource | Capacity | Owner | Grows after construction? |
|---|---|---|---|
| `request_capacity` (arena slots) | `request_capacity` | backend/arena | no |
| dispatch ring entries | `request_capacity` | backend | no |
| blocking-I/O worker threads | `worker_count` | backend | no |
| Scheduler worker count | (Runtime) | Runtime | n/a (Phase F) |
| io_uring queue depth | (Uring) | Uring | n/a (Phase D) |
| caller-owned Completions | caller | caller | n/a |

Resource equations that the implementation and tests MUST prove:

```text
0 <= accepted_outstanding <= request_capacity
0 <= slot_in_use          <= request_capacity
0 <= dispatch_queue_size  <= request_capacity
0 <= active_workers       <= worker_count

worker threads created after successful construction = 0
dispatch storage growth after construction          = 0
workers storage growth after construction           = 0
```

---

## 4. State machine (Gate 1)

`ThreadPoolBackend` adds ONE backend-visible state to the Phase-B arena lifecycle: `running`
(the worker holds execution ownership of a blocking syscall). The full lifecycle the backend
drives is:

```text
free
  | reserve                                   [backend admission, arena lock]
  v
reserved
  | prepare(kind, borrow)                     [arena lock; slot invisible]
  v
prepared
  | install_publication_binding               [arena lock]
  | begin_binding(c) CAS idle->binding        [Completion state, context lock held]
  | commit  prepared->pending, pin begins     [arena lock; LP]
  | install_binding + commit_binding          [Completion binding->outstanding]
  v
pending
  | enqueue  pending->enqueued  OR  pending->backend_ready(canceled)   [one slot-state domain]
  v
enqueued
  | dispatch: worker pop + mark_running (enqueued->running)            [work domain + arena]
  v
running
  | record_terminal(verbatim syscall result)  [first terminal winner, arena]
  v
backend_ready
  | reap: validate key/binding, close reg, install result, publish Completion-ready
  v
completion_ready
  | caller reset()/ready-destroy: release_completed_binding, generation++
  v
free
```

### 4.1 The five-stage submission transaction (per `submit_*`)

Mirrors `SyncBackend::submit_size` (the reference) exactly, then adds the real-syscall dispatch
onward. One submit does:

```text
1.  validate + normalize the descriptor (real syscall backend: AC-2/§12 of this design)
2.  reserve()                  -> SlotHandle        [arena lock; would_block if full]
3.  prepare(h, kind, borrow)                       [arena lock; invalid_argument if malformed]
4.  store fixed PreparedBlockingOp into per-slot scratch indexed by h.slot
5.  install_publication_binding(h, &c, len, &publish_size_ready|&publish_void_ready)
6.  begin_binding(c) CAS idle->binding             [loser: rollback own slot, return invalid_state]
7.  commit(h) prepared->pending, pin, ++accepted_outstanding, borrow begins
8.  install_binding(c, &arena_, h)                 [slot-release capability]
    9.  commit_binding(c) binding->outstanding         [== submit LP]
    10. `enqueue_after_commit(h)` (noexcept): under one `work_mtx_` critical section,
        `arena_.enqueue(h)` then, iff `enqueued`, `dispatch_.push_back(h)`; notify_one worker
    11. if terminal_noop: the cancel/terminal winner owns readiness; signal backend-ready progress
    12. return {}
```

The helper `enqueue_after_commit` closes the load-bearing gap where `enqueue` had published the
slot as `enqueued` and cleared the enqueue pin, but `push_back` had not yet run under a separate
critical section. Both outcomes (`enqueued` and `terminal_noop`) are handled allocation-free and
noexcept, satisfying I9.

**Pre-commit rollback (review C1, I3):** every failure before step 9 calls
`arena_.rollback_reserved_or_prepared(h)`; a lost CAS (step 6) additionally calls
`rollback_binding_before_accept(c)`. A failed validation (step 1) returns before any slot touch.
Every rejection leaves: Completion idle, no slot, no borrow, `accepted_outstanding` unchanged,
no queue entry, no worker execution.

**Post-commit (I9):** after step 9 nothing throws, allocates, rejects, drops the request, creates
a thread, or expands the queue. `enqueue` has exactly two legal outcomes: `enqueued` or
`terminal_noop`. A post-commit ring-full is a **fail-fast invariant**, not `would_block`.

### 4.2 Dispatch / dequeue ownership (the load-bearing concurrency — ADR I17 + this phase)

The forbidden gap is:

```text
worker pops h from the ring
-> worker has NOT yet mark_running
-> cancel terminalizes the enqueued request + reap publishes + caller resets (generation++)
-> worker mark_running(stale h)   # USE-AFTER-FREE on the generation
```

This is closed by making **dequeue + mark_running one coordinated ownership transfer under the
backend work domain**, never exposing a popped-but-not-running window. The enqueue→push path is
similarly one coordinated transfer under `work_mtx_` (§4.1), so the only legal cancel window for an
`enqueued` request is **before the worker pops it**:

```text
worker:
  lock work_mtx_
  h = ring.pop_front()                 # only if present
  owns = arena_.mark_running(h)        # enqueued->running UNDER work_mtx_, before unlock
  if owns: copy PreparedBlockingOp[ h.slot ], ++active_workers
  unlock work_mtx_
  if owns: execute the blocking syscall (no backend lock held)
```

`arena_.mark_running` returns `true` on `enqueued->running`; it returns `false` ONLY for the
legitimate backoff (a current-generation slot already `backend_ready` because a terminal winner
won before dispatch). A stale-generation handle is `request_arena_dispatch_stale_fail_fast` in
both Debug and Release (round-5 of Phase B) — stale dispatch is an invariant violation, not
normal backoff.

### 4.3 Cancel (ADR Decision 11, layered — AC-9)

Cancel is the **Completion-keyed** public API; it resolves `Completion* -> SlotHandle` via the
arena's bounded `resolve_completion` scan, then drives the shared state machine:

```text
cancel(Completion<T>& c):
  h = arena_.resolve_completion(&c)            # bounded O(capacity), no parallel map
  if !h: return                                 # not found / idle / stale
  lock work_mtx_
  removed = ring.remove_exact(h)                # try to take it off the dispatch ring
  disp = arena_.cancel(h)                       # pending/enqueued: may win terminal; running: intent only
  unlock work_mtx_
  if disp == terminal_won:
      tally_canceled()                          # stats->canceled_ops++ (exactly-once)
      signal_ready_progress()                   # backend-ready progress wake (epoch)
  # intent_recorded: no tally, no terminal; the syscall's real result wins verbatim
```

By state:

| Slot state | `arena_.cancel(h)` | Effect |
|---|---|---|
| `pending` | `terminal_won` (Scheme B) | stores `canceled` terminal, publishes ready linkage; the enqueue pin stays live; later `enqueue` is a `terminal_noop`; **syscall never runs** |
| `enqueued` (still on ring) | `terminal_won` after `ring.remove_exact` | request is off the ring; **worker cannot dispatch it** |
| `enqueued` (worker already popped + mark_running) | `intent_recorded` | the syscall runs; its **real result wins verbatim**; a confirmed interruption records `err(canceled)` explicitly |
| `running` | `intent_recorded` | best-effort intent only (DIV-10); real result competes and wins verbatim |
| `backend_ready` / `completion_ready` | `already_terminal` | no-op |
| free / reserved / prepared / stale | `not_found` | no-op |

The key race-closure: **`remove_exact` + `cancel` happen under one `work_mtx_`**, so a request
cannot be both off the ring (cancel removed it) and simultaneously being dispatched by a worker
(worker holds the ring lock during pop+mark_running). If `remove_exact` misses, the worker already
did pop+mark_running atomically, so `cancel` observes `running` and only records intent. There is
no external window where a request is "popped but not running." (ADR §10.4.)

Phase E does NOT implement `pthread_kill`/`tgkill` interruption (DIV-10 stays "Accepted"); a
running syscall is not forcibly interrupted, and a cancel intent MUST NOT rewrite an ordinary
success into `canceled` (round-4 closeout; AGENTS.md §11).

### 4.4 Terminal winner and publication (ADR Decision 9/12, I10/I11 — reap-only)

The worker NEVER holds a `Completion*` and NEVER calls `publish`. It does only:

```text
  result = run_syscall(prepared_op)            # pread/pwrite/fdatasync/fsync via retry_on_eintr
  terminal = syscall_to_terminal(result)       # ok_bytes(n) | ok_void() | err(errno)
  syscall_count_.fetch_add(1, relaxed)         # bookkeeping BEFORE observable terminal
  { lock work_mtx_; --active_workers_; }       # worker no longer "running" before terminal
  arena_.record_terminal(h, terminal)          # first caller wins; losers no-op
  signal_ready_progress()                       # backend-ready progress wake
```

Bookkeeping (`syscall_count_` and `active_workers_`) is published **before**
`record_terminal`. An observer that sees a backend-ready Completion therefore also sees accurate
worker bookkeeping: `active_workers` no longer includes this op and `syscall_count` includes it.

`poll()` / `wait_one()` drive `arena_.reap(sink_)`; reap is the SOLE path that installs the
result and release-stores the Completion ready (via the slot's `publish_size_ready` /
`publish_void_ready` thunk, inside the leaf domain). `outstanding()` returns
`arena_.accepted_outstanding()`; the legacy duplicate `outstanding_` counter is deleted.

### 4.5 Wake protocol — split-phase ready epochs (Gate 3, AC-6, AGENTS.md §13.2; issue #67 corrective)

The old `cv_.wait(ready deque nonempty)` is insufficient: backend-ready authority lives in the
arena (a `backend_ready` slot that is still **pinned** is temporarily reap-ineligible, I19), and a
ready result can be recorded between the snapshot and the wait without a deque entry existing. To
close the commit-to-sleep race without polling:

```text
ready domain (detail::ReadyWaitSource — a leaf domain):
  ready_mtx_, ready_cv_
  std::uint64_t ready_epoch_    # real readiness published
  std::uint64_t control_epoch_  # control-plane wake (close_admission / runtime stop)

snapshot():   { lock ready_mtx_; return {ready_epoch_, control_epoch_}; }

signal_progress():               # real readiness: state published FIRST, then notify
  { lock ready_mtx_; ++ready_epoch_; }
  ready_cv_.notify_all()         # concurrent observers possible — notify_one could strand a parker

interrupt_all():                 # control wake: ONE-SHOT re-evaluation signal
  { lock ready_mtx_; ++control_epoch_; }   # never fabricates readiness (I8)
  ready_cv_.notify_all()                   # unblocks ALL parked waiters (I6)

wait_for_change(observed):
  lock ready_mtx_
  ready_cv_.wait(lk, ready_epoch_ != observed.progress_generation
                    || control_epoch_ != observed.control_generation)
  return interrupted if control_epoch_ changed else progress
```

Two epochs (not one) distinguish a progress wake from a control wake WITHOUT a sticky interrupt
flag — a sticky flag would make every FUTURE wait return immediately and busy-spin a runtime with
outstanding work. The control wake is one-shot: future waits snapshot the advanced control
generation and park normally again. `wait_for_change` is PURE OBSERVATION: it never reaps, never
publishes a Completion, never touches request lifecycle or accounting state, so it may run
concurrently with serialized consuming backend operations (E7-C serializes only the consuming
domain; AsyncIoContext::wait_one parks in wait_for_change WITHOUT holding access_mtx_ — the
issue #67 root-cause fix).

`signal_progress` is called by: worker after `record_terminal`; cancel on `terminal_won`; and the
enqueue pin-ack re-arm path when a terminal winner preceded acknowledgement. It is NOT called by
the destructor. `interrupt_all` is called by `close_admission()` (issue #67 corrective — the
frozen design's "close does not signal waiters" constraint starved a parked wait_one and
deadlocked drain) and by ApplicationRuntime::request_stop (via AsyncIoContext::
interrupt_backend_waiters). `wait_one()` returns only the reaped count; 0 means a control-plane
interruption with no completion reaped (or, at the context level, an empty wait). The caller
stops waiting by tracking `outstanding()` or by simply ceasing to call `wait_one()`. The pin-ack
re-arm preserves the level-triggered readiness so no wake is lost (ADR Decision 4, I19;
AGENTS.md §13.2).

---

## 5. Lock order (Gate 1, AGENTS.md §13.1)

The arena slot-lifecycle mutex is a **leaf domain**. The backend adds one backend-internal work
domain. The required order is:

```text
AsyncIoContext::access_mtx_   (context serializes all backend entry points)
  -> ThreadPoolBackend::work_mtx_   (dispatch ring + dequeue/cancel arbitration)
      -> RequestArena::mutex_   (leaf: reserve/prepare/commit/enqueue/cancel/record_terminal/reap/release)
```

And separately:

```text
ThreadPoolBackend::ready_mtx_   (ready epoch + wait/wake; does NOT nest with work_mtx_ or arena)
```

**Forbidden (AGENTS.md §13.1):** any path that acquires `RequestArena::mutex_` then
`work_mtx_`; joining a thread under `work_mtx_` or `ready_mtx_`; calling a syscall, user code,
Scheduler, or ReadySink while holding `work_mtx_`. The worker copies the prepared op and
**releases `work_mtx_` before running the syscall**. `record_terminal` takes the arena leaf lock
alone (no `work_mtx_` held). The enqueue pin-ack path (which may need to re-arm readiness) leaves
the slot-state domain before signalling, per ADR Decision 5/Enqueue.

---

## 6. Descriptor validation (Gate, AC-2; this is a REAL syscall backend — DIV-14 does NOT apply)

`ThreadPoolBackend` issues real syscalls, so it MUST validate the representable causes BEFORE
commit (ADR Decision 6), and MUST NOT use `fcntl(F_GETFD)` as a preflight (TOCTOU — AGENTS.md
§9.1). Validation runs at step 1 of submit (§4.1), before `reserve`:

| Condition | Result |
|---|---|
| `fd < 0` (read/write/sync_data/sync_all) | synchronous `invalid_argument`, Completion idle |
| `len > 0 && buffer == nullptr` (read dst / write src) | `invalid_argument` |
| `offset > off_t max` (`checked_posix_offset` overflow) | `invalid_argument` |
| `len > SSIZE_MAX` / not safely representable as a syscall/terminal byte count | `invalid_argument` |
| `len == 0 && buffer == nullptr` | ALLOWED (design choice; a 0-byte read/write is a 0-result success) — covered by test |

A non-negative but **closed** fd is NOT rejected here: submit succeeds, the syscall later
returns `EBADF`, and that real error becomes the accepted terminal (AGENTS.md §9.1).

Error vocabulary (ADR Decision 6): `invalid_argument` = malformed descriptor; `invalid_state` =
Completion lifecycle misuse / admission closed / provenance; `would_block` = arena full;
`backend_error` (mapped OS errno) = post-commit syscall/backend failure.

---

## 7. Shutdown and destruction (ADR Decision 15; AGENTS.md §14)

### 7.1 `close_admission()` (real production semantics; issue #67 corrective)

Maps the legacy `destroying_` gate + `shutting_down_for_test()` onto `arena_.close_admission()`:

```text
new submit -> reserve() returns invalid_state  (Completion idle, no borrow)
existing accepted requests continue toward their ordinary terminal
cancel remains legal; poll/wait_one/reap remains legal
then interrupt_all(): advance the control epoch and notify_all every parked waiter
```

The wake is a ONE-SHOT re-evaluation signal (issue #67): the frozen design's "close does not
signal waiters" constraint let shutdown strand a participant parked in wait_one forever while
admission closed but outstanding work remained — the final backend_ready request could not be
reaped by the other participant and drain_complete_ was never satisfied. The control wake does
NOT fabricate readiness, does NOT change request state, and does NOT make future waits return
immediately (an admission-closed runtime with outstanding work must not busy-spin).

The unguarded `shutting_down_for_test()` (threadpool_backend.hpp:103) is REMOVED (replaced by
real `close_admission()` driven through the arena). The internal-testing-only seam
`unjoined_workers_for_test()` is re-evaluated against the new persistent-worker model and either
removed or re-scoped under `SLUICE_ASYNC_INTERNAL_TESTING`.

### 7.2 Quiescent destruction

Legal precondition (enforced by `~RequestArena` fail-fast + this backend):

```text
accepted_outstanding == 0
slot_in_use == 0
dispatch ring empty
active_workers == 0
no backend-ready unreaped
no completion-ready bound slot
```

Non-quiescent destruction fail-fasts in Debug AND Release. Before setting `stopping_`, the
destructor takes `work_mtx_` and uses `arena_.quiescence_snapshot()` to verify that the dispatch
ring is empty, no worker is active, and the arena reports `slot_in_use == 0`,
`accepted_outstanding == 0`, and `backend_ready == 0`. If any condition holds, it calls
`threadpool_non_quiescent_destruction_fail_fast()` (std::terminate) before any join. The
destructor does NOT implicitly cancel, drain, wait for a running syscall, publish, or discard the
queue. After the quiescence check it only does persistent-worker teardown:

```text
set stopping_ = true
notify_all idle workers
join the fixed worker_count workers
```

This join is worker-pool teardown, not an I/O drain (AGENTS.md §14). Partial construction failure
(step that fails to start the Nth worker) sets `stopping_`, notifies, joins the already-started
workers, and rethrows — it never lets a joinable thread vector `std::terminate`.

---

## 8. Operation payload — Scheme B (per-slot fixed scratch)

The implementation uses **Scheme B**: a fixed `std::vector<PreparedBlockingOp> prepared_ops_`
sized to `request_capacity` at construction, indexed by `SlotIndex` (a 1:1 mapping with arena
slots). Justification against the Scheme-B proof obligations (AGENTS.md §11):

- **generation validation:** the worker only reads `prepared_ops_[h.slot]` after
  `arena_.mark_running(h)` succeeded, which proves the slot is `running` at generation `h.generation`.
- **not a second identity map:** `prepared_ops_` is a payload store, not an identity store; the
  SlotHandle/RequestKey from the arena is the sole identity. The vector carries no `Completion*`,
  no state, no linkage.
- **rollback / stale safety:** a rolled-back reservation never reaches the worker (it was never
  enqueued); the next accepted request overwrites `prepared_ops_[slot]` before that generation is
  visible; a stale worker dispatch fails fast at `mark_running` before reading the payload.
- **queue/payload generation mismatch fail-fast:** impossible by construction — the worker only
  reaches the payload through a current-generation `mark_running`.

```cpp
struct PreparedBlockingOp {
    detail::OperationKind kind;
    int fd;
    const std::byte* buffer;   // dst for read, src for write, nullptr for sync
    std::size_t length;
    std::uint64_t offset;
};
std::vector<PreparedBlockingOp> prepared_ops_;   // size == request_capacity, at construction
```

No `std::function`, no lambda, no `Completion*`, no `RequestSlot*`, no user callback.

---

## 9. Bounded dispatch queue

A fixed ring, constructed at `request_capacity`, never grows, never allocates after construction,
stores only `SlotHandle` (never `Completion*` / `RequestSlot*`):

```cpp
class BoundedDispatchQueue {
    std::vector<SlotHandle> storage_;   // size == request_capacity
    std::size_t head_ = 0, size_ = 0, high_water_ = 0;
};
// push_back (noexcept), pop_front (noexcept), remove_exact (noexcept, O(capacity) compaction),
// contains_exact, size, capacity, high_water
```

Because `dispatch capacity == request capacity`, a committed enqueue cannot encounter ring full;
a post-commit push failure is `threadpool_dispatch_queue_invariant_fail_fast()` in Debug AND
Release (AGENTS.md §12). `remove_exact` does a bounded compaction so cancel can neutralize an
enqueued entry (ADR §10.3).

---

## 10. Formal model impact (Gate, AGENTS.md §17)

Check the TLA manifest under spec/tla. The Phase B arena model covers the slot state machine and
Scheme-B arbitration but NOT the worker dequeue/cancel/ring protocol that is the load-bearing
Phase-E race. The plan is to add a focused Phase-E blocking-dispatch model whose states are the
free / pending / enqueued / running / backend_ready / completion_ready lifecycle, proving at
minimum: NoExecuteAfterEnqueuedCancel, NoPopBeforeRunningGap, ExactlyOneTerminal,
NoReleaseWhileRunning, BoundedQueue, QuiescentShutdownOnly, PinBeforeReap, plus one
**negative/broken** model proving the verifier catches the pop-then-stale-mark_running gap. If the
scope does not permit a new model, the compliance gate records a justified formal-coverage gap
(reason, risk, revisit trigger). It NEVER claims "formally verified C++ implementation."

---

## 11. Test plan (vertical slices)

The implementation follows the TDD red→green loop, **one slice at a time**. Each slice is one
failing test that captures one contract violation of the legacy backend, then the smallest
production change to pass it. Slice order (each builds on the previous):

1. **config + construction**: default + explicit ctor; `worker_count`/`request_capacity` `> 0`;
   no thread created after construction (resource bound).
2. **capacity / `would_block`**: full arena rejects with `would_block`, Completion idle, no side
   effect.
3. **descriptor validation**: the §6 table, plus closed-fd → accepted → `EBADF` terminal.
4. **5-stage admission + reap**: write/read round-trip through the arena; worker records
   backend-ready; reap publishes; exactly-one terminal.
5. **persistent workers + bounded ring**: the N=high-count small-I/O regression (final report
   runs N=100003, buffer=1).
6. **pending cancel before enqueue** (Scheme B): cancel wins, enqueue no-op, syscall never runs.
7. **enqueued cancel wins**: remove_exact + cancel; worker does not execute.
8. **running cancel preserves real result**: intent only; ordinary success/error verbatim.
9. **dequeue/cancel/reuse stress** (TSan): no stale mark_running, no double terminal.
10. **wake epoch / no lost wake**: the §4.5 timeline matrix; `wait_one` never hangs, never
    busy-loops, never succeeds with 0 reaped.
11. **shutdown**: close_admission, quiescent destroy, non-quiescent destroy fail-fast, partial
    construction cleanup.
12. **allocation-free hot path**: post-commit enqueue / record_terminal / reap publish allocate
    nothing (counting + always-throw `operator new`; TSan limitation recorded honestly).

Every resource test is paired with a semantic test (AC-11, AGENTS.md §18): thread count alone is
never the sole proof of correctness. Deterministic phase seams (barriers, the internal-testing
pause gates), never `sleep_for` as ordering proof (AGENTS.md §13.3).

---

## 12. Forbidden completions (AGENTS.md §23)

Phase E MUST NOT be completed by: increasing OS thread limits; reducing stress scale as the sole
fix; `sleep`/yield retry; detached workers; per-op thread; `std::async`; unbounded queue;
`Completion*`/`RequestSlot*` queue; a parallel identity `unordered_map`; dynamic queue expansion
after commit; a temporary fallback thread on queue full; worker direct Completion publish;
running-cancel rewriting success to canceled; a stale handle treated as normal backoff; implicit
destructor drain; skipping TSan; skipping Release fail-fast; or hiding a baseline failure. It
MUST NOT incidentally migrate Uring, Scheduler routing, Runtime wake, Batch, or add a public
`RequestHandle`.

---

## 13. Open risks and deferred decisions

- The 2ms MIXED-WAKE observation interval (DIV-05) is unchanged; Phase E does not bridge
  backend-ready to Scheduler wake (that is Phase G). `wait_one` is the blocking progress path.
- Portable interruption of a running blocking syscall remains deferred (DIV-10).
- The exact `request_capacity`/`worker_count` defaults may be tuned by the benchmark evidence; the
  defaults above are the frozen starting point.
- Whether to add a public `RequestHandle` is a separate API ADR (out of scope).
