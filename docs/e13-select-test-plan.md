# E13 Select Test Plan

**Task**: `E13-SELECT-MULTI-WAIT-PREPARATION-CORRECTIVE-1`
**Parent**: `docs/e13-select-preparation.md`

---

## 1. Overview

This document defines the deterministic test matrix for E13 Select first scope
(Event + Timer). The test plan contains 17 deterministic tests (T1–T17).
All tests require the `sluice_async_internal_testing` build variant for
deterministic seam access.

---

## 2. Test Infrastructure

### 2.1 Required seams (from existing infrastructure)

| Seam | Source | Purpose |
|------|--------|---------|
| `AsyncTestAccess::enable_test_clock` | scheduler.hpp | Deterministic timer control |
| `AsyncTestAccess::set_clock` | scheduler.hpp | Advance logical clock |
| `Scheduler::advance_clock` | scheduler.hpp | Timer pump driver |
| `SchedulerTestHooks` (internal testing) | async_test_control.hpp | Admission/park coordination |
| `E12EventTestHooks` (internal testing) | async_test_control.hpp | Event set/admission seams |

### 2.2 New test seams required (E13-specific, first scope)

| Seam | Location | Purpose |
|------|----------|---------|
| `select_arm_registered` | SelectGroup | Pause after all arms registered, before admission check |
| `select_after_caller_make_waiting` | SelectGroup | Pause after caller make_waiting, before global_mtx_ release |
| `select_after_group_claim_before_retirement` | SelectGroup | Pause after claim, before loser retirement |
| `select_after_retirement_before_publication` | SelectGroup | Pause after retirement, before publication |
| `timer_due_before_group_claim` | Scheduler | Pause timer expiry before group claim |
| `event_set_before_group_claim` | Event | Pause Event::set() before group claim |
| `event_select_scan_complete_before_group_claim` | Event | Pause after Phase 1 scan, before Phase 2 group claims |
| `event_after_group_claim_before_arm_retirement` | Event | Pause after group claim, before loser arm retirement |

---

## 3. Test Matrix

### T1: Two admission-ready Event arms

```
Setup:
    Event A (SET), Event B (SET)
    Select with arm[0]=A, arm[1]=B

Action:
    Make select waiting

Expected:
    arm[0] wins (array-index priority)
    arm[1] is retired (Cancelled, no publication)
    Exactly one result publication
    Caller outcome: arm[0]'s result
    Caller never suspended (inline fast path)

Invariants verified: I1, I4, I10, I11
```

### T2: External set() serialized during R1 registration

```
Setup:
    Event A (UNSET), Event B (UNSET)
    Select with arm[0]=A, arm[1]=B

Sequence:
    1. Pause registration while global_mtx_ held
    2. External thread calls Event::set() -- blocks on global_mtx_
    3. Prove setter cannot complete and cannot inspect partial group state
    4. Complete registration (no admission-ready arm observed)
    5. Caller make_waiting, group -> Armed
    6. Release global_mtx_
    7. Setter proceeds, marks arm[0] CandidateReady, claims group, publishes once

Expected:
    arm[0] wins (post-suspension winner)
    arm[1] retired
    Exactly one publication
    Caller suspended then resumed

Invariants verified: I1, I4, I5, I11, I17
```

### T2b: Event already SET before Select acquires global_mtx_

```
Setup:
    Event A (SET), Event B (UNSET)

Sequence:
    1. Event A already SET before Select begins
    2. Select acquires global_mtx_, registers all arms
    3. Admission observation: arm[0] -> CandidateReady
    4. Inline completion

Expected:
    arm[0] wins (inline)
    arm[1] retired
    Caller never suspended
    make_runnable = 0, route_runnable = 0

Invariants verified: I1, I4, I19
```

### T3: Winner before caller suspension

