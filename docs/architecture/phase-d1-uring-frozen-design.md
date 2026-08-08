# Phase D1 — UringAsyncBackend Private-Ring RequestArena Migration: Frozen Design

**Status:** FROZEN DESIGN — READY FOR HUMAN REVIEW (production code lands only after this is accepted)
**Date:** 2026-08-08
**Author:** jnhu
**Governing plan:** `docs/architecture/phase-d-uring-migration-plan.md` (head `825c47b`)
**Governing contract:** `docs/adr/ADR-explicit-io-request-contract.md` (Accepted), as amended by
Decision 18 (Uring execution-ownership clarification, committed `fa66ddf`)
**Reference backend:** `ThreadPoolBackend` (portable RequestArena reference production backend)
**Architecture constitution:** `docs/architecture/architecture-constitution.md`

This is the phase-specific compliance gate (AGENTS.md §8) for the Uring private-ring migration.
It freezes every Gate 0–4 field. Evidence is `PENDING` until the exact command-backed result
exists on the final implementation head.

---

## 1. Scope and baseline

**Baseline SHA:** `fa66ddf` (D0.5 amendment head), stacked on PR #76 design head `825c47b`.
**Branch:** `feat/phase-d1-uring-private-ring-requestarena`
**Baseline test gate:** Clang Debug, 151/151 tests passing (recorded before production change).
**liburing:** present on host (`liburing.so.2` in `ldconfig`); real-path executability determined in §11.

**In scope (D1):**
- migrate Uring identity/lifetime/admission/dispatch/CQE/cancel core onto RequestArena;
- establish one-private-ring-per-backend execution ownership;
- eliminate legacy unbounded identity containers (`ops`, `comp_to_op`, `cancel_to_op`,
  `pending_sqes`, integer-id `user_data`, single-step `try_claim`, CQE-direct `publish`);
- make `io_uring_submit()` transport-only;
- prove the new lifetime with failing-first detector tests;
- fix P-D0-INF-01 (real-test link break) enough to run real-path evidence;
- integrate `BackendWaitSource` for the blocking wait.

**Out of scope (documented, not implemented):**
- M:N runtime, shard scheduler, NUMA placement, `IORING_SETUP_ATTACH_WQ` topology;
- SQPOLL, SINGLE_ISSUER/DEFER_TASKRUN, registered buffers/files;
- Batch/Scheduler/Runtime redesign (Phase F/G).

---

## 2. Ring topology for D1 (frozen)

```text
one UringAsyncBackend instance
        ↓
one private io_uring instance
        ↓
one issuer/driver domain
```

This is an **ownership/topology** boundary, not permission to enable kernel single-issuer
optimizations. D1 ring setup is conservative:

- `io_uring_queue_init(queue_depth, &ring, /*flags=*/0)` — flags=0, NO SQPOLL;
- NO `IORING_SETUP_SINGLE_ISSUER`, `DEFER_TASKRUN`, `SQ_AFF`, `ATTACH_WQ` in D1.

**Why conservative:** the current `AsyncIoContext::access_mtx_` serializes *consuming* backend
operations in userspace; it is NOT equivalent to the kernel-level `SINGLE_ISSUER` restriction,
which constrains which Linux tasks may submit to a given ring instance. The Uring D1 driver may
run submit/poll/cancel from different call sites; asserting SINGLE_ISSUER requires a proven
single-task contract that D1 does not establish. (liburing: `DEFER_TASKRUN` requires
`SINGLE_ISSUER`; combining them prematurely would regress, not optimize.)

**Future topology (documented only):**

```text
Executor/shard N ── UringBackend N ── private ring N
optional later: ATTACH_WQ (shared kernel io-wq), SQPOLL, SQ_AFF, NUMA/SMT pinning
```

D1 must leave the seam (one ring owned by one backend) without implementing the future runtime.

---

## 3. State machine (frozen)

The RequestArena canonical lifecycle (ADR Decision 4), with the Decision 18 Uring refinement:

