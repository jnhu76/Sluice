# E13 Select / Multi-Wait Preparation Design

**Task**: `E13-SELECT-MULTI-WAIT-PR16-CORRECTIVE-1`

**Status**: `PR16 CORRECTIVE — DELTA RE-AUDIT REQUIRED`

**Pre-Reaudit Hardening**: `E13-SELECT-MULTI-WAIT-PREPARATION-CORRECTIVE-1-PRE-REAUDIT-HARDENING-1`

**Corrective Authority**: `docs/reviews/E13-SELECT-MULTI-WAIT-PREPARATION-AUDIT-1.md`

---

## 0. Repository Baseline

```text
REPOSITORY:          jnhu76/Sluice
BASE_BRANCH:         master
BASE_COMMIT:         be70fde (origin/master HEAD)
WORKING_BRANCH:      feature/E13-preparation
TASK:                E13-SELECT-MULTI-WAIT-PREPARATION-CORRECTIVE-1
DATE:                2026-07-19
```

### Known governance status

```text
E10 WaitNode / WaitQueue:                       CLOSED
E11 Deadline / Timer:                           CLOSED
E12-A Event:                                    CLOSED
E12-B Semaphore:                                CLOSED
E12-C AsyncMutex:                               CLOSED
E12-D AsyncCondition:                           CLOSED
E12-E AsyncQueue<T>:                            IMPLEMENTATION COMPLETE
E12-F RwLock:                                   DEFERRED
E12-G Cross-Primitive Semantic Closure:         CLOSED / AUTHORIZED
```

### E13 First Scope (corrected)

```text
E13 FIRST SCOPE:
EVENT WAIT + TIMER ARM

SEMAPHORE:
DEFERRED — SELECT-AWARE ADMISSION AND RELEASE SEAMS NOT DESIGNED

ASYNC MUTEX:
DEFERRED — SELECT-AWARE ADMISSION AND HANDOFF SEAMS NOT DESIGNED

ASYNC QUEUE PUSH/POP:
DEFERRED — PAYLOAD OWNERSHIP AND DIRECT-HANDOFF PROTOCOL UNPROVEN

ASYNC CONDITION:
DEFERRED — MANDATORY NON-CANCELLABLE MUTEX REACQUIRE IS
INCOMPATIBLE WITH THE FIRST-SCOPE SINGLE-EPOCH ARM MODEL

CROSS-SCHEDULER SELECT:
DEFERRED
```

### Cross-primitive closure constraint

```text
primitive cancel is NOT Select-level loser authority

E13 requires a parent/group claim that orders:
    group winner selection
        BEFORE
    irreversible primitive resource commit
```

This document does not override or bypass this constraint.

---

## 1. Scope and Goals

### 1.1 What is Select

Select is a multi-wait coordination mechanism that allows one Fiber to register
waiting on multiple asynchronous alternatives simultaneously, with EXACTLY ONE
winner determining the outcome. When any arm becomes ready, the group arbitrates
a single winner, commits that arm's result, retires all losers without
publication, and publishes the caller runnable once.

### 1.2 Design goals for this preparation

1. Define first scope: Event wait + Timer arm.
2. Define ownership, lifecycle, and state machines for core types.
3. Establish the winner-before-commit protocol.
4. Specify loser retirement semantics without caller publication.
5. Prove exactly-once publication.
6. Specify Select-aware internal seams for Event and Timer resolution.
7. Identify what evidence is needed before production authorization.

### 1.3 Non-goals

- Final public C++ API names (design-level names only).
- Formal TLA+ specification (deferred to formal-model phase).
- Production implementation (denied by this task).
- Semaphore, AsyncMutex, Queue, or AsyncCondition integration.

---

## 2. First-Scope Arm Selection

### 2.1 Arm support matrix

| Arm                 | First scope | Reason |
|---------------------|:-----------:|--------|
| Event wait          | YES         | Pure readiness flag; no resource consumed. Persistent SET. |
| Timer / deadline    | YES         | Independent timer arm; no resource mutation. I4 lifetime closure. |
| Semaphore acquire   | DEFERRED    | Select-aware admission and release seams not designed. |
| AsyncMutex lock     | DEFERRED    | Select-aware admission and handoff seams not designed. |
| Queue push          | DEFERRED    | Payload ownership; no v1 cancel API. |
| Queue pop           | DEFERRED    | Payload move irreversible; peek-before-claim unproven. |
| AsyncCondition wait | DEFERRED    | Mandatory non-cancellable Mutex reacquire incompatible. |

### 2.2 Recommendation

```text
FIRST SCOPE: Event + Timer

First-scope Event and Timer integration requires narrow Select-aware
internal resolution and retirement seams. These seams are specified by
this preparation design but are not implemented by this task.

Existing single-wait behavior remains unchanged for ordinary WaitNodes.
```

---

## 3. Candidate Protocol Analysis

### 3.1 Candidate A — Central SelectGroup Claim Under Scheduler Authority

**Protocol**:

```text
arm reaches CandidateReady
    |
    v
under Scheduler::global_mtx_ coordination domain:
    group.try_claim(arm_index)
    |
    v
claim succeeds (CAS: OPEN/ARMED -> CLAIMED(index))
    |
    v
commit winner result (Event: Woken; Timer: Expired)
    |
    v
retire all loser arms without publication
    |
    v
publish caller exactly once
```

**Event adaptation**:

Event has no irreversible resource commit. The arm observes `set_ == true`.
The Select-aware resolution path sets CandidateReady instead of resolving
Woken. The group claim selects the winner. The winner's WaitNode is resolved
Woken after group claim.

```text
Event arm ready at admission or via set():
    -> arm CandidateReady (WaitNode stays Registered)
    -> group.try_claim(i) -> success
    -> resolve winner WaitNode Woken
    -> retire losers
    -> publish caller
```

**Timer adaptation**:

Timer has no irreversible resource commit. The timer pump's Select-aware
path sets CandidateReady instead of resolving Expired. The group claim
selects the winner.

```text
Timer expires -> pump's Select-aware path -> arm CandidateReady
    -> group.try_claim(i) -> success
    -> resolve winner WaitNode Expired
    -> retire losers
    -> publish caller
```

### 3.2 Candidate C — Rejected

Candidate C commits primitives before group claim, then compensates losers.
Rejected: primitive commit + compensation is fundamentally unsound for
resource-bearing primitives. This analysis is preserved from the original
design. The first scope avoids this by excluding resource-bearing primitives.

### 3.3 Protocol selection

```text
SELECTED: Candidate A — Central SelectGroup Claim Under Scheduler Authority

Rationale:
- Event and Timer integrate cleanly (no resource commit to defer).
- The group claim is serialized under the existing global_mtx_ domain.
- Select-aware resolution seams intercept Event set() and timer expiry
  to set CandidateReady instead of resolving WaitNodes.
- Loser arms are retired by a non-publishing retirement seam.
```

---

## 4. WaitNode Metadata and Type-Safe Discriminator

### 4.1 WaitNodeUserKind

The existing `WaitNode::user_` (`void*`) is ambiguous: Queue uses it for
`QueueWaitCtx*`, and Select would use it for `SelectArmMetadata*`. A raw
`static_cast` based on offset-0 layout is not production authority.

The design introduces an explicit kind tag:

```
enum class WaitNodeUserKind : uint8_t {
    None   = 0,   // ordinary single-wait (no user context)
    Queue  = 1,   // E12-E QueueWaitCtx
    Select = 2,   // E13 SelectArmMetadata
};
```

Conceptual API on WaitNode:

```
void set_user_context(WaitNodeUserKind kind, void* ptr);
WaitNodeUserKind user_kind() const;
void* user_pointer() const;
void clear_user_context();
```

### 4.2 SelectArmMetadata

```
SelectArmMetadata {
    SelectGroup* group;       // owning group (non-null while armed)
    uint8_t arm_index;        // position in group arm array
    enum ArmKind : uint8_t {
        EventWait = 0,
        TimerDeadline = 1,
    } arm_kind;
    enum SelectArmState : uint8_t {
        DETACHED = 0,
        REGISTERING = 1,
        REGISTERED = 2,
        CANDIDATE_READY = 3,
        WINNER = 4,
        LOSER = 5,
        RETIRED = 6,
    } state;
};
```

### 4.3 Type-safe resolver check

```
select_event_candidate_ready_locked(WaitNode& node):
    PRE: global_mtx_ held, Event queue mtx_ held
    switch (node.user_kind()):
        case WaitNodeUserKind::None:
            // ordinary waiter: resolve Woken, unlink, publish
            return
        case WaitNodeUserKind::Queue:
            // Queue context: resolve Woken, unlink, publish (Queue reconciler)
            return
        case WaitNodeUserKind::Select: {
            auto* meta = static_cast<SelectArmMetadata*>(node.user_pointer());
            if (meta->arm_kind == ArmKind::EventWait &&
                meta->state == REGISTERED) {
                meta->state = CANDIDATE_READY;
                // do NOT resolve WaitNode; do NOT publish
                return;
            }
            // RETIRED/LOSER/WINNER: group owns resolution
            return;
        }
```

### 4.4 Authority probes

```
P1: A Queue-tagged WaitNode must never enter the Select resolver path.
    The resolver reads user_kind() == Queue and falls through to ordinary
    or Queue path. No SelectArmMetadata* cast is attempted.

P2: A Select-tagged WaitNode must never enter Queue reconciliation.
    The Queue reconciler reads user_kind() == Select and treats it as
    an unrecognized context (assert in debug, no-op in release).

P3: user_kind and user_pointer are installed before WaitNode registration.
    They are cleared only after the node is terminal and unlinked.
```

### 4.5 Existing Queue integration

The existing Queue path must be updated to set `user_kind = Queue` when
installing `QueueWaitCtx`. The `user_pointer()` accessor is used to read
the `QueueWaitCtx*`. No structural change to Queue internals — only the
kind tag is added alongside the existing pointer. The `arm_kind` field
in `SelectArmMetadata` is NOT used to distinguish Queue from Select;
the kind tag is the sole discriminator.

---

## 5. Type and Ownership Graph

### 5.1 Type definitions

```
SelectGroup          -- the coordination container for one multi-wait operation
SelectArm            -- one registered alternative within a SelectGroup
SelectArmMetadata    -- per-arm metadata reachable via WaitNode::user_
SelectOperation      -- the caller-facing operation (stack-owned)
SelectResult         -- the outcome delivered to the caller after completion
```

### 5.2 Ownership graph

```
caller (Fiber body, stack-owned)
  +-- owns SelectOperation (stack-local)
        +-- owns SelectGroup (embedded)
        |     +-- owns N SelectArm entries (embedded)
        |     |     +-- SelectArmMetadata per arm
        |     |     +-- WaitNode per arm (caller-owned)
        |     |     +-- WaitNode user_kind = Select, user_pointer = &metadata
        |     +-- owns group_state (atomic)
        |     +-- owns result_slot (SelectResult)
        +-- owns timer_waiters_ WaitQueue (private, for Timer arms)
        +-- SelectResult returned to caller on completion

Scheduler (global_mtx_ domain)
  +-- coordinates group claim (group.try_claim under global_mtx_)
  +-- commits winner result (after claim, same CS)
  +-- retires loser arms (non-publishing retirement seam)
  +-- publishes caller runnable (exactly once)

Primitive (Event)
  +-- owns WaitQueue (private, sealed)
  +-- Select-aware resolver reads WaitNode::user_kind() for Select
  +-- sets CandidateReady on Select arm instead of resolving Woken
  +-- ordinary waiters (kind == None) unaffected
  +-- Queue waiters (kind == Queue) fall through to reconciler path

Timer pump
  +-- Select-aware expiry reads WaitNode::user_kind() for Select
  +-- sets CandidateReady on Select arm instead of resolving Expired
  +-- ordinary timed waiters (kind == None) unaffected

SelectOperation timer_waiters_ WaitQueue
  +-- private queue owned by SelectOperation
  +-- Timer arm WaitNodes are registered here, not in any primitive
  +-- outlives every ACTIVE TimerRegistration
  +-- destroyed only after all TimerRegistrations are RETIRED/CONSUMED
```

### 5.3 Property matrix

| Property          | SelectGroup | SelectArm | SelectArmMetadata | WaitNode | SelectResult |
|-------------------|:-----------:|:---------:|:-----------------:|:--------:|:------------:|
| caller-owned      | indirect    | indirect  | indirect          | YES      | YES (returned)|
| stack-owned       | YES         | YES       | YES               | YES      | YES          |
| Scheduler-owned   | NO          | NO        | NO                | NO       | NO           |
| primitive-owned   | NO          | NO        | NO                | NO       | NO           |
| address-stable    | YES (stack) | YES       | YES               | YES      | YES (returned)|
| copyable          | NO          | NO        | NO                | NO       | YES (moved)  |
| movable           | NO          | NO        | NO                | NO       | YES          |
| destruction pre   | completed   | retired   | cleared           | terminal | consumed     |

---

## 6. State Machines

### 6.1 SelectGroup state machine

```
Constructing
    | (all arms registered, caller enters make_select_waiting)
    v
Arming
    | (any arm CandidateReady at admission)
    v
WinnerClaimed(index)      [inline admission winner]

OR

Constructing -> Arming
    | (no arm ready at admission; caller make_waiting)
    v
Armed
    | (group claim CAS succeeds)
    v
WinnerClaimed(index)      [post-suspension winner]
    | (winner commit + loser retirement complete)
    v
Publishing
    | (conditional: CompleteInline or PublishSuspendedCaller)
    v
Completed
    | (result consumed by caller)
    v
Destroyed
```

**State transitions**:

| From           | To              | Trigger | Guard |
|----------------|-----------------|---------|-------|
| Constructing   | Arming          | all arms registered, make_select_waiting begins | registration_count == arm_count |
| Arming         | WinnerClaimed   | arm already CandidateReady at admission | CAS ARMING -> CLAIMED (inline) |
| Arming         | Armed           | no arm ready at admission; caller make_waiting | CAS ARMING -> ARMED |
| Armed          | WinnerClaimed   | group.try_claim CAS succeeds | CAS ARMED -> CLAIMED |
| WinnerClaimed  | Publishing      | winner commit + loser retirement done | sequential (same CS) |
| Publishing     | Completed       | caller published (inline: CompleteInline; suspended: PublishSuspendedCaller) | conditional |
| Completed      | Destroyed       | caller consumes result | stack unwinding |