```
Setup:
    Event A (SET), Event B (UNSET)

Sequence:
    1. Register arm[0] = A.wait -> CandidateReady (SET at admission)
    2. Register arm[1] = B.wait -> Armed (UNSET)
    3. make_select_waiting

Expected:
    arm[0] wins (Event already ready)
    arm[1] retired (Cancelled, no publication)
    Caller never suspends (fast path)
    Exactly one publication

Invariants verified: I1, I2, I4, I10
```

### T4: External-thread Event wake

```
Setup:
    Event E (UNSET), Event F (UNSET)

Sequence:
    1. Register arm[0] = E.wait -> Armed
    2. Register arm[1] = F.wait -> Armed
    3. make_select_waiting -> caller suspends
    4. External thread: E.set()

Expected:
    arm[0] CandidateReady -> group claim -> Winner
    arm[1] retired (Cancelled, no publication)
    Exactly one publication
    Caller resumes

Invariants verified: I1, I2, I3, I4, I10, I11
```

### T5: Timer vs Event race

```
Setup:
    Event E (UNSET), Deadline D (100ms)
    Clock at 0ms

Sequence:
    1. Register arm[0] = E.wait -> Armed
    2. Register arm[1] = D.timer -> Armed
    3. make_select_waiting -> caller suspends
    4. Advance clock to 100ms (timer expires)
    5. Pause at timer_due_before_group_claim
    6. E.set() (external thread, same instant)

Expected:
    Exactly one arm wins (CAS serializes)
    Other arm retired
    Exactly one publication
    No split-brain

Invariants verified: I1, I2, I4, I8, I10
```

### T6: Already-due Timer arm

```
Setup:
    Deadline D (0ms, already due)
    Event E (UNSET)

Sequence:
    1. Register arm[0] = D.timer -> CandidateReady (already due)
    2. Register arm[1] = E.wait -> Armed
    3. make_select_waiting

Expected:
    arm[0] wins (fast path: already CandidateReady)
    arm[1] retired
    Caller never suspends
    Timer registration consumed
    Exactly one publication

Invariants verified: I1, I2, I4, I7, I10
```

### T7: Two Event arms on same Event

```
Setup:
    Event E (SET)

Sequence:
    1. Register arm[0] = E.wait -> CandidateReady
    2. Register arm[1] = E.wait -> CandidateReady
    3. make_select_waiting

Expected:
    Both arms registered on same Event
    arm[0] wins (lower index)
    arm[1] retired (Cancelled)
    Event still SET
    Exactly one publication

Invariants verified: I1, I4, I10, I12
```

### T8: Inline completion -- no make_runnable

```
Setup:
    Event A (SET), Event B (SET)

Sequence:
    1. Register arm[0] = A.wait -> CandidateReady
    2. Register arm[1] = B.wait -> CandidateReady
    3. make_select_waiting -> inline completion

Expected:
    arm[0] wins (inline, lowest index)
    arm[1] retired (Cancelled, non-publishing)
    make_runnable count = 0 (caller was never Waiting)
    route_runnable count = 0
    SelectResult populated exactly once
    Event A and B still SET

Invariants verified: I3, I4, I10, I19
```

### T8b: Suspended completion -- exactly-one make_runnable

```
Setup:
    Event E (UNSET), Event F (UNSET)

Sequence:
    1. Register arm[0] = E.wait -> Armed
    2. Register arm[1] = F.wait -> Armed
    3. make_select_waiting -> caller suspends
    4. External thread: E.set()

Expected:
    arm[0] wins (post-suspension)
    arm[1] retired (non-publishing)
    make_runnable count = 1
    route_runnable count = 1
    Caller resumes exactly once

Invariants verified: I3, I4, I10, I20
```

### T9: Wrong Scheduler rejection

```
Setup:
    Scheduler S1, Scheduler S2
    Event E (on S2)

Sequence:
    1. Select on S1
    2. Try to add arm = E.wait (E is on S2)

Expected:
    Debug assert: cross-Scheduler arm rejected
    No registration, no group state change

Invariants verified: precondition enforcement
```

