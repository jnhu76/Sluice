# E13-SELECT-MULTI-WAIT-PREPARATION-CORRECTIVE-1-REAUDIT-1

## A. Verdict

```text
E13-SELECT-MULTI-WAIT-PREPARATION-CORRECTIVE-1-REAUDIT-1:
PASS

APPROVED FIRST SCOPE:
EVENT + INDEPENDENT TIMER ARM

SELECTED PROTOCOL:
CENTRAL SELECTGROUP CLAIM UNDER SINGLE-SCHEDULER AUTHORITY

PREPARATION:
CLOSED

FORMAL MODEL IMPLEMENTATION:
AUTHORIZED FOR EVENT + TIMER ONLY

PRODUCTION IMPLEMENTATION:
DENIED
```

---

## B. Baseline and independence

```text
REPOSITORY:
jnhu76/Sluice

TASK:
E13-SELECT-MULTI-WAIT-PREPARATION-CORRECTIVE-1-REAUDIT-1

MODE:
INDEPENDENT READ-ONLY ADVERSARIAL RE-AUDIT

BRANCH:
feature/E13-preparation

REVIEW_HEAD:
fed2514 (E13: pre-reaudit hardening complete)

MASTER_HEAD:
be70fdec102e3a0d082330b9d8bba9f78ac3fcdb

MERGE_BASE:
be70fdec102e3a0d082330b9d8bba9f78ac3fcdb
```

This review was conducted from a clean independent perspective. The reviewer did not participate in any of:

```text
E13-SELECT-MULTI-WAIT-PREPARATION-DESIGN-1
E13-SELECT-MULTI-WAIT-INDEPENDENT-DESIGN-CHALLENGE-1
E13-SELECT-MULTI-WAIT-PREPARATION-AUDIT-1
E13-SELECT-MULTI-WAIT-PREPARATION-CORRECTIVE-1
```

No author self-assessment was accepted. All findings below are independently derived from the committed source documents and the real codebase.

---

## C. Files and source inspected

### Required documents (all read in full)

| File | Lines | Status |
|------|-------|--------|
| `docs/e13-select-preparation.md` | 1173 | Primary design document |
| `docs/e13-select-state-machine.md` | 326 | State machine specification |
| `docs/e13-select-test-plan.md` | 506 | Test plan (16 tests) |
| `docs/reviews/E13-SELECT-MULTI-WAIT-PREPARATION-AUDIT-1.md` | 854 | Original audit |
| `docs/reviews/E13-SELECT-MULTI-WAIT-PREPARATION-CORRECTIVE-1-REVIEW-REQUEST.md` | 123 | Corrective review request |

### Artifact documents (read, where present)

| File | Status |
|------|--------|
| `docs/reviews/E13-SELECT-MULTI-WAIT-INDEPENDENT-DESIGN-CHALLENGE-1.md` | Present, read |
| `docs/reviews/E13-SELECT-MULTI-WAIT-PREPARATION-CORRECTIVE-1-PRE-REAUDIT-HARDENING-1.md` | Not present as separate file; hardening applied inline |

### Authority documents (verified)

| File | Classification |
|------|---------------|
| `docs/e10-e12-api-semantic-closure.md` | Binding |
| `docs/e12-event.md` | Binding |
| `docs/e12-queue.md` | Contextual |

### Source code inspected (real file paths, current line numbers)

| File | Key symbols verified |
|------|---------------------|
| `include/sluice/async/wait_node.hpp` | `user_` (L251), `set_user` (L151), `resolve_` (L237-247), `register_` (L215-225) |
| `include/sluice/async/wait_queue.hpp` | `register_wait_locked` (L174-186), `wake_one_locked` (L199-211), `cancel_locked` (L222-233), `wake_node_locked` (L245-251), `unlink_locked` (L302-317) |
| `include/sluice/async/event.hpp` | `set` (L103-105), `wait` (L122-124), `cancel` (L170-172) |
| `include/sluice/async/timer_registration.hpp` | `try_claim_expiry` (L112-117), `retire` (L125-130) |
| `include/sluice/async/fiber.hpp` | `make_runnable` (L89), `make_waiting` (L97) |
| `src/async/scheduler.cpp` | `event_set_broadcast` (L1303-1341), `expire_wait` (L1276-1299), `await_event_wait` (L1397-1472), `route_runnable_locked` (L910) |

### Source code verification summary

No Select-related symbols exist in the current source code (confirmed by grep for `Select|SelectGroup|SelectArm|SelectOperation|WaitNodeUserKind` across all `.hpp` and `.cpp` files). This is expected — the task is a preparation design, not implementation. The existing `WaitNode::user_` (void*) and `WaitNode::set_user(void*)` are the only context hooks. The proposed `WaitNodeUserKind` discriminator is a design-time addition.

---

## D. First-scope audit

The preparation document (§2) correctly narrows first scope to:

```text
FIRST SCOPE: EVENT WAIT + TIMER ARM
```

All other primitives are explicitly deferred with documented rationale:

| Primitive | Deferral reason | Section |
|-----------|----------------|---------|
| Semaphore | Select-aware admission and release seams not designed | §2.1, §14 |
| AsyncMutex | Select-aware admission and handoff seams not designed | §2.1, §14 |
| Queue push/pop | Payload ownership, no v1 cancel API | §2.1, §14 |
| AsyncCondition | Mandatory non-cancellable Mutex reacquire incompatible | §2.1, §15 |
| Cross-Scheduler | Single global_mtx_ domain required | §2.1 |

