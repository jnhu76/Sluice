# E13 Select State Machine Specification

**Task**: `E13-SELECT-MULTI-WAIT-PREPARATION-CORRECTIVE-1`
**Parent**: `docs/e13-select-preparation.md`

---

## 1. Overview

This document specifies the state machines for SelectGroup, SelectArm, and
their interaction with the existing WaitNode and Scheduler state machines.
All state transitions are serialized under `Scheduler::global_mtx_` unless
otherwise noted.

First scope: Event wait + Timer arm.

---

## 2. SelectGroup State Machine

### 2.1 States

```
Constructing    -- group created, arms being registered
Arming          -- all arms registered, entering primitive queues
Armed           -- at least one arm waiting, caller suspended
WinnerClaimed(i)-- arm i won the group claim CAS
Publishing      -- winner commit + loser retirement in progress
Completed       -- caller published runnable, result available
Destroyed       -- group and all arms are terminal
```

### 2.2 Transitions

```
                    register_arm()
    Constructing ---------------> Constructing
          |
          |  make_select_waiting()
          |  (all arms Registered, enter primitive queues)
          v
       Arming ---- arm[i] ready at admission --> WinnerClaimed(i)
          |
          |  (no arm ready; commit to Waiting)
          v
        Armed
          |
          |  arm[j] becomes CandidateReady
          |  group.try_claim(j) CAS succeeds
          v
    WinnerClaimed(j)
          |
          |  winner_commit(j) + retire_losers()
          v
     Publishing
          |
          |  route_runnable_locked(caller)
          v
     Completed
          |
          |  caller consumes result
          v
     Destroyed
```

### 2.3 CAS specification

```
group_state : atomic<GroupPhase>
group_winner_index : Nat  (valid only when group_state = WinnerClaimed)

try_claim(arm_index : Nat) : bool
    atomic CAS:
        expected = Armed (or Arming for fast-path)
        desired  = WinnerClaimed(arm_index)
    returns true  -> this arm is the unique winner
    returns false -> another arm already claimed

Synchronization: acq_rel on success, acquire on failure.
Serialization: under global_mtx_ (caller must hold it).
```

### 2.4 Fast-path

```
If an arm is already CandidateReady at make_select_waiting time
(admission observation: Event SET or deadline already due):
    -> group transitions from Arming to WinnerClaimed
    -> no intermediate Armed state
    -> the CAS is: Arming -> WinnerClaimed(i)
    -> still under global_mtx_, still single-winner
    -> CompleteInline: caller never made Waiting, make_runnable=0, route=0
```

---

## 3. SelectArm State Machine

### 3.1 States

```
DETACHED        -- arm slot exists but no primitive is bound
REGISTERING     -- arm is being added to the group
REGISTERED      -- WaitNode registered in primitive queue, metadata installed
CANDIDATE_READY -- primitive signaled readiness (Event SET or timer due)
WINNER          -- group claim succeeded for this arm
LOSER           -- group claim failed for this arm
RETIRED         -- arm fully retired (node terminal, timer retired, metadata cleared)
```

### 3.2 Transitions

```
    DETACHED ---- register(primitive) ----> REGISTERING
                                                |
                                                |  WaitNode registered, metadata installed
                                                v
                                            REGISTERED
                                                |
                                                |  Event SET / timer due
                                                |  (Select-aware resolver)
                                                v
                                         CANDIDATE_READY
                                           |         |
          group.try_claim succeeds -------+         +------- group.try_claim fails
              v                                              v
           WINNER                                          LOSER
              |                                              |
              |  winner commit +                             |  non-publishing
              |  publication                                 |  retirement
              v                                              v
          RETIRED                                         RETIRED
```

### 3.3 Transition details

| From          | To             | Action | Synchronization |
|---------------|----------------|--------|-----------------|
| DETACHED      | REGISTERING    | arm bound to group slot | none |
| REGISTERING   | REGISTERED     | WaitNode in queue, metadata installed, user_ set | global_mtx_ + queue.mtx_ |
| REGISTERED    | CANDIDATE_READY | Event SET or timer due; resolver sets metadata.state | global_mtx_ |
| CANDIDATE_READY | WINNER       | group.try_claim CAS succeeds | global_mtx_ |
| CANDIDATE_READY | LOSER        | group.try_claim CAS fails | global_mtx_ |
| WINNER        | RETIRED        | winner commit + publication complete | global_mtx_ |
| LOSER         | RETIRED        | non-publishing retirement seam | global_mtx_ |

