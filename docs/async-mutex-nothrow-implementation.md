# ASYNC-MUTEX-NOTHROW-PRODUCTION-IMPLEMENTATION-1 — Implementation Evidence

> **Decision identity:** `ASYNC-MUTEX-NOTHROW-PRODUCTION-IMPLEMENTATION-1`
>
> **Status:** `PASS — AUTHOR SELF-ASSESSMENT`
>
> **Independent adversarial implementation review:** `PASS (B1)` — completed in
> `docs/reviews/ASYNC-MUTEX-NOTHROW-PRODUCTION-IMPLEMENTATION-1-REVIEW.md`
> (commit `15dc9b4`). This document remains the author's own evidence; the
> independent review is a separate artifact.

This document records the production realization of the
`ASYNC-MUTEX-NOTHROW-AUTHORITY-1` design (Candidate A: make the existing
`sluice::async::Mutex` acquisition `noexcept` / fail-fast). The design
authority is `docs/async-mutex-nothrow-authority.md`; the independent design
review is `docs/reviews/ASYNC-MUTEX-NOTHROW-AUTHORITY-1-REVIEW.md`. This
document satisfies §L of the implementation task.

---

## 1. Verdict

```text
ASYNC-MUTEX-NOTHROW-PRODUCTION-IMPLEMENTATION-1:
PASS — AUTHOR SELF-ASSESSMENT

INDEPENDENT ADVERSARIAL IMPLEMENTATION REVIEW: PASS (B1)
  docs/reviews/ASYNC-MUTEX-NOTHROW-PRODUCTION-IMPLEMENTATION-1-REVIEW.md

E12-E QUEUE IMPLEMENTATION AUTHORIZATION (Queue-level gate):
STILL DENIED — B2/B4 OPEN
  (B1 PASS, B3 PASS)
```

This is an author self-assessment; the independent review has now passed (B1
closed). The Queue is still not authorized by this document; B2 (Corrective-2
independent review) and B4 (Queue formal model) remain open.

---

## 2. Exact production change

| File | What changed |
|---|---|
| `include/sluice/async/mutex.hpp` | `lock()`, `try_lock()`, `unlock()` gained `noexcept`; `lock()`/`try_lock()` bodies wrapped in `try { ... } catch (...) { detail::async_mutex_lock_fail_fast(); }`. New header comment documents the dual catch+noexcept rationale (authority §3). `SLUICE_*` TSA spellings unchanged. |
| `include/sluice/async/detail/fail_fast.hpp` (new) | Declares `[[noreturn]] void async_mutex_lock_fail_fast() noexcept` in `sluice::async::detail`. No parameters (authority §D2). |
| `src/async/fail_fast.cpp` (new) | Out-of-line definition: `std::terminate();`. No allocation, no locks, no I/O, no virtual/function-pointer call, no dynamic string. |

Signatures:

```cpp
// before
void lock() SLUICE_ACQUIRE() { impl_.lock(); }
bool try_lock() SLUICE_TRY_ACQUIRE(true) { return impl_.try_lock(); }
void unlock() SLUICE_RELEASE() { impl_.unlock(); }

// after
void lock() noexcept SLUICE_ACQUIRE() {
    try { impl_.lock(); }
    catch (...) { detail::async_mutex_lock_fail_fast(); }
}
bool try_lock() noexcept SLUICE_TRY_ACQUIRE(true) {
    try { return impl_.try_lock(); }
    catch (...) { detail::async_mutex_lock_fail_fast(); }
}
void unlock() noexcept SLUICE_RELEASE() { impl_.unlock(); }
```

Failure policy (authority §2): underlying acquisition success → return
normally; underlying `lock()`/`try_lock()` throws → terminate/fail-fast at
the `Mutex` boundary, the exception cannot cross; `unlock()` precondition
violation remains UB (a program invariant failure), never modeled as
recoverable.