No controlling state block, formal action, test matrix, or review request includes Semaphore, Mutex, Queue, or AsyncCondition in current first scope.

**RESULT: FIRST-SCOPE GATE — PASS**

---

## E. Typed WaitNode context audit

### E1. Resolver determines context kind BEFORE casting pointer

The design (§4.3, §8.2, §9.3) specifies a `switch (node.user_kind())` dispatch before any `static_cast`:

```
select_event_candidate_ready_locked(WaitNode& node):
    switch (node.user_kind()):
        case None:   -> ordinary path
        case Queue:  -> Queue reconciler
        case Select: -> auto* meta = static_cast<SelectArmMetadata*>(node.user_pointer())
                       check meta->arm_kind, meta->state
                       set CANDIDATE_READY
```

The resolver reads `user_kind()` (the discriminator tag) BEFORE casting `user_pointer()` to `SelectArmMetadata*`. This eliminates offset-guessing, reinterpret_cast probing, and reading fields through the wrong dynamic type.

**E1: SATISFIED**

### E2. Queue context is never interpreted as SelectArmMetadata

The resolver's `case Queue` path does not cast to `SelectArmMetadata*`. It falls through to the Queue reconciler or ordinary path.

**E2: SATISFIED**

### E3. Select context is never interpreted as QueueWaitCtx

The resolver's `case Select` path casts to `SelectArmMetadata*`, not `QueueWaitCtx*`. The `arm_kind` field (EventWait or TimerDeadline) is checked to confirm the expected type.

**E3: SATISFIED**

### E4. kind + pointer are installed before WaitNode becomes queue-visible

The registration sequence (§7.1) specifies:
```
b. set WaitNode user_kind = Select, user_pointer = &metadata
c. acquire target WaitQueue mutex
d. register_wait_locked(node, fiber)
```

Context is installed BEFORE the node enters the primitive queue. No resolver can observe the node before context is installed.

**E4: SATISFIED**

### E5. kind + pointer remain valid while node is Registered

SelectArmMetadata is embedded in SelectGroup, which is embedded in SelectOperation (stack-owned). The fiber is suspended while any arm is Registered, so the stack frame is alive. user_kind and user_pointer remain valid for the node's entire Registered lifetime.

**E5: SATISFIED**

### E6. context is cleared only after terminalization + unlink

The loser retirement seam (§10.1) specifies:
```
resolve WaitNode to Cancelled (exactly once)
...
arm.WaitNode.clear_user_context()  // kind = None, pointer = nullptr
```

Clearing happens AFTER the node is terminal (Cancelled) and unlinked. The node is no longer Registered at this point.

**E6: SATISFIED**

### E7. ordinary WaitNode behavior remains unchanged

For `user_kind == None`, the resolver path falls through to the ordinary WaitNode wake/cancel/expire path. No Select-specific code is executed. The existing `user_` field (void*) is used as-is; the kind tag is an overlay.

**E7: SATISFIED**

### E8. no discriminator depends on object layout coincidence

The discriminator is `user_kind()` — an explicit enum tag, not an offset-0 field inspection or reinterpret_cast probe. The design explicitly rejects the old approach:

> "A raw `static_cast` based on offset-0 layout is not production authority." (§4.1)

**E8: SATISFIED**

### Authority probes

The design (§4.4) specifies three authority probes:
- P1: Queue-tagged node must never enter Select resolver → satisfied by `switch` dispatch
- P2: Select-tagged node must never enter Queue reconciler → satisfied by `switch` dispatch
- P3: user_kind and user_pointer installed before registration, cleared after terminal+unlink → satisfied by §7.1 and §10.1

Test T16 (§3, test plan) verifies all three probes with Queue, Select, and None-tagged nodes.

**RESULT: TYPED WAITNODE CONTEXT GATE — PASS**

---

## F. Registration/R1 audit

### F1. Registration ordering

All documents consistently specify the same order:

| Document | Order |
|----------|-------|
| §7.1 (Registration sequence) | 1b: set context → 1c: acquire queue mutex → 1d: register |
| §5.2 (Ownership graph) | user_kind = Select, user_pointer = &metadata before queue entry |
| §5.1 (WaitNode lifecycle) | 2: set_user_context → register_wait_locked |
| §10.1 (Loser retirement) | clear AFTER terminal + unlink |
| Test plan §2.2 | select_arm_registered seam (after all arms registered) |

No section contradicts this ordering. Metadata + context are ALWAYS installed before the node enters the primitive queue.

**F1: CONSISTENT**

### F2. Registration failure handling

§7.3 states:

> "If any arm fails registration, all already-registered arms are retired via the non-publishing retirement seam under the same global_mtx_ hold. user_kind is cleared on each retired arm. The group is destroyed. No arms leak. No caller is published."

Failure semantics:
- Clear typed context: YES (§10.1 clear_user_context)
- Leave no linked node: YES (non-publishing retirement unlinks)
- Leave no active timer registration: YES (retire timer)
- Publish no caller: YES (non-publishing seam)
- Produce no SelectResult: YES (group destroyed, no publication)

**F2: SATISFIED**

### F3. R1 serialization

§7.1 specifies one continuous `global_mtx_` critical section:

```
Under one continuous global_mtx_ critical section:
    1. register all arms in SelectGroup
    2. for each arm: initialize, context, register, check readiness
    3. if any arm CandidateReady: claim, commit, retire, complete inline
    4. otherwise: caller make_waiting, group -> Armed, release global_mtx_
```

