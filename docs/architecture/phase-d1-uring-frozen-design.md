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
- fix P-D0-INF-01 (real-test link break) enough to run real-path evidence.

> **Scope correction (post-review):** `BackendWaitSource` integration (override `wait_source()`,
> split-wait) is **moved to D4**, not D1. D1 retains the single-driver kernel-blocking wait
> (`wait_one()` blocks in `io_uring_submit_and_wait` and reaps synchronously). See §11.1.

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

> **Repair note (post-review):** the original pseudocode described a "drop the prepared SQE without
> submit" rollback when `mark_running` returned false. That rollback is **liburing-impossible**:
> `io_uring_get_sqe()` advances the application-side `sqe_tail`, so the obtained SQE WILL be flushed
> by the next `io_uring_submit()` and the kernel WILL see it (context7-confirmed). Abandoning it
> would let it execute with a stale/wrong identity. The repaired transaction below has **no
> recoverable failure after `get_sqe()`**: any post-`get_sqe` failure fail-fasts.

```text
dispatch_one_locked(h):                   [under dispatch_mtx_, h == dispatch_.front()]
    require arena state == enqueued

    # ---- pre-get_sqe region: recoverable ----
    reserve a router ARRAY slot from the free-list      # exhaustion fail-fast
    sqe = io_uring_get_sqe(private_ring)
    if sqe == NULL:
        flush transport progress                         # io_uring_submit(); transport only
        if still NULL:
            push router slot back onto the free-list
            return RETRY_LATER                           # h stays at queue front; no SQE obtained

    # ---- NO-FAIL REGION: get_sqe committed the SQE to the next flush ----
    op_cookie = allocate_cookie_()                       # no-wrap; fail-fast on exhaustion
    fill SQE from RequestSlot descriptor/borrow
    io_uring_sqe_set_data64(sqe, op_cookie)              # integer token, not pointer-cast
    router_[slot] = {op_cookie, full SlotHandle{h.slot, h.generation}, in_use=true}

    owns = arena.mark_running(h)                         # enqueued -> running
    if !owns:
        FAIL-FAST                                        # invariant: cancel cannot have won
                                                         # under the dispatch_mtx_ discipline
    dispatch_->remove_exact(h)                           # MUST succeed (h is front); else fail-fast
    [end dispatch_mtx_]
```

### 4.1 Invariant: no recoverable failure after `io_uring_get_sqe()` succeeds

Once `io_uring_get_sqe()` returns a non-NULL SQE, the remainder of the ownership-transfer
transaction MUST contain no recoverable failure, because there is no ordinary rollback for an
obtained SQE — `get_sqe()` already advanced liburing's application-side `sqe_tail`, so the SQE will
be flushed by the next `io_uring_submit()`. Therefore all of the following are
**pre-reserved / pre-validated / noexcept**:

| Step | Guarantee |
|---|---|
| router ARRAY slot | reserved BEFORE `get_sqe` (recoverable: pushed back on NULL SQE); exhaustion fail-fasts |
| cookie acquisition | no-wrap counter; reaching the reserved control range fail-fasts before reuse |
| `router_` installation | pre-reserved at construction (capacity == `request_capacity`); installation noexcept |
| descriptor/native args | validated in `prepare()` (Stage 1.5), before dispatch; native length normalized |
| `mark_running` | noexcept; **`false` is an invariant violation, not a recoverable error** (see §4.2) |
| `remove_exact` | noexcept; MUST succeed (h is the queue front under the held lock); failure fail-fasts |

There is **no legitimate `mark_running == false` return** in the repaired transaction. The
"cancel-won-before-dispatch" race that the original design permitted is eliminated by §4.2's
enqueue+dispatch single critical section.

### 4.2 Dispatch/cancel coordination (no observable both-state)

Cancel and dispatch share ONE coordination domain (`dispatch_mtx_`) to prove:

```text
enqueued cancel:    DISARM LOCAL EXECUTION  →  arena.cancel() terminal winner
dispatch:           LOCAL OWNERSHIP         →  RING OWNERSHIP
```

