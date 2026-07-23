# E13 Select Production Test Plan

**Task:** `E13-SELECT-PRODUCTION-PREPARATION-1`
**Authority:** fixes the test matrix (task section T), the negative tests
(section U), the deterministic seam strategy (section T final paragraph), and
the production implementation split (section Y). No tests are written by this
task.

---

## 1. Determinism policy

No test in this plan depends on `sleep_for` or wall-clock timing. Every test
either:

- uses the deterministic logical clock (`Scheduler::advance_clock`,
  `test_clock_mode_`), which drives the timer pump synchronously under
  `global_mtx_`; or
- uses deterministic PhaseTag seams (compiled only into
  `sluice_async_internal_testing`) to pause one thread at an exact boundary
  while another thread drives the racing operation.

This is the established E10–E12 discipline
(`scheduler.hpp:1178-1232`, `tests/async_test_control_internal.hpp`). The
production `sluice_async` target exports no test phase symbol and declares no
test friend (the `ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1` banner).

---

## 2. Positive test matrix (task section T)

Every test below lists: scenario, setup, the deterministic mechanism used, and
the invariants asserted on completion. Test IDs are `ST-1`..`ST-N` (Select
Test). The numbering is stable; reviewers cite these IDs.

| ID    | Scenario                                   | Mechanism                                | Key assertions                                                                   |
|-------|--------------------------------------------|------------------------------------------|----------------------------------------------------------------------------------|
| ST-1  | Event already set → inline winner          | Event constructed `initially_set=true`   | result.index()==0, kind==event; no caller suspension; runnable_pub==0            |
| ST-2  | Timer already due → inline winner          | `advance_clock` past deadline before call| result.index()==0, kind==timer, timer_outcome==fired; no suspension              |
| ST-3  | Event set + Timer due admission tie        | Event set, Timer deadline due, both arms | lowest index wins; the other arm finalized loser; both authorities closed        |
| ST-4  | two Events already set → lowest index      | two Events `initially_set=true`          | result.index()==0; arm 1 is loser, unlinked from its SelectPort                  |
| ST-5  | two Timers already due → lowest index      | two deadlines due via `advance_clock`    | result.index()==0; loser Timer reg RETIRED                                       |
| ST-6  | same Event appears twice                   | two cases referencing the same Event     | each arm distinct registry node; lowest ready index wins; other is loser         |
| ST-7  | Event winner + Timer loser                 | Event set, Timer not yet due, post-arm   | Event wins; Timer reg RETIRED; Timer arm not dereferenced by pump                |
| ST-8  | Timer winner + Event loser                 | Timer due, Event unset; then Event set   | Timer wins (claim first); Event arm unlinked; Event SET remains                  |
| ST-9  | post-suspension Event winner               | caller suspends, then Event set          | caller resumed; result kind==event; runnable_pub==1                              |
| ST-10 | post-suspension Timer winner               | caller suspends, then `advance_clock`    | caller resumed; result kind==timer; runnable_pub==1                              |
| ST-11 | same Event wakes two arms in one group     | one Event, two arms; Event set           | one group processed once; lowest index wins; other arm loser                     |
| ST-12 | same Event completes two groups            | one Event, two callers in separate groups| each group completes once; two independent winners                               |
| ST-13 | stale Timer after Event winner             | Event wins; Timer deadline later elapses | pump observes RETIRED; no dereference (PhaseTag E13PhaseTimerPumpSkip)           |
| ST-14 | registration rollback                      | inject `SelectRegistrationFailure` after N successful registrations (synthetic seam) | already-registered arms unlinked/retired; group Aborted; no publication; no ACTIVE SelectTimerRegistration remains in Scheduler pool; not-yet-registered blocks still locally owned in tmp_pool |
| ST-15 | external thread `Event::set`               | setter thread distinct from worker       | caller routed via `group.caller_owner_` to owner worker; result correct |
| ST-16 | multi-worker owner routing                 | multi-worker run, victim on worker k     | resume routed to owner worker k; exactly one runnable enqueue                    |
| ST-17 | exactly one runnable enqueue               | suspended completion under contention    | Fiber::runnable_count delta == 1                                                 |
| ST-18 | all loser registrations removed before resume | post-suspension winner, several losers| at resume, every SelectPort empty for the group; every Timer reg != active       |
| ST-19 | wrong Scheduler rejected                   | Event bound to other Scheduler           | `std::invalid_argument` thrown before any registration                           |
| ST-20 | Event destruction contract                 | destroy Event with active Select arm     | debug assert fires (caller contract violation)                                   |
| ST-21 | Scheduler teardown contract                | destroy Scheduler with live SelectGroup  | debug assert fires (caller contract violation)                                   |
| ST-22 | external-thread select rejected            | call `select()` from a plain OS thread   | `std::logic_error` thrown before any registration                                |
| ST-23 | wrong-current-worker Scheduler rejected    | call `select(sched, ...)` from worker of a different Scheduler | `std::logic_error` thrown before any registration |

