> **HISTORICAL / EVIDENCE — NOT CURRENT AUTHORITY.** Archived from `docs/architecture/` by S0-DOCS (#290, 2026-09-04). Point-in-time record; do not cite as authority for new decisions.

# Phase D1 — UringAsyncBackend Private-Ring RequestArena Migration: Frozen Design

**Status:** IMPLEMENTED AND VERIFIED
**Date:** 2026-08-09
**Author:** jnhu
**Governing plan:** `docs/history/closeout/phase-d-uring-migration-plan.md` (head `825c47b`)
**Governing contract:** `docs/adr/ADR-explicit-io-request-contract.md` (Accepted), as amended by
Decision 18 (Uring execution-ownership clarification, committed `fa66ddf`)
**Reference backend:** `ThreadPoolBackend` (portable RequestArena reference production backend)
**Architecture constitution:** `docs/architecture/architecture-constitution.md`

This is the phase-specific compliance gate (AGENTS.md §8) for the Uring private-ring migration.
It freezes every Gate 0–4 field. The command-backed implementation evidence is recorded in §18.

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

1. **`enqueue_after_commit` holds `dispatch_mtx_` across `push_back(h)` and a FIFO front-drain**
   (one critical section). The drain repeatedly peeks `dispatch_.front()` and passes only that
   handle to `dispatch_one_locked`; it never dispatches the newly appended tail out of order.
   `cancel(h)` acquires the SAME lock, so it cannot interpose between enqueue publication and
   `mark_running`. The original design released the lock between enqueue and dispatch, opening a
   window where cancel could terminalize h first; that window is closed.
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
`io_uring_sqe_set_data64(sqe, CONTROL_TAG | cookie)`. The high-bit tag identifies a control CQE;
the low bits retain the exact target operation cookie. This closes the `get_sqe`-then-discover-
nothing-to-cancel hole (an obtained SQE cannot be abandoned) and lets control retirement resolve
the exact bounded router entry.

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

If P0-D adds this ledger, its proof identity MUST include a non-wrapping monotonic logical SQ
sequence (or an equivalent epoch + modular-distance invariant) in addition to the masked physical
ring position. `tail & ring_mask` alone is only a reusable storage slot, not physical-sequence
identity: after wrap, two different submissions occupy the same masked slot and MUST NOT be
classified as the same Class-A entry.

### 5.2 liburing semantics (context7-confirmed)

- `io_uring_submit()` returns the number of SQEs submitted on success, or negative `-errno` on
  failure. It advances the userspace SQ tail before the kernel enter.
- `io_uring_get_sqe()` advances the application-side `sqe_tail` only; the kernel sees nothing until
  enter. Returns NULL if the SQ ring is full.
- A negative `io_uring_submit()` result does NOT prove zero kernel consumption **in the general
  case** — see §6. (A narrower D1-specific theorem DOES hold under frozen preconditions; see
  `phase-d1-uring-permanent-submit-failure-audit.md` §3.4. The general statement here is the
  conservative default that governs any configuration outside those preconditions.)

---

## 6. Permanent submit failure — HARD GATE (frozen policy) — Gate: permanent failure proof

Production implementation of the permanent-failure retirement path is licensed only by the tagged
Linux 6.1/liburing 2.14 theorem and MUST implement this policy exactly. The source audit and frozen
implementation contract are in `phase-d1-uring-permanent-submit-failure-audit.md`.

> **P0-D implementation note:** A "poison new admission + treat all
> in-flight work as Class-B and wait for CQEs" policy solves the *safety* problem (do not
> prematurely release possibly-kernel-owned work) but NOT the *progress* problem (every accepted
> request must have a provable terminal path). If the kernel never consumed the poisoned batch, no
> CQE ever arrives and the request strands. The focused audit now proves the D1-specific Class-A
> path and freezes the actual-SQ-capacity physical ledger, explicit recovery controller,
> `to_submit=0` drain, control-reference accounting, and teardown preflight. Merge remains gated on
> their command-backed implementation evidence.

### 6.1 The kernel-consumption proof problem (liburing-grounded)