There is no observable state where both (a) cancel may terminalize locally and (b) a valid operation
SQE may execute. This is the Uring analogue of the ThreadPool `work_mtx_`-held
`dispatch_.remove_exact(h) + arena_.cancel(h)` pair.

**`mark_running == false` is an invariant violation, not a back-off.** This holds because:

1. **`enqueue_after_commit` holds `dispatch_mtx_` across `push_back(h) → dispatch_one_locked(h)`**
   (one critical section). `cancel(h)` acquires the SAME lock, so it cannot interpose between
   enqueue publication and `mark_running`. The original design released the lock between enqueue
   and dispatch, opening a window where cancel could terminalize h first; that window is closed.
2. **`poll()`/`wait_one()` use a peek protocol** (`while !empty: h=front(); dispatch_one_locked(h)`)
   rather than `pop_front → dispatch → push_back`. The dispatch queue membership IS local execution
   ownership: `h ∈ queue ⇔ backend owns h locally (cancel may disarm)`; `h leaves queue ⇒ h is
   ring-owned`. The successful transfer's `remove_exact(h)` retires the entry exactly once; a `pop`
   before dispatch would have contradicted that `remove_exact` (fail-fast).

The single critical section + peek protocol together make `mark_running == false` structurally
unreachable under valid operation; reaching it is a lifecycle invariant violation that fail-fasts
in BOTH Debug and Release.

### 4.3 Cancel target resolution before `get_sqe`

`issue_running_cancel(h)` resolves the operation target cookie BEFORE calling `io_uring_get_sqe()`:
scan the router for h's LIVE entry; if none exists, h is not ring-owned and there is nothing to
cancel — return WITHOUT obtaining an SQE. Only after a target cookie is resolved does the function
obtain an SQE, fill `IORING_OP_ASYNC_CANCEL(target=cookie)`, and set
`io_uring_sqe_set_data64(sqe, CONTROL_CANCEL)`. This closes the `get_sqe`-then-discover-nothing-to-
cancel hole (an obtained SQE cannot be abandoned).

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

> **Repair note (post-review): P0-D remains a HARD GATE.** A "poison new admission + treat all
> in-flight work as Class-B and wait for CQEs" policy solves the *safety* problem (do not
> prematurely release possibly-kernel-owned work) but NOT the *progress* problem (every accepted
> request must have a provable terminal path). If the kernel never consumed the poisoned batch, no
> CQE ever arrives and the request strands. The D1 repair therefore does NOT ship a production
> poison implementation; the focused liburing/kernel audit that would license one is recorded in
> `docs/architecture/phase-d1-uring-permanent-submit-failure-audit.md`. Until that audit proves a
> clean Class-A retirement path (or a `to_submit=0` drain + ring-teardown protocol), the
> `fatal_error_` field remains read-only evidence plumbing and D1 is not merge-ready on this axis.

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
SQE.user_data = op_cookie                 (operation, integer token via io_uring_sqe_set_data64)
             | CONTROL_CANCEL             (control; cancel-SQE CQE is informational only)

CQE.user_data  (read via io_uring_cqe_get_data64)
   ↓ decode: control vs op cookie
   op cookie:
       bounded O(request_capacity) scan of the fixed router for the LIVE entry
           whose cookie == cqe cookie              # KEYED BY COOKIE VALUE, not array index
       no LIVE match → DROP (stale cookie; its entry was retired, the array slot may be reused)
       LIVE match → full SlotHandle{slot, full uint64 generation}
       RequestArena full validation (context/slot/generation)
       convert cqe->res to TerminalResult
       arena.record_terminal(h, terminal)
       retire transport routing/execution reference (free the router array slot; the cookie value
           is NEVER reused, so a late duplicate CQE for the same cookie finds no LIVE entry)
   control cancel CQE:
       update only fixed cancel bookkeeping if needed
       NEVER record a request terminal