What was **not** changed (§N compliance): `include/sluice/async/lock_guard.hpp`
untouched; no AsyncQueue / Queue placeholder API; no `WorkerState::inbox_mtx`
change; no `AsyncMutex` semantics change; no reverse lock order; no
`std::mutex` failure forced via UB; `try_lock` exception is not converted to
`false`.

---

## 3. Failure-injection architecture

**Layering** (the plan-review P0-1 requirement: `mutex.hpp` must not depend on
the `sluice_async_test` namespace):

```
production Mutex entry  (include/sluice/async/mutex.hpp)
    |
    v   detail::maybe_inject_mutex_failure(op)   [under the macro]
internal-testing fault state (src/async/mutex_test_seam.cpp)
    ^
    |   MutexFailSeam -> detail::test_hooks::*    [test authority facade]
tests/async_test_control.hpp
```

| File | Role |
|---|---|
| `include/sluice/async/detail/mutex_test_seam.hpp` (new, installed) | Declares `enum MutexTestOperation`; under `SLUICE_ASYNC_INTERNAL_TESTING`, declares `maybe_inject_mutex_failure(...)` and `namespace test_hooks { arm_lock_countdown / arm_next_try_lock_fail / disarm }`. All in `sluice::async::detail`. |
| `src/async/mutex_test_seam.cpp` (new) | The **entire TU** is `#if defined(SLUICE_ASYNC_INTERNAL_TESTING)`. Defines process-global `atomic<unsigned> g_lock_countdown` and `atomic<bool> g_fail_next_try_lock` (both `memory_order_relaxed` — these count test faults only, carry no production sync), the dedicated `struct InjectedMutexFailure final {}` exception, the throw logic, and the `test_hooks`. |
| `include/sluice/async/mutex.hpp` (edited) | Inside each existing `try {}`, before `impl_.lock()`/`impl_.try_lock()`, a `#if defined(SLUICE_ASYNC_INTERNAL_TESTING) detail::maybe_inject_mutex_failure(...); #endif` call. The throw lands in the **same** `catch (...)` that handles a real underlying failure, so the death-test exercises the production fail-fast boundary. |
| `tests/async_test_control.hpp` (edited) | Adds `struct MutexFailSeam` — a thin test-facing facade routing to `detail::test_hooks::*`. Owns no state. |

**Seam-state semantics** (plan-review P1-E): `g_lock_countdown==0` disabled;
`==1` this `lock()` throws and the countdown becomes 0; `>1` this `lock()`
succeeds and the countdown decrements (used by T3). `g_fail_next_try_lock` is
a one-shot: `exchange(false)` on the next `try_lock()`. `disarm()` clears
both. Each death-test case forks a fresh child and re-arms on the child
thread before the relevant acquisition.

**Thrown type** (plan-review P1-D): the dedicated empty `struct
InjectedMutexFailure final {}`, not `std::system_error` — no `error_code`,
no message, no allocation; unambiguously exercises the production
`catch (...)` boundary.

### ODR / layout reasoning (§E3)

This implementation does **not** change `Mutex`'s class layout between
targets: `Mutex::impl_` is `std::mutex` in both. The only thing that differs
is whether the macro-gated call sites and the `mutex_test_seam.cpp` body are
compiled. Therefore the §E3 ODR questions reduce to the standard dual-target
discipline already used by `SLUICE_ASYNC_INTERNAL_TESTING` for
`AsyncTestAccess`:

* No binary links both `sluice_async` and `sluice_async_internal_testing`
  (xmake.lua:46-60 authority statement; the production targets depend on
  `sluice_async`, the internal-seam test targets depend on
  `sluice_async_internal_testing`).
* Every TU in a given final program sees a consistent macro definition:
  `SLUICE_ASYNC_INTERNAL_TESTING` is `{public = true}` on
  `sluice_async_internal_testing` (xmake.lua:85), so it reaches all
  dependents; the production `sluice_async` never defines it.
