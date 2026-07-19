# ASYNC-MUTEX-NOTHROW-PRODUCTION-IMPLEMENTATION-1 — Independent Adversarial Implementation Review

```text
ASYNC-MUTEX-NOTHROW-PRODUCTION-IMPLEMENTATION-1-INDEPENDENT-REVIEW-1
```

> **Scope:** production implementation review only. The reviewer is **not** the
> author of the authority, its independent design review, or the production
> implementation under review. No claim in the author's self-assessment, commit
> messages, or existing test output is taken as fact; every blocking claim is
> reproduced independently from current source + commits + build artifacts.
>
> The only durable file added by this review is this document. No file under
> `include/`, `src/`, `tests/`, `xmake.lua`, the authority document, or the
> author's implementation report was modified. A repo-external scratch probe
> (`/tmp/mutex_negprobe/`) and a temporary git worktree at the base commit
> were used solely to verify counterexample/GCC/sanitizer claims; the working
> tree is byte-identical to review start except for this file.

The implementation under review is the four-commit chain on
`e12-e-queue-production-impl` (see §B). The author's `PASS — AUTHOR
SELF-ASSESSMENT` stamp in `docs/async-mutex-nothrow-implementation.md` is
treated as a claim, not evidence.

---

## A. Verdict

```text
ASYNC-MUTEX-NOTHROW-PRODUCTION-IMPLEMENTATION-1-INDEPENDENT-REVIEW-1:
PASS
```

`PASS` means the production realization of `ASYNC-MUTEX-NOTHROW-AUTHORITY-1`
Candidate A is correct and verifiably so: the Mutex contract is fail-fast;
the fail-fast helper is allocation/lock/I/O-free and `[[noreturn]] noexcept`;
the failure-injection seam is compiled out of the production archive to a
zero-symbol empty object file; T1–T4 are real death tests of the production
entry (not fakes, not signature-only); the POSIX harness distinguishes every
failure mode by exit code; TSA/Clang regression is green on Debug and Release;
UBSan/TSan/ASan/ASan+UBSan are green on the Mutex-relevant targets; the GCC
gap is demonstrated to pre-date this task and to be a toolchain-config issue
unrelated to the Mutex code; and all 25 counterexamples are blocked by type
structure, build structure, test, or rationally out of scope.

```text
ASYNC-MUTEX-NOTHROW B1:
PASS
```

```text
E12-E QUEUE IMPLEMENTATION AUTHORIZATION:
STILL DENIED — B2/B3/B4 OPEN
```

This review closes **only** the Mutex substrate input to B1. It does **not**
close B2 (Queue Corrective-2 review), B3 (`E12-CONDITION-T25-MIGRATION-
REACQUIRE-HANG-AUDIT-1`), or B4 (Queue TLA+ formal model). The Queue
production implementation remains blocked at Phase 0.

### Verdict matrix

| Axis | Finding |
|---|---|
| Commit attribution & scope purity (§B) | **CLEAN** — Commit 1 touches only `mutex.hpp`/`fail_fast.hpp`/`fail_fast.cpp`; Commit 2 adds the seam + tests + xmake gates and edits `mutex.hpp` only with macro-gated seam calls; Commit 3 is docs-only; Commit 0 is the design review. No Queue code, no Scheduler change, no `AsyncMutex`/`LockGuard`/`inbox_mtx` change. |
| Production Mutex contract (§C) | **PASS** — `lock()`/`try_lock()`/`unlock()` declared `noexcept`; TSA `SLUICE_ACQUIRE()`/`SLUICE_TRY_ACQUIRE(true)`/`SLUICE_RELEASE()` retained; `catch (...)` routes to `async_mutex_lock_fail_fast()`; `unlock()` not wrapped as recoverable. |
| Fail-fast helper (§D) | **PASS** — `[[noreturn]] void async_mutex_lock_fail_fast() noexcept`; disassembly is `push %rbp; mov %rsp,%rbp; call std::terminate`; only undefined symbol is `_ZSt9terminatev`; no allocation/lock/I/O/string/iostream. |
| Injection seam boundary & zero-cost (§E) | **PASS** — production `mutex_test_seam.cpp.o` is 688 bytes with `.text` size `0x00000000` and zero defined symbols; production archive carries zero `inject`/`maybe_inject`/`MutexFail`/`async_test_control`/`InjectedMutexFailure`/`test_hooks` symbols; production preprocessed `mutex.hpp` contains 0 `maybe_inject_mutex_failure` calls; internal-testing variant has 3 (decl + lock + try_lock). |
| ODR / layout (§E.3) | **PASS** — `Mutex::impl_` is `std::mutex` in both targets; the only macro-gated difference is the call site + the empty-vs-real `mutex_test_seam.cpp`; `{public = true}` define on `sluice_async_internal_testing`; no binary links both. |
| Death tests T1–T4 (§F) | **PASS** — T1 lock / T2 try_lock / T3 `condition_variable_any::wait_for` reacquire / T4 control all exit per protocol (86/86/86/0); per-case direct invocation reproduces; T3 uses real `cv.wait_for`, not a manual second `m.lock()`; independent trace probe confirms `cv.wait_for` performs exactly one reacquire lock. |
| Test harness (§F.5) | **PASS** — deterministic `std::set_terminate -> std::_Exit(86)`; distinct exit codes 86/87/88; parent asserts exact code, does **not** treat arbitrary SIGABRT as success; per-case isolation via fork+exec self; BOGUS → 88. |
| State machine & concurrency (§F.6) | **PASS** — lock countdown atomic CAS; try_lock one-shot `exchange`; `disarm()` clears both; independent negative probes N1 (countdown=2: 1st ok, 2nd terminates), N2 (try_lock arm does not consume plain lock), N3 (disarm clears all) all pass. |
| API / ABI / docs (§H) | **PASS** — EN/ZH API reference + changelog accurately state `noexcept`, fail-fast, not-absolute ABI guarantee; no `CLOSED`/`Queue authorized` overclaim; authority status is `IMPLEMENTED — AUTHOR SELF-ASSESSMENT — INDEPENDENT IMPLEMENTATION REVIEW REQUIRED`. |
| Regression matrix (§I) | **PASS** — 23 async targets build+run green on Clang Debug and Clang Release (incl. `e12_async_condition_test` full suite with T25). |
| Sanitizers (§J) | **PASS** — UBSan (10 targets), TSan (10 targets), ASan (9 targets), ASan+UBSan (4 targets) all clean; no Mutex-attributable finding. |
| GCC gap (§K) | **NON-BLOCKING REPOSITORY BUILD-CONFIG GAP** — reproduced at base commit `eb8d974` (Mutex change absent): GCC rejects `-Wthread-safety` on `src/async/wait_policy.cpp`; isolated GCC probe compiles `Mutex` + `LockGuard` + `std::lock_guard` + `unique_lock` + `condition_variable_any` cleanly. Pre-existing, unrelated. |
| Queue isolation (§L) | **PASS** — zero new `AsyncQueue`/`QueuePort`/`QueueCore` lines in any of the four commits; no `.tla`; no Queue test; authority status not elevated. |
| 25 counterexamples (§M) | **ALL BLOCKED OR JUSTIFIED**. |

---

## B. Commit attribution

Independent verification (hashes reproduced from `git log`, not the task
summary):

```text
629617c  docs(async): independently review Mutex no-throw authority   (Commit 0)
be07564  feat(async): make Mutex acquisition fail-fast                (Commit 1)
e2cfe61  test(async): verify Mutex acquisition failure terminates     (Commit 2)
e4b08b1  docs(async): record Mutex fail-fast contract                 (Commit 3)
base:    eb8d974  docs(async): E12-E Queue Phase 0 implementation authorization — BLOCKED
HEAD:    e4b08b1
```

`git log --oneline --decorate -10` confirms the chain; hashes match the task
summary.

### Per-commit scope audit (`git show --stat`)

| Commit | Files | Scope verdict |
|---|---|---|
| 0 `629617c` | `docs/reviews/ASYNC-MUTEX-NOTHROW-AUTHORITY-1-REVIEW.md` (+615) | **CLEAN** — design review only. |
| 1 `be07564` | `include/sluice/async/detail/fail_fast.hpp` (new), `include/sluice/async/mutex.hpp` (+44/-3), `src/async/fail_fast.cpp` (new) | **CLEAN** — only the production fail-fast realization. `mutex.hpp` adds `noexcept`, the `try/catch -> async_mutex_lock_fail_fast()` body, and the dual catch+noexcept header comment. **No** Queue code, **no** `AsyncMutex` change, **no** `inbox_mtx` change, **no** Scheduler lock-order change, **no** `LockGuard` exception-spec change, **no** unrelated cleanup, **no** public API surface added. |
| 2 `e2cfe61` | `include/sluice/async/detail/mutex_test_seam.hpp` (new), `include/sluice/async/mutex.hpp` (+14), `src/async/mutex_test_seam.cpp` (new), `tests/async_test_control.hpp` (+struct MutexFailSeam), `tests/death_test_runner_posix.hpp` (new), `tests/e12_async_mutex_death_test.cpp` (new), `tests/e12_async_mutex_nothrow_authority_probe.cpp` (new), `xmake.lua` (+47) | **CLEAN** — only the internal-testing failure seam, the noexcept probe, the POSIX death-test harness, T1–T4, and the two xmake targets that gate them. The `mutex.hpp` edit is purely the `#if defined(SLUICE_ASYNC_INTERNAL_TESTING) detail::maybe_inject_mutex_failure(...); #endif` call sites inside the existing `try {}` blocks (verified by `git show e2cfe61 -- include/sluice/async/mutex.hpp`); no production behavior change when the macro is undefined. |
| 3 `e4b08b1` | `docs/api-reference.md`, `docs/api-reference-zh.md`, `docs/async-mutex-nothrow-authority.md`, `docs/async-mutex-nothrow-implementation.md`, `docs/changelog.md` | **CLEAN** — docs + implementation evidence only. |