```

`Completion*`, raw `SlotIndex`, `RequestSlot*`, and **router ARRAY INDEX** are NOT sufficient kernel
identity (ABA risk after reuse). The cookie resolves to a **full** `SlotHandle` (slot + full 64-bit
generation) which the arena re-validates. The router is keyed by the cookie VALUE (never reused), so
a stale CQE whose cookie was retired matches no LIVE entry — the array slot having been recycled for
a different cookie does not reopen the ABA window.

### 7.2 Concrete D1 representation (repaired)

For D1, the **simplest provably bounded** representation is chosen first: a fixed-capacity router
sized to `request_capacity`, keyed by cookie VALUE.

```text
struct RouterEntry {
    std::uint64_t cookie;          // 0 = not live
    SlotHandle    handle;          // full slot + full 64-bit generation
    bool          in_use;
};
```

- The SQE `user_data` carries the **cookie value** via `io_uring_sqe_set_data64` (the integer-token
  API; no pointer round-trip). CQE read via `io_uring_cqe_get_data64`.
- Cookie values are allocated from a **no-wrap 64-bit counter** (`allocate_cookie_()`). Domain is
  `[1, UINT64_MAX-1]` (0 unused; `UINT64_MAX` = `CONTROL_CANCEL`). If the counter would reach
  `CONTROL_CANCEL`, **fail-fast** (never wrap) — mirroring RequestArena generation no-wrap
  discipline. A cookie is therefore **NEVER reused within backend lifetime**.
- The router ARRAY slot is recycled via a free-list (only the array slot, never the cookie value).
  Routing keys on the cookie value, so a stale CQE for a retired cookie cannot resolve through a
  recycled array slot.
- CQE routing is a bounded **O(request_capacity) scan** for the LIVE entry whose cookie matches
  (D1 correctness/reference cut; no `unordered_map`).

Benchmark gate: the O(request_capacity) scan is acceptable as the correctness/reference first cut;
it must be benchmarked against a fixed O(1) open-addressed router at representative capacities
(`request_capacity` ∈ {64, 256, 1024}) before adding a complicated hash structure. **Absolutely no
unbounded `unordered_map`.**

The stale-cookie detector (`uring_stale_cqe_cookie_dropped_not_misdelivered`, capacity=1 forcing
array-slot reuse) proves a retired cookie is dropped and cannot misdeliver to the later occupant.

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

**Repaired (post-review):** D1's production implementation uses TWO distinct domains, not three.
Admission reuses `dispatch_mtx_` (a conservative choice; a separate `admission_mtx_` is not
allocated). The `ready_wait_` domain does not exist in D1 (§11.1: `wait_source()` returns nullptr;
`BackendWaitSource` is D4). Each domain is a leaf in its own scope; AGENTS.md §13.1 forbids
bidirectional lock order.

```text
dispatch_mtx_   (admission submit Stage 1–3c, close_admission flag, local dispatch ring,
                 dispatch/cancel arbitration, cookie/router install)
   order: dispatch_mtx_  ->  arena leaf only
   the io_uring_get_sqe / SQE fill / mark_running / remove_exact critical section
   io_uring_submit() MAY be called under dispatch_mtx_ as transport progress (syscall); it
       MUST NOT be called under the arena mutex.
   enqueue_after_commit holds dispatch_mtx_ across push_back -> dispatch_one_locked (one CS).

arena leaf mutex  (RequestArena mutex_)
   LEAF domain; holder MUST NOT call Scheduler/ReadySink/user code,
   join a thread, execute a syscall, or acquire dispatch_mtx_
