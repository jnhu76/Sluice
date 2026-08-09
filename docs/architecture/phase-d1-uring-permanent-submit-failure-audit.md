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

`io_uring_submit()` (liburing, in its `queue.c` source file — see
<https://github.com/axboe/liburing/blob/master/src/queue.c>) first flushes the
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

**Production-recovery prerequisite:** before relying on this theorem as a
*portable* Sluice guarantee, define the supported kernel/liburing baseline and
verify the theorem against the minimum supported kernel family (or identify the
stable ABI / man-page guarantee that makes source-version inspection
unnecessary). The local WSL2 kernel version alone is not a source-level
portability proof.

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

### 4.4 Control execution references and teardown quiescence

A running-operation cancel may append an informational `IORING_OP_ASYNC_CANCEL`. The original
operation CQE can retire its operation cookie and make the Completion ready before the control CQE
arrives. Therefore `live_op_cookies == 0`, arena quiescence, and an empty local dispatch queue do
not by themselves prove that the ring has no remaining control execution reference.

The P0-D implementation MUST choose one of two explicit contracts:

1. keep a bounded `live_control_sqes`/physical-ledger reference until each control CQE retires; or
2. prove that teardown may abandon informational control SQEs because they hold no user buffer,
   RequestSlot release authority, reusable target cookie, or user-visible terminal authority.

The chosen contract must appear in the destruction preflight/teardown proof and deterministic
tests. `io_uring_queue_exit()` is not treated as an unmodeled no-op.

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

- **The §3.4 theorem holds** under D1's frozen preconditions (§3.4): a negative
  `io_uring_submit()` proves zero consumed SQEs. This is the Class-A proof basis
  the original §6.5 deemed possibly unavailable. The theorem is narrowly scoped
  and must NOT be generalized beyond those preconditions; relying on it as a
  portable Sluice guarantee requires defining the supported kernel/liburing
  baseline (§3.4 production-recovery prerequisite).
- **A clean Class-A retirement path is therefore architecturally available**, BUT it requires:
  1. the bounded transport-metadata ledger (§4.1);
  2. the cancel-control-SQE classification (§4.2);
  3. the still-enqueued retirement (§4.3);
  4. the wait-path re-submission guard replacing `io_uring_submit_and_wait` with a
     `to_submit=0`/`get_events` drain (§5);
  5. a ring-teardown proof that discards the quarantined batch;
  6. a control-execution-reference/quiescence contract (§4.4); and
  7. a non-wrapping logical SQ sequence proof across masked-slot reuse (§4.1).

- **D1 does NOT implement any of (1)–(7).** Production poison is therefore NOT shipped. The
  `fatal_error_` field remains read-only evidence plumbing; submit/wait surface it but no
  retirement/recovery runs.

**P0-D status: BLOCKED on production implementation of (1)–(7).** This audit licenses the design;
a follow-up D-x phase must implement and prove it. D1 is READY FOR HUMAN REVIEW but NOT merge-ready
on the "every accepted request has a provable terminal path" axis until that implementation lands.

---

## 7. Sources inspected

- `io_uring_enter(2)` — https://man7.org/linux/man-pages/man2/io_uring_enter.2.html
- `io_uring(7)` — https://man7.org/linux/man-pages/man7/io_uring.7.html
- `io_uring_get_sqe(3)` — https://man7.org/linux/man-pages/man3/io_uring_get_sqe.3.html
- liburing `queue.c` source (`__io_uring_flush_sq`, `io_uring_submit`) —
  https://github.com/axboe/liburing/blob/master/src/queue.c
- Linux io_uring source (`io_submit_sqes`, `io_uring_enter`/`__io_uring_enter`) —
  https://github.com/torvalds/linux/blob/master/io_uring/io_uring.c
- Debian liburing-dev `io_uring_enter(2)` —
  https://manpages.debian.org/unstable/liburing-dev/io_uring_enter.2.en.html