```text
free
  ↓ reserve
reserved
  ↓ prepare
prepared
  ↓ commit / accept LP
pending
  ↓ enqueue arbitration
enqueued                     local dispatch queue owns request
  ↓ dispatch ownership transfer (one critical section)
running / ring-owned          private Uring ring owns the execution reference
  │  ┌─ io_uring_submit() / partial submit / retry / SQPOLL progress: NO state change
  │  ├─ best-effort IORING_OP_ASYNC_CANCEL: control CQE only, NO state change
  │  └─ ... until the original operation CQE ...
  ↓ original operation CQE → arena.record_terminal
backend_ready
  ↓ arena.reap (sole Completion-ready publication)
completion_ready
  ↓ reset / ready destruction
free (generation++)
```

**The load-bearing sentence:** there is NO RequestArena transition at `io_uring_submit()` return.

**Rejected alternatives (do not resurrect):**
- prepared-but-enqueued as a persistent production state;
- request-carrying neutral NOP rewrite + neutral cookie lifecycle;
- `SqSubmissionLedger` as request-lifecycle authority;
- `Completion*` / legacy id maps as kernel identity;
- shared global ring behind a mutex as the scaling model.

---

## 4. Dispatch ownership transaction (frozen) — Gate: dispatch LP

The `enqueued → running / ring-owned` transfer is **one critical section** mirroring the
ThreadPool dequeue+`mark_running` coordinated transfer.

```text
dispatch_one(h):                          [under dispatch domain lock]
    require arena state == enqueued

    sqe = io_uring_get_sqe(private_ring)
    if sqe == NULL:
        flush transport progress           # io_uring_submit(); transport only
        return RETRY_LATER                 # request stays enqueued; local cancel may still win

    op_cookie = cookie_domain.acquire()    # allocation-free, no-wrap, fail-fast
    fill SQE from RequestSlot descriptor/borrow
    SQE.user_data = op_cookie
    router_[op_cookie] = full SlotHandle{h.slot, h.generation}   # pre-reserved capacity

    bool owns = arena.mark_running(h)      # enqueued -> running
    if !owns:
        # terminal winner (e.g. enqueued cancel) won the race before dispatch.
        # Disarm: free the SQE's cookie slot, drop the prepared SQE without submit.
        router_.release(op_cookie)
        cookie_domain.release(op_cookie)
        return DISARMED                     # request already backend_ready; reap will publish
    remove_exact(h) from local dispatch ring
    [end dispatch domain lock]
```

### 4.1 Invariant: no recoverable failure after `io_uring_get_sqe()` succeeds

Once `io_uring_get_sqe()` returns a non-NULL SQE, the remainder of the ownership-transfer
transaction MUST contain no recoverable failure, because there is no ordinary rollback for a
half-created SQE. Therefore all of the following are **pre-reserved / pre-validated / noexcept**:

| Step | Guarantee |
|---|---|
| cookie acquisition | allocation-free, no-wrap; exhaustion fail-fasts before reuse |
| `router_` slot | pre-reserved at construction (capacity == `request_capacity`); installation noexcept |
| descriptor/native args | validated in `prepare()` (Stage 1.5), before dispatch |
| `mark_running` | noexcept; after revalidation under the arena lock, failure is an invariant violation, not a recoverable error |
| `remove_exact` | noexcept (dispatch ring is bounded, capacity-bounded); failure fail-fasts |

The single legitimate non-`owns` return is the **cancel-won-before-dispatch** race: a pending/enqueued
cancel terminalized the request between enqueue and dispatch. In that case the SQE is dropped
without submit and the cookie slot is released; the request is already `backend_ready` and reap will
publish. This mirrors the ThreadPool `mark_running → false` back-off.

### 4.2 Dispatch/cancel coordination (no observable both-state)

Cancel and dispatch share ONE coordination domain (the dispatch lock) to prove:

