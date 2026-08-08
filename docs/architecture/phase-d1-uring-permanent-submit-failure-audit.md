# Phase D1 — Permanent `io_uring_submit()` Failure Recovery Audit

**Status:** HARD GATE AUDIT — input to a future D-x production implementation.
**Date:** 2026-08-09
**Author:** jnhu
**Governing:** `docs/architecture/phase-d1-uring-frozen-design.md` §6 (HARD GATE)
**Scope:** determine, from authoritative liburing + Linux-kernel sources, whether a clean Class-A
local-retirement proof exists for D1's exact configuration, and whether a production poison/recovery
implementation can be licensed. **No production poison is shipped by this audit.**

---

## 1. The D1 configuration under audit

D1's `UringAsyncBackend` submits with:

```text
flags        = 0                       (NO SQPOLL, NO SINGLE_ISSUER, NO DEFER_TASKRUN)
one private io_uring per backend
all backend entries serialized by AsyncIoContext::access_mtx_ (single driver)
no concurrent io_uring_enter()
io_uring_submit(ring)  ==  __io_uring_flush_sq()  +  io_uring_enter(to_submit = N, min_complete = 0, flags = 0)
```

`io_uring_submit()` (liburing `src/queue.c`) first flushes the application-side `sqe_tail` to the
shared SQ state (`__io_uring_flush_sq` advances `*ktail`), then issues
`io_uring_enter(to_submit = N, min_complete = 0, flags = 0)`. The shared-SQ tail advance happens
**before** the `io_uring_enter` system call.

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

### 3.2 `io_submit_sqes()` — the kernel submission loop (Linux `io_uring/io_uring.c`)

The kernel's `io_submit_sqes(struct io_ring_ctx *ctx, unsigned int to_submit)` is the function
`io_uring_enter` calls to drain the SQ. Its return logic (current mainline):

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

D1 sets `flags = 0` (no `IORING_ENTER_GETEVENTS`) and `min_complete = 0`. Therefore:

- The wait/get-events phase is **gated off** (`if (flags & IORING_ENTER_GETEVENTS)` is false).
- The early-exit `if (ret != to_submit) goto out;` preserves the (possibly partial-positive) submit
  count and returns it directly.
- **A wait/control error cannot overwrite a positive submit count** because there is no wait phase.

### 3.4 Conclusion (the theorem)

> **In D1's exact configuration (flags=0, no SQPOLL, no GETEVENTS, single driver), a negative
> `io_uring_submit()` return proves ZERO SQEs from that flushed batch were consumed by the kernel.**

Corollary: those SQEs will never produce CQEs (the kernel never saw them as consumed requests).
They are provably Class-A (definitely not kernel-consumed) and may be locally retired — provided
the membership of the failed batch can be tracked with bounded metadata and the wait path is
prevented from re-submitting it.

---

## 4. Candidate recovery model (NOT implemented; requires further proof)

Given the §3.4 theorem, the candidate recovery model is:

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
    ├─ drain ONLY previously-consumed/in-flight operations via
    │      io_uring_enter(to_submit=0, min_complete=K, GETEVENTS)   [wait-path audit §5]
    │
    ├─ once old kernel-owned work is quiescent (all CQEs reaped)
    │
    ├─ locally terminalize the quarantined Class-A requests with backend_error
    │
    └─ tear down the poisoned ring
```

This is only a candidate. Each step below MUST be proven before any production implementation:

### 4.1 Bounded transport metadata

The failed-batch membership is the set of SlotHandles whose `io_uring_sqe_set_data64(cookie)`
happened but whose SQE index was in the application-side `[sqe_head, sqe_tail)` window that the
negative-return `io_uring_submit()` failed to enter-consume. Proving exact membership requires
tracking, per dispatch, the cookie → SlotHandle mapping that is already in the CqeRouter (P0-B) AND
the physical SQ order. The CqeRouter alone is insufficient (it does not record which cookies were in
the unflushed batch vs. already-entered in a prior successful submit).

Required proof: a bounded construction-time ledger of "prepared-but-not-yet-confirmed-consumed"
cookies, updated on each `get_sqe` (append) and each successful positive `io_uring_submit` return
(drain the returned count from the front). This is **transport evidence only**; it MUST NOT drive
RequestArena lifecycle (reviewer §6 — no submit-count correctness authority). It only identifies
the quarantined set after a proven-zero-consumption negative return.

### 4.2 Cancel-control-SQE classification

A cancel SQE (`IORING_OP_ASYNC_CANCEL`, user_data = CONTROL_CANCEL) may share the failed batch with
operation SQEs. Under §3.4, a negative return means zero consumed, so a quarantined cancel SQE is
also Class-A and simply never executes — its `cancel_queued` bit must be left set (no CQE will clear
it) or retired alongside the batch. Prove this does not leave the per-slot `cancel_queued` bit
stuck in a state that blocks a later cancel on a fresh ring.

### 4.3 Still-enqueued local dispatch requests

Requests still in the local dispatch queue (never dispatched, no SQE obtained) when poison fires are
trivially Class-A — they have no kernel identity at all. They may be locally terminalized with
`backend_error` directly via the existing Scheme-B `record_terminal` path (no SQE was ever
installed). Prove the dispatch-queue peek protocol (§4.2 of the frozen design) makes this set
exactly the queue contents at the poison instant.

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

## 6. Verdict

- **The §3.4 theorem holds** for D1's configuration: a negative `io_uring_submit()` proves zero
  consumed SQEs. This is the Class-A proof basis the original §6.5 deemed possibly unavailable.
- **A clean Class-A retirement path is therefore architecturally available**, BUT it requires:
  1. the bounded transport-metadata ledger (§4.1);
  2. the cancel-control-SQE classification (§4.2);
  3. the still-enqueued retirement (§4.3);
  4. the wait-path re-submission guard replacing `io_uring_submit_and_wait` with a
     `to_submit=0`/`get_events` drain (§5);
  5. a ring-teardown proof that discards the quarantined batch.

- **D1 does NOT implement any of (1)–(5).** Production poison is therefore NOT shipped. The
  `fatal_error_` field remains read-only evidence plumbing; submit/wait surface it but no
  retirement/recovery runs.

**P0-D status: BLOCKED on production implementation of (1)–(5).** This audit licenses the design;
a follow-up D-x phase must implement and prove it. D1 is READY FOR HUMAN REVIEW but NOT merge-ready
on the "every accepted request has a provable terminal path" axis until that implementation lands.

---

## 7. Sources inspected

- `io_uring_enter(2)` — https://man7.org/linux/man-pages/man2/io_uring_enter.2.html
- `io_uring(7)` — https://man7.org/linux/man-pages/man7/io_uring.7.html
- `io_uring_get_sqe(3)` — https://man7.org/linux/man-pages/man3/io_uring_get_sqe.3.html
- liburing `src/queue.c` (`__io_uring_flush_sq`, `io_uring_submit`) — https://github.com/axboe/liburing
- Linux `io_uring/io_uring.c` (`io_submit_sqes`, `io_uring_enter`/`__io_uring_enter`) —
  https://github.com/torvalds/linux/blob/master/io_uring/io_uring.c
- Debian liburing-dev `io_uring_enter(2)` —
  https://manpages.debian.org/unstable/liburing-dev/io_uring_enter.2.en.html
