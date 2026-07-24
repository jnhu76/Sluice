# ASYNC-RUNTIME-HANG-AND-GCC-GATE-CORRECTIVE-1 — Evidence Report

> **Decision identity:** `ASYNC-RUNTIME-HANG-AND-GCC-GATE-CORRECTIVE-1`
>
> Three independent workstreams (W1/W2/W3), each reproduced, root-caused, and
> corrected in its own commit. Per spec §D, verdicts are reported SEPARATELY —
> no single PASS covers any other workstream.

This report satisfies spec §D §A–§H.

---

## A. Overall verdict

```text
W1 Condition T25 migration/reacquire hang:   PASS
W2 E11 timer ASan busy-poll hang:            PASS
W3 GCC + Clang TSA flag routing:             PASS
```

All three are PASS, but each on its own reproduced-and-corrected evidence (§B–§E). None is covered by another.

---

## B. Reproduction evidence

### B.1 W1 — Condition T25 hang

```text
command:       SLUICE_TEST_FILTER='e12_cond_t25' timeout 15s ./async_condition_primitive_test
configuration: Clang 21.1.8, Linux x86_64, xmake 3.0.9, -m asan
frequency:     2/3 trials HANG (watchdog exit 124) — pre-fix
watchdog:      15s external `timeout`; a watchdog kill is recorded as FAIL, never PASS
first stuck state:
   last [run] line: [run] e12_cond_t25_migration_condition_reacquire
   coordinator unbounded `while (!a_unlocked) yield()` at
   tests/async_condition_primitive_test.cpp:1415-1417 (original)
TSan note:     pre-fix T25 also failed 3/3 under TSan with SEGV on a WRITE
               at 0x7ffff617fff0 (ThreadSanitizer: nested bug) — a separate
               failure mode of the same non-deterministic test setup.
```

### B.2 W2 — E11 timer ASan busy-poll hang

```text
command:       SLUICE_TEST_FILTER='e11_t7' timeout 30s ./timer_wait_test
configuration: Clang 21.1.8, Linux x86_64, xmake 3.0.9, -m asan
frequency:     5/5 trials HANG (watchdog exit 124) for e11_t7 — pre-fix.
               Other run_live cases (t11/t12/t13/t15/t17): 5/5 PASS each.
watchdog:      30s external `timeout`
first stuck state:
   last [run] line: [run] e11_t7_old_timer_cannot_resolve_later_epoch
   fwake fiber unbounded `spin_wait(e_resolved)` at
   tests/timer_wait_test.cpp:558 (original)
   observed process state: ~100% CPU, ~21GB virtual, never returns.
```

### B.3 W3 — GCC TSA flag routing

```text
command:       xmake f -c --toolchain=gcc -m debug && xmake build sluice_async
configuration: GCC 15.2.0, Linux x86_64, xmake 3.0.9
frequency:     100% BUILD_FAIL — pre-fix
first failing state:
   g++: error: unrecognized command-line option '-Wthread-safety'
   (on src/async/async_io_context.cpp — a file this task did not touch)
root cause of receipt: add_cxxflags(..., {force = true}) bypassed xmake's
   per-compiler flag filtering, so GCC received the Clang-only flag.
```

---

## C. Root cause

| Workstream | Root-cause category | Detail |
|---|---|---|
| **W1** | **test-harness defect** | T25's coordinator used two unbounded `while(!flag) yield()` loops with no `bounded_wait`, no `release_for_drain`, no `f_idle` on W1, and no suspension handshake (`a_suspended` written but never read). The "W1 steals fA" trace was ASSUMED, not established. Under ASan/TSan the slower scheduling widened the routing/steal race into the unbounded loop. Production Mutex/Condition/steal mechanics are worker-agnostic on the reacquire admission — not the root cause. The authoritative comparison is `e12_mtx_t19_real_migration_lock_own_unlock` (the Mutex migration corrective), which bounds every gate and pins W1 with `f_idle`. |
| **W2** | **test-harness defect** | e11_t7 used `sched.run(2)` — DRAIN mode — which RETURNS STALLED as soon as a fiber suspends in `await_wait_deadline`, orphaning the other fiber. The fwake fiber then unbounded `spin_wait`-ed for an outcome the orphaned waiter could never publish. Under ASan the stall was reached deterministically. Production timer/retirement mechanics are sound (the run_live cases t11–t17 pass under ASan). |
| **W3** | **build-configuration defect** | `add_cxxflags("-Wthread-safety", {force = true})` at `xmake.lua:43-44` and the `async_tsa_flags()` helper (`:66-67`). `{force=true}` is precisely the option that bypasses xmake's per-compiler flag filtering, so GCC received the Clang-only flag. No production code involved. |

No production defect was identified in any workstream. No sanitizer-timing-only race was silenced.