`io_uring_submit()` flushes userspace SQ state to the kernel by advancing the shared-memory SQ tail
**before** the `io_uring_enter` system call (context7: "advances the SQ tail pointer to notify the
kernel of pending operations"). Consequences:

- After the tail advance, the kernel may consume SQEs asynchronously;
- A negative `io_uring_submit()` return (e.g. `-EAGAIN`, `-EBUSY`, `-EINTR`, `-ENOMEM`, `-EFAULT`,
  `-EOVERFLOW`, `-EINVAL`) after the tail advance therefore CANNOT be interpreted **in general** as
  "all ring-owned SQEs are definitely userspace-only and may be locally retired." This is the
  conservative reading. **Qualification:** under D1's exact frozen preconditions (no SQPOLL, no
  SQ_REWIND, serialized one-driver private ring, plain `io_uring_submit()`, no concurrent enter,
  current audited liburing/kernel — see the audit doc §3.4), a narrower theorem DOES prove zero
  consumption on a negative return. Outside those preconditions this general statement governs.

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

**D1 frozen rule:** after a permanent negative submit result, every entry remaining in the bounded
prepared-but-not-confirmed-consumed ledger is Class-A. The Linux 6.1 return-precedence theorem
proves that the failing enter consumed zero entries from that flushed batch. A positive return
removes exactly that count from the ledger front as transport evidence; it does not mutate
`RequestState`.

Here `idx` is a logical, non-wrapping SQ sequence (or an equivalent wrap-safe epoch/distance),
not the wrap-masked SQ array position. The masked position may locate storage, but it cannot prove
failed-batch membership after the ring wraps.

The ledger records the exact physical batch in non-wrapping logical sequence order. Requests
removed by an earlier positive return are Class-C and remain bound for CQE; they cannot be swept
into a later failed Class-A suffix.

### 6.4 SQPOLL note

D1 keeps SQPOLL disabled. Under SQPOLL the kernel consumes SQEs from a kernel thread, so the
`[khead, ktail)` proof is still the basis, but the timing window differs and additional bounded
metadata is needed. Enabling SQPOLL later is a transport/recovery enhancement — it does not
redesign RequestArena, because the lifecycle no longer depends on the submit prefix.

### 6.5 Recovery authority separation

`submit_transport()` produces transport evidence only. A separate poison/recovery controller
consumes the negative-return theorem, marks the remaining physical-ledger batch execution-
impossible, retires its operation entries through explicit Class-A recovery authority, retires
still-local enqueued requests, and leaves previously consumed Class-C requests bound for CQE.
Neither a positive nor negative submit result is itself RequestState or terminal authority.

---

## 7. CQE identity (frozen) — Gate: CQE identity

### 7.1 Kernel boundary

```text
SQE.user_data = op_cookie                 (operation, integer token via io_uring_sqe_set_data64)
             | CONTROL_TAG | op_cookie   (control; informational terminal-wise, exact ref identity)

CQE.user_data  (read via io_uring_cqe_get_data64)
   ↓ decode: control vs op cookie
   op cookie:
       bounded O(request_capacity) scan of the fixed router for the LIVE entry
           whose cookie == cqe cookie              # KEYED BY COOKIE VALUE, not array index
       no LIVE match → DROP (stale cookie; its entry was retired, the array slot may be reused)
       LIVE match → full SlotHandle{slot, full uint64 generation}
       RequestArena full validation (context/slot/generation)
       convert cqe->res to TerminalResult
       no live control for this route → arena.record_terminal(h, terminal), retire route
       live control for this route → retain terminal in bounded RouterEntry scratch
   control cancel CQE:
       decode exact target op_cookie, retire its control execution reference
       if original terminal is deferred → arena.record_terminal(h, terminal), retire route
       NEVER choose or overwrite a request terminal; it only releases the deferred original result
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
    TerminalResult deferred;       // original CQE if matching control has not retired
    ControlState  control_state;   // none | prepared | submitted
    bool          deferred_terminal_stored;
    bool          in_use;
};
```

- The SQE `user_data` carries the **cookie value** via `io_uring_sqe_set_data64` (the integer-token
  API; no pointer round-trip). CQE read via `io_uring_cqe_get_data64`.
- Cookie values are allocated from a **no-wrap 64-bit counter** (`allocate_cookie_()`). Domain is
  `[1, 2^63-1]` (0 unused; the high bit is the control tag). If the counter would enter the tagged
  range, **fail-fast** (never wrap) — mirroring RequestArena generation no-wrap discipline. A cookie
  is therefore **NEVER reused within backend lifetime**.
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

