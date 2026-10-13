# F2 W-03 external-loop stall — root cause and closure

Status: **CLOSED**. Resolves `blocker-w03-stall.md` (OPEN at the captured
implementation head `996f434f`). Root cause proven with a deterministic
reproducer; the failure domain is the application/test-harness handoff, not a
library notification defect. The production diff is empty; the corrective diff
is test-only (`tests/threadpool_external_loop_test.cpp`, commits `359155e6`,
`b63a532e` and `25734314` — the last consumes the handoff eventfd write's
return value, which Ubuntu glibc's `warn_unused_result` rejected under CI's
`-Werror`). The corrected implementation head is `25734314f70401472067250e68a1f9227093b7eb`.

## The three facts that were being conflated

1. **Request accepted and published by RequestCore.**
   `ThreadPoolBackend::submit_request` publishes a zero-op inline:
   `core_->complete_publication` (`src/async/threadpool_backend.cpp:333`)
   makes the result visible through the core, and any later `Request::ready()`
   is a pure core lookup (`include/sluice/async/request.hpp:92`).
2. **Progress notification generated.** `publish_zero_op_inline` sets the owed
   delivery record and calls `signal_ready_progress()` (same file, lines
   336–342) → `ProgressSource::signal` writes the context eventfd.
3. **Request handle transferred to the application's `SharedRequest`.** This
   happens only after `AsyncIoContext::submit_read` returns
   (`src/async/async_io_context.cpp:252` constructs the handle; the test's
   `SharedRequest::submit_zero_op` stores it under its own mutex afterwards).

Facts 1 and 2 happen inside `submit_read`, while it holds `access_mtx_`.
Fact 3 happens after the call returns, under a different mutex, and **no
notification accompanies it**. A wake therefore implies facts 1+2, never
fact 3.

## Execution DAG (before the fix)

```text
Helper thread (SharedRequest::submit_zero_op)      Host thread (W-03 late loop)

ctx.submit_read(zero-op)
  [access_mtx_]
  core_->accept                        (accept)
  publish_zero_op_inline:
    complete_publication .............. fact 1: published
    event_owed = true
    signal_ready_progress ............. fact 2: eventfd written
  [unlock]
  Request{core, backend, key}          poll(2) wakes on the eventfd
  return Result<Request>                 |
  ...unguarded window...                 if (late.ready()) ...   -- false
                                         acknowledge_progress_notification
                                           (drains the eventfd)
                                         poll_progress pass
                                           consumes event_owed,
                                           completed=1, control released
                                         if (late.ready()) ...   -- false
mtx.lock; request = std::move(...)     poll(2) parks on the eventfd
[no notification is generated]          ...no wake ever arrives...
                                       budget expires -> return false
```

The loop in `external_poll_loop_threadpool_w03` checked `late.ready()` only
after `wait_readable` returned, and `wait_readable` returned only on a library
wake or budget expiry. Once the host consumed the zero-op's only notification
while the helper was between signal and store, the scenario state was complete
(fact 1 held, so `late.ready()` became true at the store) but nothing would
ever wake the parked host. Under two-core starvation with ten concurrent suite
copies, the helper is regularly preempted inside that window — contended
`access_mtx_` handoff to the host makes the host's drain win the race — which
is why the failure is load-conditional (0/72 idle runs, 10/40 loaded at the
captured head).

The failing CI run at the branch head (`69686d2f`, run 38379190713) is the
same interleaving at CI scale: `threadpool_external_loop_test` exit -1 after
5.16 s (the 5 s wait budget), aborting in the context destructor — the early
`return false` never retires the external notification registration, so
`~AsyncIoContext` fail-fasts on the live progress binding.

## Why the baseline does not have the window

The baseline's cross-thread submission used the compat overload
`submit_read(op, Completion<T>&)`: the application-visible carrier existed
before submission and was filled **inside** the backend call, before the
signal (`record.publish(...)` precedes `signal_ready_progress()` in the
baseline `publish_zero_op_inline`). In that shape a wake implied carrier
readiness. The baseline test file documented exactly the property that the
Request shape loses: "Two cross-thread submission shapes observe carrier
readiness before the submitter hands the Request back; the adopted binding
cannot express that (its wake can be consumed before the handoff)". F2
correctly retired the dual-state carrier; the port of the scenario to
`SharedRequest` kept the old loop protocol (wait for an I/O wake to learn
about application state) — that protocol, not the F2 authority contraction,
is the defect.