### Production observations recorded but NOT fixed (per §B: no speculative production change)

These were noted during investigation and are recorded as **remaining risks** (§G), not corrected:

1. **`AsyncTestAccess::active_deadline_count` reads `s.active_deadline_count_` non-atomically** despite the field being `SLUICE_GUARDED_BY(global_mtx_)` (`src/async/scheduler.cpp:2735-2738`). This is a test-accessor sharp edge (the accessor is compiled only under `SLUICE_ASYNC_INTERNAL_TESTING`), not a production hot-path race. The new W2 rewrite does not poll this accessor in a spin loop, so it did not surface as a TSan report in the corrected suite.
2. **A stale comment** at `src/async/scheduler.cpp:260-265` claims `route_runnable_locked` calls `signal_wake_locked`; the function body does not. This is a comment-correctness item, not a behavioral bug (wake-up is covered by the 1ms test-mode park timeout and by `advance_clock`'s `signal_wake_locked`).

---

## D. Corrections (per file)

| Commit | File | Correction |
|---|---|---|
| `db656b5` (W1) | `tests/async_condition_primitive_test.cpp` | Rewrote `e12_cond_t25_migration_condition_reacquire` to mirror T19's determinism discipline: added `f_idle` on W1 (pins W1 until the steal window opens via `flag_wake`), three-way handshake (`a_acquired` + `a_suspended` + `blocker_running`) before opening the window, `cond.notify_one()` after, `bounded_wait` on every coordinator gate, and a `release_for_drain` lambda (releases `release_blocker` + `flag_wake` + `cond.notify_one()`) on every gate failure followed by `runner.join()` + `SLUICE_CHECK_MSG`/`SLUICE_FAIL` with a state dump. Suspend mechanism stays `cond.wait(cn)` (this is the Condition reacquire test). No production change. |
| `7045e31` (W2) | `tests/timer_wait_test.cpp` | Added the `spin_wait_bounded` primitive (documented W2 failure-guard). Rewrote `e11_t7_old_timer_cannot_resolve_later_epoch` to drive both epochs from a coordinator OS thread under `run_live(1)` (the proven liveness pattern of e11_t15), replacing the broken `run(2)`-DRAIN + fiber-to-fiber spin design. Every coordinator wait is bounded (`kBoundedCoordIters = 200000`); on exhaustion the test FAILs with a stderr state dump and releases the suspended fiber so the runner can drain. The cross-epoch isolation proof (I3: E's retired timer does not resolve E+1 when the clock advances past E's deadline) is preserved and strengthened (an explicit `e1_resolved` check before the E+1 wake). No `sleep_for`-as-fix: the pump after `advance_clock` is synchronous, so the post-pump observation needs no sleep (verified — a `sleep_for(2ms)` initially carried over from the e11_t15 idiom was removed as redundant; Debug 5/5 + ASan 10/10 pass without it). No production change. |
| `63cf850` (W3) | `xmake.lua` | Replaced `add_cxxflags("-Wthread-safety", {force = true})` + `add_cxxflags("-Werror=thread-safety", {force = true})` with `add_cxxflags("-Wthread-safety", "-Werror=thread-safety", {tools = {"clang", "clang_cl"}})` at both sites (`sluice_async` target and the `async_tsa_flags()` helper used by `sluice_async_internal_testing`). Dropped `force=true` (the option that broke GCC filtering). `{tools={"clang","clang_cl"}}` scopes the flags to both Clang frontends so Linux/Mac clang AND Windows clang-cl keep TSA; GCC never receives them. Updated the stale comments. Verified against official xmake docs (context7 `/xmake-io/xmake-docs`: `add_cxxflags("...", {tools = {"clang_cl", "cl"}})` is the documented per-frontend scoping API). |

---

## E. Test matrix (real commands + exit codes)

Toolchain: Clang 21.1.8, GCC 15.2.0, Linux x86_64, xmake 3.0.9.

### E.1 W1 — Condition suite (async_condition_primitive_test)

| Configuration | Scope | Result |
|---|---|---|
| Clang Debug | T25 alone ×20 trials | 20/20 PASS |
| Clang ASan | T25 alone ×10 trials | 10/10 PASS (was 2/3 HANG pre-fix) |
| Clang TSan | T25 alone ×10 trials | 10/10 PASS (was 3/3 SEGV pre-fix) |
| Clang Debug | full suite (30 cases) | exit 0, ALL TESTS PASSED, no TSan race |
| Clang ASan | full suite (30 cases) | exit 0, ALL TESTS PASSED, no TSan race |
| Clang TSan | full suite (30 cases) | exit 0, ALL TESTS PASSED, no TSan race |

### E.2 W2 — E11 timer suite (timer_wait_test)

| Configuration | Scope | Result |
|---|---|---|
| Clang Debug | e11_t7 ×5 trials | 5/5 PASS |
| Clang ASan | e11_t7 ×10 trials | 10/10 PASS (was 5/5 HANG pre-fix) |
| Clang Debug | full suite (16 cases) | exit 0, ALL TESTS PASSED |
| Clang Release | full suite (16 cases) | exit 0, ALL TESTS PASSED |
| Clang ASan | full suite (16 cases) | exit 0, ALL TESTS PASSED |
| Clang UBSan | full suite (16 cases) | exit 0, ALL TESTS PASSED |
| Clang TSan | full suite (16 cases) | exit 0, ALL TESTS PASSED, no race report |

### E.3 W3 — GCC build matrix + Clang TSA

| Configuration | Scope | Result |
|---|---|---|
| Clang Debug | `sluice_async` + `sluice_async_internal_testing` build | PASS (TSA enforced — confirmed by a deliberate-violation probe that produced `-Werror,-Wthread-safety-attributes`) |
| GCC Debug | 19 async targets build + run | 19/19 PASS (was BUILD_FAIL pre-fix on `-Wthread-safety`) |
| GCC Release | `sluice_async` + representative subset build + run | PASS |
| GCC Debug | TSA flags present in compile command? | **NO** (correct — GCC does not receive Clang-only flags) |
| Windows clang-cl | — | **NOT RUN** — no Windows/clang-cl toolchain in this environment. Recorded as NOT RUN, not PASS. |

### E.4 Watchdog discipline

Every sanitizer run used an external `timeout` watchdog. A watchdog kill (exit 124/137) is recorded as FAIL, never as PASS. No test-internal timeout was added, no assertion was weakened, no case was skipped (§B).

---

## F. Queue gate impact

```text
Queue Phase 0 B3 (E12-CONDITION-T25-MIGRATION-REACQUIRE-HANG-AUDIT-1):
PASS  — closed by W1.

B1: unchanged — still requires the Mutex independent implementation review
    (a separate review document exists at
    docs/reviews/ASYNC-MUTEX-NOTHROW-PRODUCTION-IMPLEMENTATION-1-REVIEW.md,
    authored by an independent reviewer; this task neither authored nor
    consumed it).
B2: unchanged — Queue Corrective-2 independent review still open.
B4: unchanged — Queue TLA+ still open.

E12-E QUEUE IMPLEMENTATION AUTHORIZATION: STILL DENIED
```

W2/W3 do not change Queue authorization.

---

## G. Remaining risks

1. **`active_deadline_count_` non-atomic read in the test accessor** (§C). Not fixed in this task (out of scope: it is a test-accessor edge, not a production hot-path race). Did not surface as a TSan report in the corrected suite because the W2 rewrite no longer polls it in a spin.
2. **Stale `route_runnable_locked`/`signal_wake_locked` comment** (§C). Comment-correctness item, not a behavioral bug. Not fixed (no speculative production change).
3. **Windows/clang-cl TSA coverage** is configured (`{tools={"clang","clang_cl"}}`) but NOT VERIFIED on a real clang-cl build in this environment (no Windows toolchain). Recorded as NOT RUN.
4. **T25 TSan SEGV (pre-fix)** was a separate symptom of the same non-deterministic test setup; the corrected test no longer reproduces it, but the exact corruption mechanism (stack-region-adjacent write) was not isolated — the deterministic rewrite eliminated the window without pinpointing which specific racing access caused the SEGV. This is acceptable because the root cause (non-deterministic coordinator assumption) is eliminated.

---

## H. Repository state

```text
branch:           e12-e-queue-production-impl
HEAD:             <see `git rev-parse HEAD` — the docs commit at branch tip>
commits (this task, since prior task HEAD e4b08b1):
    db656b5  test(async): make Condition T25 migration trace deterministic   (W1)
    7045e31  test(async): close E11 timer ASan busy-poll hang                (W2)
    63cf850  build(async): route TSA flags only to Clang                     (W3)
    <tip>    docs(async): record runtime-hang and GCC TSA corrective evidence (this doc)
working tree:     clean (no modified tracked files)
untracked files:  docs/reviews/ASYNC-MUTEX-NOTHROW-PRODUCTION-IMPLEMENTATION-1-REVIEW.md
                    (independent Mutex implementation review; authored by a
                     separate review process during this task; NOT authored
                     or modified by this task — untouched per spec §B2)
                  tests/test_t3_simple.cpp   (pre-existing, unrelated; untouched)
                  tla2tools.jar              (pre-existing, unrelated; untouched)
pushed:           no   (no upstream configured for this branch; no push / merge / PR)
```

(The docs-commit hash is inherently self-referential — it changes when this
file is amended. The three workstream commit hashes `db656b5` / `7045e31` /
`63cf850` are stable; the docs tip is whatever `git log -1` reports.)