### T10: Exactly-one result/runnable publication

```
Setup:
    Event A (SET), Event B (SET)

Sequence:
    1. Register arm[0] = A.wait -> CandidateReady
    2. Register arm[1] = B.wait -> CandidateReady
    3. make_select_waiting

Expected:
    Publication count == 1
    route_runnable_locked called exactly once (or 0 for inline)
    No double-resume of caller fiber
    SelectResult populated exactly once

Invariants verified: I4, I10
```

### T11: Destruction after completed retirement

```
Setup:
    Event E (SET)

Sequence:
    1. Select arm[0] = E.wait
    2. make_select_waiting -> fast path -> Completed
    3. Read result
    4. Stack unwinds -> ~SelectOperation

Expected:
    arm[0] is Retired
    WaitNode is terminal (Woken)
    ~SelectOperation: no assert, no leak
    ~WaitNode: no assert (!is_registered())

Invariants verified: I5, I6
```

### T12a: Catchable registration exception rollback

```
Setup:
    Event E (SET)

Exception source (modeled, not real C++ exception in production):
    TimerRegistration allocation failure
    SelectOperation arm storage allocation failure
    injected registration exception seam

Sequence:
    1. Register arm[0] = E.wait -> CandidateReady
    2. Register arm[1] = fails with modeled exception
    3. Catch: retire arm[0] via non-publishing retirement seam
            resolve Cancelled + unlink
            clear typed context
            decrement waiting_waitq_count_
            mark arm RETIRED
    4. Stack unwinds

Expected:
    arm[0] WaitNode terminal (Cancelled)
    arm[0] WaitNode !is_registered()
    arm[0] WaitNode user_kind == None
    ~SelectOperation: no assert (all arms terminal)
    No dangling registrations
    No caller publication
    No SelectResult

Invariants verified: I5, I6, I7, I13
```

### T12b: Assertion/death test — misuse detection

```
Setup:
    Event E (SET)

Use case:
    same WaitNode instance reused across arms
    same arm slot registered twice
    cross-Scheduler misuse

Sequence:
    1. Attempt registration with reused WaitNode
    2. Debug assertion fires

Expected:
    debug assertion / process termination
    NOT catchable as a normal C++ exception
    Death-test harness isolates the process (if used)
    No cleanup continuation after fatal assertion

Invariants verified: precondition enforcement
```

### T13: Timer stale-entry retirement

```
Setup:
    Event E (SET), Deadline D (1000ms)
    Clock at 0ms

Sequence:
    1. Register arm[0] = E.wait -> CandidateReady
    2. Register arm[1] = D.timer -> Armed
    3. make_select_waiting -> fast path, arm[0] wins
    4. Advance clock to 1000ms
    5. Timer pump scans

Expected:
    Timer registration retired (ACTIVE -> RETIRED) during loser retirement
    Stale pump sees RETIRED, does NOT dereference WaitNode/group
    No UAF, no assertion

Invariants verified: I7, I13
```

### T14: Multi-worker Event/Timer stress

```
Setup:
    4 Workers, 100 Fibers, each with Select(2 arms)
    arm[0] = Event E (UNSET)
    arm[1] = Deadline D (100ms)
    Producer fibers: E.set() in random order

Sequence:
    Spawn all 100 Select fibers
    Spawn 100 producer fibers randomly setting Event

Expected:
    All 100 Select fibers resume exactly once
    No assertion failures under ASan + TSan
    No deadlock
    Event persistent SET preserved

Invariants verified: I1, I2, I3, I4, I10 under concurrency
```

### T15: TSan coordination verification

```
Setup:
    Full T14 scenario with Thread Sanitizer build

Expected:
    No TSan warnings
    Correct acquire-release pairing on:
        group_state atomic
        WaitNode::state_ atomic
        SelectArmMetadata::state (under global_mtx_)
```

### T16: Authority probe -- user_kind discriminator