### 2.1 Per-test invariant checklist (applies to every ST)

Every positive test asserts, in addition to its key assertions:

```text
- the winner arm:   state == Retired (winner); if Event, unlinked from SelectPort
                    if Timer, SelectTimerRegistration in CONSUMED
- every loser arm:  state == Retired (loser); Event unlinked; Timer RETIRED
- group.phase_ == Consumed at the end (caller consumed result before return)
  — test seams may observe Completed before the ConsumeResult transition
- result_publication_count == 1
- runnable_publication_count == (Inline ? 0 : 1)
- no SelectPort contains any of the group's arms at the end
- no SelectTimerRegistration for the group is ACTIVE at the end
```

These mirror the formal invariants
`C_InvAllLosersAbortedBeforeCompletion`, `C_InvCompletionRequiresAllAuthorityClosed`,
`InvNoActiveTimerAfterCompletion`, `InvEventWinnerRemovedFromEventQueue`.

---

## 3. Negative tests (task section U)

Negative tests assert that a *bug* is *caught* — either by a debug assert, by
the PhaseTag-driven deterministic reproduction of a forbidden interleaving, or
by an explicit invariant check. Each test names the bug, the deterministic
reproduction, and the assertion that fails.

| ID    | Bug                                          | Deterministic reproduction                          | Catch mechanism                                     |
|-------|----------------------------------------------|-----------------------------------------------------|-----------------------------------------------------|
| SN-1  | double claim (two arms both win)             | PhaseTag at winner CAS; force a second claim attempt | `SelectGroup::winner_` CAS fails; assert that the second claimer returns loser |
| SN-2  | double publication                           | PhaseTag at `select_publish_locked` entry; re-invoke | assert `group.phase_ != Completed` at entry (guarded) |
| SN-3  | loser publishes                              | inject a `make_runnable` call on a loser-arm path  | removed by construction; test asserts loser finalize path never reaches `route_runnable_locked` (PhaseTag counters) |
| SN-4  | Timer dereference after retirement           | PhaseTag `E13PhaseTimerPumpSkip`; force pump onto RETIRED reg | assert pump does NOT read `arm_` (instrumented load counter == 0); assert `DeadlineHeapEntry` is still in heap |
| SN-5  | Event registry node survives caller resume   | post-suspension winner; at resume, scan the Event's SelectPort | assert SelectPort is empty for the group's arms |
| SN-6  | winner changes after linearization           | PhaseTag at post-claim; attempt a second CAS with a different index | CAS fails; `winner_.load() == original`            |
| SN-7  | snapshot changes after claim                 | PhaseTag at admission snapshot; mutate an arm state after snapshot taken | admission uses an immutable local copy; assert the post-claim state matches the snapshot |
| SN-8  | rollback after suspension                    | attempt `BeginRollback` after `SuspendCaller`       | assert `ContractRollbackEnabledDomain` precondition fails |
| SN-9  | Timer loser retired before arm detach        | reorder finalize: retire CAS before arm.state = Retired | fixed source order; PhaseTag asserts the order (retire CAS timestamp > arm-state timestamp) |
| SN-10 | result published with open authority         | PhaseTag at publish; leave one arm linked           | assert at `select_publish_locked` entry: every arm authority closed |
| SN-11 | same group processed twice in one broadcast   | Phase 2 dedup bug (epoch counter bypass); visit the group twice | Phase 2 walks the intrusive worklist chain; assert each group's epoch counter matches current epoch (no duplicate links) |
| SN-12 | cross-group authority close                  | group A's finalize unlinks group B's arm            | SelectPort unlink validates `arm.group == &group`; assert rejects cross-group unlink |