### 3.4 State visibility

```
REGISTERED -> CANDIDATE_READY:
    This transition occurs from the Select-aware resolver path
    (Event::set() broadcast, timer pump expiry, or admission check).
    The resolver reads WaitNode::user_ to identify SelectArmMetadata.
    The metadata.state is set to CANDIDATE_READY under global_mtx_.
    The WaitNode is NOT resolved. The caller is NOT published.

CANDIDATE_READY -> WINNER/LOSER:
    ONLY under global_mtx_. The CAS is the serialization point.

WINNER/LOSER -> RETIRED:
    ONLY under global_mtx_ (same CS as the group claim).
```

---

## 4. SelectOperation State Machine

### 4.1 States

```
Building         -- arms being added (register_arm calls)
Ready            -- all arms registered, operation ready to wait
Waiting          -- caller suspended on the select
CompletedInline  -- inline completion (arm ready at admission)
CompletedSuspended -- suspended completion (post-arming winner)
Consumed         -- result read by caller
```

### 4.2 Transitions

```
    Building --- register_arm() ---> Building
          |
          |  (last arm registered)
          v
       Ready
          |                       \
          |  arm ready at          |  no arm ready;
          |  admission             |  caller suspended
          v                        v
  CompletedInline              Waiting
          |                       |
          |  result read           |  group winner published
          |                       v
          +----------------> CompletedSuspended
                                  |
                                  |  result read
                                  v
                              Consumed
```

Or equivalently as a unified model:

```
Ready -> Completed -> Consumed
Ready -> Waiting -> Completed -> Consumed
```

### 4.3 Publication rules

```
Inline completion:
    make_runnable = 0
    route_runnable = 0

Suspended completion:
    make_runnable = 1
    route_runnable = 1
```

### 4.4 Synchronization with SelectGroup

```
SelectGroup:  Constructing -> Arming -> WinnerClaimed -> Publishing -> Completed
SelectOp:     Building     -> Ready  -> Completed      -> Consumed

Inline path:
    SelectGroup: Constructing -> Arming -> WinnerClaimed (fast-path)
    SelectOp:  Ready -> CompletedInline

Suspended path:
    SelectGroup: Constructing -> Arming -> Armed -> WinnerClaimed
    SelectOp:  Ready -> Waiting -> CompletedSuspended
```

---

## 5. Interaction with WaitNode

### 5.1 WaitNode lifecycle within Select

```
Each SelectArm has one WaitNode. The WaitNode lifecycle is:

1. Construction (stack frame):
   WaitNode node;  // Detached, user_kind = None

2. Typed context installed (under global_mtx_ + target queue mtx):
   node.set_user_context(WaitNodeUserKind::Select, &metadata)

3. Registration (under global_mtx_ + target queue mtx):
   register_wait_locked(queue, node)
   // node: Detached -> Registered
   // Event arm: registered in Event's private waiters_
   // Timer arm: registered in SelectOperation's private timer_waiters_

4. CandidateReady (Select-aware resolver sets metadata.state):
   // node remains Registered and linked
   // WaitNode outcome NOT resolved

5. Group winner selected:
   // winner: CAS succeeds
   // loser:  CAS fails

6a. Winner finalization:
    // resolve Woken (Event) or Expired (Timer) under queue mutex
    // unlink in the same queue CS

6b. Loser finalization:
    // resolve Cancelled under arm queue mutex
    // unlink in the same queue CS

7. Close accounting:
    // decrement waiting_waitq_count_ exactly once
    // decrement active_deadline_count_ exactly once (timer arms)

8. TimerRegistration consumed/retired:
    // winner: ACTIVE -> CONSUMED
    // loser:  ACTIVE -> RETIRED

9. Arm marked RETIRED:
    arm.metadata.state = RETIRED

10. Clear typed context:
    clear_user_context()  // kind = None, pointer = nullptr
    // PRE: node is terminal AND unlinked

11. Publish winner result/runnable:
    // inline: SelectResult only (make_runnable=0)
    // suspended: make_runnable + route_runnable = 1 each

12. Destruction (stack frame):
    // node must be terminal (Woken/Cancelled/Expired)
    // user_kind must be None
    ~WaitNode();  // assert !is_registered()
```

### 5.2 WaitNode as group winner indicator

