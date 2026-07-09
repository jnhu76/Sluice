# CPP-STATIC-1 — Final Report

**Branch:** `feat/cpp-static-1-tsa` (from `E10-waitnode-wait-queue` @ `6c05069`)
**Date:** 2026-07-09 (history corrective 2026-07-10)
**Status:** ✅ COMPLETE — pilot scope proven, gated, committed
**History:** the F1/F2 production correctives first exposed by TSA were split
into a preceding commit (`async: correct statically exposed global lock
authority`) that lands on already-correct production, *before* this pilot
commit. This pilot commit adds only the TSA substrate.

---

## 1. What was built

A **pilot** Clang Thread Safety Analysis (TSA) gate over the E10 concurrency
subsystem.  Static, compiler-verified, capability-based lock-contract analysis
for `sluice::async`, with the annotation substrate erased to a no-op on
non-Clang compilers.

### New substrate headers

- **`include/sluice/async/thread_annotations.hpp`** — cross-compiler TSA macro
  vocabulary (CAPABILITY / SCOPED_CAPABILITY / GUARDED_BY / PT_GUARDED_BY /
  REQUIRES / REQUIRES_SHARED / EXCLUDES / ACQUIRE / RELEASE / TRY_ACQUIRE /
  ASSERT_CAPABILITY / NO_THREAD_SAFETY_ANALYSIS / RETURN_CAPABILITY).  Every
  macro erases to nothing under non-Clang.
- **`include/sluice/async/mutex.hpp`** — `sluice::async::Mutex`, a thin
  `SLUICE_CAPABILITY("mutex")`-annotated `std::mutex` wrapper (composition, no
  extra runtime state).
- **`include/sluice/async/lock_guard.hpp`** — `sluice::async::LockGuard`, a
  `SLUICE_SCOPED_CAPABILITY`-annotated RAII guard.  **Required**: Clang TSA
  does **not** track `std::lock_guard<std::mutex>` as acquiring a capability.

### Pilot annotations (4 surfaces)

1. **WaitQueue** (`wait_queue.hpp`): `head_` / `tail_` marked
   `GUARDED_BY(mtx_)`; `mtx_` upgraded `std::mutex → Mutex`; locked resolvers
   (`register_wait_locked`, `wake_one_locked`, `cancel_locked`,
   `unlink_locked`, `empty_locked`) annotated `REQUIRES(mtx_)`; `mtx()`
   accessor carries `RETURN_CAPABILITY(mtx_)` so the Scheduler seam threads
   through the capability.
2. **Scheduler global coordination** (`scheduler.hpp`): `waiting_size_` /
   `waiting_void_` / `waiting_ready_` / `waiting_waitq_count_` / `fiber_owner_`
   / `pending_spawn_` / `next_spawn_worker_` / `admission_` /
   `admission_owner_` marked `GUARDED_BY(global_mtx_)`; `global_mtx_` upgraded
   to `Mutex`; `classify_locked` / `external_wake_possible_locked` /
   `wake_ready_completions_locked` / `wake_ready_flags_locked` /
   `route_runnable_locked` / `route_runnable` annotated `REQUIRES(global_mtx_)`.
3. **Wake source** (`scheduler.hpp`): `wake_mtx_` upgraded to `Mutex`;
   `wake_epoch_` marked `GUARDED_BY(wake_mtx_)`; `park_on_wake_source` marked
   `NO_THREAD_SAFETY_ANALYSIS` (TSA-SUPPRESS-001) because Clang cannot track
   `condition_variable_any::wait_for` capability semantics.
4. **`SchedulerWakeHandle::Control`** (`scheduler.cpp`): `mtx` upgraded to
   `Mutex`; `scheduler` / `alive` marked `GUARDED_BY(mtx)`.

### Build gate

`xmake.lua` target `sluice_async` adds `-Wthread-safety
-Werror=thread-safety` via unconditional `add_cxxflags`.  The flags are
Clang-only; xmake does **not** pass them to GCC (GCC does not recognize
`-Wthread-safety` and would error on it), so they are filtered out of the GCC
compile command and the gate is effectively Clang-only.  On non-Clang compilers
the annotation macros additionally erase to no-ops (see
`thread_annotations.hpp`), so the GCC default toolchain is unaffected.