### 6.2 SelectArm state machine

```
DETACHED
    | (arm registered in group)
    v
REGISTERING
    | (WaitNode registered in queue, metadata installed)
    v
REGISTERED
    | (primitive signals: Event SET or timer due)
    v
CANDIDATE_READY
    |                    |
    | try_claim success  | try_claim fails
    v                    v
WINNER               LOSER
    |                    |
    v                    v
RETIRED              RETIRED
```

**State transitions**:

| From          | To             | Trigger | Synchronization |
|---------------|----------------|---------|-----------------|
| DETACHED      | REGISTERING    | arm added to group | none (init) |
| REGISTERING   | REGISTERED     | WaitNode registered, metadata installed | global_mtx_ + queue.mtx_ |
| REGISTERED    | CANDIDATE_READY | Event SET or timer due (Select-aware resolver) | global_mtx_ |
| CANDIDATE_READY | WINNER       | group.try_claim CAS succeeds | global_mtx_ |
| CANDIDATE_READY | LOSER        | group.try_claim CAS fails | global_mtx_ |
| WINNER        | RETIRED        | winner commit + publication complete | global_mtx_ |
| LOSER         | RETIRED        | non-publishing retirement | global_mtx_ |

**Synchronization rule**: All SelectArm state transitions occur under
`Scheduler::global_mtx_`. The CandidateReady flag is written by the
Select-aware resolver while holding global_mtx_. Atomic storage may be
retained for diagnostics, but global_mtx_ is the authoritative
synchronization domain.

---

## 7. Registration Protocol (R1 -- One Continuous Global Coordination Interval)

### 7.1 Registration sequence

```
Under one continuous global_mtx_ critical section:
    1. register all arms in SelectGroup
    2. for each arm:
        a. initialize SelectArmMetadata
        b. set WaitNode user_kind = Select, user_pointer = &metadata
        c. acquire target WaitQueue mutex
           - Event arm: Event's private waiters_
           - Timer arm: SelectOperation's private timer_waiters_
        d. register_wait_locked(node, fiber)
        e. release target WaitQueue mutex
        f. install TimerRegistration if timer arm (node, timer_waiters_, deadline)
        g. observe admission readiness:
           - Event arm: is Event SET? -> CandidateReady
           - Timer arm: is deadline already due? -> CandidateReady
    3. if any arm is CandidateReady:
        a. select lowest-index CandidateReady arm
        b. group claim CAS (Arming -> WinnerClaimed)
        c. commit winner result
        d. retire losers (non-publishing)
        e. complete inline (CompleteInline)
    4. otherwise:
        a. caller make_waiting
        b. group -> Armed (Arming -> Armed)
        c. release global_mtx_
        d. context_switch
```

**Rationale**: R1 prevents external Event/timer resolution from interleaving
with partial registration. All arms are registered atomically under one
global_mtx_ hold. No external resolver can observe the group or any arm
while registration holds global_mtx_.

**Critical ordering**: metadata and user_kind are installed BEFORE the
WaitNode enters the primitive queue. If registration fails, user_kind is
cleared and user_pointer is nulled before the arm is marked Retired.

### 7.2 Two distinct readiness causes

```
ADMISSION OBSERVATION (during Arming):
    Select registration code observes Event SET or deadline due while
    holding global_mtx_ during registration. No external resolver is
    involved. The observation leads to CandidateReady and inline
    completion if any arm is ready.

POST-ARMING RESOLUTION (after Armed):
    External Event setter or timer pump observes the Armed group after
    global_mtx_ is released and the caller is Waiting. The resolver
    sets CandidateReady and attempts group claim.
```

### 7.3 Edge cases

**Arm already ready at admission**: All arms registered under one global_mtx_
CS. If any arm is CandidateReady at admission (Event SET observed, deadline
already due observed), the inline path selects the lowest-index CandidateReady
arm, claims the group, commits the winner, retires losers, and returns inline.
The caller never enters Waiting state.

**No arm ready at admission**: All arms registered, no CandidateReady. Caller
make_waiting, group -> Armed, global_mtx_ released, context_switch. External
resolvers may later set CandidateReady.

**External set() during R1**: The external thread calling Event::set() blocks
on global_mtx_, which is held by the registration CS. The setter cannot
complete and cannot inspect partial group state. After registration releases
global_mtx_, the setter proceeds, observes the Armed group, marks the Event
arm CandidateReady, and attempts group claim.

**Registration failure**: If any arm fails registration (e.g., C8 reuse
rejection, timer allocation failure), all already-registered arms are retired
via the non-publishing retirement seam under the same global_mtx_ hold.
user_kind is cleared on each retired arm. The group is destroyed. No arms
leak. No caller is published.

**Empty arm list**: Debug assert. No group created.

**Multiple CandidateReady at admission**: The lowest-index CandidateReady arm
wins the CAS. All other arms are retired as losers.

### 7.4 Caller state transitions

```
Caller fiber:
    Running -> (make_select_waiting) -> Waiting -> (published) -> Running

The Waiting transition is the same as await_wait: under global_mtx_,
register all arms, then context_switch. The difference is that
make_select_waiting registers N WaitNodes instead of one.
```

---

## 8. Select-Aware Event Resolution Seam

### 8.1 Ordinary Event waiter (unchanged)

```
Event SET
    -> WaitNode Woken (resolve_ CAS)
    -> unlink from queue
    -> make_runnable
    -> route publication
```

### 8.2 Select Event arm -- type-safe resolver

```
select_event_candidate_ready_locked(WaitNode& node):
    PRE: global_mtx_ held, Event queue mtx_ held
    if node.user_kind() == WaitNodeUserKind::None:
        resolve Woken, unlink, make_runnable, route
        return
    if node.user_kind() == WaitNodeUserKind::Queue:
        // fall through to Queue reconciler path
        return
    // kind == Select
    auto* meta = static_cast<SelectArmMetadata*>(node.user_pointer())
    if meta->arm_kind == ArmKind::EventWait && meta->state == REGISTERED:
        meta->state = CANDIDATE_READY
        // do NOT resolve WaitNode; do NOT unlink; do NOT publish
        return
    // RETIRED/LOSER/WINNER: group owns resolution
```

### 8.3 Event set() broadcast -- two-phase algorithm

```
event_set_broadcast_select_aware:

PHASE 1 (scan under Event queue mtx):
    hold global_mtx_
    store Event SET = true
    acquire Event queue mtx_
    iterate queue using stable next pointer:
        ordinary waiter (kind == None):
            resolve Woken, unlink
            retire timer if present
            record for ordinary publication
        Queue waiter (kind == Queue):
            resolve Woken, unlink (Queue reconciler handles payload)
            record for ordinary publication
        Select Event arm (kind == Select, arm_kind == EventWait):
            if state == REGISTERED:
                state = CANDIDATE_READY
                leave WaitNode Registered and linked
                collect owning SelectGroup identity
                do NOT claim group
                do NOT retire any Select arm
    release Event queue mtx_

PHASE 2 (group claim under global_mtx_):
    still hold global_mtx_
    publish ordinary winners (make_runnable + route)
    deduplicate collected SelectGroups
    for each unique group:
        if group is Armed:
            choose lowest-index CandidateReady arm
            group.try_claim CAS
            if claim succeeds:
                acquire winner Event queue mutex
                call wake_node_locked(winner.node)
                    -> resolve Woken
                    -> unlink in the same queue CS
                release Event queue mutex

                decrement waiting_waitq_count_ exactly once
                mark winner arm RETIRED
                clear winner typed context

                retire every loser through the non-publishing loser seam

                populate SelectResult exactly once

                if caller was suspended:
                    make_runnable exactly once
                    route_runnable_locked exactly once
                else:
                    no runnable publication

                group -> Completed
```

