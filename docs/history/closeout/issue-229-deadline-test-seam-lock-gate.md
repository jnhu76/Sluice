> **HISTORICAL / EVIDENCE — NOT CURRENT AUTHORITY.** Archived from `docs/architecture/` by S0-DOCS (#290, 2026-09-04). Point-in-time record; do not cite as authority for new decisions.

# Issue #229 — Deadline Test-Seam Observation Race: Compliance Gate

**Change class:** focused correctness repair of a test-seam data race
(Issue #229), discovered under TSan during the #227 Phase 1 freeze
verification. Pre-existing; no involved file was touched by #227 Phase 0.
**Baseline master SHA:** `a38df5e9a7bee3603a439857f036de2b5a136bf2`
(v0.0.1 baseline; the fetch at repair time confirms `master` has not moved).
**Generic gate:** `docs/architecture/design-compliance-gate.md` (this document
covers every Gate 0–4 field for the fix and links back).
**Issue:** https://github.com/jnhu76/Sluice/issues/229

**One-line defect:** the internal-testing accessor
`Scheduler::AsyncTestAccess::active_deadline_count()` returned
`active_deadline_count_` with an unlocked read while every production
registration/retire path mutates that counter under `global_mtx_` — a TSan
data race when a live coordinator fiber polled the seam while waiter fibers
registered deadlines on other workers (~10% per focused run; observed twice in
41 pre-fix runs, rc=66).

---

## Gate 0 — Architecture Classification

```text
Affected capability:    Scheduler timer accounting observation (test seam)
Affected layer:         E7-E13 scheduler internal-testing access surface
                        (src/async/scheduler.cpp out-of-line AsyncTestAccess;
                        src/async/scheduler_test_access.hpp declaration)
Classification:         Corrective (test-only observation synchronizes with the
                        existing production synchronization authority; no
                        production behavior, state, or layout change)
Governing ADR:          ADR-execution-model.md §9.4.5 (park/wake epoch
                        protocol) — UNCHANGED by this fix; the fix merely makes
                        a test observation respect the lock domain those
                        sections assume (global_mtx_ owns timer accounting)
Conformance map change: no
Constitution rules:     AC-10 (implementation authority alignment: the test
                        seam now reads the counter through the same lock that
                        guards it, matching the GUARDED_BY declaration and the
                        sibling `earliest_active_deadline` /
                        `waiting_select_count` locked-snapshot precedent)
```

The generic-gate trigger list is not engaged on the production side: no
Scheduler wake/progress/park domain, no synchronization primitive, no state
machine, and no resource model changes; the repair adds one `LockGuard`
acquisition to an already-lock-taking internal-testing accessor family
(`earliest_active_deadline`, `waiting_select_count`, `register_test_deadline`
already take `global_mtx_`). The task (issue #229) explicitly requires the
locked-snapshot design and the §8 preflight documentation, which this note
provides.

## Root cause (proven; TSan report reproduced pre-fix)

```text
Write: Scheduler::await_wait_deadline, src/async/scheduler_timer.cpp:111
       ++active_deadline_count_ under global_mtx_ (write M0) + queue mutex
       (write M1); reached via waiter fiber entry
       tests/timer_wait_test.cpp:843 (timer_new_earlier_deadline_becomes_earliest)
Read:  Scheduler::AsyncTestAccess::active_deadline_count,
       src/async/scheduler.cpp:2034 — NO lock held (SLUICE_NO_THREAD_SAFETY_ANALYSIS)
       via TimerTestControl::active_deadline_count (tests/async_test_control.hpp:288)
       from the coordinator fiber spin-poll tests/timer_wait_test.cpp:850
       (run_live(3): coordinator fiber on its own worker polls the seam while
       waiter fibers register deadlines on other workers)
Result: TSan data race on a non-atomic std::size_t — UB per the C++ memory
       model; the repository's own TSan gate is flaky (~10%/focused run).
```

Pre-fix focused reproduction: 2 races in 41 focused runs of
`SLUICE_TEST_FILTER=timer_new_earlier_deadline_becomes_earliest xmake run
timer_wait_test` (full report captured verbatim; write+read stacks match the
issue's evidence exactly).

## Call-site and sibling-seam audit (issue §6, §7)

61 call sites of `TimerTestControl::active_deadline_count` across 12 test
files were audited for invocation context:

```text
A — external coordinator thread: yes (timer_t7 failure dumps at
    timer_wait_test.cpp:638/654/692 run while the runner thread is live)
B — live Scheduler worker/fiber: yes — exactly one live-poll:
    timer_wait_test.cpp:850 (the reported race). No other fiber-entry
    lambda calls this seam (braced-lambda scan of all set_entry bodies).
C — callback with global_mtx_ NOT held: all test call sites qualify
    (fiber entries run with no lock held; production never invokes test code)
D — callback/predicate while global_mtx_ IS held: NONE
E — after Scheduler quiesced: yes (remaining 57 sites; post-run()/join
    coordinator-thread checks)

Callers already holding global_mtx_: NONE (test code never acquires G
directly; threads/fibers only acquire it briefly inside production entry
points that release before returning).
Result: locked snapshot is deadlock-safe (see Gate 3).
```

Sibling seams under the same authority (`global_mtx_`-guarded timer state)
were audited for the same defect class per issue §7:

| seam | state observed | production mutation lock | concurrent use today | fix in #229? |
|------|----------------|--------------------------|----------------------|--------------|
| `timer_pool_size` | `timer_pool_.size()` | `global_mtx_` | quiescent only (post-run/join checks) | no — leave alone + proof recorded |
| `deadline_heap_size` | `deadline_heap_.size()` | `global_mtx_` | quiescent only | no — leave alone + proof recorded |
| `timer_pool_count_in_state` | iterates `timer_pool_` | `global_mtx_` | quiescent only | no — leave alone + proof recorded |
| `select_timer_pool_size` | `select_timer_pool_.size()` | `global_mtx_` | quiescent only | no — leave alone + proof recorded |
| `select_timer_count_in_state` | iterates `select_timer_pool_` | `global_mtx_` | quiescent only | no — leave alone + proof recorded |
| `tagged_heap_counts_by_kind` | iterates `deadline_heap_` | `global_mtx_` | quiescent only | no — leave alone + proof recorded |
| `deadline_heap_has_select_target` | iterates `deadline_heap_` | `global_mtx_` | quiescent only | no — leave alone + proof recorded |

**Leave-alone proof (issue §7):** every sibling call site (zero spin-polls,
zero `while` loops, zero fiber-entry lambdas — verified by automated scans)
runs on the coordinator thread between `run()`/`run_live()` invocations or
after worker join, when no worker thread is live; there is no concurrent
mutation window. `active_deadline_count` is the only seam with a live-run
call site (timer_wait_test.cpp:850), so it is the only seam fixed in #229.
The sibling gaps are recorded here; the six-domain campaign (#225/#226) can
revisit them if the seam family is ever called from live runs.

Note the in-repo precedent this fix follows: the sibling accessors
`earliest_active_deadline` (scheduler.cpp:2047-2051) and `waiting_select_count`
(scheduler_test_access.hpp:437-440) already take `global_mtx_` for exactly this
class of observation.

## Seam contract (issue §8)

A timer-state test accessor that may execute concurrently with production
mutation must observe that state under its authoritative synchronization
domain. `active_deadline_count()` therefore takes `global_mtx_`. Existing
sibling diagnostics remain quiescent-only and are NOT generalized by this
fix: `timer_pool_size`, `deadline_heap_size`, `timer_pool_count_in_state` and
the select-timer accessors stay unlocked with their quiescent-use
precondition (audit table above), to be revisited by a later observability
audit. Per fixed accessor the contract is one safe instantaneous
observation per call — no transactional consistency across separate calls,
no lock-free observation, no real-time monotonicity, and no atomicity across
multiple timer structures.

---

## Gate 1 — Ownership and State Machine

```text
STATE AUTHORITY:  active_deadline_count_ remains global_mtx_-guarded.
                  The accessor reads it only under global_mtx_ (unchanged
                  ownership; the repair restores observation to the declared
                  lock domain).
STATE MACHINE:    no lifecycle state machine is added or modified — the sole
                  change is where an observation happens (inside the lock
                  scope). TimerRegistration states, deadline accounting
                  semantics, and earliest-active-deadline precedence are
                  untouched.
```

## Gate 2 — Resource and Failure Model

```text
RESOURCE MODEL:   unchanged. No allocation is added (LockGuard on an existing
                  std::mutex). No capacity, accounting, or reclamation change.
FAILURE MODEL:    LockGuard cannot fail (no throw, no allocation). The
                  accessor remains noexcept, matching its prior signature and
                  its locked siblings.
```

## Gate 3 — Progress and Wake Model

```text
WAKE/PROGRESS MODEL: unchanged — the fix adds no wake, no park, no timeout,
                     and no polling dependency. The observation may briefly
                     block acquiring the existing Scheduler authority; this
                     adds no new wait-for edge or circular dependency.

DEADLOCK/LIVENESS PROOF (issue §9):
  9.1 Park discipline: no physical Fiber context switch occurs while
      global_mtx_ is held. Verified across every wait family: "Only
      context_switch is outside the lock" (scheduler_timer.cpp:94/152-161;
      scheduler_event.cpp:233/290-306; scheduler_condition.cpp:99/107-110;
      scheduler_semaphore.cpp:62/131-136; scheduler_mutex.cpp:62; scheduler
      _queue.cpp; scheduler_rwlock.cpp; scheduler_park_wake.cpp:856-916;
      select.cpp:1187-1201), commit_suspend_locked runs inside the G scope but
      the physical switch follows the scope close; the worker park
      (park_on_wake_source, scheduler_park_wake.cpp:205) is explicitly
      "called with global_mtx_ RELEASED" and sleeps via the wake-domain cv
      after releasing wake_mtx_; run_next_on's fiber switch
      (scheduler.cpp:1313-1345) holds no lock.
  9.2 Callers already holding G: none — all 61 call sites are test code
      (coordinator thread, coordinator fiber entries, quiescent checks); the
      call-site audit classified every site (above). Non-recursive lock is
      safe.
  9.3 Lock order: the accessor acquires G only; no G->WaitQueue,
      G->registry, or G->backend edge is added. The production lock order
      (G -> queue mutex) is unchanged and untouched by the accessor.
  9.4 Scheduler progress / circular wait: a caller blocks only until the
      current G holder's short critical section completes; holders never
      sleep or switch while holding G (9.1), and holders never wait for the
      observer to make progress (timer admission/retire CSs are
      self-contained). Additionally, the exact pattern is already exercised
      pre-fix in the same test: the coordinator fiber's spin-poll loop calls
      the locked `earliest_active_deadline` (timer_wait_test.cpp:853) under
      run_live(3) without deadlock. Brief blocking on the existing Scheduler
      authority is acceptable per issue §9.4.
```

## Gate 4 — Evidence Plan

```text
PRE-FIX REPRODUCTION (actual):
  base: a38df5e (clean master, v0.0.1 baseline)
  config: xmake f -m tsan --toolchain=clang -y; xmake build -g test
  command: SLUICE_TEST_FILTER=timer_new_earlier_deadline_becomes_earliest
           xmake run timer_wait_test
  attempts: 41 (1 + 40) | races detected: 2 | rc=66 with full report
  read stack: scheduler.cpp:2034 via async_test_control.hpp:288
  write stack: scheduler_timer.cpp:111 (await_wait_deadline, M0+M1 held)
  -> the intended race, matching issue #229 evidence exactly.

POST-FIX (actual results filled after execution):
  focused normal:        PASS — timer_new_earlier_deadline_becomes_earliest
                         runs green in the Debug-configured build
  focused TSan repeat:   50/50 runs clean — 0 races, 0 hangs, 0 non-zero
                         exits (pre-fix: 2 races in 41 runs); combined with
                         the Gate 3 lock-authority proof, the race is closed
  full TSan suite:       PASS — xmake run -g test (tsan, all binaries):
                         rc=0, 0 ThreadSanitizer warnings
  full Debug:            PASS — xmake f -m debug --toolchain=clang -y;
                         sluice_core + sluice_async + full test group build;
                         xmake test -v: 190/190 passed, 0 failed (rc=0)
  Release:               PASS — xmake f -m release --toolchain=clang -y;
                         sluice_core + sluice_async + full test group build;
                         xmake test -v: 190/190 passed, 0 failed (rc=0)
                         (Debug config restored afterward)
  mechanical gates:      doc links PASS; verify-architecture-docs PASS;
                         mechanical-facts PASS (LOC ledger + tracker refs
                         updated); assert-hygiene PASS; git diff --check PASS

FORMAL MODEL:  unchanged — the fix touches no state transition, admission
               rule, queue bound, terminal winner, wake rule, lifecycle,
               generation rule, or shutdown behavior; no model update is
               required (AGENTS.md §17: "update the matching model or
               explicitly document why no existing model applies" — the
               observation path is not modeled, and the modeled properties
               are untouched; recorded here).

ZIG DIVERGENCE: none — no production semantics change; zeroth-stage
                divergence classification is unchanged (the seam is
                SLUICE_ASYNC_INTERNAL_TESTING-only, absent from production
                output; Zig reference is unaffected).
```

## Change summary (issue §11)

```text
src/async/scheduler.cpp
  Scheduler::AsyncTestAccess::active_deadline_count(): acquires global_mtx_
  (LockGuard), returns the snapshot; SLUICE_NO_THREAD_SAFETY_ANALYSIS
  removed; comment updated to state the lock authority and why it differs
  from the quiescent-only size diagnostics.
src/async/scheduler_test_access.hpp
  Declaration comment updated to record the synchronized-snapshot contract
  and the sibling distinction.
docs/history/closeout/issue-229-deadline-test-seam-lock-gate.md
  This note (indexed in docs/architecture/README.md).
```

Explicitly NOT changed (issue §12): timer subsystem design, timer state
machines, timer data structures, active-deadline accounting semantics,
deadline winner precedence, `earliest_active_deadline_` semantics, wait
registration, Scheduler worker loops, `park_on_wake_source`, public async
API, atomic conversion of `active_deadline_count_` (rejected default — the
locked-snapshot design is proven possible, so mixed synchronization authority
is avoided), backend behavior, and #225/#226 scope.

## Production-impact proof (issue §15)

```text
production runtime semantics changed: NO
production hot path changed:          NO
production layout changed:            NO
public API changed:                   NO
```