* Installed public headers compile to the production shape for ordinary
  downstream (the macro is undefined).

### Zero production cost evidence (§K)

Commands actually executed (Clang 21.1.8, Linux x86_64, xmake 3.0.9):

```text
nm build/linux/x86_64/debug/libsluice_async.a | grep -c 'maybe_inject\|InjectedMutexFailure\|test_hooks'
# symbol-index count = 0   (the only "match" is the object-file name line
#                           'mutex_test_seam.cpp.o:' printed by nm as a header,
#                           not a symbol row; it compiles to a 688-byte empty
#                           object with zero defined symbols.)

# production preprocessed header — no injection call sites:
clang++ -E -std=c++20 -Iinclude include/sluice/async/mutex.hpp | grep -c maybe_inject_mutex_failure
# = 0

# internal-testing preprocessed header — injection call sites present:
clang++ -E -std=c++20 -DSLUICE_ASYNC_INTERNAL_TESTING -Iinclude -Itests include/sluice/async/mutex.hpp | grep -c maybe_inject_mutex_failure
# = 3   (1 declaration + lock + try_lock)

# fail_fast.cpp.o defines the production fail-fast entry:
ar x libsluice_async.a fail_fast.cpp.o
nm fail_fast.cpp.o
# 0000... T async_mutex_lock_fail_fast
#          U std::terminate
```

There is no test backend type, no function-pointer dispatch, no virtual call,
no runtime injection branch in the production target. The production
`Mutex::lock()` normal path still enters `std::mutex::lock()` directly (the
`try`/`catch` is optimized by the compiler to a direct call on the no-throw
fast path; the `catch (...)` body is off-path).

---

## 4. Death tests

Harness: `tests/death_test_runner_posix.hpp` + `tests/e12_async_mutex_death_test.cpp`.
POSIX self-exec discipline (§F1): the binary re-execs itself with
`--death-child=<case>`; the child installs a deterministic terminate handler
(`std::set_terminate([]{ std::_Exit(86); })`); the parent `fork`/`waitpid`s
and asserts the exact exit code (86 = terminated via the Mutex fail-fast
boundary; 87 = wrongly returned; 88 = internal assertion; signal = unexpected
crash). No sentinel files.

| Case | Entry under test | Arm | Expected | Actual (Clang Debug) |
|---|---|---|---|---|
| T1 | `Mutex::lock()` on a direct local `Mutex` | `arm_next_lock_fail()` (countdown=1) | exit 86 | exit 86 — PASS |
| T2 | `Mutex::try_lock()` on a direct local `Mutex` | `arm_next_try_lock_fail()` | exit 86 (must NOT return false) | exit 86 — PASS |
| T3 | `std::condition_variable_any::wait_for` reacquire | `arm_lock_countdown(2)` | exit 86 during reacquire | exit 86 — PASS |
| T4 | control: same entries, NO arm | `disarm()` | exit 0 | exit 0 — PASS |

Per-case direct child invocation (independent verification, not just the
parent driver):

```text
$ e12_async_mutex_death_test --death-child=T1   ; exit 86
$ e12_async_mutex_death_test --death-child=T2   ; exit 86
$ e12_async_mutex_death_test --death-child=T3   ; exit 86
$ e12_async_mutex_death_test --death-child=T4   ; exit 0
$ e12_async_mutex_death_test --death-child=BOGUS; exit 88   (unknown-case negative)
```

T3 is deterministic by construction (plan-review P0-3): no notifier thread —
`cv.wait_for(lk, 5ms)` alone drives the internal `unlock` → timeout →
reacquire-via-`Mutex::lock`, where the armed 2nd lock throws inside the cv
machinery and is caught by the production `Mutex` catch boundary. There is no
lost-wake window and no cross-thread scheduling nondeterminism.