**Safety properties**:
- No infinite loop: the scan uses a stable next pointer captured before
  releasing the queue mutex; newly registered nodes are not in the scan
- No recursive acquisition: the Event queue mtx is released before group
  claim; each arm's queue mtx is acquired sequentially, never two together
- Ordinary Event broadcast semantics preserved: ordinary waiters are resolved
  and published in Phase 1
- Multiple Select arms from one group: processed once in Phase 2 after
  deduplication
- Event stays SET after all resolution

### 8.4 Event arm at external-thread set()

```
External thread calls Event::set():
    -> global_mtx_ acquired
    -> event_set_broadcast_select_aware (two-phase, see above)
    -> global_mtx_ released
```

### 8.5 Test seams

```
event_select_scan_complete_before_group_claim
    Pause after Phase 1 scan, before Phase 2 group claims
event_after_group_claim_before_arm_retirement
    Pause after group claim, before loser arm retirement
```

---

## 9. Select-Aware Timer Resolution Seam

### 9.1 Timer arm -- independent arm with private queue

First-scope Timer is an INDEPENDENT TIMER ARM. The caller composes a group
deadline by adding a Timer arm. Each Timer arm's WaitNode is registered in
`SelectOperation::timer_waiters_` (a private WaitQueue owned by the operation),
not in any primitive's queue.

```
SelectOperation
    owns timer_waiters_ WaitQueue
    each Timer arm: WaitNode registered in timer_waiters_
    TimerRegistration(node, &timer_waiters_, deadline)
```

### 9.2 Timer authority consumption ordering

```
For admission-ready Timer (deadline already due at registration):
    1. arm -> CandidateReady (observation, under global_mtx_)
    2. group claim CAS (Arming -> WinnerClaimed)
    3. TimerRegistration: ACTIVE -> CONSUMED (try_claim_expiry)
    4. resolve WaitNode Expired
    Ordering: group claim BEFORE consumption and WaitNode commit.

For timer-pump resolution (deadline due after Armed):
    1. timer pump fires, acquires global_mtx_
    2. arm -> CandidateReady (set metadata.state)
    3. group claim CAS (Armed -> WinnerClaimed)
    4. TimerRegistration: ACTIVE -> CONSUMED (try_claim_expiry)
    5. acquire SelectOperation::timer_waiters_ mutex
    6. call expire_locked(winner.node)
           Registered -> Expired
           unlink from timer_waiters_ in the same queue CS
    7. release timer_waiters_ mutex
    8. decrement active_deadline_count_ exactly once
    9. decrement waiting_waitq_count_ exactly once
    10. mark winner arm RETIRED
    11. clear winner typed context
    12. retire loser arms without publication
    13. populate SelectResult
    14. publish caller only if suspended
    Ordering: group claim BEFORE consumption and WaitNode commit.
    The winner Timer WaitNode is never left linked in timer_waiters_.

For loser Timer arm:
    1. group claim CAS fails
    2. TimerRegistration: ACTIVE -> RETIRED (retire())
    3. acquire arm queue mutex
    4. call cancel_locked(node)
           resolve Cancelled
           unlink in the same queue CS
    5. release arm queue mutex
    6. decrement waiting_waitq_count_ exactly once
    7. mark arm state RETIRED
    8. clear typed WaitNode context
    9. do NOT make_runnable
    10. do NOT publish SelectResult
```

### 9.3 Type-safe timer expiry

```
select_timer_candidate_ready_locked(WaitNode& node):
    PRE: global_mtx_ held
    if node.user_kind() == WaitNodeUserKind::None:
        resolve Expired, unlink, make_runnable, route
        return
    if node.user_kind() != WaitNodeUserKind::Select:
        return // Queue or other: fall through
    auto* meta = static_cast<SelectArmMetadata*>(node.user_pointer())
    if meta->arm_kind == ArmKind::TimerDeadline && meta->state == REGISTERED:
        meta->state = CANDIDATE_READY
        // do NOT resolve WaitNode Expired
        // do NOT consume TimerRegistration
        // do NOT make_runnable
        if meta->group is Armed:
            try group claim
            if claim succeeds:
                TimerRegistration: ACTIVE -> CONSUMED
                resolve winner WaitNode Expired
                retire losers, publish caller
        return
```

### 9.4 Already-due timer at admission

```
At admission (under global_mtx_):
    if timer deadline is already due:
        meta->state = CANDIDATE_READY
        // TimerRegistration stays ACTIVE
        // do NOT resolve WaitNode Expired
        // group claim follows in the same CS
        // after group claim: ACTIVE -> CONSUMED, resolve Expired
```

### 9.5 Stale timer safety

```
TimerRegistration state machine:
    ACTIVE -> CONSUMED (try_claim_expiry wins, after group claim)
    ACTIVE -> RETIRED  (retire() wins, loser arm)

Exactly one transition per registration.
After SelectOperation destruction, no ACTIVE TimerRegistration retains
a pointer to SelectArm/WaitNode/group/timer_waiters_.
I4 closure: RETIRED registrations are never dereferenced by the pump.
timer_waiters_ outlives every ACTIVE registration.
```

---

## 10. Correct Loser Retirement

### 10.1 Canonical loser seam

```
select_retire_arm_locked(SelectArm& arm):

PRE:
    global_mtx_ held
    group winner is final
    arm is not winner
    arm state is REGISTERED or CANDIDATE_READY

1. acquire arm.queue.mtx()

2. call arm.queue.cancel_locked(arm.node)
       -> resolve Cancelled
       -> unlink in the same queue CS

3. release arm.queue.mtx()

4. if arm owns an ACTIVE TimerRegistration:
       retire()
       ACTIVE -> RETIRED

5. decrement waiting_waitq_count_ exactly once

6. if TimerRegistration retirement won:
       decrement active_deadline_count_ exactly once

7. mark arm state RETIRED

8. clear typed WaitNode context
       user_kind -> None
       user_pointer -> nullptr

9. do NOT call make_runnable

10. do NOT call route_runnable_locked

11. do NOT populate SelectResult
```

### 10.2 Failure handling

`cancel_locked()` must succeed for a valid Registered loser. If it fails, this
indicates a protocol invariant violation, not an ordinary idempotent cleanup
success.

Duplicate retirement may return early only when `arm.state == RETIRED` and
the node is already terminal+detached.

An already-terminal but linked node is NOT acceptable — the finalization law
requires resolve BEFORE unlink under the same queue mutex.

### 10.3 Why ordinary cancel cannot be used

The current cancel paths call `make_runnable()` + `route_runnable_locked()`
(`scheduler.cpp:1779-1781`). These publish the caller. For Select loser
retirement, the caller must NOT be published — the winner publishes exactly once.

### 10.4 Safety invariants