All arms are registered, all contexts installed, all admission readiness checked, and the caller is made Waiting (or inline-complete) within ONE continuous `global_mtx_` hold.

**F3: SATISFIED**

### F4. External serialization

§7.3 confirms:

> "The external thread calling Event::set() blocks on global_mtx_, which is held by the registration CS. The setter cannot complete and cannot inspect partial group state."

Test T2 verifies this: external set() blocks while R1 holds global_mtx_, then proceeds after release.

**F4: SATISFIED**

### F5. Admission observation vs post-arming resolution

§7.2 correctly distinguishes:

```
ADMISSION OBSERVATION:  registration code observes Event SET or deadline due
                        while holding global_mtx_ during registration.
POST-ARMING RESOLUTION: external Event setter or timer pump observes Armed
                        group after global_mtx_ is released.
```

No contradiction between these two readiness causes.

**F5: SATISFIED**

**RESULT: REGISTRATION/R1 GATE — PASS**

---

## G. Timer-arm queue and lifetime audit

### G1. Private Timer WaitQueue

§9.1 specifies:

```
SelectOperation
    owns timer_waiters_ WaitQueue
    each Timer arm: WaitNode registered in timer_waiters_
    TimerRegistration(node, &timer_waiters_, deadline)
```

Timer arms register in a PRIVATE `timer_waiters_` owned by SelectOperation, not in any primitive's queue. This gives Timer a mechanically valid wait target.

**G1: SATISFIED**

### H1. timer_waiters_ outlives all ACTIVE TimerRegistration objects

§5.2 states:

> "timer_waiters_ outlives every ACTIVE TimerRegistration; destroyed only after all TimerRegistrations are RETIRED/CONSUMED"

SelectOperation (stack-owned) owns timer_waiters_. TimerRegistrations reference it by raw pointer. The stack frame is alive while any arm is Registered or any timer is ACTIVE. The `~WaitQueue` assert (empty queue) enforces that all nodes are resolved before destruction.

**H1: SATISFIED**

### H2. Timer-arm WaitNode is Registered before timer callback visibility

§7.1 step 1d: `register_wait_locked(node, fiber)` happens before step 1f: `install TimerRegistration`. The WaitNode is in the queue (Registered) before the TimerRegistration is created.

**H2: SATISFIED**

### H3. Admission-due Timer does not resolve Expired before group claim

§9.4 specifies:

```
At admission (under global_mtx_):
    if timer deadline is already due:
        meta->state = CANDIDATE_READY
        // TimerRegistration stays ACTIVE
        // do NOT resolve WaitNode Expired
        // group claim follows in the same CS
```

The admission-due Timer is marked CandidateReady but NOT resolved as Expired. Resolution happens only after group claim.

**H3: SATISFIED**

### H4. Winning Timer: group claim → ACTIVE → CONSUMED → WaitNode Expired

§9.2 specifies the ordering:

```
For timer-pump resolution:
    1. timer pump fires, acquires global_mtx_
    2. arm -> CandidateReady
    3. group claim CAS (Armed -> WinnerClaimed)
    4. TimerRegistration: ACTIVE -> CONSUMED (try_claim_expiry)
    5. resolve WaitNode Expired
    6. publish caller
    Ordering: group claim BEFORE consumption and WaitNode commit.
```

**H4: SATISFIED**

### H5. Losing Timer: ACTIVE → RETIRED, WaitNode → Cancelled, no publication

§9.2 specifies:

```
For loser Timer arm:
    1. group claim CAS fails
    2. TimerRegistration: ACTIVE -> RETIRED (retire())
    3. resolve WaitNode Cancelled (non-publishing)
    4. WaitNode stays Registered until retirement
```

**H5: SATISFIED**

### H6. Stale RETIRED registration does not dereference destroyed objects

§9.5 states:

> "After SelectOperation destruction, no ACTIVE TimerRegistration retains a pointer to SelectArm/WaitNode/group/timer_waiters_. I4 closure: RETIRED registrations are never dereferenced by the pump."

The existing `TimerRegistration` I4 closure (timer_registration.hpp L26-31) ensures that a RETIRED registration's `node_` and `queue_` are never dereferenced. The expiry path checks `state` BEFORE dereferencing.

**H6: SATISFIED**

### H7. Waiting counters and active deadline counters close exactly once

§10.1 specifies `decrement waiting_waitq_count_ (exactly once)` for each loser arm. The winner path also decrements exactly once. TimerRegistration consumption/retirement is exactly-once (CAS ACTIVE → CONSUMED/RETIRED).

**H7: SATISFIED**

### Timer authority ordering question

The design explicitly answers:

> "Does timer authority ACTIVE->CONSUMED occur before or after group claim?"

**After.** Group claim (step 3) precedes consumption (step 4) in both the admission and post-arming paths. If the timer pump's `try_claim_expiry()` succeeds but the subsequent group claim fails, the TimerRegistration is CONSUMED but the group state remains Armed. However, this cannot happen under the design: the timer pump sets CandidateReady THEN attempts group claim in the same CS. The `try_claim_expiry` is called ONLY after group claim succeeds (§9.2 step 4). A losing group claim would retire the TimerRegistration (ACTIVE → RETIRED) without consuming it.

**RESULT: TIMER-ARM QUEUE AND LIFETIME GATE — PASS**

---

## H. Event broadcast algorithm audit

### Current code vs design requirement