### Scope-pollution negative checks

* `git diff eb8d974..e4b08b1 -- src/async/scheduler.cpp` → **0 lines** (Scheduler untouched).
* `git diff eb8d974..e4b08b1 -- include/sluice/async/scheduler.hpp include/sluice/async/async_mutex.hpp include/sluice/async/condition.hpp include/sluice/async/wait_queue.hpp include/sluice/async/lock_guard.hpp` → **0 lines** (all substrate headers untouched).
* New lines mentioning `AsyncQueue`/`QueuePort`/`QueueCore` across the four commits → **0 / 0 / 0**.
* New `.tla` files → **none**.
* `WorkerState::inbox_mtx` type → still `std::mutex` at `include/sluice/async/scheduler.hpp:149` (unchanged).
* `AsyncMutex` (`include/sluice/async/async_mutex.hpp`) → 0 diff lines; does **not** call `Mutex::lock` (independent Fiber-suspending type, verified by `grep`).

**No scope pollution found.** The four commits are reviewable and individually
reversible.

---

## C. Production implementation audit

### C.1 The actual contract (`include/sluice/async/mutex.hpp:54-81`)

```cpp
void lock() noexcept SLUICE_ACQUIRE() {
    try {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        detail::maybe_inject_mutex_failure(detail::MutexTestOperation::lock);
#endif
        impl_.lock();
    } catch (...) {
        detail::async_mutex_lock_fail_fast();
    }
}
bool try_lock() noexcept SLUICE_TRY_ACQUIRE(true) {
    try {
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
        detail::maybe_inject_mutex_failure(detail::MutexTestOperation::try_lock);
#endif
        return impl_.try_lock();
    } catch (...) {
        detail::async_mutex_lock_fail_fast();
    }
}
void unlock() noexcept SLUICE_RELEASE() { impl_.unlock(); }
```

Authoritative checks (authority §E):

| Requirement | Status | Evidence |
|---|---|---|
| `void lock() noexcept` | **PASS** | `mutex.hpp:54`. |
| `bool try_lock() noexcept` | **PASS** | `mutex.hpp:71`. |
| `void unlock() noexcept` | **PASS** | `mutex.hpp:81`. |
| `SLUICE_ACQUIRE()` retained on `lock` | **PASS** | `mutex.hpp:54`. |
| `SLUICE_TRY_ACQUIRE(true)` retained on `try_lock` | **PASS** | `mutex.hpp:71`. |
| `SLUICE_RELEASE()` retained on `unlock` | **PASS** | `mutex.hpp:81`. |
| lock underlying success → normal return | **PASS** | `impl_.lock()` returns normally; function returns. |
| try_lock underlying true/false → result preserved | **PASS** | `return impl_.try_lock();` on the no-throw path. |
| underlying exception cannot escape | **PASS** | `catch (...)` + `noexcept` (double boundary). |
| underlying exception cannot become `try_lock == false` | **PASS** | the `catch (...)` calls `async_mutex_lock_fail_fast()` (which is `[[noreturn]]`); the `return false` path is unreachable on exception. |
| underlying exception must terminate | **PASS** | `async_mutex_lock_fail_fast()` → `std::terminate()` (§D). |
| `unlock()` not wrapped as recoverable | **PASS** | one-line `impl_.unlock()`; no try/catch. |