T1/T2 use a **direct local `Mutex`** (plan-review P0-2): they do not rely on
"the next Scheduler lock" — the fault is consumed by the exact local object's
entry, so the test proves the specific call site, not whichever lock the
Scheduler happens to take first.

### Platform coverage (§F3)

```text
executed platform: Linux x86_64  — PASS
other platform (macOS):  NOT RUN — no macOS toolchain in environment;
                          the harness and xmake target are gated to
                          is_plat("linux","macosx") and would run there.
other platform (Windows): NOT RUN — POSIX fork/exec/waitpid harness not
                          implemented in this task; a separate Windows
                          death-test harness (CreateProcess/self-exec) is
                          future work.
```

No unrun platform is reported as PASS.

---

## 5. API / ABI report

* **Public header impact.** `include/sluice/async/mutex.hpp` is installed. The
  function signatures gained `noexcept`; the bodies of `lock()`/`try_lock()`
  changed to a `try/catch` fail-fast shape. No new public type or function
  was added to the public surface (`async_mutex_lock_fail_fast` lives in
  `sluice::async::detail`, and the seam types live under
  `#if defined(SLUICE_ASYNC_INTERNAL_TESTING)`, absent in production TUs).
* **Function-type impact.** `noexcept` is part of the function type. Any
  downstream that takes `&sluice::async::Mutex::lock` (or try_lock/unlock)
  must be recompiled so the function-pointer type matches. **No in-repo
  translation unit takes such an address** (verified: zero `&Mutex::lock` /
  `&Mutex::try_lock` / `&Mutex::unlock` occurrences in `include/`, `src/`,
  `tests/`).
* **Verified symbol behavior.** Under the Itanium ABI (verified for Clang
  21.1.8 and GCC 15.2.0 on Linux x86_64), `noexcept` is not part of symbol
  mangling; `Mutex::lock` has no out-of-line member (everything is inline in
  the header), so there is no "old object file" to disagree and symbol names
  are unchanged.
* **Recompile expectation.** Because `Mutex` is inline-only, every TU that
  includes `mutex.hpp` already recompiles, so the function-type change is
  transparent for in-tree consumers.
* **Limitations of the ABI claim.** This is **not** an absolute cross-toolchain
  guarantee. It is limited to: (a) the Itanium ABI, (b) the compilers and
  versions actually verified (Clang 21.1.8, GCC 15.2.0), (c) the platform
  actually verified (Linux x86_64). Other ABIs (e.g. MSVC) may differ.

---

## 6. Regression matrix

Commands actually executed, per target, per configuration. Toolchain: Clang
21.1.8, GCC 15.2.0, Linux x86_64, xmake 3.0.9. Each target built and run
individually (`xmake f -c --toolchain=<tc> -m <mode>`; `xmake build <t>`;
`xmake run <t>`).

Targets exercised: `e4_scheduler_test`, `e6_scheduler_progress_test`,
`e7_worker_test`, `e7_coord_test`, `e8_steal_test`, `e9_external_wake_test`,
`e9_wake_handle_lifetime_test`, `e10_wait_queue_test`,
`e10_scheduler_wait_test`, `e10_corrective_c1_test`,
`e10_corrective_c2_c3_test`, `e10_corrective_c5_test`, `e11_timer_wait_test`,
`e12_event_test`, `e12_semaphore_test`, `e12_async_mutex_test`,
`e12_async_condition_test`, `e12_async_mutex_death_test`,
`e12_async_mutex_nothrow_authority_probe` (19 targets; the TSA build evidence
is the Clang builds below — `-Werror=thread-safety` is per-target on
`sluice_async`/`sluice_async_internal_testing`).

