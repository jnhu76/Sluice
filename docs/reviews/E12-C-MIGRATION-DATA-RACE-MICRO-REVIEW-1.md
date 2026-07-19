# E12-C-ASYNC-MUTEX-MIGRATION-DATA-RACE-MICRO-REVIEW-1

Narrow micro-review confirming T19 migration evidence and data-race closure after Corrective-4.

---

## A. Verdict

```
E12-C-ASYNC-MUTEX-MIGRATION-DATA-RACE-MICRO-REVIEW-1:
PASS

E12-C IMPLEMENTATION REVIEW:
CLOSED
```

---

## B. Reviewed commit

```
a97aca1bb9248a2c5bc05d914ba8670de590ed34
"docs(async): E10-E12 API/semantic closure + Corrective-1 (C1-C7)"
2026-07-19

Current workspace HEAD: e75c0fe (merge of PR #13)
T19 file identical on master and review/e12-B-Semaphore branch.
```

---

## C. Historical-artifact reconstruction

The Corrective-4 record in `docs/reviews/E12-C-REVIEW.md` §L correctly describes four iterative correctives. Defects closed by each:

| Corrective | Defect closed |
|---|---|
| C-1 | T19 did not force or observe E8 migration; rewritten with gate fiber + spawn_on + current_worker_id assertions |
| C-2 | `run(2)` DRAIN caused hang; switched to `run_live(2)` LIVE. Established intended trace but left blocker-execution race |
| C-3 | Blocker-execution race: coordinator could free W1 before f_blocker was popped from W0's runnable queue. Closed via explicit `blocker_running` atomic handshake (observed, not inferred) |
| C-4 | Unsynchronized `waiting_count()` read from coordinator without `global_mtx_` — genuine C++ data race. Removed entirely; coordinator now gates solely on `a_suspended + blocker_running` |

No production semantics were touched in any corrective. All changes are test-only.

---

## D. Exact migration trace

Source: `tests/e12_async_mutex_test.cpp:1012-1190` (T19).

### D.1 Deterministic topology

```
sched.spawn(fA)       → W0 (round-robin: first spawn = worker 0)
sched.spawn(f_idle)   → W1 (second spawn = worker 1)
run_live(2)           → 2 worker OS threads
```

### D.2 Ordered causal checkpoints

All observations are through `std::atomic` release/acquire pairs. No sleep-based proof, no inferred state.

```
A_LOCKED_ON_W0:
  fA on W0:
    acquire_worker = current_worker_id() = 0
    mtx.lock(nA)                → immediate acquire (free mutex)
    spawn_on(f_blocker, 0)      → f_blocker on W0 local_runnable (behind fA)
    a_acquired = true           (release)
    a_suspended = true          (release)
    await_ready_flag(flag_wake) → make_waiting(), context_switch away

BLOCKER_RUNNING_ON_W0:
  W0 pops f_blocker from local_runnable (fA has context-switched out)
  f_blocker on W0:
    assert(current_worker_id() == 0)   ← TLS, worker-local
    blocker_running = true             (release)
    spin on release_blocker            ← keeps W0 busy

WAKE_RELEASED:
  W1 on f_idle: spin on flag_wake     ← keeps W1 busy, prevents early steal of f_blocker
  Coordinator (OS thread) observes ALL THREE before setting flag_wake:
    bounded_wait(a_acquired)          → true
    bounded_wait(a_suspended)         → true
    bounded_wait(blocker_running)     → true (proves f_blocker is ws->current on W0)
    flag_wake = true                  (release)

A_RESUMED_ON_W1:
  f_idle loop exits → W1 idle
  Readiness drain routes fA to W0's local_runnable (WaitReg.owner = W0 at suspend)
  W0 busy spinning f_blocker → CANNOT pop fA
  W1 idle → try_steal(W1) finds fA on W0:
    - state == runnable ✓
    - fiber_owner_[fA] == W0 ✓
    - remove from W0's local_runnable
    - fiber_owner_[fA] = W1 (owner transfer)
    - push to W1's local_runnable
  W1 pops fA, resume on W1

A_UNLOCKED_ON_W1:
  fA on W1:
    unlock_worker = current_worker_id() = 1
    mtx.unlock() → g_worker->current == &fA == owner_ → succeeds → owner_ = nullptr
    a_unlocked = true               (release)

BLOCKER_RELEASED + F2_ACQUIRED:
  Coordinator after observing a_unlocked (acquire):
    sched.spawn(f2)                → global_mtx_, push to worker local_runnable
    bounded_wait(f2_acquired)      → true (f2 acquires now-free mutex)
    release_blocker = true         (release)
```