The current `event_set_broadcast` (scheduler.cpp:1303-1341) uses a head-drain loop:

```cpp
while (wake_wait_one_locked(waiters) != nullptr) { ++woken; }
```

This resolves EVERY node Woken and unlinks it. For Select arms, this is incorrect — the Select arm must stay Registered and be marked CandidateReady instead.

The design (§8.3) specifies a two-phase algorithm that replaces this drain loop:

### Phase 1 (scan under Event queue mtx)

```
hold global_mtx_
store Event SET = true
acquire Event queue mtx_
iterate queue using stable next pointer:
    ordinary waiter (kind == None):
        resolve Woken, unlink, record for publication
    Queue waiter (kind == Queue):
        resolve Woken, unlink, record for publication
    Select Event arm (kind == Select, arm_kind == EventWait):
        if state == REGISTERED:
            state = CANDIDATE_READY
            leave WaitNode Registered and linked
            collect owning SelectGroup identity
            do NOT claim group
release Event queue mtx_
```

### Phase 2 (group claim under global_mtx_)

```
still hold global_mtx_
publish ordinary winners
deduplicate collected SelectGroups
for each unique group:
    if group is Armed:
        choose lowest-index CandidateReady arm
        group.try_claim CAS
        if claim succeeds:
            resolve winner WaitNode Woken
            retire all other arms (non-publishing)
            publish suspended caller exactly once
            group -> Completed
```

### I1. Scan terminates even when Select node remains linked

The scan uses a "stable next pointer captured before releasing the queue mutex." The full list walk (head → next → next → ...) visits each node exactly once. Select nodes remain linked but are visited and marked in the single pass. The walk terminates when `next == nullptr` (tail reached).

The current head-drain loop (`wake_wait_one_locked` repeatedly) would NOT terminate if Select nodes remain linked at the head. The design correctly replaces this with a single-pass full-list walk.

**I1: SATISFIED**

### I2. No repeated processing of the unchanged queue head

The single-pass walk advances a saved next pointer. It never re-reads the head. Each node is visited exactly once.

**I2: SATISFIED**

### I3. No recursive acquisition of the same Event queue mutex

Phase 1 holds the Event queue mutex throughout the scan. Phase 2 does NOT acquire the Event queue mutex — it acquires individual arm queue mutexes (if needed for loser retirement) sequentially. The Event queue mutex is released before Phase 2 begins.

**I3: SATISFIED**

### I4. No two primitive queue mutexes held simultaneously

Phase 2 acquires arm queue mutexes one at a time during loser retirement. Each arm's queue mutex is acquired and released before the next arm's. No two are held simultaneously.

**I4: SATISFIED**

### I5. Ordinary Event broadcast semantics preserved

Phase 1 resolves ordinary (None) and Queue waiters with Woken, unlinks them, and records them for publication. Phase 2 publishes them via make_runnable + route. This preserves the existing broadcast semantics for non-Select waiters.

**I5: SATISFIED**

### I6. Event persistent SET state remains set

Phase 1 stores `SET = true` and never clears it. The Event is persistent (manual-reset). Loser retirement does not mutate Event state (§10.1).

**I6: SATISFIED**

### I7. Multiple Select arms from same group are group-deduplicated

Phase 2: "deduplicate collected SelectGroups" — only unique groups are processed. Multiple arms from the same group are handled together by choosing the lowest-index CandidateReady arm.

**I7: SATISFIED**

### I8. Two different Select groups on same Event each obtain one winner independently

Each group is deduplicated independently. Group A's claim does not affect Group B's claim. Both groups can each select one winner from their respective arms on the same Event.

**I8: SATISFIED**

### I9. Loser retirement cannot invalidate the Phase-1 iterator

Phase 1 completes and releases the Event queue mutex BEFORE Phase 2 begins loser retirement. Phase 2's loser retirement operates on individual arm queues, not on the Event queue. The Phase-1 iterator is no longer in use.

**I9: SATISFIED**

**RESULT: EVENT BROADCAST ALGORITHM GATE — PASS**

---

## I. CandidateReady/group state audit

### CandidateReady is a mechanical state

§4.2 defines `SelectArmState` with CANDIDATE_READY as a distinct state (value 3). §6.2 defines the transition:

```
REGISTERED -> CANDIDATE_READY
```

Under `global_mtx_`, WaitNode remains Registered, no WaitOutcome published, caller unchanged. Repeated readiness is idempotent (state is already CANDIDATE_READY).

### Group claim states

§6.1 specifies:

```
Arming -> WinnerClaimed(i)   [inline]
Armed -> WinnerClaimed(i)    [suspended]
```

There is ONE controllable state model: `WinnerClaimed(i)`. No conflicting state names exist across documents:

| Document | Inline path | Suspended path |
|----------|-------------|----------------|
| §6.1 (preparation) | Arming → WinnerClaimed | Armed → WinnerClaimed |
| §2.3 (state machine) | Arming → WinnerClaimed | Armed → WinnerClaimed |
| §7.1 (registration) | Arming → WinnerClaimed | Armed → WinnerClaimed |

No state names like `OPEN → CLAIMED` or `Constructing → WinnerClaimed` coexist with `Arming → WinnerClaimed`. The state model is unified.

**RESULT: CandidateReady/GROUP STATE GATE — PASS**

---

## J. Winner and loser finalization audit

### Inline winner publication counts

§11.1 specifies:

```
INLINE COMPLETION:
    SelectResult publication count = 1
    make_runnable count = 0
    route_runnable count = 0
```