| Configuration | Result |
|---|---|
| Clang 21.1.8, Debug | **PASS** — all 19 targets build + run exit 0. |
| Clang 21.1.8, Release | **PASS** — all 19 targets build + run exit 0. |
| GCC 15.2.0, Debug | **NOT RUN (pre-existing environment blocker)** — GCC rejects `-Wthread-safety` (`g++: error: unrecognized command-line option '-Wthread-safety'`). The flag is applied with `{force = true}` at `xmake.lua:43-44/66-67`, overriding xmake's GCC filtering; the build fails on the first `src/async/*.cpp` (`async_io_context.cpp`, which this task did NOT touch). This is a pre-existing toolchain-config limitation, **not** a Mutex regression. Reported here as NOT RUN per §J ("不得把未支持或未运行写成 PASS"). Fixing the `add_cxxflags(...,{force=true})` is out of scope for this task (§N: no unrelated behavior change). |
| GCC 15.2.0, Release | **NOT RUN** — same pre-existing `-Wthread-safety` blocker as GCC Debug. |
| TSA (`-Wthread-safety -Werror=thread-safety`) | **PASS** — exercised by both Clang configurations above; the flags are on `sluice_async` and `sluice_async_internal_testing` targets, both built green. |
| `sluice_async_internal_testing` build | **PASS** — compiles green (TSA), injection symbols present as expected. |
| Death tests | **PASS** — T1/T2/T3/T4 under Clang Debug (see §4). |

### Condition T25 handling (§J1)

This task does not fix T25 (`e12_cond_t25_migration_condition_reacquire`). In
the regression matrix runs above, `e12_async_condition_test` passed in full
(Clang Debug and Clang Release), including T25. Per §J1, a single green run
does **not** close T25: T25 is the documented nondeterministic coordinator
spin (authority `docs/e12-queue-implementation-authorization.md`), tracked
separately under `E12-CONDITION-T25-MIGRATION-REACQUIRE-HANG-AUDIT-1` (B3).
The Mutex change did not add a timeout, skip, or weaken any Condition
assertion, and did not worsen T25. The Condition suite is reported green only
for the runs actually executed.

---

## 7. Sanitizer matrix

Sanitizers via xmake modes (xmake.lua:3): `xmake f -c --toolchain=clang -m
<mode>` then build/run each target. Clang only (sanitizers are not run under
GCC, which is already blocked above for the unrelated `-Wthread-safety`
reason). A per-test 60–90s `timeout` wrapper is used so a single slow test
cannot wedge the matrix; the wrapper does not modify any test's internal
assertions or timeouts (§J1: "不得增加 timeout" forbids weakening the T25
assertion — here we only bound the *harness* wait, not the test).

| Sanitizer | Result |
|---|---|
| ASan (`-m asan`) | **PASS with one documented T25 timeout.** All targets clean (`e4_scheduler_test`, `e10_wait_queue_test`, `e9_external_wake_test`, `e12_event_test`, `e12_semaphore_test`, `e12_async_mutex_test`, `e12_async_mutex_nothrow_authority_probe`, `e12_async_mutex_death_test` — incl. T1–T4 death cases). `e12_async_condition_test` exceeded the 90s harness timeout on `e12_cond_t25_migration_condition_reacquire` — this is the known independent T25 blocker (§J1, B3), NOT a Mutex regression: the same suite with T25 filtered (`SLUICE_TEST_FILTER='-e12_cond_t25'`) is clean under ASan (`ALL TESTS PASSED`). `e11_timer_wait_test` is EXCLUDED from the ASan cell: it pre-existingly busy-spins under ASan (test-clock polling amplified by ASan overhead), unrelated to this task. No ASan error report on any run target. |
| UBSan (`-m ubsan`) | **PASS** — all 9 targets clean, including `e12_async_condition_test` (full suite, T25 included — UBSan does not amplify the T25 coordinator spin the way ASan does) and `e12_async_mutex_death_test` (T1–T4). No UBSan `runtime error`. |
| TSan (`-m tsan`) | **PASS** — all targets clean: `e4_scheduler_test`, `e10_wait_queue_test`, `e9_external_wake_test`, `e12_event_test`, `e12_semaphore_test`, `e12_async_mutex_test`, `e12_async_mutex_nothrow_authority_probe`, `e12_async_mutex_death_test` (T1–T4), and `e12_async_condition_test` with T25 filtered (the T25 case is excluded under TSan for the same documented-hang reason as ASan; it is the known B3 blocker). No `ThreadSanitizer: data race` report on any run target. |
| ASan+UBSan (`-m asanubsan`) | **PASS** — `e4_scheduler_test`, `e10_wait_queue_test`, `e12_async_mutex_test`, `e12_async_mutex_nothrow_authority_probe`, `e12_async_mutex_death_test` (T1–T4) all clean. No sanitizer finding. |

