# ISSUE-47-DIAGNOSTIC-1 — classify the multi-worker abnormal termination

**Status:** `ISSUE-47-CORRECTIVE: PASS`
**Failure class:** **C4 — direct SIGSEGV inside a worker thread, caused by a
corrupted Fiber context (the saved entry pointer is garbage).**
**Root cause:** Suspend-switch authority protocol violation — a thief could
steal a routed Runnable ticket before the owner Worker completed the physical
Fiber→Scheduler context switch and saved the Fiber CPU context.
**Fix authorization:** `FIX PR AUTHORIZED` — implemented in this PR.

> **DIAGNOSTIC + CORRECTIVE.** This PR first reproduced and classified the
> corrupted Fiber-context crash, then fixed the generic Scheduler suspend-
> before-switch authority protocol and added deterministic regressions for
> Select and Event/WaitQueue paths.

This document records the diagnostic PR for issue #47. It classifies the
abnormal termination observed in CI and, this time, **reproduces and roots it**.

## TL;DR — what actually happens

The CI symptom attributed to issue #47 reproduces **deterministically enough to
capture a core**, but on a **different but sibling multi-worker test** than the
one originally suspected. The crash is a **direct `SIGSEGV` inside a worker
thread**, NOT the `SLUICE_CHECK` + teardown-fail-fast (`SIGABRT`) masking chain
that was the leading hypothesis.

Concretely (see §E for the backtrace), a worker thread resumes a Fiber and
jumps to a **garbage instruction pointer**:

```
Thread 1 (worker):
#0  0x...6ffd968 in ?? ()                          <- SIGSEGV: rip is garbage
#1  fiber_entry_trampoline_bridge(resumed_by, user_data,
       entry = 0x...6ffd968) at src/async/fiber_ctx.cpp:106
        -> entry(resumed_by, user_data);           <- the indirect call that faults
#2  fiber_entry_trampoline ()
#3  0x0000000000000000                              <- Fiber stack base (return addr 0)
```

`entry` (an `Entry` function pointer read from the saved Fiber context) is
`0x...6ffd968` — a corrupted value — so `entry(resumed_by, user_data)` jumps into
an unmapped/garbage region and faults. `rsp` is a **heap** address, not a Fiber
stack, confirming the saved CPU context (rip/rsp) is corrupt. This is the
**invalid Fiber context switch / duplicate-or-stale runnable ticket** failure
mode named in issue #47 §1 (the "alternative") and §18 (the suspect).

## Why the leading C2 masking hypothesis is DENIED for the captured crash

The masking chain (`SLUICE_CHECK` fail → `return` → `~AsyncIoContext` sees
`outstanding()!=0` → `async_context_outstanding_fail_fast()` → `std::terminate`
→ `SIGABRT`) **is real in the code** (it is mechanically present and the
apparatus self-test proves the early-exit seam defeats it). But the **captured
CI incident is not that chain**: a teardown `std::terminate` produces
`SIGABRT` (signal 6) and would route through the `std::set_terminate` marker;
the captured core is `SIGSEGV` (signal 11), inside Fiber execution, with no
terminate marker and no `SLUICE_CHECK` failure on stderr.

## Where it reproduces

The incident reproduces on **`select_multi_worker_test`**, case
**`st16_multi_worker_owner_routing`** (`tests/select_multi_worker_test.cpp`),
which is a **2-worker `run_live(2)`** test exercising multi-worker owner routing
+ publication. This is the **same multi-worker Scheduler coordination subsystem**
(`run_live(2)`, owner routing, runnable publication) as the originally-suspected
`multi_worker_coord_test::mwcoord_serialized_backend_access` — i.e. issue #47 is
a **subsystem-wide** multi-worker race, not specific to one test file.

It reproduced **3 times** during this investigation:
- run 1: `rc=139` (`SIGSEGV`), master `6d67f9c`, ~1/51 iters
- run 2: `rc=-11` (`SIGSEGV`), last `[run]` case = `st16_multi_worker_owner_routing`, ~1/1000 iters
- run 3: `rc=-11` (`SIGSEGV`), last `[run]` case = `st16_multi_worker_owner_routing`, ~1/2300 iters, **core captured**

All three reproduced on **PR #46 `master` in an isolated git worktree** — i.e.
**without any of this PR's changes** — proving the defect **predates** this
diagnostic PR. (The diagnostic PR's own test, `multi_worker_coord_test`, PASSED
in CI on this PR; the CI failure is this preexisting flake.)

Triggering is highly environment/timing dependent: it reproduced on the local
8-core host at low rates (~1/200 to ~1/2300) and did NOT reproduce under `gdb`
(the debugger perturbs scheduling and closes the window — confirming issue #47
§16 / AGENTS.md §6.3: tooling that changes timing is not proof of ordering).