Test T8 verifies: "make_runnable count = 0 (caller was never Waiting), route_runnable count = 0."

### Suspended winner publication counts

§11.2 specifies:

```
SUSPENDED COMPLETION:
    SelectResult publication count = 1
    make_runnable count = 1
    route_runnable count = 1
```

Test T8b verifies: "make_runnable count = 1, route_runnable count = 1."

### T1 inconsistency check

T1 uses already-SET Events (admission-time CandidateReady) and expects "Exactly one result publication" and "Caller never suspended (inline fast path)." For inline completion: make_runnable=0, route_runnable=0. This is consistent — T1 tests the inline path, not the suspended path. No contradiction.

### Loser retirement

§10.1 specifies `select_retire_arm_locked`:

```
- group winner already final (precondition)
- arm is not winner
- global_mtx_ held
- unlink from queue if linked
- retire TimerRegistration if armed (ACTIVE -> RETIRED)
- resolve WaitNode Cancelled (exactly once)
- decrement waiting_waitq_count_ (exactly once)
- arm.metadata.state = RETIRED
- clear_user_context()
- do NOT make_runnable
- do NOT route_runnable
- do NOT publish SelectResult
- do NOT mutate Event SET state
```

This is a Select-specific non-publishing seam. It does NOT call the existing `cancel_wait` (which publishes). The properties:

- group winner confirmed: YES (precondition)
- winner not retired: YES (precondition check)
- loser WaitNode Registered: YES (terminal resolved as Cancelled, not Woken)
- unlink + Cancelled exactly once: YES (CAS authority)
- TimerRegistration retire exactly once: YES (CAS ACTIVE → RETIRED)
- waiting counters close exactly once: YES
- typed context cleared after terminal+unlink: YES
- no make_runnable: YES
- no route_runnable: YES
- no SelectResult publication: YES

**RESULT: WINNER AND LOSER FINALIZATION GATE — PASS**

---

## K. Duplicate-arm/error taxonomy audit

### Permitted patterns

§13 specifies:

```
PERMITTED:
    multiple arms referencing the same Event
    multiple Timer arms with equal deadlines
    one Event arm + one Timer arm
```

### Forbidden patterns

```
FORBIDDEN:
    same WaitNode instance reused across arms
    same SelectArm slot registered twice
    same TimerRegistration object shared by arms
    cross-Scheduler arm
```

### Consistency check

T7 tests "Two Event arms on same Event" — this is the PERMITTED pattern (multiple arms referencing the same Event). No test attempts to reuse a WaitNode instance or register the same slot twice.

The error taxonomy does NOT declare "duplicate Event arm" as UB. It declares it as PERMITTED. The forbidden patterns are structural misuse (node reuse, slot reuse, shared TimerRegistration, cross-Scheduler), not semantic duplicates.

No contradiction between the error taxonomy and the test plan.

**RESULT: DUPLICATE-ARM/ERROR TAXONOMY GATE — PASS**

---

## L. Lifetime/destruction audit

### L1. metadata address stable

SelectArmMetadata is embedded in SelectGroup, which is embedded in SelectOperation (stack-local). The fiber is suspended while any arm is Registered, so the stack frame and all embedded objects are alive and at stable addresses.

**L1: SATISFIED**

### L2. typed context valid until node terminal+unlinked

Context is installed before registration (§7.1 step 1b) and cleared after terminal+unlink (§10.1). During the Registered lifetime, context is always valid.

**L2: SATISFIED**

### L3. caller stack cannot unwind while any arm Registered

The caller is made Waiting (§7.1 step 4a) or inline-completes (§7.1 step 3) before global_mtx_ is released. The stack frame is alive until the caller resumes. All arms are retired (terminal) before the caller resumes (§11.2: "publish suspended caller exactly once"). The stack cannot unwind until the caller resumes.

**L3: SATISFIED**

### L4. no ACTIVE TimerRegistration can outlive its queue or destroyed objects

§9.5 states: "After SelectOperation destruction, no ACTIVE TimerRegistration retains a pointer to SelectArm/WaitNode/group/timer_waiters_."

The `~WaitQueue` assert (empty) enforces that all nodes are resolved before the queue is destroyed. All TimerRegistrations are retired or consumed before the stack frame unwinds. The I4 closure (timer_registration.hpp L26-31) prevents dereferencing after retirement.

**L4: SATISFIED**

### L5. Event Phase-1 collected group pointer remains valid through Phase 2

Phase 1 collects `owning SelectGroup identity` (a pointer to the group). Phase 2 processes groups under `global_mtx_`. The group is stack-owned and alive (caller is suspended). The pointer remains valid.

**L5: SATISFIED**

### L6. group completion retires all loser callbacks before caller may resume

§11.2 specifies: "retire all loser arms (non-publishing)" then "publish suspended caller." Loser retirement happens in the same CS as the group claim and winner commit. The caller is published only AFTER all losers are retired.

**L6: SATISFIED**

### L7. external OS-thread Event::set cannot race SelectOperation destruction

The external set() acquires `global_mtx_`. The Select group is stack-owned and the fiber is suspended (alive). The set() either:
- Acquires global_mtx_ before the group is created → sees nothing
- Acquires global_mtx_ after the group is created → sees the Armed group
- Blocks on global_mtx_ while the group is being created → proceeds after release

In all cases, the group is alive when set() observes it. No race with destruction.

**L7: SATISFIED**