```
InvLoserResolveBeforeUnlink:
    cancel_locked resolves Cancelled BEFORE unlinking

InvLoserTerminalAndDetached:
    loser WaitNode is Cancelled and unlinked before arm RETIRED

InvLoserNeverPublishes:
    loser path never calls make_runnable or route_runnable

InvNoContextClearWhileRegistered:
    clear_user_context only after node is terminal AND unlinked

InvRetiredArmHasNoQueueMembership:
    arm.state == RETIRED => not arm.node.is_registered()
```

---

## 11. Inline Winner vs Suspended Winner

### 11.1 Admission-time winner (inline)

```
At registration (all arms under one global_mtx_ CS):
    if any arm is CandidateReady:
        select lowest-index CandidateReady arm
        group claim CAS (Arming -> WinnerClaimed)
        commit winner result:
            Event arm: resolve WaitNode Woken
            Timer arm: ACTIVE->CONSUMED, resolve WaitNode Expired
        retire all loser arms (non-publishing)
        CompleteInline:
            populate SelectResult
            caller never made Waiting
            make_runnable count = 0
            route_runnable count = 0
            SelectResult publication count = 1
```

### 11.2 Post-suspension winner

```
After registration, no arm ready:
    caller make_waiting
    group -> Armed
    release global_mtx_
    context_switch

Later, external resolver (Event::set or timer pump):
    under global_mtx_:
        arm -> CandidateReady
        if group is Armed:
            try group claim (Armed -> WinnerClaimed)
            if claim succeeds:
                commit winner result
                retire losers (non-publishing)
                PublishSuspendedCaller:
                    make_runnable(caller) -> exactly once
                    route_runnable_locked(caller) -> exactly once
                    group -> Completed
```

### 11.3 Publication counts

```
INLINE COMPLETION:
    SelectResult publication count = 1
    make_runnable count = 0
    route_runnable count = 0

SUSPENDED COMPLETION:
    SelectResult publication count = 1
    successful make_runnable count = 1
    route_runnable count = 1
```

### 11.4 Ordering constraint

```
make_waiting MUST happen before any make_runnable.

Under R1, this is guaranteed: all arms are registered and make_waiting
is called before global_mtx_ is released. External resolvers cannot
observe the group as Armed until global_mtx_ is released.
After release, the caller is Waiting, so make_runnable will succeed.
```


---

## 12. WaitNode Finalization Law

### 12.1 Canonical law

A Select arm WaitNode must become terminal before it is structurally detached
from its WaitQueue.

Terminal resolution and unlink must occur under the arm queue mutex by using
the existing WaitQueue authority:

```
Event winner:
    wake_node_locked(node)
        = resolve(Woken) + unlink

Timer winner:
    expire_locked(node)
        = resolve(Expired) + unlink

Select loser:
    cancel_locked(node)
        = resolve(Cancelled) + unlink
```

After the queue-level terminalization succeeds:

```
1. close waiting counters exactly once
2. close TimerRegistration authority exactly once
3. mark SelectArm RETIRED
4. clear typed WaitNode context
5. publish SelectResult only for the winner
6. publish runnable only for a suspended winner
```

### 12.2 Forbidden patterns

```
unlink
then resolve

clear context while Registered

destroy a terminal node that remains linked

publish a loser
```

### 12.3 Structural law confirmation

The existing structural law is confirmed and reused:

```
resolve terminal outcome
    BEFORE
unlink from WaitQueue

resolve + unlink occur under the same queue mutex critical section
```

---

## 13. Correct Event Winner Finalization

The two-phase Event broadcast (§8.3) uses the corrected winner path.

### 13.1 Phase 1 (unchanged)

```
- hold global_mtx_
- store Event SET
- scan Event queue under its queue mutex
- ordinary waiters: normal wake path
- Select Event arms: mark CandidateReady and leave Registered+linked
- collect affected SelectGroups
- release Event queue mutex
```

### 13.2 Phase 2 (corrected winner path)

```
for each affected SelectGroup:
    choose lowest-index CandidateReady arm
    claim group

    if Event arm wins:
        acquire winner Event queue mutex
        call wake_node_locked(winner.node)
            -> resolve Woken
            -> unlink in the same queue CS
        release queue mutex

        decrement waiting_waitq_count_ exactly once
        mark winner arm RETIRED
        clear winner typed context

        retire every loser through the non-publishing loser seam

        populate SelectResult exactly once

        if caller was suspended:
            make_runnable exactly once
            route_runnable_locked exactly once
        else:
            no runnable publication
```

### 13.3 Explicit invariants

```
InvEventWinnerTerminalAndDetachedBeforePublication
    Event winner WaitNode is Woken + unlinked before any caller publication

InvNoCompletedEventWinnerRemainsLinked
    After completion, no Event winner remains linked in Event queue

InvWinnerContextClearedAfterTerminalDetach
    clear_user_context only after resolve + unlink
```

### 13.4 Deterministic test assertions

```
winner WaitNode outcome == Woken
winner WaitNode !is_registered()
winner WaitNode user_kind == None
Event queue no longer contains winner node
caller publication count is correct
```

---

## 14. Correct Timer Winner Finalization

### 14.1 Ordering principle

```
group claim
BEFORE
TimerRegistration ACTIVE -> CONSUMED
BEFORE
WaitNode Expired commit
```

### 14.2 Correct Timer winner protocol

```
1. Timer arm becomes CandidateReady.
2. Group claim succeeds.
3. TimerRegistration.try_claim_expiry():
       ACTIVE -> CONSUMED.
4. Acquire SelectOperation::timer_waiters_ mutex.
5. call expire_locked(winner.node):
       Registered -> Expired
       unlink from timer_waiters_ in the same queue CS.
6. Release timer_waiters_ mutex.
7. decrement active_deadline_count_ exactly once.
8. decrement waiting_waitq_count_ exactly once.
9. mark winner arm RETIRED.
10. clear winner typed context.
11. retire loser arms without publication.
12. populate SelectResult.
13. publish caller only if suspended.
```

### 14.3 Key statements

```
The winner Timer WaitNode is never left linked in timer_waiters_.

SelectOperation destruction requires timer_waiters_ to be empty.

A CONSUMED TimerRegistration may remain physically in the Scheduler's
lazy timer pool, but it may not retain executable callback authority and
must never dereference the terminal detached WaitNode.
```

### 14.4 Invariants

```
InvTimerWinnerTerminalAndDetachedBeforePublication
    Timer winner WaitNode is Expired + unlinked before caller publication

InvCompletedTimerQueueEmpty
    After completion, timer_waiters_ is empty for the completed operation

InvConsumedTimerNeverDereferencesWaitNode
    CONSUMED TimerRegistration must not dereference terminal detached WaitNode

InvNoActiveTimerAfterCompletion
    group_phase in {Completed, Destroyed} => no TimerRegistration is Active
```

### 14.5 Deterministic test assertions

```
Timer winner:
  - TimerRegistration == CONSUMED
  - WaitNode outcome == Expired
  - WaitNode !is_registered()
  - timer_waiters_ empty for the completed operation
  - typed context cleared
  - active/waiting counters each closed once
```

---

## 15. Correct Lifecycle Documentation

### 15.1 Unified WaitNode lifecycle

All documents must use the same order:

```
1. Detached, no typed context
2. typed context installed
3. Registered and linked
4. CandidateReady while still Registered and linked
5. group winner selected
6a. winner:
        resolve Woken/Expired + unlink
6b. loser:
        resolve Cancelled + unlink
7. close accounting
8. TimerRegistration consumed/retired
9. arm Retired
10. clear typed context
11. publish winner result/runnable
12. destruction
```

### 15.2 Enforcement

The preparation design (§4), state machine (§5), test plan, and formal model
(§24) must all use this order. Any document that deviates must be corrected.

---

## 16. Correct SelectOperation State Machine

### 16.1 Inline completion support

The current SelectOperation state machine must explicitly support inline
completion without entering Waiting.

```
Building
    -> Ready

Ready
    -> CompletedInline
        when an admission-ready Event/Timer wins
    -> Waiting
        when no arm is ready

Waiting
    -> CompletedSuspended
        when a post-arming winner publishes

CompletedInline
    -> Consumed

CompletedSuspended
    -> Consumed
```

Or an equivalent unified model:

```
Ready -> Completed -> Consumed
Ready -> Waiting -> Completed -> Consumed
```

### 16.2 Publication rules

```
Inline completion:
    make_runnable = 0
    route_runnable = 0

Suspended completion:
    make_runnable = 1
    route_runnable = 1
```

### 16.3 Refinement mapping

SelectGroup and SelectOperation state machines must be synchronized:

```
SelectGroup:  Constructing -> Arming -> WinnerClaimed -> Publishing -> Completed -> Destroyed
SelectOp:     Building     -> Ready  -> Completed      -> Consumed
```

When the group takes the inline path (Constructing -> Arming -> WinnerClaimed),
the operation transitions Ready -> CompletedInline without entering Waiting.

When the group takes the suspended path (Armed -> WinnerClaimed), the operation
transitions Ready -> Waiting -> CompletedSuspended.

---

## 17. Correct Timer Invariants

### 17.1 Problem

The current invariant:

```
forall timer: timer_state in {None, Retired, Consumed}
```

is invalid during registration and Armed phases because `Active` is legitimate.

### 17.2 Phase-aware replacement

```
InvTimerTerminalAfterCompletion:
    group_phase in {Completed, Destroyed}
        =>
    forall timer arm:
        timer_state in {Retired, Consumed}

InvTimerActiveOnlyBeforeCompletion:
    timer_state == Active
        =>
    group_phase in {
        Constructing,
        Arming,
        Armed,
        WinnerClaimed
    }

InvTimerTransitionsOnce:
    every TimerRegistration performs at most one:
        Active -> Retired
        Active -> Consumed

InvTimerEventuallyClosedForCompletedGroup:
    group_phase == Completed
        =>
    no TimerRegistration is Active
```

### 17.3 Formal actions

```
ClearTypedContext
    clears user_kind and user_pointer after terminal+detach

FinalizeEventWinner
    wake_node_locked: resolve Woken + unlink under Event queue mutex

FinalizeTimerWinner
    consume TimerRegistration, expire_locked: resolve Expired + unlink

FinalizeLoser
    cancel_locked: resolve Cancelled + unlink under arm queue mutex

CloseWaitAccounting
    decrement waiting_waitq_count_ exactly once per arm
    decrement active_deadline_count_ exactly once per timer arm
```

### 17.4 Additional invariant

```
InvNoContextClearWhileRegistered:
    clear_user_context only after node is terminal AND unlinked
```

---

## 18. Correct Tests

### 18.1 Test count

The final active test plan must have one consistent count. Tests are T1–T16.

All active metadata must say:

```
16 deterministic tests
```

For the original pre-hardening review request (`E13-SELECT-MULTI-WAIT-PREPARATION-AUDIT-1-REVIEW-REQUEST.md`),
the document is annotated as:

```
HISTORICAL SNAPSHOT — originally 15 tests
```

The final corrective plan contains 16 tests.

### 18.2 T12 registration failure — split into two sub-tests

#### T12a — Catchable registration exception rollback

Use an explicit modeled exception such as:

```
TimerRegistration allocation failure
SelectOperation arm storage allocation failure
injected registration exception seam
```

Expected:

```
exception caught
already registered arms resolve Cancelled + unlink
timer registrations retired
typed contexts cleared
no caller publication
no SelectResult
```

#### T12b — Assertion/death test

Use:

```
same WaitNode reused
same arm slot registered twice
cross-Scheduler misuse
```

Expected:

```
debug assertion/process termination
not catchable as a normal C++ exception
```

Cleanup after a fatal assertion is not specified unless the death-test
harness isolates the process.

### 18.3 Winner/loser checks

Add explicit checks for:

```
winner terminal + detached
loser terminal + detached
all typed contexts cleared
Event winner removed from Event queue
Timer winner removed from timer_waiters_
completed SelectOperation has no Registered WaitNode
```

---

## 19. Internal-Testing Seam Integration

### 19.1 Build variant confinement

All proposed E13 causal seams are compiled only into:

```
sluice_async_internal_testing
```

under:

```
SLUICE_ASYNC_INTERNAL_TESTING
```

### 19.2 Production exclusion

The production runtime must expose:

```
no test phase state
no controller symbol
no test hook public API
```

### 19.3 PhaseTag integration

New `PhaseTag` values for E13 integrate with the existing internal test
controller (`AsyncTestController`, `SchedulerTestHooks`) without publishing
forgeable friend authority.

This is a design clarification only; seams are not implemented in this task.

---

## 20. Fairness
(Original section 12 — renumbered due to PR16 corrective insertions)

```
ADMISSION TIE:
    lowest arm index among CandidateReady arms

CONCURRENT POST-ARMING ARRIVAL:
    the resolver that first obtains global_mtx_ and successfully claims wins

CROSS-CALL FAIRNESS:
    not guaranteed (first-scope limitation)

EVENT QUEUE FAIRNESS:
    ordinary Event broadcast semantics preserved

TIMER FAIRNESS:
    deadline order as provided by timer heap; same-deadline arm ties
    resolved by serialized group claim
```

---

## 21. Error and Misuse Taxonomy

| Category                          | Mechanism      | First-scope handling |
|-----------------------------------|:--------------:|----------------------|
| Empty select (zero arms)          | debug assert   | UB in release        |
| Cross-Scheduler arms              | debug assert   | UB in release        |
| Destroy with armed operation      | debug assert   | UB in release        |
| WaitNode reuse (C8 violation)     | debug assert   | Registration fails   |
| Registration exception            | catch + rollback | Partial cleanup    |
| Scheduler shutdown with armed     | caller contract| No automatic cleanup |
| arm > max_arms                    | debug assert   | UB in release        |

### Permitted and forbidden arm patterns

```
PERMITTED:
    multiple arms referencing the same Event
    multiple Timer arms with equal deadlines
    one Event arm + one Timer arm

FORBIDDEN:
    same WaitNode instance reused across arms
    same SelectArm slot registered twice
    same TimerRegistration object shared by arms
    cross-Scheduler arm
```

### Unsupported first-scope operations

```
These are NOT supported in first scope:

- Cross-Scheduler Select
- Semaphore acquire arms (deferred)
- AsyncMutex lock arms (deferred)
- Queue push/pop arms (deferred)
- AsyncCondition arms (deferred)
- group-level cancellation (only per-arm non-publishing retire)
```