```

`access_mtx_` (AsyncIoContext) is NOT a backend lock: it serializes ALL backend operations
(submit/poll/wait_one/cancel) at the context layer, making D1 single-driver. It is held across a
backend call but the backend's wait_one never blocks under it in the kernel-park path.

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
| submit (reserve→commit) | dispatch_mtx_ → arena leaf (transient, per method) → [release dispatch_mtx_ before returning; enqueue re-acquires it] |
| enqueue + dispatch | dispatch_mtx_ (held across push_back → dispatch_one_locked, one CS) → arena leaf (transient inside mark_running) → submit syscall may run under dispatch_mtx_ (transport) |
| cancel pending/enqueued | dispatch_mtx_ → arena leaf (inside `arena.cancel`) |
| cancel running | dispatch_mtx_ (resolve target + scratch cancel_queued bit) → arena leaf (inside `arena.cancel`) |
| CQE handler → record_terminal | arena leaf ALONE |
| reap (poll/wait_one) | arena leaf → (released) → ReadySink invoked WITHOUT the lock |
| wait_one park | kernel block in io_uring_submit_and_wait (NO dispatch_mtx_, NO arena lock) |

---

## 11. Wait / close / drain / destruction (frozen) — Gate: shutdown

### 11.1 Wait — D1 retains the single-driver kernel-blocking model

**Repaired (post-review):** the original §11.1 required D1 to integrate `BackendWaitSource`
(override `wait_source()`). The production implementation deliberately does NOT, and that is the
correct call for D1's topology:

```text
D1:  single-driver; wait_one() blocks in io_uring (io_uring_submit_and_wait) and reaps
                 CQEs synchronously on the calling thread. There is no separate CQE-reaper
                 thread to signal a ReadyWaitSource, so declaring split-wait capability would
                 make AsyncIoContext::wait_one park in wait_for_change forever.
      wait_source() returns nullptr (the default).
      signal_ready_progress() is a no-op seam (kept for a future shard/M:N topology).
```

`BackendWaitSource` integration (override `wait_source()`, split-wait, eventfd/control wake,
close/drain interruption) is moved to **D4** (full close/drain/wait redesign). D1 must NOT add
`BackendWaitSource` merely to satisfy stale design prose. The frozen design is reconciled to the
implementation here so the gate and the code agree.

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
| Gate 2 — lock/atomic authority | DONE (§10, reconciled to the 2-domain production model) |
| Gate 3 — resource capacity | DONE (§9) |
| Gate 4 — wake/progress + shutdown | DONE (§11; D1 wait_source()==nullptr, BackendWaitSource→D4) |
| Clang Debug | PASS — 152/152, `xmake test -v` on the repaired head |
| Clang Release | PENDING (final gate step) |
| ASan + UBSan | PENDING (final gate step) |
| TSan | PENDING (final gate step — modified race classes) |
| negative-compile | PENDING (final gate step) |
| conformance manifest | PENDING (final gate step) |
| real liburing evidence | PASS — `xmake run uring_submit_failure_test`, 8/8 cases (incl. stale-cookie + length boundary + scripted-partial detectors) |
| docs (check-doc-links, verify-architecture-docs) | PASS — `python3 scripts/check-doc-links.py`, `python3 scripts/verify-architecture-docs.py` on the repaired head |
| D0.5 ADR amendment | DONE (`fa66ddf`) |
| **P0-D permanent-submit-failure** | **BLOCKED** — see `docs/architecture/phase-d1-uring-permanent-submit-failure-audit.md`; the §3.4 theorem holds but production poison/recovery (ledger + wait-path guard + teardown proof) is not implemented. D1 is READY FOR HUMAN REVIEW, NOT merge-ready on the provable-terminal-path axis. |

---

## 19. Success criterion

Not "new io_uring code compiles". It is:

> `UringAsyncBackend` has moved onto the private-ring / ring-owned RequestArena lifetime model;
> no request lifecycle decision depends on `io_uring_submit()` accepted-prefix accounting;
> cancellation cannot release ring-owned work; Completion publication is reap-only; storage is
> bounded; and every accepted request has a provable terminal path.

**Status (post-review repair):** the D1 repair achieves the first five clauses (private-ring
ownership, no submit-prefix lifecycle authority, cancel cannot release ring-owned work, reap-only
publication, bounded storage). The sixth clause — *every accepted request has a provable terminal
path* — is **BLOCKED** on the permanent-submit-failure proof (P0-D, §6). Under a permanent
`io_uring_submit()` failure with no kernel consumption, a ring-owned request has no CQE and no
local retirement path, so it strands. D1 is therefore **READY FOR HUMAN REVIEW but NOT merge-ready**
until the P0-D audit (`docs/architecture/phase-d1-uring-permanent-submit-failure-audit.md`) proves
a clean Class-A retirement path or drain+teardown protocol and a production implementation lands.