## Deterministic reproduction (commit `359155e6`)

`late_handoff_wake_is_application_owned` in
`tests/threadpool_external_loop_test.cpp` controls the boundary with a
`HandoffGate`: the helper publishes the submitted `RequestId`, then parks
between the submit-read return and the mutex store until the host releases
it. No sleeps; every step is a release/acquire or poll(2) fact.

Phase 1 — causal witness (forced interleaving, forced schedule):

```text
w03-handoff phase1: core_published=1 signalled=1 delivered=1
  visible_before_release=0 quiescent_after_drive=1 library_park_expired=1
  visible_after_handoff=1 handoff_generates_no_signal=1 epoch_delta=1
```

At the pause the core is already published (`ctx.lookup(id)` == ready), the
eventfd is readable (exactly one progress epoch was generated), the host's
acknowledge+drive consumes the one owed delivery event, the handle is not
observable, the fd is quiescent, a host parked on the library fd alone times
out — and after the release the handle becomes ready with **no** further
signal. That is the W-03 stall, forced: the library owes nothing further at
that point (delivered=1, control released, fd quiescent), so no library fix
could remove the park expiry.

Phase 2 — corrected protocol: with an application-owned handoff eventfd
written after the store and monitored alongside the notification fd
(`ExternalLoopHost::wait_readable_or_handoff`), the parked host is released
by the handoff wake alone (`wake_source=handoff`, notification fd quiescent
for the whole budget) and the result is correct.

Phase 3 — missing-notification negative: the helper completes the handoff
without announcing it (the pre-fix protocol); the corrected oracle detects
the missing edge as a bounded wait expiry (`wake_source=none`) while the
handle is ready but unannounced — a detection, never a hang.

## Negative and positive witnesses

- **Pre-fix negative** (`raw/prefix-mutation-red.log`): with phase 2
  temporarily forced through the pre-fix single-fd wait (the exact park shape
  of the pre-fix loop), the suite fails `late_handoff_wake_is_application_owned`
  in a bounded 2 s (`wake_source=0 completed=0`, exit 1). Mutation reverted
  immediately after the run; not part of any commit.
- **Post-fix positive** (`raw/postfix-green.log`, `raw/postfix-suite.log`):
  all 20 scenarios pass, including the corrected
  `external_poll_loop_threadpool_w03` and the untouched R14
  wake-reassertion oracle.

## Classification

**A — application/test-harness handoff defect.** The zero-op was published
and correctly signalled; the host consumed the notification before the
application-level handle transfer, and no further progress signal was owed by
the library. Options B/C were excluded by the phase-1 evidence (nothing is
lost, mis-acknowledged or undelivered: `delivered=1`, `epoch_delta=1`,
`quiescent_after_drive=1`, and the parked-host expiry is observed with the
helper paused — no backend worker, dispatch or publication edge is involved).

## Corrective diff (test-only)

- `SharedRequest` gains an optional handoff eventfd written **after** the
  mutex store, and an optional `HandoffGate` pause for the deterministic
  schedule.
- `ExternalLoopHost` gains `enable_handoff_notification` /
  `wait_readable_or_handoff` / `drain_handoff_notification` /
  `close_handoff_notification`; the single-fd `wait_readable` and every other
  scenario are unchanged.
- `external_poll_loop_threadpool_w03`: the late loop now waits on both fds,
  drains the handoff notification, and the helper announces the handoff. All
  original assertions are retained (real poll(2), multiple outstanding
  operations, host-owned progress, coalesced completions, sticky control
  wake, late cross-thread submission, result visibility, progress after
  acknowledgement, quiescent fd after retirement, detach/shutdown ownership);
  R14's restored wake-reassertion oracle is untouched.

No production file changed: `git diff --stat 996f434f..25734314` touches
`tests/threadpool_external_loop_test.cpp` only. The application-level
notification is written after the handle is safely observable (store under the
mutex, then write), restoring at the application level the store-before-signal
edge the baseline carrier had structurally.