---

## 2. Substrate findings (probed, not assumed)

| Probe | Question | Result |
|-------|----------|--------|
| Raw `std::mutex` + `std::lock_guard` | Does Clang TSA track the std types? | **NO** — `RAW_STD_MUTEX_PARTIAL_OR_UNRECOGNIZED`.  TSA detects the lock obligation but does not recognize `std::lock_guard` as acquiring the capability. |
| Annotated `Mutex` + `std::lock_guard` | Does an annotated Mutex help alone? | **NO** — the guard must also be annotated. |
| Annotated `Mutex` + annotated `LockGuard` | Does the scoped-capability pair work? | **YES** — full GUARDED_BY / REQUIRES / ACQUIRE / RELEASE tracking. |
| `condition_variable_any::wait_for` | Can TSA track unique_lock through cv.wait? | **NO** — suppressed on `park_on_wake_source` (TSA-SUPPRESS-001). |
| Brace-init placement | Where does the attribute go? | Attribute must precede the brace initializer: `T x GUARDED_BY(m){init}`. |

---

## 3. Verification

There are two distinct kinds of verification, which must not be conflated:

```text
production TSA gate:
automated for sluice_async production TUs (xmake adds -Wthread-safety
-Werror=thread-safety to the sluice_async target; Clang only)

negative compiler proofs:
manual provenance; not part of the xmake gate
```

### Clang TSA gate (the point of the work)
- `xmake build sluice_async` under Clang 21.1.8 with `-Wthread-safety
  -Werror=thread-safety`: **zero warnings, zero errors.**

### Negative compiler proofs (N1–N3) — manual provenance probes
External fixtures under `tests/tsa-probe/`, compiled by hand with Clang TSA
outside the xmake gate.  These are **manual negative compiler provenance
probes**, not part of the automated xmake gate.  Each **must fail to compile**:
- **N1** (`tsa_negative_n1.cpp`): unguarded write to a `GUARDED_BY` field →
  REJECTED (`writing variable 'head_' requires holding mutex 'q.mtx_'`).
- **N2** (`tsa_negative_n2.cpp`): call to a `REQUIRES` function without the
  capability → REJECTED (`calling function 'classify_locked' requires holding
  mutex 'global_mtx_'`).
- **N3** (`tsa_negative_n3.cpp`): unguarded read/write of Control fields →
  REJECTED (two diagnostics).

All three correctly rejected. Positive probes S1–S5 correctly accepted.

### Regression — E7–E10 test suite (GCC release)
All 11 targets passed:
```
e7_coord_test            : ALL TESTS PASSED
e7_dup_publication_test  : ALL TESTS PASSED
e7_worker_test           : ALL TESTS PASSED
e8_steal_test            : ALL TESTS PASSED
e9_external_wake_test    : ALL TESTS PASSED
e9_wake_handle_lifetime_test : ALL TESTS PASSED
e10_wait_queue_test      : ALL TESTS PASSED
e10_scheduler_wait_test  : ALL TESTS PASSED
e10_corrective_c1_test   : ALL TESTS PASSED
e10_corrective_c2_c3_test: ALL TESTS PASSED
e10_corrective_c5_test   : ALL TESTS PASSED
```

### Sanitizers
- **ASan + UBSan** (`-m asanubsan`, GCC): all 5 E10 targets passed, no leaks,
  no UB.
- **TSan** (`-m tsan`, GCC): `e10_wait_queue_test` passed, no data races.

### Toolchain compatibility
- Clang 21.1.8: zero-warning TSA build.
- GCC 15.2.0: full release build + test pass; annotation macros erased to
  no-ops; the TSA flags are filtered out of the GCC compile command (GCC does
  not recognize `-Wthread-safety`).

---

## 4. Production correctives first exposed by TSA

