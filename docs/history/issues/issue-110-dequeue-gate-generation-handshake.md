# Issue #110 — Scheme-B Pause-Gate Cross-Iteration Protocol Hole: Root-Cause Investigation

> **Archived 2026-08-25 (issue #167 Step 5).** Moved from
> `docs/investigations/`; classification at move: CLOSED-HISTORY (fixed
> test-infrastructure defect; production impact NONE). Body preserved
> as-written; see `docs/history/README.md`.

**Status:** TEST_PROTOCOL_ROOT_CAUSE_FIXED
**Baseline master SHA:** `28f30d7e708544f45c608798412295c9587dad86`
**Fix branch:** `fix/issue-110-test-gate-generation`
**Classification:** test-infrastructure synchronization defect (SLUICE_ASYNC_INTERNAL_TESTING only); production impact NONE

---

## 1. Executive summary

`BeforeWorkerDequeuePauseGate`'s boolean protocol published `exited == true`
at the wrong causal boundary: the moment the worker left the pause gate, NOT
the moment it consumed (or observed empty) the current iteration's dispatch
entry. In `tp_cancel_races_worker_terminal_exactly_one`, a cancel-won
iteration needs no worker execution to drain (the canceled terminal is reaped
by `poll()`), so after observing `exited == true` the test rearmed the gate
and submitted iteration N+1 while the descheduled iteration-N worker
continuation was still between gate exit and `pop_front()`. That continuation
then popped N+1's freshly pushed entry without visiting the re-armed gate,
ran its syscall, and parked in `work_cv_` — after which no worker could ever
publish `paused == true` for N+1. The main thread blocked in `wait_paused`
forever; the case watchdog eventually aborted with the #110 signature
(`phase=wait_paused`, `gate: paused=0 resume=0 exited=1`).

**Repair (test-only, guarded):** the gate now carries a monotonic
generation-scoped handshake —

```text
test:  arm(N)                     before submitting iteration N
worker: pauses                    publishes paused_at >= N
test:  observes paused_at >= N    races cancel vs dequeue
test:  resume(N)
worker: pop_front decision made   publishes acked_at >= N   (the ACK)
test:  waits acked_at >= N        ONLY THEN arms N+1
```

The ACK is published after the pausing cycle's `pop_front` decision. Once a
worker continuation has popped (or observed an empty ring), its only path to
consuming another entry re-enters `work_cv_` wait -> gate check -> pop, so
after ACK(N) it cannot consume N+1's entry without N+1's gate observation.
The single-visit boolean trio is retained unchanged for the single-shot
consumers (death test, C2c/C2d/C2e windows, cases B/E/G); arming a
generation selects the new path. No production semantics changed.

## 2. Reproduction (pre-fix, baseline master)

- Linux Clang Debug, baseline `28f30d7`, filter
  `SLUICE_TEST_FILTER=tp_cancel_races_worker_terminal_exactly_one`:
- 20 parallel processes: 20/20 PASS (window too narrow at that load).
- 40 parallel processes (8-core WSL2 host): **5/40 abort (rc=134)**.
- Captured diagnostic (`/tmp/sb_c_1.log`):

```text
ThreadPool test watchdog: GENUINE NO-PROGRESS STALL ...
  case=tp_cancel_races_worker_terminal_exactly_one
  phase=wait_paused
  iteration=16
  gate: paused=0 resume=0 exited=1
```

`exited=1` with `paused=0` is the hole's fingerprint: the gate was rearmed
for iteration 17, but the worker that would have served it had already
consumed (or was about to consume) the entry through the pre-pop continuation
of iteration 16.

## 3. Root cause (flag semantics audit)

| flag | publisher | program point | proves | does NOT prove |
|------|-----------|---------------|--------|----------------|
| `paused` | worker | before `resume.wait` | worker reached the pre-dequeue pause for some iteration | which iteration |
| `resume` | test | resume decision | test released the pause | which iteration was released |
| `exited` | worker | **immediately after** `resume.wait` returns | worker left the pause gate | that the visit's dispatch entry was consumed / ring observed empty |

`exited` fires before `lk.lock(); pop_front()`. Between the two the worker
can be descheduled arbitrarily long, and a cancel-won iteration's drain does
not require the worker — so the test's `exited -> rearm -> submit(N+1)`
sequence races the continuation's pending pop. `exited` also carries no
iteration identity, so nothing distinguished iteration N's exit from N+1's
gate state even without descheduling (boolean ABA under rearm).

## 4. The fix

### 4.1 Handshake fields (`BeforeWorkerDequeuePauseGate`, guarded)

`std::atomic<std::uint64_t> armed / paused_at / resumed_at / acked_at` —
monotonic for the life of the gate object, never reset (no ABA). The test is
the sole writer of `armed`/`resumed_at`; the single worker is the sole writer
of `paused_at`/`acked_at`. Publications are monotonic max (C++20 has no
`fetch_max` — P0493 is C++26 — so a `compare_exchange_weak` loop is used) and
every publication a waiter can block on is paired with `notify_all`;
consumers use predicate loops because `atomic::wait` permits spurious
unblocking.

### 4.2 ACK linearization point (`worker_loop`, guarded seam)

```text
lk.unlock()
gen = wait_before_dequeue_pause_()      // legacy bool visit returns 0
wait_post_resume_pre_pop_hold_()        // #110 regression seam (below)
lk.lock()
if (stopping_ && empty) { ack(gen); return; }
popped = dispatch_.pop_front(h)
ack(gen)                                // THE ACK: after the consume decision
if (!popped) continue
mark_running ...                        // execution continues unchanged
```

Why an N continuation cannot consume N+1 after ACK(N): the ACK is published
strictly after this cycle's `pop_front` decision; the continuation's next
opportunity to consume is the next loop iteration, whose order is
`work_cv_.wait -> gate check -> pop_front`. The gate check observes
`armed >= N+1` (armed before the N+1 submit), so the N+1 entry cannot be
popped without pausing for N+1 first.

### 4.3 Race-loop migration

`tp_cancel_races_worker_terminal_exactly_one` iterations now run
`arm(N) -> submit -> wait paused_at>=N -> [barrier] cancel ‖ resume(N) ->
join canceler -> wait acked_at>=N` — no boolean rearm step exists anymore.
Every original assertion is preserved (exactly-one publication, one ready
Completion, verbatim real result / canceled code, `canceled_ops` and
`syscall_count` tallies, `outstanding == 0`, `arena_slot_in_use == 0` after
reset, winner totals == 64).

### 4.4 Deterministic regression + hold seam

`PostResumePrePopHoldGate` (new single-visit test seam) holds the worker in
the exact post-resume/pre-pop window — the point where the old protocol
already showed `exited == true`. `tp_dequeue_gate_generation_blocks_cross_iteration_theft`:

1. arm(1), submit, wait paused_at>=1, cancel (cancel-wins), resume(1);
2. worker parks in the hold → **deterministic**: `acked_at == 0` while
   provably pre-pop (single worker parked; only the test releases the hold);
3. a helper thread blocked in the gen-1 ACK wait is provably unfinished
   before the hold release (causal, not timed; this libstdc++ has
   `atomic::wait` but no `atomic::wait_for`);
4. release the hold → pop (empty) → ACK(1) → helper completes;
5. arm(2), submit → `paused_at >= 2` reached **with the gen-2 entry still on
   the dispatch ring** (`dispatch_size_for_test() == 1`, `syscall_count == 0`)
   — the gen-1 continuation did not steal it; pre-fix this wait stalled
   forever;
6. resume(2) → ordinary worker winner → real 1-byte result verbatim, exactly
   one publication, tallies intact.

No sleep, no timeout, no yield loop anywhere in the case.

## 5. Evidence matrix (actual runs, this branch)

| gate | command | result |
|------|---------|--------|
| Baseline (pre-fix, master `28f30d7`) | `xmake test -v` (Debug) | 167/167 PASS |
| Pre-fix hostile stress | 20 × parallel filtered race case | 20/20 PASS (window too narrow at that load) |
| Pre-fix hostile stress | 40 × parallel filtered race case (8-core WSL2) | **5/40 STALL abort (SIGABRT)** |
| Post-fix hostile stress | 3 rounds × 40 × parallel filtered race case | **120/120 PASS** |
| Clang Debug full | `xmake test -v` | 167/167 PASS |
| Clang Release full | `xmake test -v` | 167/167 PASS |
| TSan full | `xmake f -m tsan … && xmake test -v` | 167/167 PASS, 0 ThreadSanitizer reports (both modified race cases ran) |
| ASan+UBSan full | `xmake f -m asanubsan … && xmake test -v` | 167/167 PASS, 0 sanitizer reports |
| Docs / mechanical gates | `scripts/gates/pre-push.sh` (links, architecture docs, mechanical facts, whitespace) | ALL CHECKS PASSED |

## 6. Production impact

NONE — mechanically proven, not asserted: building the PRODUCTION
`sluice_async` (Clang Release, no `SLUICE_ASYNC_INTERNAL_TESTING`) from
baseline `28f30d7` and from this branch yields a **byte-identical
`threadpool_backend.cpp.o` (identical SHA-256) and byte-identical members
for all 28 objects** of `libsluice_async.a` (the whole-archive SHA differs
only by `ar` timestamp metadata — the untouched `sluice_core` exhibits the
same whole-archive difference). Every changed line in `src/` / `include/`
sits inside `#if defined(SLUICE_ASYNC_INTERNAL_TESTING)`; the production
variant of `worker_loop` keeps the identical single
`if (!dispatch_.pop_front(h)) continue;` statement. The single-visit boolean
gates used by the death test and the C2c/C2d/C2e windows are byte-identical
in behavior; those suites are unmigrated and still pass.