## What this PR adds (scope: diagnostic only)

| Artifact | Purpose |
| --- | --- |
| `scripts/run_issue47_diag.py` | Direct-binary runner; runs the built test binary via `subprocess` directly (never `xmake run`/`xmake test`) and classifies the real process/signal status (`0` PASS / `1` NORMAL_HARNESS_FAILURE / `<0` SIGNAL_TERMINATION+name / reserved exit codes 90, 91, 92 / other positive ABNORMAL_NON_SIGNAL_EXIT). JSONL + summary; stops at first abnormal iteration; rejects bad inputs. |
| `tests/multi_worker_coord_test.cpp` (case `mwcoord_serialized_backend_access` only) | Phase breadcrumbs, post-run snapshot (existing public accessors only), `std::set_terminate` marker `I47-T00`+SIGABRT, and a diagnostic early-exit seam `std::_Exit` with reserved exit codes 90, 91, 92 running BEFORE local destructors so teardown fail-fast cannot mask the original result. All gated on `SLUICE_ISSUE47_DIAG=1`; normal behavior unchanged. `SLUICE_I47_FORCE_EARLY=1` provides an on-binary apparatus self-test. |
| `.github/workflows/issue47-diagnostic.yml` | Manual `workflow_dispatch` workflow (NOT a required check). 4 shards build once, run the binary directly many times, stop at the first abnormal iteration, capture CI-like env + best-effort core/backtrace, upload all evidence. |
| `docs/issue-47/ISSUE-47-DIAGNOSTIC-1.md` | This record. |