### C.2 `catch (...)` coverage

The `catch (...)` is a universal handler — it catches any exception type,
including the dedicated `InjectedMutexFailure` (a `final struct`) thrown by
the seam and any real `std::system_error` from `std::mutex`. The throw lands
inside the same `try {}` as `impl_.lock()`, so it is caught by the same
production boundary. Verified at runtime by T1/T2/T3 (§F).

### C.3 `noexcept` as language-level backstop

Even if the explicit `catch (...)` were removed, a throw out of a `noexcept`
function invokes `std::terminate` directly per `[except.handle]`/
`[res.on.exception.handling]`. The header comment at `mutex.hpp:11-33`
documents this dual rationale explicitly. Both layers are present and
required; the implementation does not rely on one alone.

### C.4 No alternate acquisition wrapper

* `grep -rn 'lock_or_terminate\|FailFastMutex' include/ src/ tests/` → **0 matches**.
* `grep -rn '&Mutex::lock\|&Mutex::try_lock\|&Mutex::unlock' include/ src/ tests/` → **0 matches** (no in-repo TU takes the member address; supports the ABI claim in §H).
* Every `Mutex` consumer (`LockGuard`, `std::unique_lock<Mutex>`, `std::condition_variable_any`) routes through `Mutex::lock`/`unlock` — there is no bypass.

### C.5 `system_error` inventory

`grep -rn 'system_error' include/ src/` → matches only in **comments**
(`mutex.hpp:16`, `mutex_test_seam.cpp:20`). No runtime consumer catches
`std::system_error` from any `Mutex`. The authority's obligation "no current
call site depends on catching `std::system_error` from `Mutex`" is satisfied.

---

## D. Fail-fast helper audit

### D.1 Declaration (`include/sluice/async/detail/fail_fast.hpp:31`)

```cpp
[[noreturn]] void async_mutex_lock_fail_fast() noexcept;
```

| Authority §F requirement | Status |
|---|---|
| `[[noreturn]]` | **PASS** |
| `noexcept` | **PASS** |
| No parameters (or only static no-behavior params) | **PASS** — zero parameters; the header comment explicitly rejects adding one. |
| No allocation | **PASS** — see disassembly §D.2. |
| No locking | **PASS** |
| No I/O | **PASS** |
| No formatting | **PASS** |
| No logging system | **PASS** |
| No callback | **PASS** |
| No function pointer | **PASS** |
| No virtual function | **PASS** |
| Ultimately calls `std::terminate` or equivalent | **PASS** |

### D.2 Definition (`src/async/fail_fast.cpp:13-15`) and disassembly

```cpp
[[noreturn]] void async_mutex_lock_fail_fast() noexcept {
    std::terminate();
}
```

Independent disassembly of `fail_fast.cpp.o` extracted from the production
archive `build/linux/x86_64/debug/libsluice_async.a`:

```text
0000000000000000 <_ZN6sluice5async6detail26async_mutex_lock_fail_fastEv>:
   0:   55                      push   %rbp
   1:   48 89 e5                mov    %rsp,%rbp
   4:   e8 00 00 00 00          call   9 <...+0x9>   # call std::terminate
```

`nm -u fail_fast.cpp.o` lists exactly one undefined symbol: `_ZSt9terminatev`
(`std::terminate`). **No** mutex/lock/allocate/string/iostream/__cxa symbol
is referenced. The function is a tail-call to `std::terminate` with the
standard prologue; it cannot allocate, lock, or recover.

### D.3 Counterexample refutations (helper-side)

* `catch -> helper -> logging mutex` — **refuted**: no logging symbol.
* `catch -> helper -> std::string allocation` — **refuted**: no
  `_Znwm`/`_ZNSs` reference.
* `catch -> helper returns` — **refuted**: `[[noreturn]]` + disassembly has
  no `ret` after the call.
* `catch -> helper throws` — **refuted**: `noexcept`; no `__cxa_throw`
  reference.

---

## E. Injection boundary and zero-cost proof

### E.1 The compiled-out Option 2 (compile-time elimination)

The seam is the macro-gated call-site pattern (authority §G.2 option 5,
refined to a call rather than a member-type swap):

```cpp
#if defined(SLUICE_ASYNC_INTERNAL_TESTING)
    detail::maybe_inject_mutex_failure(detail::MutexTestOperation::lock);
#endif
```

* `mutex.hpp` depends only on `sluice::async::detail` (the include of
  `detail/mutex_test_seam.hpp` is always present textually, but the
  declarations it exposes live under `#if defined(SLUICE_ASYNC_INTERNAL_TESTING)`
  and therefore vanish in production TUs).
* `mutex.hpp` never references the `sluice_async_test` namespace. The test
  facade `MutexFailSeam` lives in `tests/async_test_control.hpp`, which is
  **not** installed and **not** compiled into `sluice_async`.
* The fault state (`g_lock_countdown`, `g_fail_next_try_lock`,
  `InjectedMutexFailure`, `test_hooks::*`) is owned solely by
  `sluice_async_internal_testing` (compiled from `src/async/mutex_test_seam.cpp`
  under the macro).
* The test controller (`MutexFailSeam`) only arms/disarms library-internal
  counters; it does **not** own the production `catch`.

### E.2 Production archive evidence (independently reproduced)

Commands actually run (Clang 21.1.8, Linux x86_64, xmake 3.0.9, Debug):

```text
$ nm build/linux/x86_64/debug/libsluice_async.a | grep -i inject
$ # exit=1  (zero matches)

$ nm build/linux/x86_64/debug/libsluice_async.a | grep -i maybe_inject
$ # exit=1

$ nm build/linux/x86_64/debug/libsluice_async.a | grep MutexFail
$ # exit=1

$ nm build/linux/x86_64/debug/libsluice_async.a | grep async_test_control
$ # exit=1

$ nm build/linux/x86_64/debug/libsluice_async.a | grep InjectedMutexFailure
$ # exit=1

$ nm build/linux/x86_64/debug/libsluice_async.a | grep test_hooks
$ # exit=1
```

The production archive defines exactly one fail-fast symbol:

```text
$ nm build/linux/x86_64/debug/libsluice_async.a | grep async_mutex_lock_fail_fast
                 U _ZN6sluice5async6detail26async_mutex_lock_fail_fastEv
0000000000000000 T _ZN6sluice5async6detail26async_mutex_lock_fail_fastEv
```

### E.3 Production `mutex_test_seam.cpp.o` is an empty TU

The production archive contains `mutex_test_seam.cpp.o` (because the source
glob `src/async/*.cpp` is shared by both targets), but under the production
target the macro is undefined and the entire TU is gated:

```text
$ ar x libsluice_async.a mutex_test_seam.cpp.o
$ ls -la mutex_test_seam.cpp.o
-rw-r--r-- 688 bytes
$ nm mutex_test_seam.cpp.o ; echo exit=$?
exit=0                                  # zero symbols
$ objdump -h mutex_test_seam.cpp.o
Sections:
  0 .text         00000000 ...         # SIZE ZERO
  1 .comment      00000028 ...
  2 .note.GNU-stack 00000000 ...
  3 .llvm_addrsig 00000000 ...
```

The internal-testing variant of the same TU is **37456 bytes** and defines
`maybe_inject_mutex_failure`, `test_hooks::arm_lock_countdown` /
`arm_next_try_lock_fail` / `disarm`, the `InjectedMutexFailure` typeinfo, and
references `__cxa_allocate_exception` / `__cxa_throw` (verified by `nm`).

### E.4 Preprocessor evidence (independently reproduced)

```text
$ clang++ -E -std=c++20 -Iinclude include/sluice/async/mutex.hpp | grep -c maybe_inject_mutex_failure
0
$ clang++ -E -std=c++20 -Iinclude include/sluice/async/mutex.hpp | grep -c SLUICE_ASYNC_INTERNAL_TESTING
0
$ clang++ -E -std=c++20 -DSLUICE_ASYNC_INTERNAL_TESTING -Iinclude -Itests include/sluice/async/mutex.hpp | grep -c maybe_inject_mutex_failure
3       # declaration + lock call + try_lock call
```

The production preprocessed `lock()` body contains only `impl_.lock()` inside
the `try {}`:

```text
            impl_.lock();
        } catch (...) {
            detail::async_mutex_lock_fail_fast();
```

### E.5 No runtime branch in production

There is no runtime test of `SLUICE_ASYNC_INTERNAL_TESTING`; the macro is
`#if`-gated and absent in production, so the call does not exist. There is no
function-pointer dispatch, no virtual call, no test backend type. The
production `Mutex::lock()` normal path is `try { impl_.lock(); } catch (...)`
which the compiler lowers to a direct call (the catch is off-path).

### E.6 ODR / layout

`Mutex::impl_` is `std::mutex` in **both** targets; the class layout is
identical. The only macro-gated differences are (a) the two `#if` call sites
in `mutex.hpp` and (b) the empty-vs-real body of `mutex_test_seam.cpp`. The
discipline is the same one already used by `Scheduler::AsyncTestAccess`
under the same macro:

* `SLUICE_ASYNC_INTERNAL_TESTING` is `{public = true}` only on
  `sluice_async_internal_testing` (`xmake.lua:85`); it reaches all dependents.
* `sluice_async` never defines it.
* No binary links both variants (the production targets depend on
  `sluice_async`; the internal-seam test targets depend on
  `sluice_async_internal_testing`).

No mixed-macro ODR hazard exists for any single final program.

---

## F. Death-test audit

### F.1 Harness (`tests/death_test_runner_posix.hpp`)

Exit-code protocol (deliberately small, sentinel-file-free):

| Code | Meaning |
|---|---|
| `kExpectedTerminateExit` (86) | child reached `std::terminate` via the Mutex fail-fast boundary. |
| `kUnexpectedReturnExit` (87) | child returned past a must-terminate call (fail-fast boundary did NOT fire). |
| `kChildTestFailExit` (88) | child internal assertion failed (e.g. unknown case, exec/readlink failure). |
| other exit code / signal | unexpected crash or exec failure; reported verbatim. |

The terminate handler is `std::set_terminate([]() noexcept { std::_Exit(86); })`.
It does **not** allocate, does **not** lock, and uses `std::_Exit` (not
`exit`) to skip atexit/destructor ordering. The parent `waitpid`s and asserts
the **exact** exit code; it explicitly rejects any `WIFSIGNALED` as failure
(`expect_terminated_via_fail_fast` returns false if `r.signaled`), so an
arbitrary `SIGABRT` is **not** counted as success.

Self-exec discipline: the child re-execs `/proc/self/exe` with
`--death-child=<case>`, so the child is the same binary linked against the
same `sluice_async_internal_testing`. No fake class; no in-process
return-from-terminate.

### F.2 T1 — `Mutex::lock()` (`e12_async_mutex_death_test.cpp:57-63`)

```cpp
void child_t1_lock_failure() {
    Mutex m;                                 // real sluice::async::Mutex
    MutexFailSeam::disarm();
    MutexFailSeam::arm_next_lock_fail();     // countdown = 1
    m.lock();                                // MUST terminate; never returns
    std::_Exit(kUnexpectedReturnExit);       // 87 if it returned
}
```

Independent per-case invocation (`build/linux/x86_64/debug/e12_async_mutex_death_test --death-child=T1`):
**exit 86**. The body after `m.lock()` is unreachable in practice; the
`_Exit(87)` makes the regression loud if the boundary ever fails.

### F.3 T2 — `Mutex::try_lock()` (`e12_async_mutex_death_test.cpp:67-73`)

```cpp
MutexFailSeam::arm_next_try_lock_fail();
(void)m.try_lock();                          // MUST terminate; must NOT return false
std::_Exit(kUnexpectedReturnExit);
```

Per-case invocation: **exit 86**. The `(void)` discards the (unreachable)
return value; the fail-fast boundary fires before `try_lock` can return
`false`, so counterexample #2 (exception → false) is mechanically impossible.

### F.4 T3 — `std::condition_variable_any::wait_for` reacquire (`e12_async_mutex_death_test.cpp:86-97`)

```cpp
MutexFailSeam::arm_lock_countdown(2);        // 1st ok, 2nd (reacquire) throws
std::unique_lock<Mutex> lk(m);               // 1st lock (2 -> 1)
cv.wait_for(lk, 5ms);                        // reacquire = 2nd lock (1 -> 0, throws)
std::_Exit(kUnexpectedReturnExit);
```