### 7.3 Tagged control identity

D1 reserves the high half of the 64-bit `user_data` domain for controls. An AsyncCancel carries
`CONTROL_TAG | target_op_cookie`; decoding recovers the exact live router entry without using a
recycled array index. The cancel CQE remains informational for terminal selection, but it is exact
authority for retirement of that control execution reference. Future control kinds require a
separately frozen tag layout; they may not collide with the operation-cookie domain.

### 7.4 Reap remains sole Completion publication

The CQE handler may retain a terminal in fixed router scratch while a tagged control reference is
live; once that reference retires, it calls only `arena.record_terminal(h, terminal)` for lifecycle
mutation. It MUST NOT call `AsyncBackend::publish()` directly. `arena.reap(sink)` — invoked from
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
SQE.user_data = CONTROL_TAG | op_cookie
```

The cancel CQE is **control/informational only**. It MUST NOT own RequestKey, publish Completion,
release RequestSlot, overwrite an ordinary result, or independently win the operation terminal.

The **original operation CQE** decides (context7-confirmed: cancel produces two CQEs, order not
guaranteed; cancel CQE `res` ∈ {0, -ENOENT, -EALREADY} is informational):
- ordinary success → success;
- ordinary error → that error;
- `-ECANCELED` → canceled terminal if it wins.

The RequestSlot and router entry remain live until **both** the original operation reference and any
matching submitted control reference retire (or a §6 Class-A proof retires them). If the original
CQE arrives first, its terminal is stored in the fixed RouterEntry and is not made reap-eligible until
the tagged control CQE retires. Thus ordinary callers cannot observe `accepted_outstanding == 0`
while a control reference remains. Repeated cancel calls MUST NOT enqueue unbounded simultaneous
cancel SQEs; one fixed per-slot `cancel_queued` bit plus the router control state is sufficient.

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
| transport ledger (if kept) | actual SQ capacity (`ring.sq.ring_entries`; see §6 audit §4.1) | construction | n/a (TRANSPORT EVIDENCE ONLY) | n/a | n/a | backend destruction |
| io_uring SQ/CQ | `queue_depth` | `io_uring_queue_init` | transport pressure; accepted request stays alive | kernel CQE | SQ pressure count | `io_uring_queue_exit` |
| owned backend worker threads | 0 (D1 has no dedicated driver thread; driver = serialized caller) | n/a | n/a | n/a | n/a | n/a |

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
                 dispatch/cancel arbitration, cookie/router installation and cancel-side lookup)
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
(submit/poll/wait_one/cancel) at the context layer, making D1 single-driver. **As-built for D1:**
because `UringAsyncBackend::wait_source()` returns `nullptr`, `AsyncIoContext::wait_one()` takes
its legacy branch and holds `access_mtx_` across `backend_->wait_one()`. Uring's `wait_one()`
parks in `io_uring_submit_and_wait` inside that call, so **the D1 kernel park happens UNDER
`access_mtx_`**. This is safe ONLY under the documented single-driver eligibility restriction (one
caller drives the context; `ApplicationRuntime` rejects such backends at build time, so this path is
reachable only when the context is driven manually). A split-wait-capable backend (D4
`BackendWaitSource`) parks WITHOUT `access_mtx_`; D1 is not that backend.

The same single-driver contract serializes CQE cookie lookup and router/free-list/scratch
retirement against dispatch installation and cancel-side lookup. CQE retirement deliberately does
not acquire `dispatch_mtx_`, because `arena.record_terminal()` must run with the arena leaf alone;
the header and implementation MUST describe this narrower lock scope and MUST NOT claim that
`dispatch_mtx_` guards all router/scratch mutation. A future call domain that permits concurrent
backend driving must add one shared synchronization protocol before relaxing this D1 restriction.

**Critical rules (AGENTS.md §13.1):**
- Code holding the arena leaf mutex MUST NOT acquire `dispatch_mtx_` or call backend progress.
- `io_uring_submit()` / `io_uring_enter()` (syscalls) MUST NOT be called under the arena mutex.
  D1 calls submit under `dispatch_mtx_` at most, and only as transport progress.
- Joining threads under any queue/progress mutex is forbidden.
- The ordinary CQE path calls `record_terminal` with the arena leaf ALONE. The Class-A recovery
  controller instead follows the documented `dispatch_mtx_ -> arena leaf` order while the poisoned
  ownership snapshot is frozen. No path acquires `dispatch_mtx_` from the arena leaf.

**Lock-order table (explicit):**

| Path | Locks acquired (in order) |
|---|---|
| submit (reserve→commit) | dispatch_mtx_ → arena leaf (transient, per method) → [release dispatch_mtx_ before returning; enqueue re-acquires it] |
| enqueue + dispatch | dispatch_mtx_ (held across push_back → dispatch_one_locked, one CS) → arena leaf (transient inside mark_running) → submit syscall may run under dispatch_mtx_ (transport) |
| cancel pending/enqueued | dispatch_mtx_ → arena leaf (inside `arena.cancel`) |
| cancel running | dispatch_mtx_ (resolve target + scratch cancel_queued bit) → arena leaf (inside `arena.cancel`) |
| CQE lookup/retirement → record/defer terminal | AsyncIoContext single-driver serialization; optional bounded router defer; arena leaf ALONE inside `record_terminal`; no `dispatch_mtx_` |
| permanent-submit recovery | dispatch_mtx_ → arena leaf (Class-A `record_terminal`); no wait, reap, ReadySink, or user code |
| reap (poll/wait_one) | arena leaf → (released) → ReadySink invoked WITHOUT the lock |
| healthy wait_one submit+park | dispatch_mtx_ → kernel `io_uring_submit_and_wait` under AsyncIoContext::access_mtx_; NO arena lock |
| poisoned wait_one park | direct `io_uring_enter(to_submit=0)` under AsyncIoContext::access_mtx_; NO dispatch_mtx_, NO arena lock |

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

`wait_one()` may return `0` only after observing `accepted_outstanding == 0` and
`live_control_sqes == 0`. A route with a live/prepared control defers its original terminal before
`arena.record_terminal`, so `accepted_outstanding` cannot reach zero ahead of that route's control
quiescence. Transient
`-EINTR`/`-EAGAIN`/`-EBUSY`, or a wake that produces no reapable CQE while accepted work remains,
retries the kernel wait; it cannot fabricate a drained boundary for callers.

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
quiescent contract and adds NO `BackendWaitSource` (see §11.1 — `BackendWaitSource` integration is
D4 work). D1 destructors DO preflight quiescence before ring teardown (mirrors ThreadPoolBackend).

**P0-D control-plane contract:** each positively submitted informational cancel SQE holds one
bounded `live_control_sqes` execution reference until its control CQE retires. A cancel SQE in the
proven Class-A poisoned batch never executes and is recovery-retired without incrementing that
count. `CONTROL_TAG | target_op_cookie` resolves the exact route. If the operation CQE arrives first,
its terminal remains in bounded router scratch and the accepted request stays outstanding until the
matching control retires; normal public drain therefore also establishes control quiescence.
Destruction preflight requires `live_control_sqes == 0`; a poisoned ring may retain only
recovery-retired Class-A ledger evidence whose shared-SQ representation is discarded by
`io_uring_queue_exit()` after all user and control execution references are gone.

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

All under `dispatch_mtx_` through Stage 3c, released before enqueue — exactly the proven ThreadPool
shape (D1 reuses `dispatch_mtx_` for admission; there is no separate `admission_mtx_`). Capacity
full → synchronous `would_block`, Completion idle, no borrow, no SQE, zero residue.

### 12.1 Config (public API)

```cpp
struct UringConfig {
    std::size_t request_capacity = 64;   // arena + dispatch ring + router capacity
    unsigned    queue_depth      = 64;   // io_uring SQ/CQ depth
};
```

Legacy `UringAsyncBackend(unsigned queue_depth = 64)` is preserved for source compatibility (maps to
`UringConfig{.request_capacity = queue_depth, .queue_depth = queue_depth}` or a documented default).
The legacy value `0` maps to `64`. The explicit config is validated before RequestArena, vector,
pimpl, or ring allocation: `request_capacity` is in `[1, UINT32_MAX]` (the representable
`SlotIndex` domain) and `queue_depth > 0`. `queue_depth` is already `unsigned`; D1 adds no smaller
artificial upper bound, and unsupported kernel/liburing depths remain ring-init capability
failures. `request_capacity` and `queue_depth` are independent; `request_capacity > queue_depth`
is legal.

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
15. enqueue fast-path SQ pressure drains from the FIFO front and never dispatches a newly appended
    tail ahead of an older queued request;
16. transient `wait_one()` results cannot return `0` while accepted work remains; and
17. invalid `UringConfig` is rejected before backend-state allocation, including
    `request_capacity > UINT32_MAX` on platforms where `size_t` can represent it;
18. a permanent negative submit retires the exact physical-ledger Class-A batch and still-local FIFO;
19. poisoned wait drains old Class-C work with `to_submit=0` without executing quarantined Class-A;
20. original-CQE-before-control ordering defers publication until the exact tagged control retires;
21. a Class-A control suffix releases a deferred Class-C operation result verbatim; and
22. an operation+control Class-A batch retires exactly once with the defined backend error.

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

## 18. Evidence ledger (command-backed)

**Attribution:** the repository GitHub Actions workflow enforces Linux Clang Debug build/tests,
documentation verification, negative-compile authority probes, and backend conformance. It does
not run Release, ASan+UBSan, TSan, or real-liburing execution. Every result below is LOCAL; GitHub
run results are recorded separately per commit and apply only to their recorded head. A cell marked
PASS cites the exact local command. Do NOT imply GitHub CI ran a gate it did not, or that a
prior-head run proves this diff.

| Field | Status |
|---|---|
| Gate 0 — scope & authority | DONE (this document) |
| Gate 1 — state machine | DONE (§3) |
| Gate 2 — lock/atomic authority | DONE (§10, reconciled to the 2-domain production model) |
| Gate 3 — resource capacity | DONE (§9) |
| Gate 4 — wake/progress + shutdown | DONE (§11; D1 wait_source()==nullptr, BackendWaitSource→D4) |
| Clang Debug, real liburing | PASS — `xmake test -v`, 153/153 test targets (local, `--with-liburing=true`) on the P0-D implementation diff |
| Clang Release, real liburing | PASS — `xmake test -v`, 153/153 test targets (local, `--with-liburing=true`) |
| Clang Debug, stub/off | PASS — `xmake test -v`, 151/151 test targets (local, `--with-liburing=false`) |
| ASan + UBSan | PASS — full test group with real liburing; final focused `uring_submit_failure_test` 16/16 |
| TSan | PASS — full test group with real liburing; no sanitizer report |
| negative-compile | PASS — completion authority 12/12, RequestArena 6/6, async API 9/9, async identity 3/3, external backend authority 2/2 |
| conformance manifest | PASS — `python3 scripts/verify-backend-conformance.py` |
| real liburing evidence | PASS — `xmake run uring_submit_failure_test`, 16/16 cases + `xmake run uring_backend_death_test`, 2/2 cases (local, liburing) |
| formal coverage | PASS — `python3 scripts/formal/verify.py suite d1-uring-poison`; correct model passes and all three named broken-model invariants are killed. Structural inventory check also passes. |
| docs (check-doc-links, verify-architecture-docs) | PASS — link checker self-test/full check and architecture verification |
| diff hygiene | PASS — `git diff --check` (local) |
| D0.5 ADR amendment | DONE (`fa66ddf`) |
| **P0-D permanent-submit-failure** | **PASS** — production ledger/poison/control recovery and the complete local evidence matrix are present. |

PASS rows are command-backed results on the final local P0-D implementation diff. GitHub CI is a
separate merge gate and is recorded on the pushed commit.

---

## 19. Success criterion

Not "new io_uring code compiles". It is:

> `UringAsyncBackend` has moved onto the private-ring / ring-owned RequestArena lifetime model;
> no request lifecycle decision depends on `io_uring_submit()` accepted-prefix accounting;
> cancellation cannot release ring-owned work; Completion publication is reap-only; storage is
> bounded; and every accepted request has a provable terminal path.

**Status:** the source proof, production recovery contract, implementation, and local evidence
matrix are complete. Merge still requires the pushed-head GitHub CI gate.