Failure-path disclosure: an early `return false` inside the deterministic
test's forced-pause window leaves the helper parked until the suite's 120 s
alarm, and an early return in the corrected W-03 loop still aborts through the
context destructor instead of printing `FAIL` — both are the file's uniform
pre-existing failure style, bounded by the alarm, and reachable only when the
oracle is already failing for another reason.

## Differential stress (raw/finalhead-campaign/, plus raw/ for the intermediate campaign)

`taskset -c 0,1`, ten concurrent suite copies per round, four primary rounds
per tree plus four additional interleaved rounds per tree (80 runs per tree),
debug + real-liburing builds, otherwise idle 20-core host (environment in
`raw/environment.txt`):

| Tree | Runs | Failures | Failure mode |
|---|---|---|---|
| baseline `0ed16588` | 80 | 0 | — |
| pre-fix F2 `996f434f` | 80 | 17 | exit 134 (SIGABRT) after ≈5.2 s: the 5 s wait budget expires, the early return leaves a live binding, context-destructor fail-fast (`raw/prefix-stall-r2-c2.log`) |
| corrected `25734314` | 80 | 0 | every run completes all 20 scenarios under full contention |

The pre-fix failure rate (21.25%) reproduces the recorded blocker measurement
(10/40 = 25% at four rounds; an intermediate campaign at the
pre-portability-fix corrected head `b63a532e` measured 16.25% pre-fix and 0
corrected the same way); every failure is the same interleaving. Zero
corrected failures under the same schedule is complementary evidence only —
the deterministic phase-1 reproduction is the causal oracle, and zero
failures does not prove zero probability of failure.

## Gates at the corrected head

Recorded in `gates.json`, re-collected at the corrected implementation head
`25734314f70401472067250e68a1f9227093b7eb` (GitHub CI run `38387419975`,
success). The six-profile matrix (Debug/Release × no-ring/real-ring, native
Clang ASan+UBSan 18/18, native Clang TSan 17/18 with the recorded
`request_scope_test` link exclusion) passes every runnable target, including
W-01 (`direct_w01_consumer_probe`), W-02/W-03 suites, the app regressions and
the external-loop suite with the new deterministic test; the durable capture
subset is persisted under `matrix/` here. One intermittent surfaced during
the first matrix re-run and is recorded, not hidden:
`backend_conformance_e1_threadpool_test` flaked once under ASan with a
`stack-use-after-scope` in the E1 test's own `GateGuard` teardown race —
re-measured at 2/12 (corrected head) and 4/12 (captured pre-fix head) on
identical binaries, so it predates this closure; see
`../known-preexisting-gaps.md` §3. The B-01 package capture and installed
probes were re-run at the same head
(`F2_B01_V5_CANDIDATE_FRESH_PACKAGE_PASS 0ed16588… 25734314…`, persisted
under `b01-v6/` and `probes/`). The exact changed-input comparison for the
package gate: `git diff 996f434f 25734314 -- include/ src/ apps/ xmake/` is
empty — every production/package input is byte-identical, and the
re-capture's inventories match the captured head's (35/35 objects and 56
headers before, 32/30 objects and the identical 49-header installed list
after). The package tarball hashes themselves differ from the captured
head's run because the tarballs embed build artifacts that are not
bit-reproducible across capture runs; the changed-input proof is the empty
production diff plus the inventory equality, not a tar hash. The 64-mutant
campaign is likewise retained from the captured head: every mutant mutates
production code or production-facing seams, and the corrective diff changed
no production input, so the recorded kill set cannot have shrunk (the new
deterministic test can only add kills). The TLA+ gate is retained unchanged
from the captured head: no formal input changed (`formal/` is untouched by
the corrective diff), and `rcore-live` remains INCOMPLETE, never recorded as
a pass.

## Remaining merge blockers

- The installed-surface API break still awaits the owner's separate decision
  (`installed-surface-risk-disposition.md`); PR #516 stays Draft.
- The TLA `rcore-live` configuration remains outstanding (INCOMPLETE).
- Pre-existing gaps unrelated to this closure are unchanged
  (`known-preexisting-gaps.md`).