```
Setup:
    One Queue operation, one Select operation, one ordinary Event wait

Sequence:
    1. Register Queue WaitNode with user_kind=Queue
    2. Register Select Event arm WaitNode with user_kind=Select
    3. Register ordinary Event WaitNode with user_kind=None
    4. Event::set() fires

Expected:
    Queue-tagged node: enters Queue reconciler path, not Select path
    Select-tagged node: enters Select resolver, set CandidateReady
    None-tagged node: ordinary wake path, resolve Woken, publish
    No wrong-context cast (debug assert if any occurs)

Invariants verified: I14, I15
```

### T17: ACTIVE authority lease serialization

```
Seam:
    timer_after_active_check_before_node_dereference

Setup:
    Event E (UNSET), Deadline D (100ms)
    Clock at 0ms

Sequence:
    1. Register arm[0] = E.wait -> Armed
    2. Register arm[1] = D.timer -> Armed
    3. make_select_waiting -> caller suspends
    4. Advance clock to 100ms (timer expires)
    5. Timer pump acquires global_mtx_, checks reg == ACTIVE
    6. Pause pump before reg.node() dereference
    7. Another thread fires E.set()
    8. Prove E.set() blocks on global_mtx_ (pump holds it)
    9. Release pump
    10. Pump safely dereferences node, marks CandidateReady, claims group,
        consumes registration, expire_locked winner, finalizes
    11. Loser Event arm retired
    12. No UAF, no double retire, no double publication

Expected:
    Timer arm wins:
        WaitNode outcome == Expired
        TimerRegistration == CONSUMED
    Event arm loses:
        WaitNode outcome == Cancelled
        Event arm terminal + detached
    Exactly one SelectResult publication
    Exactly one suspended runnable publication
    No double retire
    No stale dereference

Invariants verified: I7, I8, I10, I29, I30, I31
```

---



### 3.2 Explicit winner/loser verification checks

Every test with a winner and loser must verify:

```
WINNER:
    WaitNode outcome == Woken (Event winner) or Expired (Timer winner)
    WaitNode !is_registered()
    WaitNode user_kind == None (after clear)
    arm.metadata.state == RETIRED

LOSER:
    WaitNode outcome == Cancelled
    WaitNode !is_registered()
    WaitNode user_kind == None (after clear)
    arm.metadata.state == RETIRED

QUEUE MEMBERSHIP:
    Event winner: not present in Event queue after Phase 2
    Timer winner: not present in timer_waiters_ after expiry

COMPLETED OPERATION:
    no Registered WaitNode for any arm
    typed context cleared for every arm
    waiting_waitq_count_ correctly closed (0 remaining for this group)
    active_deadline_count_ correctly closed (0 remaining for timer arms)
```

These checks apply to T1, T2, T2b, T3, T4, T5, T6, T7, T8, T8b, T10, T11, T12a, T13, T17.

---

## 4. Future Test Obligations (Deferred)

### Semaphore arms (when Select-aware seams are designed)

```
T18: Semaphore admission-ready arm vs Event
T19: Semaphore queued release vs Event
T20: Semaphore loser -- permit conservation
T21: Two Semaphore arms on same Semaphore
```

### AsyncMutex arms (when Select-aware seams are designed)

```
T22: Mutex admission-ready arm vs Event
T23: Mutex unlock handoff vs Event
T24: Mutex loser -- single-owner preservation
T25: Mutex handoff with mixed Select/ordinary waiters
```

### Queue arms (when Queue cancel is designed)

```
T26: Queue pop arm wins -- payload in SelectResult
T27: Queue pop arm loses -- payload NOT moved
T28: Queue push arm wins -- item committed
T29: Queue push arm loses -- item returned to caller
```

### AsyncCondition arms (when Condition Select integration is designed)

```
T30: Condition arm wins at wake -- Mutex reacquire in progress
T31: Condition arm loses before reacquire
T32: Condition arm loses after reacquire started
```

---