### D.3 Core assertions

From `tests/e12_async_mutex_test.cpp:1178-1189`:

```cpp
SLUICE_CHECK_MSG(blocker_running.load(),   "f_blocker was executing on W0 (anti-race handshake)");
SLUICE_CHECK_MSG(aw == 0,                  "acquire_worker == W0");
SLUICE_CHECK_MSG(uw == 1,                  "unlock_worker == W1 (stolen from W0 while owning)");
SLUICE_CHECK_MSG(aw != uw,                 "worker changed between lock and unlock");
SLUICE_CHECK_MSG(a_unlocked.load(),        "fA resumed after migration and unlocked");
SLUICE_CHECK_MSG(nA.was_woken(),           "mutex acquired (Woken)");
SLUICE_CHECK_MSG(f2_acquired.load(),       "F2 acquired after unlock (ownership released)");
SLUICE_CHECK_MSG(nF2.was_woken(),          "F2 resolved Woken");
SLUICE_CHECK_MSG(sched.waiting_count()==0, "no unresolved waits remain");
SLUICE_CHECK_MSG(fA.state()==FiberState::done, "fA completed");
SLUICE_CHECK_MSG(f2.state()==FiberState::done, "f2 completed");
```

### D.4 Blocker-execution exclusion proof

`blocker_running` is written by f_blocker executing on W0 after asserting `current_worker_id() == 0`. Because f_blocker was queued behind fA on W0's `local_runnable`, f_blocker can execute only after fA has completed `await_ready_flag` (registered in `waiting_ready_`, committed `make_waiting()`, and context-switched away). The coordinator observes `blocker_running` through an atomic acquire load. When W1 is freed via `flag_wake`, f_blocker is provably `ws->current` on W0 — popped from W0's runnable queue and no longer stealable.

This is state **OBSERVATION**, not inference from `waiting_count`, `running_fiber_count`, or successful completion.

---

## E. Data-race audit

### E.1 Coordinator-worker shared variables (T19)

| Variable | Type | Written by | Read by | Synchronization | Race? |
|---|---|---|---|---|---|
| `flag_wake` | `std::atomic<bool>` | coordinator (release) | f_idle (acquire), readiness drain (under `global_mtx_`) | release/acquire pairs | NO |
| `a_acquired` | `std::atomic<bool>` | fA on W0 (release) | coordinator (acquire) | release/acquire | NO |
| `a_suspended` | `std::atomic<bool>` | fA on W0 (release) | coordinator (acquire) | release/acquire | NO |
| `blocker_running` | `std::atomic<bool>` | f_blocker on W0 (release) | coordinator (acquire) | release/acquire | NO |
| `a_unlocked` | `std::atomic<bool>` | fA on W1 (release) | coordinator (acquire) | release/acquire | NO |
| `acquire_worker` | `std::atomic<unsigned>` | fA on W0 (release) | coordinator (acquire) | release/acquire; single writer | NO |
| `unlock_worker` | `std::atomic<unsigned>` | fA on W1 (release) | coordinator (acquire) | release/acquire; single writer | NO |
| `f2_acquired` | `std::atomic<bool>` | f2 (release) | coordinator (acquire) | release/acquire | NO |
| `release_blocker` | `std::atomic<bool>` | coordinator (release) | f_blocker on W0 (acquire) | release/acquire | NO |

All coordinator-accessible shared variables are `std::atomic` with matching release/acquire ordering. No non-atomic shared variables exist between coordinator and workers.

### E.2 Scheduler internals