### 3.1 Compile-fail tests (new)

These tests verify that the `requires` clause rejects invalid call sites at
compile time. They are `static_assert`-based or compile-fail harness tests:

| ID    | Scenario                                      | Mechanism                                |
|-------|-----------------------------------------------|------------------------------------------|
| SF-1  | zero arms rejected                            | `select(sched)` — empty pack             |
| SF-2  | too many arms rejected                        | `select(sched, c0, c1, ..., c8)` — 9 cases |
| SF-3  | wrong case type rejected                      | `select(sched, 42, event_case)` — int as case |

### 3.2 No sleep-based reproduction

Every SN test above uses a PhaseTag seam or an invariant assertion, never a
timing assumption. The seam pauses one thread at the exact boundary (e.g.
"after winner CAS, before finalize"); the test driver performs the racing
operation on the other thread; the seam releases; the assertion fires.

---

## 4. Deterministic internal-testing seams

The seams are compiled only into the `sluice_async_internal_testing` variant
under `SLUICE_ASYNC_INTERNAL_TESTING`. The production `sluice_async` target
has none of these symbols.

### 4.1 Event adapter seams

```text
E13PhaseEventScanDone       after Phase 1 scan, before Phase 2 (intrusive worklist built)
E13PhaseEventGroupClaimed   after one group's claim, before finalize
E13PhaseEventArmUnlinked    after winner arm unlink, before publish
E13PhaseEventWorklistWalk   pause mid-worklist walk (test multi-group chain)
```

### 4.2 Timer adapter seams

```text
E13PhaseTimerPumpActive     after ACTIVE check, before CandidateReady
E13PhaseTimerConsumed       after ACTIVE->CONSUMED, before arm finalize
E13PhaseTimerRetired        after ACTIVE->RETIRED on a loser
E13PhaseTimerPumpSkip       observe a non-active entry being skipped
```

### 4.3 Admission / publication seams

```text
E13PhaseAdmissionArmed      after registration, before admission snapshot
E13PhaseAdmissionClaimed    after inline claim, before finalize
E13PhaseAdmissionConsumed   after phase_ = Completed, before Consumed (test lifecycle ordering)
E13PhasePublishEntry        at select_publish_locked entry (precondition check)
E13PhasePublishDone         after phase_ = Completed
```

### 4.4 Rollback seam

```text
E13PhaseRollbackMid         after N arms registered, before FinishRegistration
                            (inject a synthetic SelectRegistrationFailure here)
```

### 4.5 Forgeable-authority exclusion

The production installed headers declare **no** test-hook type and grant **no**
test friend. The seams are reached ONLY through the non-installed internal-
testing controller. An ordinary production TU cannot name the controller type
and cannot forge the seam grant. This mirrors the existing
`ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1` discipline
(`scheduler.hpp:39-58`).

---

## 5. Test file layout (planned, not created by this task)

```text
tests/select_type_test.cpp                P1 type construction + SF-1..SF-3
tests/select_event_registry_test.cpp      P2 Event registry structural tests
tests/select_timer_registration_test.cpp  P3 Timer heap/stale tests
tests/select_claim_test.cpp               P4 claim/finalization negative tests
tests/select_inline_test.cpp            P5 ST-1..ST-8 (inline admission winners, plus ST-6 duplicate Event)
tests/select_suspended_test.cpp         P6 ST-9..ST-13 (post-suspension winners + stale)
tests/select_multi_group_test.cpp         P8 ST-11, ST-12 (multi-group shared Event)
tests/select_timer_adapter_test.cpp       P3+P5 ST-2, ST-5, ST-7, ST-8 (Timer arms)
tests/select_registration_rollback_test.cpp  P7 ST-14 (registration rollback)
tests/select_multi_worker_test.cpp      P6 ST-15, ST-16, ST-17 (external thread + routing)
tests/select_call_context_contract_test.cpp  P7 ST-18..ST-23 (lifetime + contract violations + caller validation)
tests/select_negative_test.cpp            P9 SN-1..SN-12 (negative tests)
```