```text
enqueued cancel:    DISARM LOCAL EXECUTION  →  arena.cancel() terminal winner
dispatch:           LOCAL OWNERSHIP         →  RING OWNERSHIP
```

There is no observable state where both (a) cancel may terminalize locally and (b) a valid operation
SQE may execute. This is the Uring analogue of the ThreadPool `work_mtx_`-held
`dispatch_.remove_exact(h) + arena_.cancel(h)` pair.

---

## 5. `io_uring_submit()` is transport only (frozen) — Gate: submit semantics

After a request becomes `running / ring-owned`:

```text
io_uring_submit()
io_uring_enter()
partial submit
zero progress
EAGAIN / EBUSY
future SQPOLL push
```

MUST NOT mutate RequestArena state. No code equivalent to
`accepted-prefix -> mark_running()` may remain.

### 5.1 Transport ledger (optional, non-authoritative)

A construction-time-bounded transport ledger MAY track physical SQ order IF required for
submit-failure diagnostics/recovery (§6). Its scope is strictly:

```text
physical SQ transport evidence
```

It MUST NOT become request identity, lifecycle authority, terminal authority, or Completion
publication authority. Deleting/corrupting it may break transport diagnostics but may NOT fabricate
an `enqueued → running` lifecycle transition (that transition already happened at ring ownership
transfer). The legacy unbounded `pending_sqes` container is eliminated regardless.

### 5.2 liburing semantics (context7-confirmed)

- `io_uring_submit()` returns the number of SQEs submitted on success, or negative `-errno` on
  failure. It advances the userspace SQ tail before the kernel enter.
- `io_uring_get_sqe()` advances the application-side `sqe_tail` only; the kernel sees nothing until
  enter. Returns NULL if the SQ ring is full.
- A negative `io_uring_submit()` result does NOT prove zero kernel consumption — see §6.

---

## 6. Permanent submit failure — HARD GATE (frozen policy) — Gate: permanent failure proof

**Production implementation of the permanent-failure retirement path is FORBIDDEN until this policy
is implemented exactly as frozen here.** This is the one remaining hard problem (D0 plan §8.2).

### 6.1 The kernel-consumption proof problem (liburing-grounded)