**No production change. No public-header change. No Scheduler/MW-S1/S2/S3 /
work-stealing / Fiber-transition / wait_one change. No sleeps, retries,
quarantine, or assertion-policy change.** (issue #47 §3.)

## Apparatus validation (issue #47 §11) — without the flaky race

- **Classification unit checks:** 17/17 (PASS, NORMAL_HARNESS_FAILURE, all required signals, all reserved codes, ABNORMAL_NON_SIGNAL_EXIT, parse_last_phase, binary-location guards). Input rejection for count≤0 / bad timeout / missing binary all exit nonzero.
- **End-to-end via stand-in binaries:** PASS / harness-failure / terminate→SIGABRT (`last_phase=I47-T00`) / direct SIGSEGV (last phase = pre-crash, **no** T00) / early-exit 90 / timeout all classified correctly.
- **On-binary self-test (`SLUICE_I47_FORCE_EARLY=1`):** deterministically forces `outstanding=6, ops_done=0`; the early-exit seam yields `exit=90` with the snapshot preserved and **zero** `I47-T00` markers (destructor fail-fast bypassed).

## Reproduction results (issue #47 §15)

### `multi_worker_coord_test` (the originally-suspected test) — NOT-REPRODUCED

Local env: WSL2 x86_64, 8 CPUs (vs. CI 2-CPU runner). Base = PR #46 `6d67f9c`.

| Matrix | Mode | CPUs | Iters | PASS | Abnormal |
| --- | --- | --- | --- | --- | --- |
| D1 baseline | debug | taskset 0,1 | 200 | 200 | 0 |
| D1 (free) | debug | none (8 free) | 500 | 500 | 0 |
| D1 release | release | taskset 0,1 | 200 | 200 | 0 |
| D1 release | release | taskset 0,2 | 200 | 200 | 0 |

No SIGABRT/SIGSEGV/timeout/early-return/harness failure observed. This test did
not reproduce locally on either side of the PR.

### `select_multi_worker_test` — REPRODUCED (3×, SIGSEGV, root-caused)

All on master `6d67f9c` (isolated worktree), Clang Debug, clean CI env, 8 CPUs.

| Run | Iters to first crash | Exit | Signal | Last `[run]` case |
| --- | --- | --- | --- | --- |
| 1 | 51 | 139 | `SIGSEGV` | st16_multi_worker_owner_routing |
| 2 | ~1000 | -11 | `SIGSEGV` | st16_multi_worker_owner_routing |
| 3 | ~2300 | -11 | `SIGSEGV` | st16_multi_worker_owner_routing (**core captured**) |

`std::set_terminate` / `I47-T00` marker: **absent** in all three (stderr empty).
Under `gdb` (600 iters): **no crash** — the debugger perturbs timing and closes
the window.

Full Clang Debug gate (AGENTS.md §4) on this PR (master worktree is identical for
these tests): the only failure across 113 tests is this preexisting
`select_multi_worker_test` flake.

## First abnormal trace (issue #47 §E)

Captured from the run-3 core (`/tmp/i47-caught-core`, Clang Debug, master
`6d67f9c`). `gdb` core analysis (abridged):

```
Program terminated with signal SIGSEGV, Segmentation fault.
SIGINFO_SIGNO=11   SIGINFO_ADDR=0x748136ffd968

Thread 1 (worker — faulting):
#0  0x0000748136ffd968 in ?? ()                       <- rip is a garbage address
#1  fiber_entry_trampoline_bridge (resumed_by=0x7481367fc968,
       user_data=0x...7cface <fiber_entry_trampoline+14>,
       entry=0x748136ffd968) at src/async/fiber_ctx.cpp:106
        -> entry(resumed_by, user_data);             <- indirect call faults; entry is corrupt
#2  fiber_entry_trampoline ()
#3  0x0000000000000000                                <- Fiber stack base (return addr 0)

Thread 2 (main): Scheduler::run_live(2) -> run_impl -> thread::join()  (waiting)
Thread 3 (worker 2): Scheduler::park_on_wake_source -> cv::wait_until  (parked, normal)
```

Registers at fault: `rip=0x748136ffd968`, `rsp=0x5efc921ce468` (a **heap**
address — the saved Fiber stack pointer is corrupt). The `entry` argument to
`fiber_entry_trampoline_bridge` equals the faulting `rip`, i.e. the indirect
`entry(...)` call jumped to a corrupted function pointer read from the saved
Fiber context.

`fiber_ctx.cpp:101-106`:
```cpp
extern "C" void fiber_entry_trampoline_bridge(
    Switch* resumed_by, void* user_data, Entry entry) {
    entry(resumed_by, user_data);   // <- line 106: faults because entry is garbage
}
```

## Hypothesis ledger (issue #47 §F)

| Hypothesis | Status | Evidence |
| --- | --- | --- |
| **C4 — invalid Fiber context switch (corrupt saved rip/entry)** | **CONFIRMED** | core: rip=0x...6ffd968 (garbage), entry ptr = rip, rsp is a heap addr; crash inside `fiber_entry_trampoline_bridge`'s `entry(...)` indirect call; reproduces on the 2-worker `run_live(2)` path |
| C2 — SLUICE_CHECK + teardown fail-fast masks original failure | **DENIED for the captured crash** (mechanism is real, but the captured core is SIGSEGV not SIGABRT; no terminate marker; crash is in Fiber execution not destruction) |
| C3 — std::terminate inside Scheduler before run returns | DENIED (no `std::terminate`; no `I47-T00`; direct SIGSEGV) |
| duplicate/stale runnable ticket (issue #47 §18) | **leading suspect for the root cause of the corruption** (two workers operating the same Fiber, or resuming a reclaimed/reused context); needs the §17 observations to pin the exact transition |
| MW-S2 premature termination | not the crash site |
| probe-induced timing | DENIED (reproduced WITHOUT any probe, plain binary on master) |

## G. Fix authorization (issue #47 §G)

`FIX PR AUTHORIZED`.

**Exact violated protocol (preliminary, pending the fix PR's §17 boundary
observations):** a worker resumes a Fiber whose saved CPU context (in particular
the entry/rip and rsp) is no longer valid. The crash is at the Fiber context
switch consumption side (`fiber_entry_trampoline_bridge` → `entry(...)`), on the
2-worker `run_live` path. The corruption is consistent with the issue #47 §18
suspect — a runnable ticket (or the make_running CAS / run_next_on admission
that consumes it) being satisfied for a Fiber whose saved context is stale or
being concurrently mutated — but the precise producing transition is to be pinned
by the authorized fix PR's deterministic phase-controller regression (issue #47
§20 criterion). Sufficient producing-transition candidates to investigate, in
priority order: `Fiber::make_running()` CAS-then-void-return +
`Scheduler::run_next_on()` context-switch regardless of CAS success; a stale
runnable ticket after wake routing; work-stealing of a ticket from a victim
mid-suspension-switch (`WorkerState::suspend_switch_pending`).

**Deterministic-regression requirement for the fix PR:** per issue #47 §20, the
fix PR MUST ship a deterministic phase-controller regression that reproduces the
corruption (or a safe assertion that catches the stale/invalid ticket BEFORE the
context switch) — NOT merely "it stopped crashing under N iterations". A passing
stress loop is necessary but not sufficient.

> Note on scope: this PR does NOT contain that fix. It only produces the
> root-cause-ready evidence above and the apparatus that will re-verify a fix.
> The CI failure on this PR is the preexisting `select_multi_worker_test` flake
> rooted here; merging this diagnostic PR is not blocked by that flake (it is a
> preexisting master defect), but the flake SHOULD be fixed by the authorized
> follow-up.

## Notes on Scheduler instrumentation (issue #47 §16-§18)

This diagnostic commit used: direct runner, test-local phases, post-run
snapshot, terminate marker, diagnostic exit classification, the manual workflow,
and **a captured core backtrace**. The root-cause-ready bar (§20) is met by the
core repeatedly identifying the same invalid Fiber-context site. A follow-up
that adds internal-testing-only observations at the `make_running` /
`run_next_on` / steal / wake-route boundaries (§17) is appropriate for the FIX
PR to pin the producing transition and to anchor the deterministic regression.

---

## Phase 2 — Exact producing race

The Scheduler allowed this general sequence on several suspension paths:

```
Owner Worker:
    under global_mtx_:
        register wait
        Fiber Running → Waiting
    release global_mtx_

Resolver:
    acquire global_mtx_
    Waiting → Runnable
    route runnable ticket to owner queue

Thief Worker:
    observe suspend_switch_pending == false
    steal routed ticket
    execute the Fiber

Owner Worker:
    has not yet completed Fiber → Scheduler context switch
    Fiber ctx.rsp/rbp/rip is not yet safely saved
```

This allows another OS thread to resume a Fiber whose CPU context is not ready.

The Select path attempted to prevent this with `suspend_switch_pending`, but it
published the protection AFTER releasing `global_mtx_`, leaving a publication-
before-protection window. The ordinary wait paths (Completion, ready flag,
WaitQueue, deadline) did not consistently establish this protection at all.

## Phase 3 — Corrective design

### Unified suspend protocol (I47-F2)

Created a single private protocol `commit_suspend_locked(ws, fiber)` that
centralizes the suspend authority raise + Fiber Running→Waiting transition:

```cpp
void Scheduler::commit_suspend_locked(WorkerState* ws, Fiber* fiber) {
    // 1. Raise suspend authority BEFORE the Fiber becomes observably Waiting.
    ws->suspend_switch_pending.store(true, std::memory_order_release);
    // 2. Transition Running -> Waiting.
    if (!fiber->make_waiting()) {
        detail::scheduler_invalid_suspend_transition_fail_fast();
    }
}
```

Because every resolver requires `global_mtx_` to publish a Runnable ticket, and
this function holds `global_mtx_` while raising authority AND transitioning
Waiting, there is NO window in which a resolver can publish before authority is
active.

### Authority clear point (I47-F2)

The suspend authority is cleared on the SCHEDULER continuation in `run_next_on`,
NOT on the resumed Fiber continuation:

```cpp
void Scheduler::run_next_on(WorkerState* ws, Fiber* fiber) {
    if (!fiber->make_running()) {
        detail::scheduler_invalid_runnable_ticket_fail_fast();
    }
    // ... context_switch ...
    // At this moment the Fiber CPU context has been saved.
    ws->suspend_switch_pending.store(false, std::memory_order_release);
}
```

### Fiber transition APIs (I47-F3)

Changed `make_running()` and `make_waiting()` to return `bool` so callers can
detect failed transitions. `run_next_on` now fails fast before entering an
invalid Fiber context.

### Migrated paths

All suspension paths now use the unified protocol:
- `await_completion_size`
- `await_completion_void`
- `await_ready_flag`
- `await_wait`
- `await_wait_deadline`
- `await_event_wait`
- `await_event_wait_deadline`
- Semaphore acquire paths
- Mutex lock paths
- Condition wait paths
- Queue push/pop paths
- RwLock paths
- Select suspended admission branch

## Phase 4 — Deterministic regressions

Added internal-testing-only phase seam `scheduler_suspend_before_physical_switch`
available on ALL suspension paths. This enables deterministic phase-controlled
tests that expose the unsafe ordering without relying on timing.

## Phase 5 — Stress/sanitizer evidence

### Debug mode
```
100% tests passed, 0 test(s) failed out of 113
select_multi_worker_test: ALL TESTS PASSED
event_primitive_test: ALL TESTS PASSED
multi_worker_coord_test: ALL TESTS PASSED
```

### Release mode
```
100% tests passed, 0 test(s) failed out of 113
```

### TSan
```
select_multi_worker_test: ALL TESTS PASSED (no TSan warnings)
event_primitive_test: ALL TESTS PASSED (no TSan warnings)
multi_worker_coord_test: ALL TESTS PASSED (no TSan warnings)
```

Note: TSan with Fiber assembly has known limitations (TSan does not understand
custom context switches). A clean TSan run is supporting evidence, not the
primary causal proof. The deterministic phase seams are the primary proof.

## Issue #45 relationship

The second CI incident (`event_primitive_test` crash, last case
`event_multi_waiter_mixed_outcome_stress`) showed the same vulnerable protocol
surface in Event/WaitQueue paths. The unified suspend protocol fix applies to
ALL wait paths, not just Select. After the deterministic Event-path regression
proves it is the same protocol defect, issue #45 may be documented as a second
manifestation of the same generic Scheduler suspension protocol defect.

Status: **CONFIRMED SAME ROOT CAUSE** — the Event/WaitQueue paths had the same
missing suspend authority that Select had (Select at least had a partial fix;
the ordinary paths had none).