---

## 22. Queue-Specific Analysis (Deferred)

Queue is deferred from first scope. Reasons:
- Queue v1 has no public wait-epoch cancel API (D10)
- Queue push payload ownership is irreversible
- Queue pop payload move is irreversible
- Direct handoff exposes commit to third party
- Capacity reservation may be irreversible

Future scope requires a new internal seam: Queue reconciler must check
group authority before executing ProducerGrantCommit or ConsumerGrantCommit.

---

## 23. AsyncCondition-Specific Analysis (Deferred)

AsyncCondition is deferred from first scope. The two-epoch protocol
(Condition epoch + mandatory Mutex reacquire) is fundamentally incompatible
with the first-scope single-epoch arm model. The mandatory reacquire
(C-H5, `condition.hpp:28`) is untimed and non-cancellable.

---

## 24. Formal-Model Plan (Future)

### 24.1 State variables (first scope)

```
VARIABLES:
    winner              : Nat u {none}
    group_phase         : enum
    arm_state[i]        : enum per arm
    arm_kind[i]         : {EventWait, TimerDeadline}
    event_state[i]      : {UNSET, SET} per Event arm
    timer_state[i]      : {None, Active, Retired, Consumed} per arm
    caller_state        : {Running, Waiting, Completed}
    publication_count   : Nat  (must be <= 1)
    registration_count  : Nat
    retirement_count    : Nat
```

### 24.2 Safety properties (first scope)

```
InvAtMostOneWinner              : winner = none OR exists! i: winner = i
InvWinnerChosenBeforeArmCommit  : commit[i] => winner = i
InvLoserNeverPublishes          : winner != i => NOT publication[i]
InvExactlyOneSelectResult       : group_phase = Completed => publication_count = 1
InvAtMostOneRunnablePublication : make_runnable_count <= 1
InvInlineWinnerNeverMakesRunnable : inline winner => make_runnable_count = 0
InvNoMakeRunnableBeforeMakeWaiting : make_runnable => caller was Waiting
InvCandidateReadyIsNotPrimitiveTerminal : CandidateReady => WaitNode Registered
InvNoArmedArmAfterCompletion    : group_phase = Completed => forall i: arm_state[i] in {Retired}
InvAllLosersRetired             : winner = i => forall j != i: arm_state[j] = Retired
InvTimerActiveOnlyBeforeCompletion :
    timer_state == Active
        =>
    group_phase in {Constructing, Arming, Armed, WinnerClaimed}

InvTimerTerminalAfterCompletion :
    group_phase in {Completed, Destroyed}
        =>
    forall i: timer_state[i] in {Retired, Consumed}

InvTimerTransitionsOnce :
    every TimerRegistration performs at most one:
        Active -> Retired
        Active -> Consumed

InvTimerEventuallyClosedForCompletedGroup :
    group_phase == Completed
        =>
    no TimerRegistration is Active
InvNoCallbackAfterDestroy       : group_phase = Destroyed => no timer ACTIVE
InvEventPersistentStateNotConsumed : Event SET after loser retirement
InvRegistrationFailureLeavesNoVisibleArm : rollback => forall i: arm_state[i] = Retired
```

### 24.3 Actions (first scope)

```
BeginRegistration
InstallTypedUserContext
RegisterEventArm
RegisterTimerArmInPrivateQueue
ObserveAdmissionReady
FinishArmingInline
FinishArmingSuspended
EventScanMarksCandidates
ProcessCandidateGroups
TimerPumpMarksCandidate
TryClaimFromArming
TryClaimFromArmed
ConsumeWinningTimerRegistration
RetireLosingTimerRegistration
ResolveWinner
RetireLosersWithoutPublication
CompleteInline
PublishSuspendedCaller
RollbackRegistration
DestroyCompletedOperation

ClearTypedContext
    -- clear user_kind and user_pointer after WaitNode is terminal AND detached
    -- PRE: node.outcome() in {Woken, Expired, Cancelled}
    -- PRE: not node.is_registered()
    -- POST: node.user_kind() == None

FinalizeEventWinner
    -- wake_node_locked: resolve Woken + unlink under Event queue mutex

FinalizeTimerWinner
    -- consume TimerRegistration, expire_locked: resolve Expired + unlink

FinalizeLoser
    -- cancel_locked: resolve Cancelled + unlink under arm queue mutex

CloseWaitAccounting
    -- decrement waiting_waitq_count_ exactly once per arm
    -- decrement active_deadline_count_ exactly once per timer arm
```

### 24.4 New invariants (corrected)

```
InvUserKindMatchesDynamicContext
    user_kind == Select iff arm_kind in {EventWait, TimerDeadline}
InvNoWrongContextCast
    resolver reads user_kind before casting user_pointer
InvUserContextInstalledBeforeRegistration
    user_kind and user_pointer set before WaitNode enters queue
InvTimerArmHasValidQueue
    Timer arm WaitNode registered in timer_waiters_, not in any primitive
InvNoExternalReadyDuringR1Registration
    external resolver cannot observe group while global_mtx_ held during R1
InvEventScanTerminates
    Phase 1 scan uses stable next pointer; no infinite loop
InvNoQueueMutexRecursiveAcquisition
    Phase 2 acquires arm queue mutexes sequentially, never two together
InvInlineCompletionDoesNotRoute
    inline winner: make_runnable = 0, route_runnable = 0
InvSuspendedCompletionRoutesExactlyOnce
    suspended winner: make_runnable = 1, route_runnable = 1
```

InvNoContextClearWhileRegistered
    clear_user_context only after WaitNode is terminal AND unlinked

InvWinnerTerminalAndDetachedBeforePublication
    winner WaitNode is resolved and unlinked before any caller publication

InvLoserTerminalAndDetached
    loser WaitNode is resolved Cancelled and unlinked before arm marked RETIRED

InvEventWinnerRemovedFromEventQueue
    Event winner WaitNode is unlinked from Event queue in Phase 2

InvTimerWinnerRemovedFromTimerWaiters
    Timer winner WaitNode is unlinked from timer_waiters_ after expiry

InvCompletedOperationHasNoRegisteredNode
    group_phase == Completed => forall i: not arm[i].node.is_registered()

InvWinnerContextClearedAfterTerminalDetach
    winner arm: clear_user_context only after resolve + unlink

InvLoserResolveBeforeUnlink
    loser arm: cancel_locked resolves Cancelled BEFORE unlinking

InvConsumedTimerNeverDereferencesWaitNode
    CONSUMED TimerRegistration must not dereference terminal detached WaitNode

InvNoActiveTimerAfterCompletion
    group_phase in {Completed, Destroyed} => no TimerRegistration is Active

InvRetiredArmHasNoQueueMembership
    arm.state == RETIRED => not arm.node.is_registered()
```

### 24.5 Negative models

```
NEG-1: winner claim after arm commit (violates InvWinnerChosenBeforeArmCommit)
NEG-2: publish before caller waiting (violates InvNoMakeRunnableBeforeMakeWaiting)
NEG-3: cancel used as group authority (violates C3)
NEG-4: loser remains armed after completion (violates InvNoArmedArmAfterCompletion)
NEG-5: two ready arms both commit (violates InvAtMostOneWinner)
NEG-6: timer and event both publish (violates InvAtMostOneRunnablePublication)
NEG-7: partial registration leak (violates InvRegistrationFailureLeavesNoVisibleArm)
```

### 24.6 Deferred extension obligations

```
Future Semaphore integration requires:
    Select-aware admission seam
    Select-aware queued release seam
    FIFO/no-barging preservation
    permit conservation invariant