| Variable | Protection Mechanism | Concurrent access? | Race? |
|---|---|---|---|
| `global_mtx_` | `std::mutex` | Held by all mutex_* seams, spawn, classify, drain | NO |
| `waiting_ready_` | `global_mtx_` | await_ready_flag / wake_ready_flags_locked | NO |
| `waiting_waitq_count_` | `global_mtx_` | mutex_lock/unlock/cancel/handoff | NO |
| `fiber_owner_` | `global_mtx_` | try_steal, spawn, spawn_on, run distribute | NO |
| `local_runnable` (each worker) | per-worker `inbox_mtx` | worker pop, spawn push, try_steal under vlk | NO |
| `workers_` | `global_mtx_` | run, spawn, try_steal victim iteration | NO |
| `m_owner` (AsyncMutex::owner_) | `global_mtx_` + `waiters_.mtx()` | all mutex seams (try_lock/lock/unlock/handoff/cancel) | NO |

### E.3 Post-join reads (safe)

- `sched.waiting_count()` at `e12_async_mutex_test.cpp:1187` — after `runner.join()`. **SAFE** (no concurrent modifications).
- `fA.state()`, `f2.state()` at `e12_async_mutex_test.cpp:1188-1189` — after `runner.join()`. **SAFE**.
- `nA.was_woken()`, `nF2.was_woken()` — WaitNode outcome set atomically via `resolve_` CAS under `global_mtx_`. Read after `runner.join()`. **SAFE**.

### E.4 Corrective-4 verification

Corrective-4 claimed: "removed the unsynchronized `waiting_count()` observation from the T19 coordinator gate."

**Before C-4** (reconstructed from review doc): coordinator used `bounded_wait_pred` reading `sched.waiting_count() > 0` without `global_mtx_` — genuine C++ data race.

**After C-4** (current `e12_async_mutex_test.cpp:1114-1148`):
```cpp
if (!bounded_wait(a_acquired)) { ... }
if (!bounded_wait(a_suspended)) { ... }
if (!bounded_wait(blocker_running)) { ... }
flag_wake.store(true, std::memory_order_release);
```

No `waiting_count()` call in any coordinator gate. `bounded_wait` reads only the `std::atomic<bool>` flag passed by reference — never Scheduler protected state.

The remaining `sched.waiting_count()` at `e12_async_mutex_test.cpp:1187` (`SLUICE_CHECK_MSG(sched.waiting_count() == 0, ...)`) is after `runner.join()` — all worker threads terminated, no concurrent container modifications.

**Corrective-4 is properly applied. No unsynchronized Scheduler reads remain.**

### E.5 `bounded_wait` is safe

```cpp
inline bool bounded_wait(std::atomic<bool>& flag, unsigned max_iters) {
    for (unsigned i = 0; i < max_iters; ++i) {
        if (flag.load(std::memory_order::acquire)) return true;
        std::this_thread::yield();
    }
    return flag.load(std::memory_order::acquire);
}
```

Reads only the `std::atomic<bool>` reference parameter. `std::this_thread::yield()` is a voluntary context switch with no shared-state side effects. No unsynchronized Scheduler state reads. **SAFE.**

### E.6 `sched.spawn(f2)` from coordinator OS thread

`Scheduler::spawn()` at `scheduler.cpp:349` acquires `global_mtx_` and, if workers exist, pushes to the round-robin target worker's `local_runnable` under `inbox_mtx` with `inbox_cv.notify_one()`. Same path used by any external-thread spawn (e.g., T10). `global_mtx_` serializes concurrent access to `workers_`, `fiber_owner_`, and all Scheduler containers. **SAFE.**

---

## F. Failure-path boundedness

Every coordinator wait in T19 uses `bounded_wait` with 200,000 iteration limit:

| Gate | Line | Bounded |
|---|---|---|
| `bounded_wait(a_acquired)` | 1114 | YES |
| `bounded_wait(a_suspended)` | 1120 | YES |
| `bounded_wait(blocker_running)` | 1136 | YES |
| `bounded_wait(a_unlocked)` | 1151 | YES |
| `bounded_wait(f2_acquired)` | 1163 | YES |