## 5. Test Build Requirements

```
All E13 tests require:
    - sluice_async_internal_testing variant (deterministic seams)
    - Clang ASan + LSan (memory safety)
    - Clang TSan (thread safety)
    - Debug build (assertions enabled)

Test file naming convention:
    select_test.cpp         -- core Select tests (T1-T17)
    e13_select_authority_probe.cpp -- authority/seal verification
    select_stress_test.cpp  -- high-contention stress (T14-T15)

Formal model:
    TLA+ spec for Candidate A protocol (Event + Timer)
    TLC model check with:
        2-4 arms, Event + Timer
        All negative models (NEG-1 through NEG-7, NEG-T1)
```

---

## 6. Verification Matrix

| Test | I1 | I2 | I3 | I4 | I5 | I6 | I7 | I8 | I9 | I10| I11| I12| I13| I14| I15| I16| I17| I18| I19| I20|
|------|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|:--:|
| T1   | X  |    |    | X  |    |    |    |    |    | X  | X  |    |    |    |    |    |    |    | X  |    |
| T2   | X  |    |    | X  | X  |    |    |    |    | X  | X  |    |    |    |    |    | X  |    |    |    |
| T2b  | X  |    |    | X  |    |    |    |    |    | X  |    |    |    |    |    |    |    |    | X  |    |
| T3   | X  | X  |    | X  |    |    |    |    |    | X  |    |    |    |    |    |    |    |    | X  |    |
| T4   | X  | X  | X  | X  |    |    |    |    |    | X  | X  |    |    |    |    |    |    |    |    |    |
| T5   | X  | X  |    | X  |    |    |    | X  |    | X  |    |    |    |    |    |    |    |    |    |    |
| T6   | X  | X  |    | X  |    |    | X  |    |    | X  |    |    |    |    |    |    |    |    |    |    |
| T7   | X  |    |    | X  |    |    |    |    |    | X  |    | X  |    |    |    |    |    |    |    |    |
| T8   |    |    | X  | X  |    |    |    |    |    | X  |    |    |    |    |    |    |    |    | X  |    |
| T8b  |    |    | X  | X  |    |    |    |    |    | X  |    |    |    |    |    |    |    |    |    | X  |
| T9   |    |    |    |    |    |    |    |    | X  |    |    |    |    |    |    |    |    |    |    |    |
| T10  |    |    |    | X  |    |    |    |    |    | X  |    |    |    |    |    |    |    |    |    |    |
| T11  |    |    |    |    | X  | X  |    |    |    |    |    |    |    |    |    |    |    |    |    |    |
| T12a |    |    |    |    | X  | X  | X  |    |    |    |    |    | X  |    |    |    |    |    |    |    |
| T12b |    |    |    |    |    |    |    |    | X  |    |    |    |    |    |    |    |    |    |    |    |
| T13  |    |    |    |    |    |    | X  |    |    |    |    |    | X  |    |    |    |    |    |    |    |
| T14  | X  | X  | X  | X  |    |    |    |    |    | X  |    |    |    |    |    |    |    |    |    |    |
| T15  | X  | X  | X  | X  |    |    |    |    |    | X  |    |    |    |    |    |    |    |    |    |    |
| T16  |    |    |    |    |    |    |    |    |    |    |    |    |    | X  | X  |    |    |    |    |    |
| T17  |    |    |    |    |    |    | X  | X  |    | X  |    |    |    |    |    |    |    |    |    |    |

### 6.1 Supplemental Timer-authority matrix

The main matrix (§6) covers invariants I1–I20. The Timer pre-dereference
authority invariants (I29, I30, I31) and the NEG-T1 stale-pump-dereference
negative model are tracked here to avoid widening the main matrix to 31 columns.

| Test | I29 | I30 | I31 | NEG-T1 |
|------|:---:|:---:|:---:|:------:|
| T13  |  X  |     |     |   X    |
| T17  |  X  |  X  |  X  |   X    |