Per-case invocation: **exit 86**. This is **not** a manual second `m.lock()`;
the 2nd lock is invoked inside the `condition_variable_any` machinery after
the `wait_for` times out and reacquires. Single-threaded: the timeout alone
drives the reacquire, so there is no notify-before-wait window and no
cross-thread nondeterminism.

**Independent trace probe** (`/tmp/mutex_negprobe/t3_trace.cpp`, armed
`countdown=3`): the explicit `m.lock()` *after* `cv.wait_for` returns is the
one that throws (exit 86), proving `cv.wait_for` consumes **exactly one**
reacquire lock (countdown 3 → 1 across ctor+reacquire, then 1 → 0 on the
explicit call). This rules out an implementation that does extra
unlock/relock cycles inside `wait_for` (which would have consumed the
countdown earlier and masked the reacquire claim).

### F.5 T4 — control (`e12_async_mutex_death_test.cpp:102-118`)

Disarmed; exercises `lock`/`unlock`/`try_lock`/`cv.wait_for`. Per-case
invocation: **exit 0`. The seam is provably inert when disarmed.

### F.6 Per-case dispatch + unknown-case negative

`dispatch_child("BOGUS")` → `_Exit(88)`. Verified: `--death-child=BOGUS` →
exit 88.

### F.7 Parent driver summary

```text
$ xmake run e12_async_mutex_death_test
[death] T1: PASS (terminated via Mutex fail-fast boundary, exit=86)
[death] T2: PASS (terminated via Mutex fail-fast boundary, exit=86)
[death] T3: PASS (terminated via Mutex fail-fast boundary, exit=86)
[death] T4: PASS (control: lock/try_lock/cv-wait all succeeded)
ALL DEATH TESTS PASSED (T1 lock / T2 try_lock / T3 cv-reacquire / T4 control)
exit=0
```

### F.8 Independent state-machine negative probes (`/tmp/mutex_negprobe/negprobe.cpp`)

| Probe | Arm | Expected | Result |
|---|---|---|---|
| N1 countdown=2 | `arm_lock_countdown(2)` then `lock; unlock; lock` | 1st ok, 2nd terminates (86) | **PASS** |
| N2 try_lock arm, plain lock | `arm_next_try_lock_fail()` then `m.lock()` | lock succeeds (0) — try_lock state is independent | **PASS** |
| N3 disarm clears all | arm both, `disarm()`, then lock+try_lock | both succeed (0) | **PASS** |

These prove: (a) countdown decrements rather than throws on every armed lock;
(b) `try_lock` state does not bleed into `lock`; (c) `disarm()` clears both
counters.

### F.9 Platform coverage

| Platform | Result |
|---|---|
| Linux x86_64 (POSIX fork/exec/waitpid) | **PASS** (independently reproduced, Clang Debug). |
| macOS | **NOT RUN** — no macOS toolchain in environment. The xmake target is gated to `is_plat("linux","macosx")`; the same harness would run there. |
| Windows | **NOT RUN** — POSIX harness not implemented on Windows. A Windows-specific harness (CreateProcess / self-exec) is future work; the death-test target is **not** built on Windows. |

No unrun platform is reported as PASS.

---

## G. ODR / layout proof

See §E.3 and §E.6. The `Mutex` class layout is identical across targets
(`std::mutex impl_` member, same offsets). The macro is `{public = true}`
only on `sluice_async_internal_testing`, so within any single final program
every TU sees a consistent macro definition. No binary links both variants.
No mixed-macro ODR violation is constructible in-tree.

The public installed header compiles to the production shape for ordinary
downstream (macro undefined): the seam declarations under
`#if defined(SLUICE_ASYNC_INTERNAL_TESTING)` vanish, so no public API surface
is added by `include/sluice/async/detail/mutex_test_seam.hpp` being installed.

---

## H. API / ABI audit

### H.1 Source-level contract

`noexcept` is part of the function type. Any downstream that takes
`&sluice::async::Mutex::lock` (or `try_lock`/`unlock`) must be recompiled so
the function-pointer type matches. **No in-repo TU takes such an address**
(`grep -rn '&Mutex::lock\|&Mutex::try_lock\|&Mutex::unlock' include/ src/ tests/`
→ 0 matches). The `Mutex` surface is entirely inline in the header, so every
TU that includes `mutex.hpp` already recompiles.

### H.2 Symbol-level behavior

Under the Itanium ABI (Clang 21.1.8 / GCC 15.2.0 on Linux x86_64),
`noexcept` is **not** part of symbol mangling; `Mutex::lock` has no
out-of-line member, so symbol names are unchanged. The production archive's
only fail-fast symbol is `_ZN6sluice5async6detail26async_mutex_lock_fail_fastEv`,
mangled without a noexcept component.

### H.3 Documentation accuracy (EN + ZH + changelog)

`docs/api-reference.md:365-406` and `docs/api-reference-zh.md:365-399`
state:

* `lock`/`try_lock`/`unlock` are `noexcept`.
* An underlying acquisition failure is not propagated as a recoverable
  exception; the `Mutex` boundary converts it to process termination via
  `std::terminate`.
* The runtime cannot resume user execution after such a failure while
  preserving ownership/queue-membership/publication invariants.
* `unlock()` precondition violation is UB, not a recoverable error.
* `noexcept` is part of the function type; downstream taking the member
  address must recompile.
* The ABI claim is **explicitly bounded** to the Itanium ABI and the
  compilers/versions actually verified; it is **not** claimed as an absolute
  cross-toolchain/platform guarantee.

`docs/changelog.md:5-19` records the change under "unreleased — async
substrate" with the same bounded ABI note. No overclaim found.

### H.4 Authority status

`docs/async-mutex-nothrow-authority.md:5-17` records:

```text
Design status:       PASS — INDEPENDENT REVIEW REQUIRED
Production status:   IMPLEMENTED — AUTHOR SELF-ASSESSMENT —
                     INDEPENDENT IMPLEMENTATION REVIEW REQUIRED
```

The status is **not** `CLOSED`. The document explicitly states the author's
self-assessment "is **not** a closed gate and does not authorize the Queue."
No occurrence of `CLOSED`, `Queue authorized`, `B1 PASS`, `absolute ABI`,
or `all platforms unaffected` appears in any of the authority,
implementation, API-reference, or changelog files (independently grepped).

---

## I. Regression matrix

Toolchain actually used: Clang 21.1.8, Linux x86_64, xmake 3.0.9. Each target
built and run individually (`xmake f -c --toolchain=clang -m <mode>`;
`xmake build <t>`; `xmake run <t>`, with a 90s/120s/180s `timeout` wrapper in
sanitizer cells only). Full target list (23 async targets):