`io_uring_submit()` flushes userspace SQ state to the kernel by advancing the shared-memory SQ tail
**before** the `io_uring_enter` system call (context7: "advances the SQ tail pointer to notify the
kernel of pending operations"). Consequences:

- After the tail advance, the kernel may consume SQEs asynchronously;
- A negative `io_uring_submit()` return (e.g. `-EAGAIN`, `-EBUSY`, `-EINTR`, `-ENOMEM`, `-EFAULT`,
  `-EOVERFLOW`, `-EINVAL`) after the tail advance therefore CANNOT be interpreted as
  "all ring-owned SQEs are definitely userspace-only and may be locally retired."

The proof must distinguish three classes of work:

```text
CLASS A — definitely NOT kernel-consumed
CLASS B — possibly kernel-consumed
CLASS C — already known submitted/consumed (a prior positive submit returned this count)
```

### 6.2 Frozen retirement policy

| Class | Disposition |
|---|---|
| **A — definitely not consumed** | May be locally retired with `backend_error` ONLY after a structural proof that no future original CQE for that work can exist |
| **B — possibly kernel-owned** | MUST remain bound for its CQE / recovery; never locally retired by transport error alone |
| **C — already known consumed** | MUST remain bound for its CQE; the original operation CQE is the terminal candidate |

Rules:
1. A transport error alone CANNOT release a ring-owned RequestSlot.
2. Possibly-kernel-owned work MUST remain bound for CQE/recovery.
3. Definitely-unconsumed work may receive local `backend_error` only after a structural proof that
   no future original CQE can exist for it.
4. Once the ring is permanently poisoned, new public submit MUST reject synchronously with the
   stored backend failure (`backend_error`); Completion remains idle; no borrow; zero residue.
5. No double execution, double terminal, or lost accepted request is permitted.

### 6.3 Class-A proof mechanism (D1 frozen choice)

For the initial non-SQPOLL mode, the Class-A proof is bounded by what liburing exposes about
application-side vs. kernel-side state:

- The **application-side** `sqe_head` / `sqe_tail` (in `struct io_uring_sq`) count SQEs the
  application has *prepared* but liburing has not yet flushed.
- The **shared** `khead` / `ktail` (kernel-controlled, in shared memory) count SQEs the kernel has
  *seen*. A SQE whose index is in `[khead, ktail)` has been visible to the kernel and is therefore
  at least Class-B.

**D1 frozen rule:** a request is Class-A (definitely not consumed) iff its prepared SQE index has
NOT yet been advanced into the shared `[khead, ktail)` window at the moment of the permanent
failure — i.e. it is still purely application-side (`sqe_head <= idx < sqe_tail` AND not yet
flushed). This is the only structurally provable Class-A set under non-SQPOLL liburing.

Because maintaining an exact per-SQE "flushed-but-not-entered" set requires tracking the
application→shared tail advance precisely, and because liburing batches the flush, D1 takes the
**conservative** stance: if the proof for a given request is not structurally available from
bounded metadata, the request is treated as **Class-B** (possibly consumed) and remains bound for
its CQE. We retire locally ONLY requests for which the proof is affirmative.

### 6.4 SQPOLL note

D1 keeps SQPOLL disabled. Under SQPOLL the kernel consumes SQEs from a kernel thread, so the
`[khead, ktail)` proof is still the basis, but the timing window differs and additional bounded
metadata is needed. Enabling SQPOLL later is a transport/recovery enhancement — it does not
redesign RequestArena, because the lifecycle no longer depends on the submit prefix.

### 6.5 If the proof cannot be made cleanly

If, during implementation, the Class-A proof cannot be made with the current liburing API and
bounded metadata, D1 implementation STOPS at this frozen-design gate and reports the exact
blocker. The old prefix-driven RequestArena model is NOT resurrected to make this easier; instead
all ring-owned work remains Class-B and the ring-poison path rejects new admissions while bound
work completes via CQE.

---

## 7. CQE identity (frozen) — Gate: CQE identity

### 7.1 Kernel boundary

```text
SQE.user_data = opaque op_cookie          (operation) | reserved control value (control)

CQE.user_data
   ↓ decode: control vs op cookie
   op cookie:
       resolve construction-time bounded CqeRouter  →  full SlotHandle{slot, full uint64 generation}
       RequestArena full validation (context/slot/generation)
       require ring-owned execution state
       convert cqe->res to TerminalResult
       arena.record_terminal(h, terminal)
       retire transport routing/execution reference
   control cancel CQE:
       update only fixed cancel bookkeeping if needed
       NEVER record a request terminal
```

`Completion*`, raw `SlotIndex`, and `RequestSlot*` are NOT sufficient kernel identity (ABA risk
after slot reuse). The cookie resolves to a **full** `SlotHandle` (slot + full 64-bit generation)
which the arena re-validates.

### 7.2 Concrete D1 representation

For D1, the **simplest provably bounded** representation is chosen first: a fixed-capacity
`op_cookie → SlotHandle` router sized to `request_capacity`, with cookie values allocated from a
no-wrap counter and a release/reuse free-list so cookies are unique within backend lifetime.
Exhaustion fail-fasts before reuse (mirrors the arena's `UINT64_MAX` generation fail-fast).

Benchmark gate: an O(request_capacity) scan is acceptable as the correctness/reference first cut;
it must be benchmarked against a fixed O(1) open-addressed router at representative capacities
(`request_capacity` ∈ {64, 256, 1024}) before adding a complicated hash structure. **Absolutely no
unbounded `unordered_map`.**

### 7.3 Reserved control values

Following liburing's own `LIBURING_UDATA_TIMEOUT = (__u64)-1` precedent, D1 reserves a small
explicit control range outside the operation-cookie allocation domain:
- `CONTROL_CANCEL` — the cancel-SQE `user_data` (cancel CQE is informational only).

Reserving more control values (wake/timeout) is permitted later; the operation-cookie allocator
must exclude the reserved range.

### 7.4 Reap remains sole Completion publication

The CQE handler calls ONLY `arena.record_terminal(h, terminal)` and `signal_ready_progress()`. It
MUST NOT call `AsyncBackend::publish()` directly. `arena.reap(sink)` — invoked from
`poll()` / `wait_one()` — is the sole Completion-ready publication path. This eliminates the legacy
`reap_op_cqe → publish` anti-pattern.

---

## 8. Cancellation (frozen) — Gate: cancel proof

### 8.1 Pending/enqueued: local cancel may win

```text
lock dispatch domain
    resolve full handle h
    remove_exact(h) from local dispatch queue if present     # DISARM FIRST
    disp = arena.cancel(h)                                   # terminal winner SECOND
unlock
if disp == terminal_won: signal_ready_progress()
```

The ordering is deliberate: **DISARM LOCAL EXECUTION FIRST, TERMINAL WIN SECOND.**

Strong guarantee: a locally canceled `pending` / `enqueued` request's operation SQE was NEVER
installed into the ring and CANNOT execute.

Scheme-B pending-cancel (cancel wins before enqueue): preserve the existing enqueue-pin/no-op
behavior — `arena.enqueue(h)` observes `backend_ready` and returns `terminal_noop`, acknowledging
the pin as its final slot access.

### 8.2 Running/ring-owned: intent only

```text
disp = arena.cancel(h)              # returns intent_recorded; stores nothing
if disp == intent_recorded:
    scratch[h.slot].cancel_requested = true
```

The driver may append:

```text
IORING_OP_ASYNC_CANCEL(target = op_cookie)
SQE.user_data = CONTROL_CANCEL
```

The cancel CQE is **control/informational only**. It MUST NOT own RequestKey, publish Completion,
release RequestSlot, overwrite an ordinary result, or independently win the operation terminal.

The **original operation CQE** decides (context7-confirmed: cancel produces two CQEs, order not
guaranteed; cancel CQE `res` ∈ {0, -ENOENT, -EALREADY} is informational):
- ordinary success → success;
- ordinary error → that error;
- `-ECANCELED` → canceled terminal if it wins.

The RequestSlot and op cookie remain live until the original CQE (or a §6 proven transport-failure
retirement). Repeated cancel calls MUST NOT enqueue unbounded cancel SQEs; one fixed per-slot
`cancel_requested` / `cancel_queued` bookkeeping bitset is sufficient.

### 8.3 What disappears

No neutral NOP rewrite, no neutral cookie, no prepared-but-enqueued cancel terminal win, no slot
release before a request-derived NOP CQE. `enqueued` has no SQE; `running` permits no local
terminal cancel.

---

## 9. Capacity and allocation model (frozen) — Gate: resources

| Resource | Bound | Allocation time | Full behavior | Reclamation | High-water metric | Shutdown owner |
|---|---|---|---|---|---|---|
| RequestArena | `request_capacity` | construction | synchronous `would_block`; Completion idle; no borrow | reap → reset/release | `high_water_mark()` | context close |
| local dispatch ring | `request_capacity` | construction | reserved before commit; no post-accept allocation | dispatch / cancel remove | dispatch ring size | backend destruction |
| CqeRouter | `request_capacity` | construction | cookie exhaustion fail-fasts | record_terminal / disarm | live cookie count | backend destruction |
| per-slot Uring scratch | `request_capacity` | construction | n/a (pre-reserved per slot) | reap leaves scratch for cancel bookkeeping | n/a | backend destruction |
| transport ledger (if kept) | `request_capacity` | construction | n/a (diagnostics only) | n/a | n/a | backend destruction |
| io_uring SQ/CQ | `queue_depth` | `io_uring_queue_init` | transport pressure; accepted request stays alive | kernel CQE | SQ pressure count | `io_uring_queue_exit` |
| worker/driver thread | 1 (D1 single-driver) | construction | n/a | idle join on quiescent destroy | n/a | backend destruction |

**Capacity equation:**

```text
0 <= accepted_outstanding <= request_capacity
0 <= slot_in_use <= request_capacity
0 <= live_op_cookies <= request_capacity
0 <= dispatch_ring_size <= request_capacity
request_capacity > queue_depth  is legal
```

Excess accepted work remains in the local `enqueued` dispatch ring until an SQE is available.

**After acceptance, the normal terminal path requires ZERO unbounded allocation:**

```text
commit → enqueue → dispatch/ring-owned → submit/retry → CQE → record_terminal → reap → reset/release
```

All storage on that path is construction-time fixed/bounded.

---

## 10. Lock order (frozen) — Gate: lock/atomic authority

Three distinct domains, each a leaf in its own scope (mirrors ThreadPool three-mutex discipline;
AGENTS.md §13.1 forbids bidirectional lock order):

```text
admission_mtx_  (submit Stage 1–3c, close_admission)
   order: admission_mtx_  ->  arena leaf only
   released BEFORE enqueue

dispatch_mtx_   (local dispatch ring + dispatch/cancel arbitration + cookie/router install)
   order: dispatch_mtx_  ->  arena leaf only
   the io_uring_get_sqe / SQE fill / mark_running critical section
   io_uring_submit() may be called OUTSIDE dispatch_mtx_ (transport only, no arena mutation)

arena leaf mutex  (RequestArena mutex_)
   LEAF domain; holder MUST NOT call Scheduler/ReadySink/user code,
   join a thread, execute a syscall, or acquire dispatch_mtx_

ready_wait_  (ReadyWaitSource — BackendWaitSource impl)
   LEAF domain; never nested with dispatch_mtx_ or the arena lock
```

**Critical rules (AGENTS.md §13.1):**
- Code holding the arena leaf mutex MUST NOT acquire `dispatch_mtx_` or call backend progress.
- `io_uring_submit()` / `io_uring_enter()` (syscalls) MUST NOT be called under the arena mutex.
  D1 calls submit under `dispatch_mtx_` at most, and only as transport progress.
- Joining threads under any queue/progress mutex is forbidden.
- `record_terminal` takes the arena leaf lock ALONE (no `dispatch_mtx_` held) — first caller wins,
  losers no-op (mirrors ThreadPool worker `record_terminal` comment).

**Lock-order table (explicit):**

| Path | Locks acquired (in order) |
|---|---|
| submit (reserve→commit) | admission_mtx_ → arena leaf (transient, per method) → release admission_mtx_ before enqueue |
| enqueue + dispatch | dispatch_mtx_ → arena leaf (transient) → release dispatch_mtx_ before submit syscall |
| cancel pending/enqueued | dispatch_mtx_ → arena leaf (inside `arena.cancel`) |
| cancel running | arena leaf (inside `arena.cancel`) → scratch write (lock-free or under dispatch_mtx_ for the cancel-queue bit) |
| CQE handler → record_terminal | arena leaf ALONE |
| reap (poll/wait_one) | arena leaf → (released) → ReadySink invoked WITHOUT the lock |
| wait_one park | ReadyWaitSource leaf (NO access_mtx_, NO dispatch_mtx_) |

---

## 11. Wait / close / drain / destruction (frozen) — Gate: shutdown

### 11.1 Wait (BackendWaitSource integration)

The Uring backend MUST reuse the existing `detail::ReadyWaitSource` abstraction (as ThreadPool does)
— it MUST NOT invent a new blocking wait under `AsyncIoContext::access_mtx_`.

- override `wait_source()` to return `&ready_wait_`;
- override `wait_one_is_nonblocking()` per the split-wait contract;
- `signal_ready_progress()` (= `ready_wait_.signal_progress()`) is called after every
  `record_terminal` and every Scheme-B `terminal_noop`;
- `wait_one()` implements the snapshot → reap → park-in-`wait_for_change` loop (no access_mtx_
  across the park).

### 11.2 Close / destruction (quiescent, ADR Decision 15)

- destructors do NOT implicitly close admission, cancel accepted work, drain, wait for async I/O,
  or publish terminal results;
- destruction with accepted or bound requests is a contract violation (fail-fast where required);
- the explicit lifecycle: `close admission → continue progress → reap accepted → callers reset/destroy
  ready Completions → accepted_outstanding == 0 → slot_in_use == 0 → backend progress ownership == 0
  → destroy`;
- a persistent worker/driver join during quiescent destruction is teardown, not implicit I/O drain.

D4 (full close/drain/destruction redesign) is separately scoped; D1 preserves the explicit
quiescent contract and adds only the minimal `BackendWaitSource` adaptation required for correctness.

---

## 12. Admission — mirrors ThreadPool five-stage transaction

```text
reserve
  → prepare (Stage 1.5 descriptor validation AFTER reserve)
  → install_publication_binding
  → Completion binding election (begin_binding)
  → commit / accept LP (commit_binding publishes outstanding)
  → enqueue
  → dispatch (§4)
```

All under `admission_mtx_` through Stage 3c, released before enqueue — exactly the proven ThreadPool
shape. Capacity full → synchronous `would_block`, Completion idle, no borrow, no SQE, zero residue.

### 12.1 Config (public API)

```cpp
struct UringConfig {
    std::size_t request_capacity = 64;   // arena + dispatch ring + router capacity
    unsigned    queue_depth      = 64;   // io_uring SQ/CQ depth
};
```

Legacy `UringAsyncBackend(unsigned queue_depth = 64)` is preserved for source compatibility (maps to
`UringConfig{.request_capacity = queue_depth, .queue_depth = queue_depth}` or a documented default).
`request_capacity` and `queue_depth` are independent; `request_capacity > queue_depth` is legal.

---

## 13. Conformance manifest (frozen target)

D1 flips the five Uring `not_implemented` records to `implemented` ONLY when the exact required
real-path command-backed evidence exists on the final implementation head:

- `uring_capacity_not_implemented` → `request_capacity` admission/accounting conformance;
- `uring_c2b_identity_not_implemented` → RequestArena identity/generation/cancel/reap;
- `uring_c2c_borrow_waiter_not_implemented` → borrow/waiter/lease lifecycle;
- `uring_c2d_failure_injection_not_implemented` → RequestArena-based failure injection;
- `uring_c2e_close_drain_not_implemented` → close/drain/destruction.

Stub green is NEVER a substitute for KernelIo evidence. Records remain `not_implemented` until real
liburing evidence exists or are explicitly documented as `REAL LIBURING EVIDENCE UNAVAILABLE` per §14.

---

## 14. Real / stub evidence (frozen)

- Fix P-D0-INF-01 first (real-test link break: unconditional `<liburing.h>` include without
  `SLUICE_HAS_LIBURING` guard).
- Determine `available() == true` on the host explicitly.
- If real io_uring cannot execute: do NOT claim real-path PASS; manifest records stay
  `not_implemented` where real evidence is required; report `REAL LIBURING EVIDENCE UNAVAILABLE`;
  still run compile/link/stub/doc evidence.
- If real io_uring is available: run the relevant real-path tests, record exact commands/results.
- Never flip a `uring_*_not_implemented` record without the exact required real-path evidence on
  the final head.

---

## 15. Failing-first detector tests (frozen coverage) — Gate: tests

Before each production slice, add a detector that FAILS on the current/incorrect behavior. D1 must
add deterministic coverage for:

1. public commit occurs BEFORE any SQE acquisition;
2. capacity full ⇒ `would_block` / Completion idle / zero SQ residue;
3. `enqueued → ring-owned` happens independently of `io_uring_submit()` return count;
4. partial submit does NOT change RequestState;
5. enqueued cancel prevents any operation SQE from becoming ring-owned;
6. running cancel does not release/reuse RequestSlot;
7. cancel CQE cannot publish/overwrite terminal;
8. stale CQE cookie cannot mutate reused slot generation;
9. original CQE → `backend_ready` only; Completion not ready before reap;
10. no legacy `Completion*` / id maps are needed for cancel/CQE identity;
11. `request_capacity > queue_depth` works;
12. accepted path survives always-throw allocator where contract requires no allocation;
13. non-quiescent destruction still fails fast;
14. one backend owns exactly one ring; no shared-ring multi-producer implementation appears.

All test seams guarded by existing internal-testing macros. No production test-only state.

---

## 16. Validation gates (frozen) — Gate: change-class

For the actual change class (backend migration, ownership, cancellation, synchronization):

```text
Clang Debug
Clang Release          (public headers / API contract change)
ASan + UBSan           (ownership, allocation, buffer lifetime)
TSan                   (submit vs dispatch, enqueued cancel vs dispatch,
                        running cancel vs terminal, backend-ready vs reap,
                        wake vs wait, reset/reuse after reap, shutdown wake)
negative-compile gates (completion-authority, request-arena)
backend conformance manifest self-tests
aggregate conformance gate
documentation checks   (check-doc-links, verify-architecture-docs)
git diff --check
```

Run focused Uring real-path tests separately with liburing enabled. TSan evidence must include the
modified race classes, not merely unrelated tests.

---

## 17. Authority summary (which ADR / AC-N rules apply)

| Concern | Authority |
|---|---|
| RequestKey `(context, slot, generation)` | ADR Decision 1, I1 |
| unified state machine + Decision 18 refinement | ADR Decision 4, Decision 18 |
| five-stage admission transaction | ADR Decision 5 |
| error vocabulary (`would_block` / `invalid_argument` / `backend_error`) | ADR Decision 6 |
| Completion binding two-stage | ADR Decision 5, I2 |
| borrow lifetime | ADR Decision 8 |
| identity-bearing reap, reap-only publication | ADR Decision 9, I5 |
| terminal winner exactly-once | ADR Decision 12 |
| bounded capacity | ADR Decision 13 |
| accepted terminal path no unbounded allocation | ADR Decision 14 |
| close/drain/destruction quiescent | ADR Decision 15 |
| AsyncBackend author contract | ADR Decision 16 |
| execution ownership (Uring private ring) | ADR Decision 18 (this phase) |

AC-N rules: derived from `docs/architecture/architecture-constitution.md` (read before
implementation; all applicable AC-N rules identified per AGENTS.md §8).

---

## 18. Evidence ledger (PENDING until executed)

| Field | Status |
|---|---|
| Gate 0 — scope & authority | DONE (this document) |
| Gate 1 — state machine | DONE (§3) |
| Gate 2 — lock/atomic authority | DONE (§10) |
| Gate 3 — resource capacity | DONE (§9) |
| Gate 4 — wake/progress + shutdown | DONE (§11) |
| Clang Debug | PENDING |
| Clang Release | PENDING |
| ASan + UBSan | PENDING |
| TSan | PENDING |
| negative-compile | PENDING |
| conformance manifest | PENDING |
| real liburing evidence | PENDING |
| docs (check-doc-links, verify-architecture-docs) | PENDING |
| D0.5 ADR amendment | DONE (`fa66ddf`) |

---

## 19. Success criterion

Not "new io_uring code compiles". It is:

> `UringAsyncBackend` has moved onto the private-ring / ring-owned RequestArena lifetime model;
> no request lifecycle decision depends on `io_uring_submit()` accepted-prefix accounting;
> cancellation cannot release ring-owned work; Completion publication is reap-only; storage is
> bounded; and every accepted request has a provable terminal path.