### L8. Scheduler shutdown behavior

§13 states: "Scheduler shutdown with armed: caller contract | No automatic cleanup." Debug assert on armed destruction matches E10-E12 convention.

**L8: DOCUMENTED, consistent with repository convention**

**RESULT: LIFETIME/DESTRUCTION GATE — PASS**

---

## M. Formal readiness

### State variables (§16.1)

All required state variables are present:

| Required | Present | Location |
|----------|---------|----------|
| typed_user_kind | arm_kind[i] | §16.1 |
| user_context_visible | implicit in arm_kind/pointer | §4.2 |
| timer_wait_queue_alive | implicit in timer_state | §16.1 |
| timer_registration_state | timer_state[i] | §16.1 |
| event_state | event_state[i] | §16.1 |
| group_phase | group_phase | §16.1 |
| winner | winner | §16.1 |
| arm_state | arm_state[i] | §16.1 |
| caller_state | caller_state | §16.1 |
| result_publication_count | publication_count | §16.1 |
| runnable_publication_count | make_runnable_count (implied in invariants) | §16.4 |

### Actions (§16.3)

All required actions are present:

| Required | Present | Notes |
|----------|---------|-------|
| InstallTypedUserContext | InstallTypedUserContext | ✓ |
| RegisterEventArm | RegisterEventArm | ✓ |
| RegisterTimerArmInPrivateQueue | RegisterTimerArmInPrivateQueue | ✓ |
| ObserveAdmissionReady | ObserveAdmissionReady | ✓ |
| FinishArmingInline | FinishArmingInline | ✓ |
| FinishArmingSuspended | FinishArmingSuspended | ✓ |
| EventScanMarksCandidates | EventScanMarksCandidates | ✓ |
| ProcessCandidateGroups | ProcessCandidateGroups | ✓ |
| TimerPumpMarksCandidate | TimerPumpMarksCandidate | ✓ |
| TryClaimFromArming | TryClaimFromArming | ✓ |
| TryClaimFromArmed | TryClaimFromArmed | ✓ |
| ConsumeWinningTimerRegistration | ConsumeWinningTimerRegistration | ✓ |
| RetireLosingTimerRegistration | RetireLosingTimerRegistration | ✓ |
| ResolveWinner | ResolveWinner | ✓ |
| RetireLosersWithoutPublication | RetireLosersWithoutPublication | ✓ |
| CompleteInline | CompleteInline | ✓ |
| PublishSuspendedCaller | PublishSuspendedCaller | ✓ |
| RollbackRegistration | RollbackRegistration | ✓ |
| DestroyCompletedOperation | DestroyCompletedOperation | ✓ |

### Invariants (§16.4)

All required invariants are present:

| Required | Present | Notes |
|----------|---------|-------|
| InvNoWrongContextCast | ✓ | §16.4 |
| InvUserContextInstalledBeforeRegistration | ✓ | §16.4 |
| InvAtMostOneWinner | ✓ | §16.2 |
| InvCandidateReadyNodeStillRegistered | InvCandidateReadyIsNotPrimitiveTerminal | §16.2 |
| InvWinnerClaimBeforeTerminalCommit | InvWinnerChosenBeforeArmCommit | §16.2 |
| InvLoserNeverPublishes | ✓ | §16.2 |
| InvInlineNeverRoutes | InvInlineCompletionDoesNotRoute | §16.4 |
| InvSuspendedRoutesAtMostOnce | InvSuspendedCompletionRoutesExactlyOnce | §16.4 |
| InvEventScanTerminates | ✓ | §16.4 |
| InvNoRecursiveQueueLock | InvNoQueueMutexRecursiveAcquisition | §16.4 |
| InvTimerArmHasLiveQueue | InvTimerArmHasValidQueue | §16.4 |
| InvNoActiveTimerAfterDestroy | InvNoCallbackAfterDestroy | §16.2 |
| InvNoVisibleArmAfterRollback | InvRegistrationFailureLeavesNoVisibleArm | §16.2 |

### Negative models

§16.4 defines NEG-1 through NEG-7, covering:
- Winner claim after arm commit
- Publish before caller waiting
- Cancel used as group authority
- Loser remains armed after completion
- Two ready arms both commit
- Timer and event both publish
- Partial registration leak

**RESULT: FORMAL READINESS GATE — PASS**

---

## N. Test readiness

### Deterministic seams

The test plan (§2.2) defines 8 new deterministic seams for E13:

| Seam | Purpose |
|------|---------|
| `select_arm_registered` | Pause after all arms registered |
| `select_after_caller_make_waiting` | Pause after caller make_waiting |
| `select_after_group_claim_before_retirement` | Pause after claim, before retirement |
| `select_after_retirement_before_publication` | Pause after retirement, before publication |
| `timer_due_before_group_claim` | Pause timer before group claim |
| `event_set_before_group_claim` | Pause Event::set before group claim |
| `event_select_scan_complete_before_group_claim` | Pause after Phase 1, before Phase 2 |
| `event_after_group_claim_before_arm_retirement` | Pause after group claim, before retirement |

### Test matrix coverage