Each file corresponds to a review stage in §7.

---

## 6. What is NOT tested in the first scope

These are out of scope and have no test:

```text
whole-Select cancellation (no production behavior to test)
post-suspension cancellation (deferred)
shutdown cancellation (deferred)
Semaphore / AsyncMutex / Condition / Queue arms (deferred)
wait-all (deferred)
cross-Scheduler Select (forbidden; only a rejection test ST-19)
alternative selection strategies (only Central Claim)
liveness / starvation guarantees (deferred)
```

---

## 7. Production implementation split (task section Y)

The implementation is divided into reviewable stages. Each stage lists its
allowed files, entry assumptions, exit gates, the production behavior it
enables, and the behavior still denied at its boundary.

### 7.1 P1 — type graph + group/arm lifecycle

```text
allowed files:
    include/sluice/async/select.hpp
    include/sluice/async/detail/select_port.hpp
    include/sluice/async/detail/select_registration.hpp
    include/sluice/async/scheduler.hpp          (add WorkerState::owner_scheduler)
    src/async/select.cpp                (skeleton only)
    tests/select_type_test.cpp           (NEW — type construction tests)
    tests/async_test_control_internal.hpp (SF-1..SF-3 compile-fail tests)
entry assumptions:
    E10–E12 closed; the formal model closed (PR #17/#18)
exit gates:
    SelectGroup, SelectArmSlot, SelectTimerRegistration, SelectPort
    compile and are unit-tested for construction/destruction only
    SF-1..SF-3 (compile-fail) pass
production behavior enabled:
    NONE (no select() entry point yet)
remaining denied behavior:
    everything
```

### 7.2 P2 — Event Select private registry

```text
allowed files:
    src/async/select_event.cpp          (SelectPort link/unlink + Phase 1 scan)
    include/sluice/async/event.hpp      (add the embedded SelectPort — no public accessor)
    tests/select_event_registry_test.cpp (NEW — Event registry structural tests)
entry assumptions:
    P1 types exist
exit gates:
    SelectPort::link_locked / unlink_locked / scan_locked work under global_mtx_
    Event::set() scans select_port_ after the ordinary drain (no-op without arms)
production behavior enabled:
    NONE (no admission yet; scan produces CandidateReady only)
remaining denied behavior:
    claim, finalize, publish, the select() entry point
```

### 7.3 P3 — Timer Select stable registration

```text
allowed files:
    src/async/select_timer.cpp          (SelectTimerRegistration + pump branch)
    include/sluice/async/scheduler.hpp  (deadline_heap_ → DeadlineHeapEntry;
                                         pool/heap helper signatures)
    src/async/scheduler.cpp             (deadline_heap_ migrated to DeadlineHeapEntry;
                                         pump learns the Select branch)
    tests/select_timer_registration_test.cpp (NEW — Timer heap/stale tests)
    tests/async_test_control_internal.hpp (E13PhaseTimerPumpActive, E13PhaseTimerPumpSkip)
entry assumptions:
    P1 types exist; the deadline heap accepts the new entry kind
exit gates:
    SelectTimerRegistration ACTIVE/RETIRED/CONSUMED transitions verified
    pump skips non-active entries without dereferencing arm_ (SN-4)
production behavior enabled:
    NONE (no admission yet)
remaining denied behavior:
    claim, finalize, publish, the select() entry point
```

### 7.4 P4 — Central Claim + winner/loser finalization core

```text
allowed files:
    src/async/select.cpp                (claim + finalize core)
    src/async/select_event.cpp          (Event winner/loser finalize)
    src/async/select_timer.cpp          (Timer winner/loser finalize)
    tests/select_claim_test.cpp          (NEW — claim/finalization negative tests)
    tests/async_test_control_internal.hpp (E13PhaseEventGroupClaimed, E13PhaseAdmissionClaimed)
entry assumptions:
    P2 + P3 registries work
exit gates:
    select_process_group_locked runs claim + finalize without publication
    winner/loser source order verified (section O)
    SN-1, SN-3, SN-9, SN-10 pass
production behavior enabled:
    claim + finalize core (no admission yet; test-driven via direct seam calls)
remaining denied behavior:
    admission, publication, select() entry point
```