```
The WaitNode's outcome() is NOT the group winner indicator.
The group winner is determined by the group_state CAS.

A WaitNode with outcome() == Woken means:
    "this arm's primitive resolved this wait as woken"

It does NOT mean:
    "this arm is the group winner"

The group winner is determined separately by the group claim CAS.
For first-scope Event + Timer, the primitive resolver does NOT resolve
the WaitNode -- it sets CandidateReady on the metadata. The group claim
winner then resolves the WaitNode.
```

---

## 6. Interaction with TimerRegistration

### 6.1 Timer arm lifecycle

```
1. Registration (at arm creation):
   TimerRegistration tr(node, queue, deadline);
   // tr.state_ = Active

2. Retirement (by group loser retirement):
   tr.retire();  // Active -> Retired
   // Called under global_mtx_ in the same CS as the group claim

3. Consumption (by timer pump, if timer wins):
   tr.try_claim_expiry();  // Active -> Consumed
   // Called by pump_deadlines_locked under global_mtx_

4. Physical reclamation (lazy):
   // TimerRegistration block remains in heap until its deadline
   // is reached, then erased by pump. Same as E11.
```

### 6.2 Timer arm in group claim

```
If a timer arm wins:
    -> timer pump fires Select-aware expiry -> arm CandidateReady
    -> group claim CAS: arm wins
    -> timer registration consumed (try_claim_expiry)
    -> winner WaitNode resolved Expired
    -> loser retirements: non-publishing retire of other arms

If an Event arm wins (and timer arm is loser):
    -> Event arm claims group
    -> timer arm retired via non-publishing retirement seam
    -> timer registration retired (Active -> Retired)
    -> timer pump later erases the block lazily
```

---

## 7. Thread Safety Summary

| State transition         | Thread(s)           | Synchronization     |
|--------------------------|---------------------|---------------------|
| DETACHED -> REGISTERING  | calling Fiber       | none                |
| REGISTERING -> REGISTERED| calling Fiber       | global_mtx_ + queue.mtx_ |
| REGISTERED -> CANDIDATE_READY | any (waker/pump) | global_mtx_    |
| CANDIDATE_READY -> WINNER| any (under global_mtx_) | global_mtx_ CAS |
| CANDIDATE_READY -> LOSER | any (under global_mtx_) | global_mtx_ CAS |
| WINNER -> RETIRED        | same CS as claim    | global_mtx_         |
| LOSER -> RETIRED         | same CS as claim    | global_mtx_         |
| Group: Constructing -> Armed | calling Fiber   | global_mtx_         |
| Group: Armed -> WinnerClaimed | any (under global_mtx_) | global_mtx_ CAS |

---

## 8. Invariant Checklist

```
I1.  At most one Winner per group lifetime
I2.  Winner claim precedes arm commit (same CS)
I3.  Loser never publishes caller
I4.  Exactly one make_runnable per SelectOperation (0 for inline, 1 for suspended)
I5.  All arms Retired before SelectOperation destruction
I6.  WaitNode is terminal before its stack frame is destroyed
I7.  TimerRegistration is Retired or Consumed before node destruction
I8.  No arm is CandidateReady after group Completed
I9.  registration_count == arm_count when group enters Arming
I10. publication_count <= 1 at all times
I11. CandidateReady => WaitNode is Registered (not Woken/Expired)
I12. Event persistent SET state is not consumed by Select
I13. Registration failure leaves no armed arm
I14. user_kind matches dynamic context (Select iff arm_kind valid)
I15. user_kind and user_pointer installed before WaitNode registration
I16. Timer arm WaitNode registered in timer_waiters_, not in any primitive
I17. No external resolver observes group during R1 registration
I18. Event broadcast Phase 2 never holds two queue mutexes simultaneously
I19. Inline completion: make_runnable=0, route_runnable=0
I20. Suspended completion: make_runnable=1, route_runnable=1
I21. Event winner: wake_node_locked resolves Woken + unlinks in one queue CS
I22. Timer winner: expire_locked resolves Expired + unlinks in one queue CS
I23. Loser: cancel_locked resolves Cancelled BEFORE unlink
I24. No context clear while Registered (must be terminal + detached)
I25. Timer invariants phase-aware: Active allowed before completion
I26. Completed operation has no Registered WaitNodes
I27. Winner typed context cleared only after terminal + detach
I28. Consumed TimerRegistration never dereferences WaitNode
```