Future AsyncMutex integration requires:
    Select-aware admission seam
    Select-aware direct handoff seam
    FIFO/no-barging preservation
    single-owner invariant
```

---

## 25. Residual Risks

| ID   | Severity | Description | Mitigation |
|------|----------|-------------|------------|
| R-E13-1 | Medium | global_mtx_ contention with many Select arms | First scope acceptable |
| R-E13-2 | Medium | No formal model yet | Formal model plan defined (§24) |
| R-E13-3 | Low | Fairness is array-index only | Documented limitation |
| R-E13-4 | Medium | Queue/Condition/Semaphore/Mutex deferred | Deferred by design |
| R-E13-5 | Low | Debug-assert-only in release | Matches E10-E12 convention |

---

## 26. Review Finding Disposition

### 26.1 Original audit (E13-SELECT-MULTI-WAIT-PREPARATION-AUDIT-1)

```
P0-1 Semaphore release:            ACCEPTED -- removed from first scope
P0-2 Mutex handoff:                ACCEPTED -- removed from first scope
P0-3 Event set unconditional:      CLOSED -- Select-aware Event resolution seam specified (S8)
P0-4 Timer pump unconditional:     CLOSED -- Select-aware Timer resolution seam specified (S9)
P1-1 Select-aware admission seam:  CLOSED FOR EVENT + TIMER ONLY
P1-2 Semaphore release seam:       DEFERRED
P1-3 Mutex handoff seam:           DEFERRED
P1-4 CandidateReady external:      CLOSED (S8.4, S9.2)
P1-5 ordinary cancel publishes:    CLOSED -- non-publishing retirement seam specified (S10)
P1-6 WaitNode user_ absent:        CLOSED (S4)
P1-7 no-internal-modification:     CLOSED -- corrected to "requires narrow seams" (S2.2)
P1-8 queued Semaphore release:     DEFERRED
P1-9 Mutex unlock handoff:         DEFERRED
P1-10 partial registration race:   CLOSED -- R1 protocol (S7)
```

### 26.2 Independent re-audit P2 observations (non-blocking, now closed)

```
P2-1 Missing ClearTypedContext formal action:
    CLOSED -- added to formal model plan (§24.3)

P2-2 Missing InvNoContextClearWhileRegistered:
    CLOSED -- added to formal invariants (§24.4)

P2-3 Internal test seam integration:
    CLOSED -- integration clarified (§M)

P2-4 Cancel path disposition wording:
    CLOSED -- wording clarified in erratum (§U.1)
```

### 26.3 PR #16 external review findings

```
Gemini-1 Event winner remains linked:
    CLOSED -- Event winner finalization corrected (§F)

Gemini-2 Timer winner remains linked:
    CLOSED -- Timer winner finalization corrected (§G)

CodeRabbit-1 winner/loser finalization incomplete:
    CLOSED -- WaitNode finalization law established (§E);
    Event winner (§F), Timer winner (§G), loser (§H) each corrected

CodeRabbit-2 loser unlink-before-resolve:
    CLOSED -- loser retirement corrected to resolve-before-unlink (§H)

CodeRabbit-3 Timer invariant phase-invalid:
    CLOSED -- phase-aware Timer invariants added (§K)

CodeRabbit-4 missing inline SelectOperation transition:
    CLOSED -- inline completion path added (§J)

CodeRabbit-5 test count mismatch:
    CLOSED -- 16 deterministic tests consistently specified (§L)

CodeRabbit-6 T12 exception/assertion conflation:
    CLOSED -- T12a (catchable) / T12b (death test) split (§L)

CodeRabbit-7 untracked provenance:
    CLOSED -- audit erratum (§U.2), reaudit supersession (§T.5)

Independent-1 historical implementation authorization ambiguity:
    CLOSED -- audit erratum (§U.1)

Independent-2 missing ClearTypedContext formal action:
    CLOSED -- added to formal model plan (§24.3)

Independent-3 missing InvNoContextClearWhileRegistered:
    CLOSED -- added to formal invariants (§24.4)

Independent-4 E13 internal test seam integration:
    CLOSED -- integration clarified (§M)
```

---

## 27. Files Modified

```
docs/e13-select-preparation.md          (PR16 corrective — event/timer/loser/state-machine/invariant corrections)
docs/e13-select-state-machine.md        (PR16 corrective — inline path, timer invariants)
docs/e13-select-test-plan.md            (PR16 corrective — T12 split, winner/loser checks)
docs/reviews/E13-SELECT-MULTI-WAIT-PREPARATION-AUDIT-1-REVIEW-REQUEST.md (historical annotation)
docs/reviews/E13-SELECT-MULTI-WAIT-PREPARATION-AUDIT-1.md (erratum added)
docs/reviews/E13-SELECT-MULTI-WAIT-PREPARATION-CORRECTIVE-1-REVIEW-REQUEST.md (test count update)
docs/reviews/E13-SELECT-MULTI-WAIT-PREPARATION-CORRECTIVE-1-REAUDIT-1.md (supersession notice added)
docs/reviews/E13-SELECT-MULTI-WAIT-PR16-CORRECTIVE-1-REVIEW-REQUEST.md (new — delta re-audit request)
```

**No modifications to**: `include/**`, `src/**`, `docs/spec/**`, `tests/**`, `xmake.lua`.

---

## 28. Final Report

```
E13-SELECT-MULTI-WAIT-PR16-CORRECTIVE-1:
PASS — AUTHOR SELF-ASSESSMENT

EVENT WINNER FINALIZATION:
CORRECTED

TIMER WINNER FINALIZATION:
CORRECTED

LOSER TERMINALIZATION:
CORRECTED — RESOLVE BEFORE UNLINK

TIMER FORMAL INVARIANTS:
CORRECTED

INLINE SELECTOPERATION PATH:
CORRECTED

EXTERNAL REVIEW FINDINGS:
ADDRESSED

DELTA RE-AUDIT:
REQUIRED

FORMAL MODEL IMPLEMENTATION:
DENIED

PRODUCTION IMPLEMENTATION:
DENIED

PR #16 MERGE:
DENIED PENDING DELTA RE-AUDIT
```

### Summary

| Item | Value |
|------|-------|
| First scope | Event + Timer |
| Selected protocol | Candidate A: Central SelectGroup Claim Under Scheduler Authority |
| Registration model | R1: One continuous global coordination interval |
| WaitNode discriminator | WaitNodeUserKind (None/Queue/Select) |
| Timer arm queue | SelectOperation::timer_waiters_ (private) |
| Event broadcast | Two-phase: Phase 1 scan, Phase 2 group claims |
| Semaphore | Deferred |
| AsyncMutex | Deferred |
| Queue | Deferred |
| AsyncCondition | Deferred |
| Residual risks | 5 items |

### Confirmation

```
PRODUCTION CODE CHANGES: NONE
TEST CODE CHANGES: NONE
FORMAL SPEC CHANGES: NONE
BUILD POLICY CHANGES: NONE
```
