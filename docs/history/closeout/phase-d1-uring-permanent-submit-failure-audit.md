> **HISTORICAL / EVIDENCE — NOT CURRENT AUTHORITY.** Archived from `docs/architecture/` by S0-DOCS (#290, 2026-09-04). Point-in-time record; do not cite as authority for new decisions.

# Phase D1 — Permanent `io_uring_submit()` Failure Recovery Audit

**Status:** IMPLEMENTED AND VERIFIED — source audit and merge-gate evidence complete.
**Date:** 2026-08-09
**Author:** jnhu
**Governing:** `docs/history/closeout/phase-d1-uring-frozen-design.md` §6 (HARD GATE)
**Scope:** determine, from authoritative liburing + Linux-kernel sources, whether a clean Class-A
local-retirement proof exists for D1's exact configuration, and freeze the production
poison/recovery contract licensed by that proof.

## 0. Supported baseline and implementation authority

The production theorem is supported on **Linux 6.1 or newer** with **liburing 2.14**. The build
gate pins liburing 2.14; Linux 6.1 is the minimum supported kernel family for this recovery path.
The proof was checked against tagged upstream sources, not the local WSL2 version:

- Linux v6.1 `io_submit_sqes()` preserves a positive consumed prefix and changes the result to
  `-EAGAIN` only when zero requests were submitted;
- Linux v6.1 `io_uring_enter()` returns early when that submit result differs from `to_submit`, and
  a GETEVENTS result replaces the return value only when the submit result was zero;
- liburing 2.14 `io_uring_submit()` performs `__io_uring_flush_sq()` before enter, while the poison
  drain calls `io_uring_enter(fd, 0, 1, IORING_ENTER_GETEVENTS, nullptr)` and therefore cannot
  consume the quarantined shared-SQ tail;
- liburing 2.14 `io_uring_queue_exit()` unmaps the SQ/CQ and closes the ring fd; Linux v6.1 then
  runs `io_ring_ctx_wait_and_kill()`. Teardown is licensed only after the §4.4/§5 preflight proves
  backend progress ownership is zero.

This is a source-level support contract, not runtime version parsing. Kernels older than Linux 6.1
are outside the supported real-backend recovery contract.

`submit_transport_locked()` remains transport evidence only. It updates the physical ledger, but it
never mutates `RequestState`, chooses a terminal, or publishes a Completion. On a permanent
negative result, a distinct recovery controller consumes the §3.4 proof, establishes execution
impossibility for the failed ledger batch, and then invokes explicit recovery-retirement authority.

---

## 1. The D1 configuration under audit

D1's `UringAsyncBackend` submits with:

```text
app-call flags = 0                     (NO SQPOLL, NO SINGLE_ISSUER, NO DEFER_TASKRUN)
one private io_uring per backend
all backend entries serialized by AsyncIoContext::access_mtx_ (single driver)
no concurrent io_uring_enter()
io_uring_submit(ring)  ==  __io_uring_flush_sq()  +  io_uring_enter(to_submit = N, min_complete = 0)
```

The application never requests `IORING_ENTER_GETEVENTS` or `min_complete > 0`
on the submit path. **Qualification:** current liburing MAY internally add
`IORING_ENTER_GETEVENTS` to the underlying `io_uring_enter` during a plain
`io_uring_submit()` when CQ flushing / task_work requires an enter, even though
the caller's wait count is zero. The theorem in §3.4 accounts for this and does
not depend on "the syscall never carries GETEVENTS"; see §3.3.

`io_uring_submit()` (liburing 2.14, in its `queue.c` source file — see
<https://github.com/axboe/liburing/blob/liburing-2.14/src/queue.c>) first flushes the
application-side `sqe_tail` to the shared SQ state (`__io_uring_flush_sq`
advances `*ktail`), then issues
`io_uring_enter(to_submit = N, min_complete = 0)`. The shared-SQ
tail advance happens **before** the `io_uring_enter` system call.

---

## 2. The audit question

> In D1's exact configuration, if `io_uring_submit()` returns a negative syscall-level error, were
> ZERO SQEs from that flushed batch consumed by the kernel?

If YES → a clean Class-A proof basis exists: a negative return ⟹ zero consumed ⟹ those SQEs will
never produce CQEs ⟹ they may be locally retired with `backend_error` after a structural proof.
If NO → D1 must keep all in-flight work Class-B and P0-D remains unresolved.

---

## 3. Authoritative findings

### 3.1 `io_uring_enter(2)` — the system-call boundary

From `io_uring_enter(2)`:

- On success, returns "the number of I/Os successfully consumed."
- "errors that occur not on behalf of a submission queue entry are returned via the system call
  directly"; per-SQE operation errors are delivered through CQEs, not as the syscall's negative
  return.
- "When the system call returns that a certain amount of SQEs have been consumed and submitted, it's
  safe to reuse SQE entries in the ring" — and "if the kernel requires later use of a particular SQE
  entry, it will have made a private copy of it."

A negative return is therefore a kernel-level/control error, distinct from per-SQE CQE errors.

### 3.2 `io_submit_sqes()` — the kernel submission loop (Linux io_uring source)

The kernel's `io_submit_sqes(struct io_ring_ctx *ctx, unsigned int to_submit)` is the function
`io_uring_enter` calls to drain the SQ. Its return logic in the audited Linux v6.1 source is:

```c
ret = left = entries;          /* entries == min(to_submit, SQ-available) */
/* ... loop submitting SQEs; on each failure 'left' stops decrementing ... */
ret -= left;                   /* ret = number actually submitted */
/* ... */
if (!ret && io_req_cache_empty(ctx))
    ret = -EAGAIN;             /* overwrite ONLY when ZERO submitted AND cache empty */
return ret;
```

The load-bearing facts:

1. `ret` starts at the requested count and is decremented by the un-submitted remainder. **Once at
   least one SQE is consumed, `ret` is positive.**
2. The `-EAGAIN` overwrite is gated on `!ret` — it fires **only when zero SQEs were submitted**.
   A batch that consumed ≥1 SQE and then hit an allocation failure returns the *positive* count,
   not `-EAGAIN`.
3. Therefore: **`io_submit_sqes` returns negative ⟺ zero SQEs consumed** (for the `-EAGAIN` path;
   other `-errno` control errors occur before the loop consumes anything).

### 3.3 `io_uring_enter` / `__io_uring_enter` — phase combination

`io_uring_enter` (the syscall, `SYSCALL_DEFINE6(io_uring_enter, ...)`) evaluates submit and wait
phases independently:

```c
ret = io_submit_sqes(ctx, to_submit);
if (ret != to_submit) {
    mutex_unlock(&ctx->uring_lock);
    goto out;
}
/* wait / get-events phase: */
if (flags & IORING_ENTER_GETEVENTS) {
    /* ... wait logic; can assign a wait error to a separate variable ... */
}
out:
    return ret;
```

D1 sets `flags = 0` (no `IORING_ENTER_GETEVENTS`) and `min_complete = 0` at the
*application* call surface. **Caveat (qualification):** current liburing MAY
internally add `IORING_ENTER_GETEVENTS` during a plain `io_uring_submit()` when
CQ flushing/task_work requires an enter, even though the caller's `wait_nr` is
zero. The theorem below therefore does NOT rely on "the syscall never carries
GETEVENTS"; it relies on the kernel's submit-count-vs-wait-result precedence
when GETEVENTS happens to be present. Therefore:

- If the submit phase consumed ≥1 SQE, `io_submit_sqes` returns a **positive**
  count and the early-exit `if (ret != to_submit) goto out;` preserves it (the
  count is returned directly).
- A GETEVENTS wait result, if present, replaces `ret` **only when the submit
  `ret` was already zero** (no SQEs consumed). It cannot overwrite a positive
  submit count.
- A negative control error from the submit phase (`-EAGAIN` via the zero-
  submitted path, or other `-errno` before the loop consumes anything) is
  therefore not a mask for a positive partial submission.

### 3.4 Conclusion (the theorem)

> **In D1's exact configuration — no SQPOLL, no SQ_REWIND, a serialized
> one-driver private ring, plain `io_uring_submit()` with no concurrent
> `io_uring_enter()`, under the current audited liburing/kernel submit
> semantics — a negative `io_uring_submit()` return proves ZERO SQEs from that
> flushed batch were consumed by the kernel.**

Proof sketch (the load-bearing precedence):

```text
io_submit_sqes:   partial consumption  =>  positive count preserved
io_uring_enter:   if submission count != to_submit  =>  return that count/error
                  (GETEVENTS may add a wait result, but only when submit ret == 0)
therefore:        a negative plain-submit return cannot hide a positive
                  partial submission
```

Corollary: those SQEs will never produce CQEs (the kernel never saw them as consumed requests).
They are provably Class-A (definitely not kernel-consumed) and may be locally retired — provided
the membership of the failed batch can be tracked with bounded metadata and the wait path is
prevented from re-submitting it.

**Scoping — the theorem MUST NOT be generalized to:** SQPOLL, SQ_REWIND,
multi-issuer rings, a future private-shard topology, or an arbitrary future
kernel/liburing implementation. It holds only for D1's frozen preconditions
above.

**Production-recovery prerequisite: SATISFIED.** Section 0 defines Linux 6.1 + liburing 2.14 and
records the tagged-source verification. The local WSL2 kernel version is execution evidence only,
not the source-level portability proof.

---

## 4. Frozen production recovery model

Given the §3.4 theorem, the production recovery model is:

```text
negative permanent io_uring_submit()
    │
    ├─ freeze/poison NEW admission (submit_* rejects with backend_error)
    │
    ├─ identify the failed flushed batch membership (bounded transport metadata:
    │      the set of SlotHandles whose SQEs were in the unsubmitted application-side
    │      sqe_tail window at the moment of the negative return)
    │
    ├─ NEVER call io_uring_enter with to_submit > 0 on that ring again
    │      (prevents re-submitting the quarantined batch)
    │
    ├─ locally terminalize the quarantined Class-A requests with backend_error
    │
    ├─ drain ONLY previously-consumed/in-flight operation and control CQEs via
    │      io_uring_enter(to_submit=0, min_complete=K, GETEVENTS)   [wait-path audit §5]
    │
    ├─ once old kernel-owned work is quiescent (all CQEs reaped)
    │
    └─ tear down the poisoned ring
```

The implementation satisfies each rule below; command-backed evidence is recorded in the frozen
design's §18 ledger.

### 4.1 Bounded transport metadata

The failed-batch membership is the set of SlotHandles whose `io_uring_sqe_set_data64(cookie)`
happened but whose SQE index was in the application-side `[sqe_head, sqe_tail)` window that the
negative-return `io_uring_submit()` failed to enter-consume. Proving exact membership requires
tracking, per dispatch, the cookie → SlotHandle mapping that is already in the CqeRouter (P0-B) AND
the physical SQ order. The CqeRouter alone is insufficient (it does not record which cookies were in
the unflushed batch vs. already-entered in a prior successful submit).

**Capacity bound (frozen rule for the future ledger):**

```text
transport_ledger_capacity = actual initialized SQ entry capacity
                          = ring.sq.ring_entries
```

The ledger is bounded by the **actual SQ entry capacity returned by ring setup**
(`sq_entries` / `ring.sq.ring_entries`), NOT by `request_capacity` and NOT
necessarily by the raw configured `UringConfig.queue_depth`: Linux rounds SQ
entries to the actual ring size (normally a power of two). Counterexample:

```text
configured queue_depth = 65
actual SQ capacity     = 128
```

A ledger sized to 65 (or to `request_capacity`) cannot represent all physical
pending SQEs when the kernel rounded 65 up to 128. Use the real initialized
ring capacity, queried from the live `ring.sq.ring_entries` after
`io_uring_queue_init`.

**Per-entry representation (physical SQ authority, distinct from RequestArena
authority):** each ledger entry records, at minimum,

```text
non-wrapping monotonic logical SQ sequence (or equivalent epoch + modular distance)
masked physical SQ array position (storage location only)
kind:
    operation
        op_cookie
        full SlotHandle (slot + generation)
    cancel_control
        target op_cookie / target SlotHandle
```

A scalar "pending operation count" is **forbidden**. The physical SQ sequence

```text
OP A
CANCEL A
OP B
CANCEL B
...
```

is a legal physical SQ order (a cancel SQE may share the failed batch with the
operation SQEs it targets). A scalar count collapses operation and control
entries and cannot represent that interleaving; only a per-entry ledger can.

The Class-A proof MUST NOT use `tail & ring_mask` as identity. That value wraps and is reused: for
a 64-entry ring, logical sequences 5 and 69 both occupy masked position 5. Retirement must compare
the non-wrapping logical sequence (or an explicitly proven wrap-safe epoch/distance invariant) so a
consumed pre-wrap entry can never be reclassified as an unconsumed post-wrap entry.

Required proof: a bounded construction-time ledger (capacity = actual SQ
entries) of "prepared-but-not-yet-confirmed-consumed" entries, updated on each
`get_sqe` (append, classified operation/control) and each successful positive
`io_uring_submit` return (drain the returned count from the front). This is
**TRANSPORT EVIDENCE ONLY**; it MUST NEVER drive:

```text
RequestState
the terminal winner
Completion publication
the enqueued -> running transition
```

It only identifies the quarantined set after a proven-zero-consumption negative
return (reviewer §6 — no submit-count correctness authority).

### 4.2 Cancel-control-SQE classification

A cancel SQE (`IORING_OP_ASYNC_CANCEL`, `user_data = CONTROL_TAG | target_op_cookie`) may share the
failed batch with operation SQEs. Under §3.4, a negative return means zero consumed, so a
quarantined cancel SQE is also Class-A and never executes. Recovery resolves the exact bounded
router entry from the target cookie, clears its `prepared` control state / `cancel_queued` bit, and
releases any original terminal deferred behind that control. It never invents or overwrites the
operation result.

### 4.3 Still-enqueued local dispatch requests

Requests still in the local dispatch queue (never dispatched, no SQE obtained) when poison fires are
trivially Class-A — they have no kernel identity at all. They may be locally terminalized with
`backend_error` directly via the existing Scheme-B `record_terminal` path (no SQE was ever
installed). Prove the dispatch-queue peek protocol (§4.2 of the frozen design) makes this set
exactly the queue contents at the poison instant.

### 4.4 Control execution references and teardown quiescence

A running-operation cancel may append an informational `IORING_OP_ASYNC_CANCEL`. Without an exact
control identity, the original operation CQE could retire its operation cookie and make the
Completion ready before the control CQE arrives. D1 closes that gap by tagging the control CQE with
its target operation cookie and retaining the router entry until both references retire.

The frozen contract chooses the stronger first option: every positively submitted control entry
transitions its exact router control state `prepared -> submitted`, increments bounded
`live_control_sqes`, and only its tagged control CQE decrements that reference. If the original CQE
arrives first, its terminal stays in fixed RouterEntry scratch and is not recorded into RequestArena
until the control retires; public `accepted_outstanding` therefore cannot reach zero early. A control
entry in the proven Class-A failed batch never increments the live count and its per-slot
bookkeeping is retired by the recovery controller. Destruction preflight requires:

```text
dispatch queue empty
live operation cookies == 0
live control SQEs == 0
arena slot_in_use == accepted_outstanding == backend_ready == 0
normal ring: transport ledger empty
poisoned ring: every retained ledger entry is proven Class-A and recovery-retired
```

`io_uring_queue_exit()` is not treated as an unmodeled no-op: it may discard only the final
quarantined shared-SQ representation after all operation/control execution references and all user
lifecycle ownership have already retired.

---

## 5. Wait-path re-submission audit (reviewer §10.2)

After poison, the ordinary `wait_one()` path is SUSPECT: `io_uring_submit_and_wait(ring, 1)` both
flushes pending SQEs (re-submitting the quarantined batch) and blocks for a CQE. A recovery design
MUST NOT use `io_uring_submit_and_wait` after poison.

Candidate drain primitives for already-kernel-owned operations (Class-B/C) after poison:

```text
io_uring_get_events(ring)                              # liburing helper: enter(to_submit=0, GETEVENTS)
io_uring_enter(to_submit=0, min_complete=K, IORING_ENTER_GETEVENTS)
io_uring_peek_batch_cqe(...)                           # non-blocking peek (already used by reap_cqes)
```

Required proof:

- `to_submit = 0` cannot accidentally consume the quarantined shared-SQ tail (it submits nothing).
  Confirm via the `__io_uring_flush_sq` path: with nothing new prepared, `sqe_head == sqe_tail`,
  so the flush is a no-op and `io_uring_enter(to_submit=0)` submits zero — but it MAY still drive
  `min_complete` completion waits. Prove this drains Class-B/C CQEs without touching the
  quarantined batch.
- A poisoned ring's eventual `io_uring_queue_exit` cannot execute the quarantined SQEs (teardown
  drops the ring; unconsumed SQEs are simply discarded). Confirm via kernel `io_ring_ctx_wait_and_kill`.

---

## 6. Implementation verdict

- **The §3.4 theorem holds** under D1's frozen preconditions (§3.4): a negative
  `io_uring_submit()` proves zero consumed SQEs. This is the Class-A proof basis
  the original §6.5 deemed possibly unavailable. The theorem is narrowly scoped
  and must NOT be generalized beyond those preconditions; relying on it as a
  portable Sluice guarantee requires defining the supported kernel/liburing
  baseline (§3.4 production-recovery prerequisite).
- **A clean Class-A retirement path is architecturally licensed** and production must implement:
  1. the bounded transport-metadata ledger (§4.1);
  2. the cancel-control-SQE classification (§4.2);
  3. the still-enqueued retirement (§4.3);
  4. the wait-path re-submission guard replacing `io_uring_submit_and_wait` with a
     `to_submit=0`/`get_events` drain (§5);
  5. a ring-teardown proof that discards the quarantined batch;
  6. a control-execution-reference/quiescence contract (§4.4); and
  7. a non-wrapping logical SQ sequence proof across masked-slot reuse (§4.1).

**P0-D status: PASS.** The production implementation, deterministic C++ regressions, focused
formal model, real/stub liburing gates, sanitizers, documentation checks, and adversarial review
all pass on the final local diff. Pushed-head GitHub CI remains a separate repository merge gate.

---

## 7. Sources inspected

- `io_uring_enter(2)` — https://man7.org/linux/man-pages/man2/io_uring_enter.2.html
- `io_uring(7)` — https://man7.org/linux/man-pages/man7/io_uring.7.html
- `io_uring_get_sqe(3)` — https://man7.org/linux/man-pages/man3/io_uring_get_sqe.3.html
- liburing 2.14 `queue.c` source (`__io_uring_flush_sq`, `io_uring_submit`, `io_uring_get_events`) —
  https://github.com/axboe/liburing/blob/liburing-2.14/src/queue.c
- liburing 2.14 `setup.c` source (`io_uring_queue_exit`) —
  https://github.com/axboe/liburing/blob/liburing-2.14/src/setup.c
- Linux v6.1 io_uring source (`io_submit_sqes`, `io_uring_enter`, `io_ring_ctx_wait_and_kill`) —
  https://github.com/torvalds/linux/blob/v6.1/io_uring/io_uring.c
- Debian liburing-dev `io_uring_enter(2)` —
  https://manpages.debian.org/unstable/liburing-dev/io_uring_enter.2.en.html
