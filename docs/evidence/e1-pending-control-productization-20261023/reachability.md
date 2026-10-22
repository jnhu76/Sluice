# E1 Productization — P>0 Reachability Analysis (Gate R4, first half)

Answers, from source, where the SQE-exhausted sticky branch can actually be
entered on this stack, before any measurement is attempted. Host: E5,
kernel 7.2.9-201.fc44, liburing 2.13 (distro package `liburing-2.13-2.fc44`),
gcc 16.2.1.

## The sticky branch and its precise entry condition

`issue_running_cancel_locked_` (`src/async/uring_transport.cpp:636-671` at
product HEAD):

1. `sqe = io_uring_get_sqe(...)` — fails iff the app-visible SQ ring has
   `sqe_tail - *khead == ring_entries` (liburing `_io_uring_get_sqe`,
   `/usr/include/liburing.h:1917`: the constraint is against the
   **kernel-consumed head**, not the app flush head).
2. On failure it calls `submit_transport_locked()` once (the real
   `io_uring_submit` in production) and retries `io_uring_get_sqe`.
3. Sticky intent is retained only if the retry **also** fails
   (`sqe == nullptr` twice).

For step 2 to leave the SQ ring full, `io_uring_submit` must fail to free a
slot. In liburing 2.13 (`src/queue.c` at tag `liburing-2.13`, verified
against the upstream source):

- `io_uring_submit` = `__io_uring_submit(ring, __io_uring_flush_sq(ring), 0, false)`.
- `__io_uring_flush_sq` writes the shared-memory tail (`*sq->ktail = tail`)
  and advances `sq->sqe_head` to `sqe_tail` — every prepared SQE is handed to
  the kernel before the enter.
- `__io_uring_submit` calls `__sys_io_uring_enter(fd, submitted, 0, flags, NULL)`
  and, crucially, adds `IORING_ENTER_GETEVENTS` when
  `cq_ring_needs_enter(ring)` — which is true when the kernel set
  `IORING_SQ_CQ_OVERFLOW` (CQ overflow pending) or `IORING_SQ_TASKRUN`. That
  enter flushes parked overflow completions into the CQ ring, freeing CQ
  capacity as a side effect.

On a non-SQPOLL ring, the kernel consumes submitted SQEs **inside** the
`io_uring_enter` syscall itself, so after a successful enter the app-visible
SQ ring is empty and `io_uring_get_sqe` succeeds. The retry in step 2 can
therefore still fail only when the enter could not consume:

- **(a) `-EBUSY` from CQ congestion**: the kernel-side CQ ring is full of
  unreaped completions (and overflow already flushed), so it refuses new
  submissions. liburing 2.13's internal GETEVENTS drain only helps while
  events remain parked in overflow; a *visible* CQ ring full of CQEs the
  application has not reaped is freed only by reaping.
- **(b) SQPOLL with a sleeping SQ thread** (`IORING_SQ_NEED_WAKEUP` regime):
  the kernel consumes asynchronously. Sluice does not configure SQPOLL
  (submission policy is fixed by the backend; no SQPOLL in this change's
  scope), so (b) does not apply.
- **(c) A submit error** (e.g. `-EINVAL`): that path poisons the backend
  rather than retaining intent — a different branch (`submit` failure
  accounting), verified by the poison suite.

## Why the tested workloads do not reach (a)

Sluice's dispatch is eager-per-accept: `dispatch_after_accept` prepares the
SQE and immediately calls `submit_transport_locked()`
(`uring_transport.cpp:87`), and `poll()` likewise submits before reaping
(`uring_backend.cpp` `poll()`: dispatch drain → submit → service). The CQ
therefore accumulates only within a pass, and each pass reaps its visible CQ
ring before the next submission burst needs control SQEs. The research
campaign's congestion storm (instant-completing tmpfs reads at C32/Q8/W24)
was constructed to jam exactly this window: its monitor observed live
control-path SQEs in the kernel (`control_sqe_observations` up to 42, and 6
samples in the archived supplement) with `p_positive_samples = 0` across 12
runs / 1,731 monitor samples — i.e. by the time a cancel needed an SQE, the
preceding submit had always freed one.

Distinguishing the four states the task names:

| State | Where it lives | Can it produce sticky intent here? |
|---|---|---|
| Ring physically full (kernel-side SQ) | `sqe_tail - *khead == entries` | Only via (a)/(b); (b) unused, (a) requires CQ full of unreaped completions at the retry submit — not produced under eager-submit + per-pass reap on this stack |
| CQ congestion | kernel CQ ring + overflow | Drained by liburing 2.13's internal GETEVENTS on overflow, and by Sluice's reap before the next submit burst; the observed EBUSY storms drained before the control path sampled `get_sqe` |
| Control SQE already prepared | `control_state == prepared` | Not a sticky state: the flag is false (INV-2) and the count is zero |
| Retained sticky intent | `control_pending == true` (count > 0) | Requires both `get_sqe` calls to fail; not observed on this stack in the research campaign's 1,731+ samples |

## Version sensitivity (why this is NOT declared unreachable in general)

The drain behavior is a property of liburing 2.13's `__io_uring_submit`
adding `IORING_ENTER_GETEVENTS` on `cq_ring_needs_enter`. Older liburing
without that internal drain (or an application-driven submission path that
batches without reaping, or SQPOLL rings, or a future submission-policy
change in Sluice) can legitimately leave the SQ ring full at the retry, and
the sticky branch is reachable there — the deterministic suites
(`uring_pending_control_parity_test`) exercise it through the controlled
submit seam, and the contract (CANCEL-01) requires the sticky intent to be
retained and serviced whenever it occurs. No claim of general unreachability
is made; the claim is scoped to: real-kernel reachability was not observed
on E5 / kernel 7.2.9 / liburing 2.13 under the protocol-legal workloads
attempted (research campaign + the product probe below).

## Product-HEAD confirmation probe (Path A attempt)

The formal real-kernel campaign evidence is the research round's
(`docs/research/mw-hw-1/native-linux-e1/pending-control-pressure.md`:
`P_POSITIVE_COST = NOT_MEASURED`, 12 runs / 1,731 monitor samples / 0 sticky
observations). To confirm the product HEAD behaves the same, a bounded probe
(the research probe binary rebuilt against the product library) re-ran the
congestion workload; results are archived under `native-p5-probe/` in this
directory. Per the preregistered rule, absence of P>0 on the real kernel
does **not** discharge the cost obligation — it routes Gate R4 to Path B
(seam-based mechanism cost bound), with
`P_POSITIVE_NATIVE = NOT_MEASURED` retained.