### 7.5 P5 — registration + inline admission

```text
allowed files:
    src/async/select.cpp                (select() entry + admission scan)
    tests/select_inline_test.cpp       (NEW — inline admission tests ST-1..ST-8,
                                         including ST-6 duplicate Event in one group)
    tests/async_test_control_internal.hpp (E13PhaseAdmissionArmed, E13PhaseAdmissionClaimed,
                                           E13PhaseAdmissionConsumed)
entry assumptions:
    P4 finalize core works
exit gates:
    ST-1..ST-8 pass
production behavior enabled:
    Event already-set inline winner
    Timer already-due inline winner
    admission tie-break (lowest index)
    Event/Timer loser finalization on inline winner
    duplicate Event in one group (ST-6)
remaining denied behavior:
    suspended completion (no caller suspension yet)
```

### 7.6 P6 — suspension + result/runnable publication

```text
allowed files:
    src/async/select.cpp                (suspended admission branch;
                                         Completed → Consumed transition)
    src/async/scheduler.cpp             (select_publish_locked with fail-fast)
    tests/select_suspended_test.cpp    (NEW — suspension tests ST-9..ST-10)
    tests/select_multi_worker_test.cpp (NEW — external thread + routing ST-15..ST-17)
    tests/async_test_control_internal.hpp (E13PhasePublishEntry, E13PhasePublishDone)
entry assumptions:
    P5 inline path works
exit gates:
    ST-9, ST-10 pass
production behavior enabled:
    post-suspension Event winner
    post-suspension Timer winner
    exactly-once runnable publication
    Completed → Consumed lifecycle
remaining denied behavior:
    multi-group shared Event
```

### 7.7 P7 — rollback + destruction

```text
allowed files:
    src/async/select.cpp                (rollback path)
    include/sluice/async/detail/select_port.hpp
    (Consumed precondition for destruction)
    tests/select_registration_rollback_test.cpp  (NEW — rollback/destruction tests ST-14, SN-8)
    tests/select_call_context_contract_test.cpp  (NEW — contract tests ST-18..ST-23)
    tests/async_test_control_internal.hpp (E13PhaseRollbackMid)
entry assumptions:
    P6 publication works
exit gates:
    ST-14, SN-8 pass
production behavior enabled:
    registration-failure rollback
    destruction contract enforcement (requires Consumed or Aborted)
remaining denied behavior:
    multi-group shared Event
```

### 7.8 P8 — multi-group shared Event (intrusive worklist)

```text
allowed files:
    src/async/select_event.cpp          (Phase 2 intrusive worklist dedup + per-group iteration)
    tests/select_multi_group_test.cpp    (NEW — multi-group worklist tests ST-11, ST-12)
    tests/async_test_control_internal.hpp (E13PhaseEventScanDone, E13PhaseEventWorklistWalk)
entry assumptions:
    P7 rollback works
exit gates:
    ST-11, ST-12 pass
production behavior enabled:
    same Event shared across groups (ST-11, ST-12)
    intrusive worklist chain (no fixed-size array limit)
    (ST-6 duplicate Event in one group moved to P5)
remaining denied behavior:
    none within the first scope
```

### 7.9 P9 — full deterministic + adversarial tests

```text
allowed files:
    tests/select_*.cpp              (all test files)
    tests/async_test_control_internal.hpp (E13 PhaseTags, internal-testing variant only)
entry assumptions:
    P1–P8 complete
exit gates:
    ST-1..ST-23, SN-1..SN-12, SF-1..SF-3 all pass
    production sluice_async target exports no E13 test symbol
production behavior enabled:
    complete first-scope Select
remaining denied behavior:
    everything outside the first scope (section 0.2)
```

Each stage is independently reviewable. A stage's exit gates are the test IDs
it must pass; a stage does not merge until its gate tests are green under both
`sluice_async` and `sluice_async_internal_testing`.
