# Issue #123 — `phase_g_closeout_test` Parallel-Debug Flake: Root-Cause Investigation

**Status:** ROOT_CAUSE_PROVEN_AND_FIXED (test-infrastructure false-failure mechanism)
**Baseline master SHA:** `41c392d` (pre-change working tree; issue #123 reported on 2026-08-18)
**Fix branch:** `fix/issue-123-phase-g-closeout-flake`
**Classification:** test-methodology false failure, NOT a production defect. No
Core/Runtime concurrency semantics changed.

---

## 1. Executive summary

`phase_g_closeout_test` failed exactly once in a full parallel Clang Debug gate
(1/180) on 2026-08-18, then passed isolated, on rerun, and in Release — the
signature of a load-amplified test-infrastructure flake, not a product
regression (the working tree at the time contained only doc/comment changes).

The false-failure mechanism is now **causally established and reproduced** at a
13.5% rate (26/192) under the documented CPU-contention recipe (issue #116 §14
/ commit 22de0ee's "3 spinners + concurrent copies on 4 CPUs"), and **eliminated
to 0/192** by the fix. Root cause is two-layer, both test-side:

1. **Baseline-after-release inversion (the reproduced failure):** the TP-G5
   post-resume observation read its progress baseline AFTER
   `resume_threadpool_gate()`. The gate release is the ONLY trigger for the
   terminal publication; when the worker published the terminal (ready epoch
   0→1) before the test thread read its baseline, the baseline captured 1 and
   the observation waited for a second, never-coming advance. Every captured
   failure shows `token=(ready=1,ctrl=1)` — the terminal HAD been published;
   the test's observation window was inverted, not the protocol stalled. Case
   D1's progress observation has the identical latent inversion.
2. **Yield-busy-spin + wall-clock deadlines (the defect class):** all six
   observation helpers (`wait_flag`, `wait_count_at_least`, `wait_token`,
   seam-pause `is_paused` loops, `RunDriver::join_or_fail`, and the TP-G5
   progress spin) used `yield()` loops with 5 s/10 s deadlines. Under parallel
   oversubscription the test thread's own spinning starves the very scheduler
   worker it waits for (issue #86-B's self-inflicted starvation), so scheduler
   latency was misclassified as a protocol violation.

**Fix:** every correctness observation is now a **blocking handshake** — zero
CPU, no wall-clock deadline, no scheduler-latency dependency — and the single
bounded element per case is a fail-closed case-level watchdog (the issue #86-B /
#92 / #101 pattern) that aborts rc 70 only on a genuine no-progress stall,
printing case, phase, gate state, park domain, backend token, outstanding,
backend-ready, prepark count, and pid. The TP-G5 and D1 baselines are read
BEFORE the triggering gate release.

No production `sluice_async` symbol changed (verified: the production archive
carries zero new symbols); all seams are `SLUICE_ASYNC_INTERNAL_TESTING`-guarded.

---

## 2. Symptom and historical evidence

- **Issue #123 (2026-08-18):** one failure of `phase_g_closeout_test` in a
  parallel Clang Debug `xmake test -v` (180 tests). Rerun 180/180, isolated
  pass, Release pass. No case name captured.
- **Issue #116 §14 (2026-08-15):** `phase_g_closeout_tp_g5_close_admission_
  while_parked` failed once in a GitHub CI run — "exit 70 after the test's
  ~5.3 s bounded spin: the gated worker's terminal publication did not land
  within the bound"; reproduced only under forced 4-CPU contention (with-seam
  1/24). Same head re-ran green. Attributed at the time to "pre-existing
  load-amplified flake of the raw-layer construction".
- **Commit 22de0ee (2026-08-17):** documented the same TP-G5 observation as a
  load-amplified CI flake.

These records attribute the TP-G5 observation to a slow publication. The
forensics captured in this investigation show the opposite: the publication
HAPPENED (ready=1) and the test's own baseline was read after it.

---

## 3. Reproduction (baseline)

Baseline = working tree at `41c392d` with no changes.

| Recipe | Result |
| --- | --- |
| Isolated `phase_g_closeout_test`, 20 runs | 20/20 PASS |
| Full parallel Clang Debug `xmake test`, 3 runs | 3×181/181 PASS |
| CPU contention (issue #116 §14 recipe: 3 spinners on CPUs 1–3 + 16 concurrent copies on CPUs 0–3, 12 rounds) | **26/192 FAIL (13.5%)** |
| Failure case (all 26) | `tp-g5-ready-never-published` |
| Forensic signature (all 26) | `token=(ready=1,ctrl=1) outstanding=1 terminate=1` — the terminal was ALREADY published |

Under the same recipe the baseline binary also exhibited extreme self-inflicted
slowdown: round 1 alone (8 copies) took ~26 minutes wall-clock for ~7 ms of
isolated work, because each copy's yield-spins burned the CPU the workers
needed.

## 4. Root cause

### 4.1 Mechanism 1 — baseline-after-release inversion (the reproduced failure)

TP-G5 (pre-fix):

```cpp
sa::resume_threadpool_gate(f.gate);                       // (A) release the read
std::uint64_t progress = ...progress_generation;          // (B) baseline AFTER
while (...progress_generation == progress) { ... 5 s ... }// (C) wait for change
```

The gate release (A) unblocks the worker, which then runs the syscall and
publishes progress (ready 0→1). The baseline (B) races that publication. When
the test thread is delayed (parallel oversubscription), (B) reads 1 and (C)
waits for a second advance that never comes → 5 s false fail-closed. The
captured forensics (`ready=1`) prove the publication won the race.

Case D1's progress observation has the identical structure (baseline read after
`resume_threadpool_gate`), so it carries the same latent inversion; it was
fixed pre-emptively. D2, TP-G1, and the control-epoch observations already read
their baselines before their trigger and are correct apart from the spin.

### 4.2 Mechanism 2 — yield-spin + wall-clock correctness deadlines (the defect class)

Every observation below was a correctness synchronization carried by a
wall-clock deadline. Under oversubscription the test thread's `yield()` loop
starves the worker it waits for — the exact issue #86-B defect — and a 5 s
deadline becomes a false protocol verdict.

| Observation | Lines (pre-fix) | Class | Dependency on scheduler latency |
| --- | --- | --- | --- |
| `wait_flag` (wait_phase_entered / pgate.paused) | 105–112 | correctness sync | yes — yield + 5 s |
| `wait_count_at_least` (prepark / resumed) | 114–121 | correctness sync | yes — yield + 5 s |
| `wait_token` (control/progress epoch) | 123–135 | correctness sync | yes — yield + 5 s |
| Case A/B seam-pause `is_paused` loops | 303–312, 384–394 | correctness sync | yes — yield + 5 s |
| `RunDriver::join_or_fail` | 150–163 | deadlock watchdog | yield + 10 s |
| TP-G5 post-resume progress spin | 750–762 | correctness sync | yes — yield + 5 s (inverted baseline) |

The determinism policy stated "bounded deadlines appear solely as hang
watchdogs", but five of the six observation helpers were correctness
synchronization riding on deadlines.

---

## 5. The fix

### 5.1 Blocking handshakes (zero CPU, no deadline)

- `wait_flag` → `atomic::wait(false)` loop. The wait-phase flag and the
  progress-seam `paused` flag are published with store+notify_all.
- `wait_count_at_least` → `atomic::wait(old)` loop. The prepark counter now
  notifies after its test-only increment (`ReadyWaitSource::wait_for_change`,
  `SLUICE_ASYNC_INTERNAL_TESTING`); the test's own `resumed` counter notifies
  after its fiber increment.
- `wait_token` → blocks on a new test-only epoch observer
  (`ReadyWaitSource::wait_epoch_changed`): a zero-CPU observer whose predicate
  is the ACTUAL epoch pair (single source of truth — no second counter).
  Review-round correction (§12): the observer parks on the SAME
  `mtx_` + `ready_cv_` domain that `interrupt_all()`/`signal_progress()` use;
  the original dedicated observer cv/mutex pair had a lost-wake window.
- Case A/B seam-pause → `stest::wait_paused` (blocking controller cv).
- `RunDriver::join()` → blocking `done.wait(false)` (with `done.notify_all()`
  after the store).
- TP-G5 and D1 progress baselines read BEFORE `resume_threadpool_gate()`.

### 5.2 One fail-closed case-level watchdog (the only bounded element)

Per case: `CloseoutProbe` (case name, phase, progress epoch, gate atomics) +
`CloseoutWatchdog` (30 s budget). The abort trigger is a GENUINE no-progress
stall — the progress epoch frozen for the entire budget (issue #101 model: a
case-total wall-clock deadline is not a liveness oracle). Starvation pauses
progress for seconds and resumes; it never reaches a full-budget freeze. On
abort it prints case, phase, gate state, park domain (via a new non-blocking
`AsyncTestAccess::worker_park_domain_try`, which uses `try_lock` on
`global_mtx_` so the watchdog can never block behind a stalled worker holding
that lock), backend token, outstanding, backend-ready, prepark count, and pid,
then `_Exit(70)`. The watchdog reads only lock-free atomics and non-blocking
(try) reads — it can never deadlock behind the defect it is diagnosing
(issue #86-B / #92 discipline). Review-round correction (§12): the backend
token / outstanding / backend-ready reads are try-locks
(`try_wait_token_for_test` / `try_outstanding_for_test` /
`try_backend_ready_count_for_test`) printing `locked` when the leaf domain is
contended; the blocking `snapshot()`/`outstanding()` reads would have defeated
the watchdog property when the stall involves that domain.

### 5.3 Isolation hardening

`TempFile` now uses a unique per-process/per-instance temp path (pid +
monotonic counter, following the Phase-G forensics pattern), instead of the
fixed `/tmp/sluice_phase_g_closeout.tmp` shared by every concurrent process.
This is isolation hardening, not the root cause (the O_TRUNC collisions cannot
break the closeout protocol — a short read still produces a valid terminal).

## 6. Before/after matrix

| Recipe | Before (41c392d) | After (fix) |
| --- | --- | --- |
| Isolated, 20 runs | 20/20 | 20/20 |
| Full parallel Debug suite, 3 runs | 3×181/181 | 3×181/181 |
| Parallel Debug stress (concurrency 2/4/8) | — | 70/70 |
| **CPU contention, 12×16 = 192 runs** | **26/192 FAIL (13.5%)** | **0/192 FAIL** |
| Contended run wall-time (4 concurrent) | ~26 min (round-1 batch) | ~1 s |
| Release full suite | 181/181 | 181/181 |
| ASan+UBSan full group | — | ALL PASS |
| TSan full group | — | ALL PASS, 0 warnings |
| TSan closeout ×5 | — | 5/5, 0 warnings |
| Mutation (prepark notify removed, 3 s watchdog) | — | rc 70 in ~3.1 s, full forensics; 3/3 under TSan with 0 warnings |

The 26/192 → 0/192 contention result is the same recipe that previously
reproduced the TP-G5 flake (issue #116 §14, commit 22de0ee), now with
case/phase attribution on every failure.

## 7. Production-vs-test-only diff classification

All changes are test-side or `SLUICE_ASYNC_INTERNAL_TESTING`-guarded:

- `tests/phase_g_closeout_test.cpp` — the methodology fix (test only).
- `include/sluice/async/detail/ready_wait_source.hpp` — prepark notify + epoch
  observer channel, both under `SLUICE_ASYNC_INTERNAL_TESTING` (layout cost in
  the internal-testing target accepted, AGENTS.md §15).
- `include/sluice/async/threadpool_backend.hpp` —
  `wait_epoch_changed_for_test` forwarding (guarded).
- `include/sluice/async/scheduler.hpp` — `worker_park_domain_try`
  (guarded `AsyncTestAccess`).
- `src/async/async_io_context.cpp` — `pause_after_wait_source_progress_`
  made bidirectional (notify + blocking resume) like the issue #92
  ThreadPool gates; the whole function is guarded.

**Verification:** `nm -C build/.../debug/libsluice_async.a | grep
wait_epoch_changed|worker_park_domain_try` → 0 matches; the production archive
carries none of the new symbols. No production concurrency semantics changed.

## 8. Mutation evidence

To prove the replacement retains fail-closed sensitivity to a genuinely broken
handshake (requirement: a broken handshake must still fail boundedly):

- Temporarily removed the prepark counter `notify_all()` and shortened the
  watchdog to 3 s.
- The blocking `wait_count_at_least(prepark, N)` then parks forever on the
  value change with no wakeup; the watchdog fires in ~3.1 s, exit 70, printing
  the full forensic state (e.g. `case=case-b-notify-after-arm-before-wait
  phase=observe-repark gate: paused=1 resume=0 exited=0 prepark=2
  park_domain[0]=2(Backend)` — the counter HAD reached 2; only the notify was
  missing).
- 3/3 under TSan with 0 ThreadSanitizer warnings, proving the watchdog's abort
  path reads are race-free.
- Reverted; clean build re-verified (ALL TESTS PASSED).

## 9. Validation (executed)

| Gate | Command | Result |
| --- | --- | --- |
| Debug full | `xmake f -m debug --toolchain=clang -c -y; xmake build -g test; xmake test` | 3×181/181 |
| Release full | `xmake f -m release --toolchain=clang -c -y; ...; xmake test` | 181/181 |
| ASan+UBSan | `xmake f -m asanubsan --toolchain=clang -c -y; ...; xmake run -g test` | ALL PASS |
| TSan | `xmake f -m tsan --toolchain=clang -c -y; ...; xmake run -g test` | ALL PASS, 0 warnings |
| Mechanical/docs | `git diff --check`; `check-doc-links.py`; `verify-architecture-docs.py`; `mechanical-facts.py` | all PASS |

## 10. Remaining risks

- The watchdog budget (30 s) is a deadlock safety net. A pathological
  host-scheduler freeze of a single closeout case for >30 s with zero phase
  progress would abort a correct case. The blocking handshakes make the
  correct-case convergence time independent of the test thread's CPU share, so
  this is far beyond the previously demonstrated failure boundary (the old
  5 s deadlines were the actual correctness verdicts). The budget is
  configurable at one constant.
- `phase_g_closeout_uring_test` (real liburing only, out of the default gate)
  shares the same observation helpers; its UR-G5/D1 `WaitSourceProgressPauseGate`
  resume publisher was migrated to the unified helper in the review round
  (§12). Its yield-spin observation helpers (`wait_flag` deadlines) predate
  this methodology; a follow-up could apply the same blocking-handshake +
  case-watchdog treatment there.

## 12. Review round (PR #128, 2026-08-19)

The draft PR review confirmed the root cause and the methodology direction and
raised three findings on the new infrastructure itself; all three were
verified as real and fixed (plus one additional latent test race the fix
exposed).

### 12.1 Epoch-observer lost wake (review P1)

`ReadyWaitSource::wait_epoch_changed` parked on a dedicated
`observer_mtx_`/`observer_cv_` pair while its predicate read epochs that are
mutated under `mtx_`, and `signal_progress()`/`interrupt_all()` notified
`observer_cv_` without holding `observer_mtx_`. An epoch advance landing
between the observer's predicate check and its park was notified before the
park — the wake was lost and the observer slept until a later epoch change.
The claim "a missed notification cannot lose the observation" only holds when
the predicate state and the cv wait share one synchronization domain; they did
not.

Fix: the observer now parks on the native `mtx_` + `ready_cv_` predicate
domain (the same protocol as the production `wait_for_change`); the dedicated
observer mutex/cv pair and its notify sites were deleted.

### 12.2 Pause-gate resume publishers not migrated (review P1)

`pause_after_wait_source_progress_` blocks in `resume.wait(false)`, but the
C2e D4-RM13 detector and the real-liburing UR-G5/D1 case still published the
resume with a plain `store` — and the seam comment claimed a plain store
releases the waiter. It does not: `atomic::wait` is woken only by a notifying
atomic operation; a store racing the park leaves the consumer parked (the
same store-without-notify defect class the prepark mutation already proved in
§8). The green CI on those paths was scheduler luck, not evidence.

Fix: `WaitSourceProgressPauseGate::resume` is now private
(`AsyncIoContext` is the friend performing the wait), and the only publisher
is `AsyncIoContext::resume_wait_source_progress_gate_for_test` (store +
`notify_all`) — the issue #92 `resume_threadpool_gate` model, enforced
structurally: a direct `gate.resume.store(...)` fails to compile. All three
publishers (closeout D1, UR-G5/D1, C2e) were migrated, and the false comment
was corrected.

### 12.3 Watchdog diagnostic blocked on the diagnosed domain (review P1/P2)

`diagnose_and_abort` read the backend token via the blocking
`snapshot()`, and outstanding/backend-ready via the arena-locked counters —
so a stall involving the `ReadyWaitSource` or `RequestArena` leaf domain would
have stalled the watchdog inside its own diagnostic, defeating the `_Exit(70)`
guarantee. Fix: guarded try-reads (`ReadyWaitSource::try_snapshot`,
`RequestArena::try_accepted_outstanding`/`try_backend_ready_count`,
`ThreadPoolBackend::try_wait_token_for_test`/`try_outstanding_for_test`/
`try_backend_ready_count_for_test`); the diagnostic prints `locked` when a
domain is contended. Watchdog diagnostics prefer less information over losing
the watchdog property.

### 12.4 Exposed: C2e defensive-reset clobber race

Migrating the C2e resume to store+notify exposed a latent race in the
detector itself: the test reset the probe's `paused` flag AFTER releasing the
context pause gate. With the old plain-store resume the participant woke late,
so the reset almost always won; with the deterministic immediate wake the
participant could set the second-pause observation BEFORE the reset, which
then clobbered the very flag the test waited for (`T1 never returned after
resume`, reproducible ~30% isolated, 12/12 under load). Fix: the defensive
reset now runs BEFORE the gate release — while the participant is provably
held in the gate, it cannot touch `ws->paused`, so the reset is race-free.

### 12.5 Review-round evidence

- M1 mutation probe (standalone racer, observer vs one-shot
  `signal_progress`, both threads pinned to 2 contended CPUs): fixed observer
  — 300,000 iterations, zero lost wakes; pre-fix observer domain restored via
  a shadow header — LOST WAKE at iteration 17,230 (epoch advanced and
  `notify_all` fired; the observer never woke).
- M2 structural rule probe: `gate.resume.store(true, ...)` in a test TU fails
  to compile (`'resume' is a private member of ...WaitSourceProgressPauseGate`).
- Contention recipe rerun (§3, 12×16 = 192): 0/192 FAIL.
- C2e stress: D4-RM13 case 100× isolated + 40× contended (2 spinners, 4
  concurrent copies) — 0 failures.
- Full gates rerun after the review fixes: Debug 181/181, Release, ASan+UBSan,
  TSan, and real-liburing `phase_g_closeout_uring_test` (UR-G5/D1) — see §9
  for the executed matrix of this round.

## 13. Files changed

- `tests/phase_g_closeout_test.cpp` — blocking handshakes, TP-G5/D1 baseline
  fixes, per-case watchdog with forensics, unique temp path; review round:
  try-read diagnostics, unified resume helper, observer-domain comment.
- `include/sluice/async/detail/ready_wait_source.hpp` — prepark notify +
  test-only epoch observer on the native wait domain + `try_snapshot`
  (guarded).
- `include/sluice/async/threadpool_backend.hpp` — `wait_epoch_changed_for_test`
  + try-read forwards (guarded).
- `include/sluice/async/detail/request_arena.hpp` — `try_accepted_outstanding`
  / `try_backend_ready_count` (guarded).
- `include/sluice/async/scheduler.hpp` — `worker_park_domain_try` (guarded).
- `include/sluice/async/async_io_context.hpp` — pause gate: private `resume`
  + `resume_wait_source_progress_gate_for_test` (guarded).
- `src/async/async_io_context.cpp` — progress seam made bidirectional
  (guarded); resume-semantics comment corrected.
- `tests/async_io_context_split_wait_c2e_test.cpp` — unified resume helper;
  pre-release defensive reset (§12.4).
- `tests/phase_g_closeout_uring_test.cpp` — UR-G5/D1 unified resume helper.
- This document.