```
e4_scheduler_test, e5_a1_ready_flag_test, e5_a2_evented_future_test,
e5_b_evented_group_test, e6_scheduler_progress_test, e7_worker_test,
e7_coord_test, e7_dup_publication_test, e8_steal_test,
e9_external_wake_test, e9_wake_handle_lifetime_test, e10_wait_queue_test,
e10_scheduler_wait_test, e10_corrective_c1_test, e10_corrective_c2_c3_test,
e10_corrective_c5_test, e11_timer_wait_test, e12_event_test,
e12_semaphore_test, e12_async_mutex_test, e12_async_condition_test,
e12_async_mutex_death_test, e12_async_mutex_nothrow_authority_probe
```

| Configuration | Build | Run | Notes |
|---|---|---|---|
| Clang 21.1.8 Debug | **23/23 ok** | **23/23 exit 0** | incl. `e12_async_condition_test` full suite (T25 green); incl. `e12_async_mutex_death_test` (T1–T4) and `e12_async_mutex_nothrow_authority_probe`. |
| Clang 21.1.8 Release | **23/23 ok** | **23/23 exit 0** | same coverage; `e12_async_condition_test` T25 green. |
| TSA (`-Wthread-safety -Werror=thread-safety`) | (in Clang builds above) | — | The flags are on `sluice_async` and `sluice_async_internal_testing` targets; both built green. |
| GCC 15.2.0 Debug | **NOT RUN** | — | See §K: pre-existing `-Wthread-safety` toolchain-config blocker, reproduced at base commit. |
| GCC 15.2.0 Release | **NOT RUN** | — | Same. |

### Condition T25 handling

`e12_async_condition_test` (including `e12_cond_t25_migration_condition_reacquire`)
passed in full on Clang Debug and Clang Release in this review's independent
runs. T25 is the documented nondeterministic coordinator-spin case
(`docs/e12-queue-implementation-authorization.md`, B3); a single green run
does **not** close `E12-CONDITION-T25-MIGRATION-REACQUIRE-HANG-AUDIT-1`. The
Mutex change did not add a timeout, skip, or weaken any Condition assertion.

---

## J. Sanitizer evidence

Toolchain: Clang 21.1.8 with xmake sanitizer modes. `UBSAN_OPTIONS`/
`TSAN_OPTIONS`/`ASAN_OPTIONS` set `halt_on_error=0` so a single finding does
not mask later ones; `detect_leaks=0` on ASan (the death-test children fork
and `_Exit`, which is not a leak). A per-test `timeout` wrapper bounded the
harness wait only; no test's internal assertion was weakened.

| Sanitizer | Targets run | Result |
|---|---|---|
| UBSan (`-m ubsan`) | e4_scheduler_test, e10_wait_queue_test, e9_external_wake_test, e12_event_test, e12_semaphore_test, e12_async_mutex_test, **e12_async_condition_test** (full, T25 incl.), e12_async_mutex_death_test (T1–T4), e12_async_mutex_nothrow_authority_probe, e11_timer_wait_test | **10/10 PASS**, zero `runtime error:` lines. |
| TSan (`-m tsan`) | same 10 targets | **10/10 PASS**, zero `ThreadSanitizer: data race` lines. |
| ASan (`-m asan`) | e4_scheduler_test, e10_wait_queue_test, e9_external_wake_test, e12_event_test, e12_semaphore_test, e12_async_mutex_test, **e12_async_condition_test** (T25 incl.), e12_async_mutex_death_test (T1–T4), e12_async_mutex_nothrow_authority_probe | **9/9 PASS**, zero `AddressSanitizer` findings. |
| ASan+UBSan (`-m asanubsan`) | e4_scheduler_test, e12_async_mutex_test, e12_async_mutex_death_test (T1–T4), e12_async_mutex_nothrow_authority_probe | **4/4 PASS**, zero findings. |

### Death tests under sanitizers

`e12_async_mutex_death_test` ran clean under UBSan, TSan, ASan, and
ASan+UBSan. The child `std::set_terminate -> std::_Exit(86)` is
sanitizer-stable: `_Exit` skips atexit, so no leak report is emitted for the
forked child, and the parent asserts the exact exit code (not a signal).

### e11_timer_wait_test under ASan — disposition

The author's report records `e11_timer_wait_test` as a pre-existing ASan
busy-spin and excludes it from the ASan cell. This review independently
verified the causality claim by checking out the base commit `eb8d974`
(Mutex change absent) in a temporary worktree and running the same ASan
configuration:

```text
base commit eb8d974, Clang ASan, e11_timer_wait_test:
  timeout 120 -> exit 124 at t7   (HANG REPRODUCED before the Mutex change)
```

The hang is reproducible **without** the Mutex change, so it is not a Mutex
regression. On the current commit, in this review's environment, the same
target actually passed ASan in 0.81s — the hang is machine/load-sensitive,
which is consistent with the authority's "nondeterministic busy-spin"
characterization. Either way, the causality is clear: the Mutex change did
not introduce the e11 ASan behavior.

### T25 under ASan/TSan

`e12_async_condition_test` (T25 included) ran clean under UBSan, TSan, and
ASan in this review's environment. The author's report excludes T25 under
ASan/TSan as the known B3 blocker; this review's green run does not close
B3 (a single green run is insufficient for a documented nondeterministic
spin), but it does confirm the Mutex change introduces no new data race or
memory error in the Condition suite.

---

## K. GCC disposition

```text
GCC gap classification: NON-BLOCKING REPOSITORY BUILD-CONFIG GAP
```

### K.1 Root cause

`xmake.lua:43-44` and `xmake.lua:66-67` apply the TSA flags with
`{force = true}`:

```lua
add_cxxflags("-Wthread-safety", {force = true})
add_cxxflags("-Werror=thread-safety", {force = true})
```

`git blame` shows these lines were introduced by `1d4b4c9c` (2026-07-11) and
`1396be14` (2026-07-12) — **six days before** this task's `be07564`
(2026-07-17). `force = true` overrides xmake's compiler-specific flag
filtering, so the flag reaches GCC, which does not recognize
`-Wthread-safety` and errors out.

### K.2 Reproduction at base commit

Independent verification in a temporary worktree at `eb8d974` (Mutex change
absent):

```text
$ xmake f -c --toolchain=gcc -m debug
$ xmake build sluice_async
error: g++: error: unrecognized command-line option '-Wthread-safety'
  in src/async/wait_policy.cpp
```