Targets covered across the matrix: `e4_scheduler_test`, `e10_wait_queue_test`,
`e9_external_wake_test`, `e12_event_test`, `e12_semaphore_test`,
`e12_async_mutex_test`, `e12_async_condition_test`,
`e12_async_mutex_nothrow_authority_probe`, `e12_async_mutex_death_test`.
Excluded and honestly recorded: `e11_timer_wait_test` (pre-existing ASan
busy-spin, not this task), and the T25 case of `e12_async_condition_test`
under ASan/TSan (known B3 blocker).

No unrun sanitizer is reported as PASS.

---

## 8. Remaining risks

* **GCC build is blocked by a pre-existing `-Wthread-safety` config issue**
  (§6), unrelated to this task. It is reported honestly as NOT RUN, not
  masked. Until it is resolved (a separate task to drop `{force=true}` on
  those `add_cxxflags` or otherwise gate the flags to Clang), the GCC matrix
  cell stays open.
* **`e11_timer_wait_test` busy-spins under ASan** (test-clock polling
  amplified by ASan overhead). Pre-existing, not introduced by this task;
  excluded from the ASan cell and recorded honestly. Not a Mutex regression.
* **`e12_cond_t25_migration_condition_reacquire` hangs under ASan and TSan**
  (the harness 60–90s wait is exceeded). This is the known independent B3
  blocker (`E12-CONDITION-T25-MIGRATION-REACQUIRE-HANG-AUDIT-1`); it is NOT
  closed by this task and NOT a Mutex regression. The Mutex change did not
  add or weaken any Condition assertion. Per §J1, the T25 hang is reported
  here as the independent Condition blocker, distinct from the Mutex
  substrate regression result.
* **Windows death-test harness is not implemented** (§4). The POSIX harness
  does not run there; a Windows-specific harness is future work.
* **The throw is injected by a macro-gated call, not a real `std::mutex`
  failure.** This is the design-review-sanctioned seam (§G.2 option 5): it
  proves the production catch/fail-fast boundary without depending on the
  platform actually throwing `std::system_error` (which it does not reliably
  do). The risk that the seam diverges from a real failure is bounded: the
  throw occurs inside the same `try` and is caught by the same `catch (...)`
  that would catch a real `std::system_error`.
* **Author self-assessment only.** An independent adversarial implementation
  review is required before B1 can be marked PASS.

---

## 9. Repository state

```text
branch:           e12-e-queue-production-impl
HEAD:             92be506 (docs(async): record Mutex fail-fast contract)
commits (this task):
    629617c  docs(async): independently review Mutex no-throw authority   (Commit 0)
    be07564  feat(async): make Mutex acquisition fail-fast                (Commit 1)
    e2cfe61  test(async): verify Mutex acquisition failure terminates     (Commit 2)
    92be506  docs(async): record Mutex fail-fast contract                 (Commit 3)
working tree:     clean (no modified tracked files)
untracked files:  tests/test_t3_simple.cpp   (pre-existing, unrelated; untouched)
                  tla2tools.jar              (pre-existing, unrelated; untouched)
pushed:           no   (no upstream configured for this branch; no push / merge / PR)
```
