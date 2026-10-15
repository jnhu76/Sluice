# F2 E1 pause-gate teardown lifetime — correction receipt

Status: **CORRECTED**. Closes `../known-preexisting-gaps.md` §3 (ASan
`stack-use-after-scope` flakes in `backend_conformance_e1_threadpool_test`,
measured 2/12 at the corrected W-03 head `25734314` and 4/12 at the captured
head `996f434f` on identical binaries). Raw logs: `raw/red-unfixed-guard.log`
(8/8 deterministic FAIL at the unfixed guard) and
`raw/green-fixed-guard.log` (all 19 scenarios pass with the fix).

## Defect

`GateGuard` in `tests/backend_conformance_e1_test.cpp` performed only

```cpp
gate.resume.store(true, std::memory_order_release);
gate.resume.notify_all();
```

The parked party's seam protocol is `exited=false; paused=true; resume.wait;
exited=true; notify` — after the guard's resume it still owes the
`exited=true` acknowledgment, and the scenario families then cleared the gate
pointer and returned, destroying stack gate storage. Two windows follow:

1. **Current-pass exit**: the woken worker's `resume.wait` state and its
   `exited` store touch the gate after the scenario's teardown completed —
   the observed ASan read inside
   `ThreadPoolBackend::wait_unclaimed_retirement_pause_`
   (`src/async/threadpool_backend.cpp:652`).
2. **Load-before-registration**: a worker that loaded the gate pointer before
   the scenario's `set_..._gate(nullptr)` but had not yet registered
   (`exited=false; paused=true`) reaches the object after detach and after
   destruction. No wait protocol on the gate can close this window: the
   unsynchronized raw-pointer load precedes any registration the test could
   observe, and `exited` starts `true`, so waiting for it cannot distinguish
   "no pass" from "pass about to register".

Both windows require gate storage to die while a backend worker may still
execute the seam; worker-side gates were declared after the context, so they
died *before* the backend joined its workers.

## Correction (test-only)

1. `GateGuard` now waits out the acknowledgment:
   `resume=true; notify_all; wait exited==true` (atomic wait/notify, no
   polling). The worker stores `exited` unconditionally after its resume
   wait, so the wait is deadlock-free; never-entered gates start `exited`
   and return immediately.
2. Safe ownership ordering: every worker-side pause gate
   (`WorkerClaimedPauseGate`, `WorkerUnclaimedRetirementPauseGate`) is now
   declared *before* the backend/context it parks, so its storage dies only
   after `~AsyncIoContext` joined the workers. A stale pointer load after
   detach can then only ever touch live storage, and the guard's
   left-`true` resume latch makes any such late pass return from
   `resume.wait` immediately instead of parking — memory-safe and
   progress-safe with no additional mechanism.

No production file changed: the defect and its correction live in the E1
test's own gate lifecycle. The ThreadPool pause seams and their call sites
are untouched, and every original semantic assertion is retained.

## Deterministic witness

`gate_teardown_retires_registered_passes_before_detach`
(`tests/backend_conformance_e1_test.cpp`, ThreadPool variant):

- Two cancel-won stale entries are staged through the accepted-pre-dispatch
  gate (the existing two-pass pattern).
- The worker parks in its retirement seam on entry #1 (entry #2 queued).
- The pass is released through the guard and the worker is pinned *between*
  the first pass's exit and entry #2's dequeue by arming the before-dequeue
  seam — the retirement latch is re-armed with the worker provably outside
  the gate, so the pin is a handshake, not a sleep.
- The worker dequeues entry #2 and registers a **second pass** on the
  still-armed retirement gate — exactly the load-before-registration shape —
  observed as `exited==false` while parked on the re-armed latch.
- The scenario-family teardown then runs: `GateGuard` + pointer detach. The
  contract assertion: no registered pass may remain when the gate's storage
  may die (`exited==true` at detach).

RED (unfixed guard, this worktree before the fix): the guard returned after
resume+notify without the acknowledgment; the registered second pass was
still parked, and the assertion failed **8/8 identical runs** (the main
thread's clear-and-read cannot lose to the worker's futex wake and store),
with every other scenario passing.

GREEN (fixed guard): the guard waits out the pass; the assertion holds and
all 19 scenarios pass, 8/8 reruns.

## Sanitizer evidence (fixed guard)

Native Clang ASan+UBSan (`--mode=asanubsan --liburing=n`, the recorded
sanitizer scope), `backend_conformance_e1_threadpool_test`:

- Idle host: 12/12 runs clean, zero sanitizer reports (recorded pre-fix
  baseline: 2/12 failures at `25734314`).
- Two-CPU contention (`taskset -c 0,1`, 12 concurrent suite copies): 12/12
  clean, zero sanitizer reports.

Sibling suites under the same sanitizer profile:
`threadpool_core_cutover_test`, `threadpool_progress_race_test`,
`threadpool_external_loop_test`, `shutdown_lifecycle_test`,
`request_core_protocol_test`, `request_core_publication_test`,
`public_request_test` all PASS. Both E1 variants under release + real
liburing (`backend_conformance_e1_threadpool_test`,
`backend_conformance_e1_uring_test`) and `uring_progress_race_test`,
`threadpool_external_loop_test` PASS with the fix.

## Gates

Re-collected at the final implementation head in the closeout matrix
(`../gates.json`); see the PR closeout receipt.