The same failure occurs at the base commit. The Mutex change is not the
cause.

### K.3 Isolated GCC compile probe (Mutex code)

```text
$ g++ -std=c++20 -Iinclude -c src/async/async_io_context.cpp -o /tmp/test_gcc.o
$ # exit=0   (no -Wthread-safety)

$ g++ -std=c++20 -Iinclude -Wthread-safety -c src/async/async_io_context.cpp
g++: error: unrecognized command-line option '-Wthread-safety'

$ cat > /tmp/gcc_mutex_probe.cpp <<EOF
  #include <sluice/async/mutex.hpp>
  #include <sluice/async/lock_guard.hpp>
  #include <condition_variable>
  #include <mutex>
  static void use(sluice::async::Mutex& m) {
      sluice::async::LockGuard g(m);
      std::lock_guard<sluice::async::Mutex> lg(m);
      std::unique_lock<sluice::async::Mutex> ul(m);
      std::condition_variable_any cv;
      cv.wait_for(ul, std::chrono::milliseconds(1));
      (void)m.try_lock(); m.unlock();
  }
  int main() { return 0; }
EOF
$ g++ -std=c++20 -Iinclude -c /tmp/gcc_mutex_probe.cpp -o /tmp/gcc_mutex_probe.o
$ # exit=0
```

GCC compiles the full Mutex substrate (`Mutex`, `LockGuard<Mutex>`,
`std::lock_guard<Mutex>`, `std::unique_lock<Mutex>`,
`std::condition_variable_any`) cleanly. The failure is solely the
Clang-only `-Wthread-safety` flag, not the Mutex code.

### K.4 Conclusion

The GCC gap satisfies all four NON-BLOCKING conditions:

* the issue predates the implementation (K.1, K.2);
* the error is on a Clang-only flag, not on Mutex code (K.3);
* the isolated GCC probe compiles (K.3);
* the current change introduces no new GCC compile error (K.2 vs current).

It does **not** block B1 PASS. Fixing the `add_cxxflags(..., {force = true})`
is a separate toolchain-config task (out of scope per §N: no unrelated
behavior change).

---

## L. 25 counterexamples

Each marked per the brief's vocabulary:
`BLOCKED BY TYPE STRUCTURE` / `BLOCKED BY BUILD STRUCTURE` / `BLOCKED BY TEST` /
`NOT BLOCKED` / `OUT OF SCOPE WITH JUSTIFICATION`. No counterexample relies
on a debug assertion as its only evidence.

| # | Counterexample | Disposition | Independent evidence |
|---|---|---|---|
| 1 | `Mutex::lock()` injected exception escapes | **BLOCKED BY TYPE STRUCTURE** | `mutex.hpp:54` `lock() noexcept`; the `catch (...)` calls `async_mutex_lock_fail_fast()` (`[[noreturn]]`); `noexcept` is the language backstop. T1 reproduces exit 86. |
| 2 | `try_lock()` injected exception becomes `false` | **BLOCKED BY TYPE STRUCTURE** | `mutex.hpp:71` the `catch (...)` calls the noreturn helper before any `return false` is reachable. T2 reproduces exit 86 (not false). |
| 3 | helper locks or allocates internally | **BLOCKED BY TYPE STRUCTURE** | `fail_fast.cpp:13-15` disassembly is `push rbp; mov rsp,rbp; call std::terminate`; `nm -u` shows only `_ZSt9terminatev`. |
| 4 | production binary retains the injection branch | **BLOCKED BY BUILD STRUCTURE** | `objdump -h` on production `mutex_test_seam.cpp.o` shows `.text` size 0; preprocessed `mutex.hpp` has 0 `maybe_inject_mutex_failure` calls; `nm` finds 0 injection symbols. |
| 5 | production binary links the test controller | **BLOCKED BY BUILD STRUCTURE** | `nm libsluice_async.a` has zero `async_test_control` / `MutexFail` / `test_hooks` symbols; the test facade lives in the non-installed `tests/async_test_control.hpp`. |
| 6 | `mutex.hpp` depends on the tests namespace | **BLOCKED BY TYPE STRUCTURE** | `mutex.hpp` references only `sluice::async::detail`; the test-facing `MutexFailSeam` is in `sluice_async_test` in `tests/async_test_control.hpp`, which `mutex.hpp` does not include. |
| 7 | T1 directly calls the helper | **BLOCKED BY TEST** | T1 calls `m.lock()` on a real `Mutex`; the helper is reached only via the production `catch (...)`. Source `e12_async_mutex_death_test.cpp:57-63`. |
| 8 | T2 does not prove it avoids returning `false` | **BLOCKED BY TEST** | T2 exit is 86 (terminate), never 87 (returned); if the catch returned `false` the child would hit `_Exit(87)`. |
| 9 | T3 is actually a manual second `m.lock()` | **BLOCKED BY TEST** | T3 source uses `cv.wait_for(lk, 5ms)`; the 2nd lock is inside the cv machinery. Trace probe (`t3_trace.cpp`) confirms exactly one reacquire lock inside `wait_for`. |
| 10 | T3 may suffer notify-before-wait | **BLOCKED BY TEST** | T3 has no notifier thread; the timeout alone drives the reacquire. Single-threaded, deterministic. |
| 11 | any SIGABRT is treated as success | **BLOCKED BY TEST** | `expect_terminated_via_fail_fast` returns false if `r.signaled`; it asserts the exact exit code 86. |
| 12 | child exec failure misjudged as terminate | **BLOCKED BY TEST** | `fork_exec_child` returns -1 on fork/exec/readlink failure; `expect_terminated_via_fail_fast` reports `FAIL (fork/exec/waitpid error)`. The child `_Exit(kChildTestFailExit)` (88) on execv failure, distinct from 86. |
| 13 | control path did not really run | **BLOCKED BY TEST** | T4 exercises `lock`/`unlock`/`try_lock`/`cv.wait_for` and exits 0; `expect_normal_exit_zero` asserts exit 0. |
| 14 | failure state leaks to the next case | **BLOCKED BY TEST** | each case forks a fresh child and calls `disarm()` before arming; negative probe N3 confirms `disarm()` clears both counters. |
| 15 | internal-testing/production layouts differ | **BLOCKED BY TYPE STRUCTURE** | `Mutex::impl_` is `std::mutex` in both; only macro-gated call sites + the empty-vs-real seam TU differ. |
| 16 | mixed macro definitions form an ODR violation | **BLOCKED BY BUILD STRUCTURE** | `{public = true}` define propagates to all dependents of `sluice_async_internal_testing`; no binary links both variants. |
| 17 | `LockGuard` or `condition_variable_any` no longer compiles | **BLOCKED BY TEST** | `e12_async_mutex_nothrow_authority_probe` static_asserts compile + run green; the full async suite (which uses `std::unique_lock<Mutex>` and `cv.wait_until` at `scheduler.cpp`) builds + runs green on Clang Debug/Release. |
| 18 | TSA annotation accidentally removed | **BLOCKED BY TEST** | `mutex.hpp:54/71/81` retain `SLUICE_ACQUIRE()`/`SLUICE_TRY_ACQUIRE(true)`/`SLUICE_RELEASE()`; the TSA build (`-Werror=thread-safety`) is green. |
| 19 | `inbox_mtx` changed to a new strategy | **BLOCKED BY TYPE STRUCTURE** | `scheduler.hpp:149` `std::mutex inbox_mtx;` unchanged; `git diff eb8d974..e4b08b1 -- include/sluice/async/scheduler.hpp` is 0 lines. |
| 20 | `AsyncMutex` Fiber semantics changed | **BLOCKED BY TYPE STRUCTURE** | `git diff eb8d974..e4b08b1 -- include/sluice/async/async_mutex.hpp` is 0 lines; `AsyncMutex` does not call `Mutex::lock`. |
| 21 | GCC failure actually caused by current code | **BLOCKED BY BUILD STRUCTURE** | reproduced at base commit `eb8d974`; isolated GCC probe compiles Mutex/LockGuard/unique_lock/cv cleanly (§K). |
| 22 | sanitizer hang hidden | **NOT BLOCKED — REPRODUCED AS PRE-EXISTING** | e11 ASan hang reproduces at base commit (§J); Mutex suite is clean under UBSan/TSan/ASan/ASan+UBSan. |
| 23 | API doc overclaims ABI guarantees | **BLOCKED BY TEST** | EN/ZH API refs explicitly bound the ABI claim to Itanium + verified compilers; no `absolute ABI` / `all platforms` phrasing. |
| 24 | authority wrongly marked CLOSED | **BLOCKED BY TEST** | authority status is `IMPLEMENTED — AUTHOR SELF-ASSESSMENT — INDEPENDENT IMPLEMENTATION REVIEW REQUIRED`; no `CLOSED` anywhere. |
| 25 | Queue prematurely authorized | **OUT OF SCOPE WITH JUSTIFICATION** | zero `AsyncQueue`/`QueuePort`/`QueueCore` lines added; no `.tla`; Queue Phase 0 remains denied (§N). This review closes only the Mutex substrate input to B1, not B2/B3/B4. |