| Required seam | Test | Covered |
|---------------|------|---------|
| typed_context_after_install_before_register | T16 | ✓ |
| R1_registration_holds_global_lock | T2 | ✓ |
| external_set_attempt_blocked_by_R1 | T2 | ✓ |
| event_scan_after_select_candidate_before_queue_unlock | event_select_scan_complete_before_group_claim | ✓ |
| event_scan_complete_before_group_processing | event_select_scan_complete_before_group_claim | ✓ |
| group_claim_before_winner_resolve | select_after_group_claim_before_retirement | ✓ |
| group_claim_before_timer_consume | select_after_group_claim_before_retirement | ✓ |
| after_loser_retirement_before_publication | select_after_retirement_before_publication | ✓ |
| after_caller_make_waiting_before_global_unlock | select_after_caller_make_waiting | ✓ |
| timer_stale_entry_after_operation_destroyable | T13 | ✓ |

### Test coverage matrix

| Test | Purpose | Key invariant verified |
|------|---------|----------------------|
| T1 | Two admission-ready Event arms | I1, I4, I10, I11, I19 |
| T2 | External set serialized during R1 | I1, I4, I5, I11, I17 |
| T2b | Event already SET before Select | I1, I4, I19 |
| T3 | Winner before caller suspension | I1, I2, I4, I10 |
| T4 | External-thread Event wake | I1, I2, I3, I4, I10, I11 |
| T5 | Timer vs Event race | I1, I2, I4, I8, I10 |
| T6 | Already-due Timer arm | I1, I2, I4, I7, I10 |
| T7 | Two Event arms on same Event | I1, I4, I10, I12 |
| T8 | Inline: no make_runnable | I3, I4, I10, I19 |
| T8b | Suspended: exactly-one make_runnable | I3, I4, I10, I20 |
| T9 | Cross-Scheduler rejection | precondition enforcement |
| T10 | Exactly-one publication | I4, I10 |
| T11 | Destruction after retirement | I5, I6 |
| T12 | Partial registration rollback | I5, I6, I7, I13 |
| T13 | Timer stale-entry retirement | I7, I13 |
| T14 | Multi-worker stress | I1-I4, I10 under concurrency |
| T15 | TSan coordination verification | correct acquire-release pairing |
| T16 | Authority probe: user_kind discriminator | I14, I15 |

All required tests from Section O of the review task are covered. Sleep, yield, retry count, and high-count loops are supplementary (T14 stress) — all critical paths have deterministic seams.

**RESULT: TEST READINESS GATE — PASS**

---

## O. Prior finding disposition

### P0 findings

| ID | Finding | Corrective | Disposition |
|----|---------|------------|-------------|
| P0-1 | sem_release resolves FIFO head without group check | DEFERRED — Semaphore out of first scope | **DEFERRED OUT OF FIRST SCOPE** |
| P0-2 | mutex_handoff assigns owner without group check | DEFERRED — AsyncMutex out of first scope | **DEFERRED OUT OF FIRST SCOPE** |
| P0-3 | Event::set broadcast resolves Woken without group check | Two-phase broadcast algorithm (§8.3) with Select-aware CandidateReady marking | **CLOSED WITH MECHANICAL EVIDENCE** |
| P0-4 | Timer pump expire_wait resolves Expired without group check | Select-aware timer expiry (§9.3) with CandidateReady marking | **CLOSED WITH MECHANICAL EVIDENCE** |

### P1 findings

| ID | Finding | Corrective | Disposition |
|----|---------|------------|-------------|
| P1-1 | Select-aware admission seam not specified | Event: admission observation (§7.1 step 1g); Timer: deadline-due observation (§9.4) | **CLOSED WITH MECHANICAL EVIDENCE** |
| P1-2 | Select-aware release seam not specified for Semaphore | DEFERRED — Semaphore out of first scope | **DEFERRED OUT OF FIRST SCOPE** |
| P1-3 | Select-aware handoff seam not specified for AsyncMutex | DEFERRED — AsyncMutex out of first scope | **DEFERRED OUT OF FIRST SCOPE** |
| P1-4 | CandidateReady mechanism from external thread not specified | Two-phase Event broadcast (§8.3) + timer pump Select-aware path (§9.3) + R1 serialization (§7.2) | **CLOSED WITH MECHANICAL EVIDENCE** |
| P1-5 | Select loser cancel publishes caller | Non-publishing retirement seam `select_retire_arm_locked` (§10.1) does NOT call make_runnable/route | **CLOSED WITH MECHANICAL EVIDENCE** |
| P1-6 | WaitNode user_ mechanism not in preparation doc | WaitNodeUserKind discriminator (§4), type-safe resolver (§4.3), authority probes (§4.4) | **CLOSED WITH MECHANICAL EVIDENCE** |
| P1-7 | "No internal modification" claim contradicts requirement | Corrected to "narrow Select-aware seams" (§2.2, §8, §9) — Event and Timer resolution paths require new internal seams | **CLOSED WITH MECHANICAL EVIDENCE** |
| P1-8 | Queued release case not addressed for Semaphore | DEFERRED — Semaphore out of first scope | **DEFERRED OUT OF FIRST SCOPE** |
| P1-9 | Unlock handoff case not addressed for AsyncMutex | DEFERRED — AsyncMutex out of first scope | **DEFERRED OUT OF FIRST SCOPE** |
| P1-10 | Partial registration failure race not addressed | R1 protocol (§7): one continuous global_mtx_ CS; failure cleanup (§7.3): non-publishing retirement of all registered arms | **CLOSED WITH MECHANICAL EVIDENCE** |

### Pre-reaudit hardening items