TSA first exposed two statically-reachable global lock-authority violations in
`Scheduler`.  They are **not** summarized as one bug; they have distinct
classifications.  The production corrective for both landed in the **preceding**
commit (`async: correct statically exposed global lock authority`) on
already-correct production, *before* this TSA pilot commit.  This pilot commit
only adds the TSA substrate that surfaced them; it contains no production
corrective.

- **F1 — `next_spawn_worker_` reset outside `global_mtx_`.**
  `run_impl()` reset `next_spawn_worker_` after releasing `global_mtx_`
  following the `pending_spawn_` distribute, while a concurrent `spawn()`
  mutates `next_spawn_worker_` under `global_mtx_`.  Classification:
  **REAL_RUNTIME_CONCURRENCY_DEFECT** (a production-reachable race:
  `T1 run_impl` unlocked `next_spawn_worker_ = 0` ‖ `T2 spawn()
  next_spawn_worker_++` under `global_mtx_`).  Corrective: move the reset into
  the existing `global_mtx_` critical section.

- **F1 — `admission_` / `admission_owner_` reset outside `global_mtx_`.**
  `run_impl()` reset these alongside `next_spawn_worker_`, outside the lock
  that protects them.  Classification:
  **REAL_LOCK_AUTHORITY_VIOLATION_NO_REACHABLE_RACE_PROVEN** (no concurrent
  producer races the reset the way `spawn()` races `next_spawn_worker_`, but
  the fields are `GUARDED_BY(global_mtx_)`).  Corrective: same as above — reset
  under the accepted global lock authority.

- **F2 — MW-S2 Phase-B committed-path `admission_` read outside `global_mtx_`.**
  The committed path committed `admission_ = committed` under the lock and then
  re-read `admission_` in the Phase-C guard *outside* the lock.  Classification:
  **REAL_LOCK_AUTHORITY_VIOLATION_NO_REACHABLE_RACE_PROVEN** (only the committed
  worker can change `admission_` from committed, per the MW-S2 state-machine
  invariant).  Corrective: capture the committed/admission fact under
  `global_mtx_` into a local (`phase_b_committed`) and use the local after the
  lock.

No lock set, lock acquisition order, lexical lock lifetime, critical-section
boundary, or runtime state transition was changed by either corrective; both
only move existing field accesses into the lock scope that already protects
them.

---

## 5. Scope and limitations

**In scope (annotated):** the four pilot surfaces above.

**Out of scope (deliberately):** `WorkerState::inbox_mtx` / `inbox_cv` (still
`std::mutex`); the test-seam mutexes (`admission_seam_mtx_`, `park_seam_mtx_`,
`lifetime_seam_mtx_`); `sluice_core`.  These remain on `std::mutex` / raw
`std::lock_guard` and are **not** checked by the gate.

**Suppression budget:** 1 function (`park_on_wake_source`) + its inline cv
predicate lambda, justified by TSA-SUPPRESS-001 (Clang cannot model cv.wait
capability semantics).  The production lock fact
(`wake_epoch_ GUARDED_BY(wake_mtx_)`) is independently accepted by the gate
elsewhere and was proven in E9.

**Non-goal of this slice:** closing the suppression budget (would require an
annotated `unique_lock` + cv adapter); expanding coverage to the whole async
runtime; per-call-site annotations on every Scheduler method.

---

## 6. Files

| File | Change |
|------|--------|
| `include/sluice/async/thread_annotations.hpp` | NEW — TSA macro vocabulary |
| `include/sluice/async/mutex.hpp` | NEW — annotated `Mutex` |
| `include/sluice/async/lock_guard.hpp` | NEW — annotated `LockGuard` |
| `include/sluice/async/wait_queue.hpp` | pilot annotations |
| `include/sluice/async/scheduler.hpp` | pilot annotations |
| `src/async/scheduler.cpp` | pilot annotations (Mutex/LockGuard/`condition_variable_any`/contracts). The F1/F2 production correctives live in the preceding commit. |
| `xmake.lua` | TSA gate on `sluice_async` |
| `tests/tsa-probe/` | N1–N3 manual negative compiler proofs + S1–S5 substrate probes |
| `.agents/skills/test-driven-development/testing-anti-patterns.md` | TEST SIGNAL LAW recorded |