---

## M. Required correctives

**None blocking.** The implementation satisfies the authority §7 obligations
and the design review's P0/P1/P2 correctives:

* P0 (failure-injection seam + death-test): **closed** — `mutex_test_seam.hpp`/`.cpp` + `e12_async_mutex_death_test` (T1–T4) + independent negative probes (§F.8).
* P1 (public API contract update): **closed** — EN/ZH API reference + changelog updated (§H.3).
* P2 (single named fail-fast helper): **closed** — `async_mutex_lock_fail_fast` (§D).

### Non-blocking observations (not required for B1 PASS)

1. **GCC `-Wthread-safety {force=true}` config gap (§K).** Pre-existing,
   reproduced at base commit. A separate toolchain-config task to gate the
   flag to Clang (or drop `force=true`) would unblock the GCC matrix cell;
   until then the GCC cell stays honestly NOT RUN. Not a Mutex defect.
2. **`e11_timer_wait_test` ASan busy-spin.** Pre-existing, reproduced at base
   commit. Independent E11 issue. Not a Mutex regression.
3. **`e12_cond_t25` nondeterministic coordinator spin (B3).** Passed in this
   review's runs but a single green run does not close the audit. Independent
   B3 blocker. Not a Mutex regression.
4. **Windows death-test harness.** Not implemented; POSIX-only by design.
   The xmake target is gated to `linux`/`macosx` and reports NOT RUN
   elsewhere. A Windows harness is future work.

---

## N. Queue gate impact

This review closes **only** the Mutex substrate input to B1. The Queue
production implementation remains blocked at Phase 0:

```text
B1  Mutex no-throw substrate                — PASS (this review)
B2  Queue Corrective-2 independent review   — OPEN (separate task)
B3  E12-CONDITION-T25-MIGRATION-REACQUIRE-HANG-AUDIT-1 — OPEN (separate task)
B4  Queue TLA+ formal model                 — OPEN (no .tla exists)
```

The four commits under review introduce **zero** Queue code:

* new lines matching `AsyncQueue` / `QueuePort` / `QueueCore` across the
  chain: **0 / 0 / 0**;
* new `.tla` files: **none**;
* `QueueCore::state_mtx_` / Queue `WaitQueue` mutex: **not present** (no
  Queue source exists yet);
* authority status not elevated to `QUEUE AUTHORIZED` or `CLOSED`.

A Mutex PASS here unblocks **one** of the four B1–B4 inputs. B2, B3, and B4
remain open and must be closed by their own separate reviews/tasks before
the E12-E Queue can be authorized.

---

## O. Repository state

```text
branch:           e12-e-queue-production-impl
HEAD:             e4b08b1 (docs(async): record Mutex fail-fast contract)
commits (this task):
    629617c  docs(async): independently review Mutex no-throw authority   (Commit 0)
    be07564  feat(async): make Mutex acquisition fail-fast                (Commit 1)
    e2cfe61  test(async): verify Mutex acquisition failure terminates     (Commit 2)
    e4b08b1  docs(async): record Mutex fail-fast contract                 (Commit 3)
files changed:    none in include/, src/, tests/, or xmake.lua
working tree:     this file is the only addition (docs/reviews/)
untracked files:  tests/test_t3_simple.cpp   (pre-existing, unrelated; never
                                              committed; not referenced by xmake.lua)
                  tla2tools.jar              (pre-existing, unrelated; never committed)
pushed:           no   (no upstream configured for this branch; no push / merge / PR)
```

### Verification of "no production change"

A repo-external scratch probe (`/tmp/mutex_negprobe/`) and a temporary git
worktree at the base commit `eb8d974` were used solely to verify the
counterexample, GCC-gap, and sanitizer-causality claims. Both are outside
the durable working tree. `git status --short` shows only this review
document plus the two pre-existing untracked files.

No commit, push, merge, or PR was created.