On any gate timeout, `release_for_drain()` sets both `release_blocker` and `flag_wake` to `true`, allowing the LIVE run to drain and `runner.join()` to return. The test then reports FAILURE with a descriptive message. No infinite hang is possible.

```cpp
auto release_for_drain = [&] {
    release_blocker.store(true, std::memory_order_release);
    flag_wake.store(true, std::memory_order_release);
};
```

Timeout is a failure protection mechanism, never causal synchronization.

---

## G. Repetition results

Linux x86_64, Clang release build, 500 consecutive invocations each, 0 failures:

| Gate | Binary | Filter | Iterations | Result |
|---|---|---|---|---|
| T19 ownership migration | `e12_async_mutex_test` | `e12_mtx_t19_real_migration` | 500 / 500 | ALL PASSED |
| E8 steal reference | `e8_steal_test` | `e8_t3` | 500 / 500 | ALL PASSED |
| E12-C coordination stress | `e12_async_mutex_test` | `e12_mtx_t20_coordination_500` | 500 / 500 | ALL PASSED |

Runner: bash `for` loop via `SLUICE_TEST_FILTER=<filter> ./binary > /dev/null 2>&1`. Each invocation started binary, executed test, asserted all checkpoints, exited successfully. Zero launch failures, zero retries, zero skips.

---

## H. Sanitizer results

All three sanitizer modes compiled freshly per mode (not reused binaries) and executed the full AsyncMutex suite (T0–T22, 23 tests each):

| Mode | Build | Result |
|---|---|---|
| TSan (ThreadSanitizer) | `xmake f --mode=tsan -c && xmake -j$(nproc)` | ALL TESTS PASSED, 0 race reports |
| ASan (AddressSanitizer) | `xmake f --mode=asan -c && xmake -j$(nproc)` | ALL TESTS PASSED, 0 errors |
| UBSan (UndefinedBehavior) | `xmake f --mode=ubsan -c && xmake -j$(nproc)` | ALL TESTS PASSED, 0 errors |

TSan binary confirmed instrumented (25 `__tsan` symbols). Run with `TSAN_OPTIONS=report_atomic_races=1` — no races reported at any output verbosity.

---

## I. Findings

**No new findings.**

All historical findings (F1–F4) resolved:

| ID | Severity | Finding | Status |
|---|---|---|---|
| F1 | P1 | T19 does not prove actual E8 migration | **RESOLVED** (C-1/C-2/C-3) — deterministic steal topology, explicit `current_worker_id()` assertions, `blocker_running` handshake |
| F2 | P3 | §20.7 table header "Real" contradicts test's own "possible" | **RESOLVED** (doc corrective) |
| F3 | P3 | "after_steal" comment misleading | **RESOLVED** (corrective) |
| F4 | P2 | Unsynchronized `waiting_count()` read in T19 coordinator | **RESOLVED** (C-4) — removed; coordinator gates on `a_suspended + blocker_running` only |

**Supplementary:** `xmake.lua` deprecation warnings for `mode.tsan`/`mode.asan`/`mode.ubsan` resolved (replaced with `set_policy`-based sanitizer configuration).

---

## J. Final E12-C status

```
E12-C-ASYNC-MUTEX-MIGRATION-DATA-RACE-MICRO-REVIEW-1:
PASS

E12-C IMPLEMENTATION REVIEW:
CLOSED
```

The migration evidence is sound: T19 provably demonstrates a Fiber acquiring AsyncMutex on Worker 0, suspending while owning, being stolen by Worker 1 via real E8 `try_steal`, resuming on Worker 1, and unlocking on Worker 1 — all observed through atomic checkpoints, not inferred.

The data-race audit finds zero unsynchronized shared-variable accesses between the coordinator OS thread and the worker threads. Every coordinator-accessible shared variable is `std::atomic` with matching release/acquire ordering. No Scheduler guarded state is read without `global_mtx_`. Corrective-4's removal of the unsynchronized `waiting_count()` observation is verified complete — no replacement with another unsynchronized observation.

500/500 repetition across all three gates. TSan/ASan/UBSan all PASS with zero reports. No remaining P1 or P2 findings.