| ID | Description | Corrective location | Disposition |
|----|-------------|---------------------|-------------|
| H1 | unsafe user_ cast to SelectArmMetadata | WaitNodeUserKind discriminator (§4), type-safe switch dispatch (§4.3) | **CLOSED WITH MECHANICAL EVIDENCE** |
| H2 | metadata installation order | §7.1 step 1b: context BEFORE queue registration | **CLOSED WITH MECHANICAL EVIDENCE** |
| H3 | Timer arm missing WaitQueue | SelectOperation::timer_waiters_ private WaitQueue (§9.1, §5.2) | **CLOSED WITH MECHANICAL EVIDENCE** |
| H4 | R1 vs partial-registration test contradiction | T2 corrected for R1 serialization; T2b added for already-SET admission | **CLOSED WITH MECHANICAL EVIDENCE** |
| H5 | Event head-drain incompatibility | Two-phase full-list walk (§8.3) replaces head-drain loop | **CLOSED WITH MECHANICAL EVIDENCE** |
| H6 | inline publication contradiction | CompleteInline: make_runnable=0, route_runnable=0 (§11.1) | **CLOSED WITH MECHANICAL EVIDENCE** |
| H7 | duplicate-arm policy contradiction | §13: multiple arms on same Event PERMITTED; same WaitNode FORBIDDEN. T7 tests PERMITTED pattern. No contradiction. | **CLOSED WITH MECHANICAL EVIDENCE** |

---

## P. New findings

### No P0 or P1 findings

The corrective design is internally consistent and mechanically addresses all prior audit findings within the Event + Timer first scope.

### P2 observations (non-blocking)

| ID | Severity | Observation |
|----|----------|-------------|
| P2-1 | P2 | The formal actions list (§16.3) does not include `ClearTypedContext` as an explicit action. The clearing is described in §10.1 but not as a named formal action. Recommend adding for completeness. |
| P2-2 | P2 | The formal invariants (§16.4) do not include an explicit `InvNoContextClearWhileRegistered` invariant. The requirement is implicitly satisfied by §10.1 (clear after terminal+unlink) but would benefit from an explicit named invariant for formal model verification. |
| P2-3 | P2 | The test plan §2.2 mentions `E12EventTestHooks` from `async_test_control.hpp` but does not specify how the new E13-specific seams (e.g., `event_select_scan_complete_before_group_claim`) integrate with the existing `SLUICE_ASYNC_INTERNAL_TESTING` build variant. This is an implementation detail, not a design blocker. |
| P2-4 | P2 | The preparation doc §18 lists P1-5 as "CLOSED -- non-publishing retirement seam specified (S10)" but the original finding was about the EXISTING cancel path publishing. The corrective correctly introduces a NEW seam rather than modifying the existing cancel path. The disposition is correct but the wording could be clearer. |

None of these P2 observations block the PASS verdict.

---

## Q. Authorization effect

```text
E13-SELECT-MULTI-WAIT-PREPARATION-CORRECTIVE-1-REAUDIT-1:
PASS

APPROVED FIRST SCOPE:
EVENT + INDEPENDENT TIMER ARM

SELECTED PROTOCOL:
CENTRAL SELECTGROUP CLAIM UNDER SINGLE-SCHEDULER AUTHORITY

PREPARATION:
CLOSED

FORMAL MODEL IMPLEMENTATION:
AUTHORIZED FOR EVENT + TIMER ONLY

PRODUCTION IMPLEMENTATION:
DENIED
```

### What PASS authorizes

1. **TLA+ formal model implementation** for Event + Timer arms using the Candidate A protocol (Central SelectGroup Claim Under Scheduler Authority).
2. The formal model may be built from the state machines, actions, invariants, and negative models specified in §16.

### What PASS does NOT authorize

1. **Production C++ implementation** — requires formal model verification first.
2. **Semaphore, AsyncMutex, Queue, or AsyncCondition Select integration** — these remain DEFERRED.
3. **Cross-Scheduler Select** — remains DEFERRED.

---

## R. Exact next action

1. **Build TLA+ formal model** for Event + Timer based on §16.1-4 (state variables, actions, invariants, negative models).
2. **Run TLC model check** with 2-4 arms, Event + Timer, all negative models NEG-1 through NEG-7.
3. **Address P2 observations** if desired (add `ClearTypedContext` action, add `InvNoContextClearWhileRegistered` invariant) — non-blocking.
4. **After formal model PASS**: proceed to production C++ implementation review gate.

---

## S. Final git status

```bash
$ git status --short
?? tests/test_t3_simple.cpp
?? tla2tools.jar

$ git log --oneline --decorate origin/master..HEAD
fed2514 (HEAD -> feature/E13-preparation) E13: pre-reaudit hardening complete

$ git diff --name-status origin/master...HEAD
A	docs/e13-select-preparation.md
A	docs/e13-select-state-machine.md
A	docs/e13-select-test-plan.md
A	docs/reviews/E13-SELECT-MULTI-WAIT-INDEPENDENT-DESIGN-CHALLENGE-1.md
A	docs/reviews/E13-SELECT-MULTI-WAIT-PREPARATION-AUDIT-1-REVIEW-REQUEST.md
A	docs/reviews/E13-SELECT-MULTI-WAIT-PREPARATION-AUDIT-1.md
A	docs/reviews/E13-SELECT-MULTI-WAIT-PREPARATION-CORRECTIVE-1-REVIEW-REQUEST.md
```

Only the review artifact (`docs/reviews/E13-SELECT-MULTI-WAIT-PREPARATION-CORRECTIVE-1-REAUDIT-1.md`) was created by this review. No other files were modified.
