# E13 Select Locking and Publication

**Task:** `E13-SELECT-PRODUCTION-PREPARATION-1`
**Authority:** fixes the central claim authority (section K), the locking plan
(section N), the winner/loser source order (section O), and the publication
protocol (section P). No production code is changed.

---

## 1. Central Claim authority — SELECTED: C1+C2 hybrid

### 1.1 The two candidates

The brief offers two winner authorities:

**Candidate C1 — under Scheduler global lock**
```text
winner_index:  plain field protected by global_mtx_
```
All offer and claim paths acquire `global_mtx_` first.

**Candidate C2 — atomic CAS**
```text
winner_index.compare_exchange(NoWinner, arm_index)
```
Even with the global lock held, the CAS is the explicit linearization
authority.

### 1.2 The selected hybrid

This design uses **both**: the CAS is the linearization authority; the global
lock serializes the paths so the CAS is uncontended in the common case but
remains the explicit linearization point.

```text
SelectGroup::winner_ : std::atomic<uint32_t>
    initial value: kNoWinner (sentinel)

linearization point:
    winner_.compare_exchange_strong(kNoWinner, arm_index,
                                    std::memory_order::acq_rel,
                                    std::memory_order::acquire)
```

Every offer/claim path runs under `global_mtx_`. The CAS still happens, for
four reasons the brief requires weighed:

| Concern                                              | Hybrid answer                                       |
|------------------------------------------------------|-----------------------------------------------------|
| Is there an offer path that does NOT hold `global_mtx_`? | **No.** Event scan, timer pump, admission all run under `global_mtx_`. |
| Order of timer `try_claim_expiry` vs group claim      | Group claim **first** (winner CAS), then `ACTIVE→CONSUMED`. The group claim is the linearization; the consume is a consequence. |
| External `Event::set` thread                          | Takes `global_mtx_` like every other path; no lock-free bypass. |
| Future global-lock reduction                          | The CAS is already the linearization authority, so a future design that drops `global_mtx_` from some path does not change the linearization semantics — it only widens the CAS contention domain. |
| Memory order                                          | `acq_rel` on success (publishes winner + arm finalization), `acquire` on failure (loser observes the winner's published state). Matches `WaitNode::resolve_` (`wait_node.hpp:237-247`) and `TimerRegistration::try_claim_expiry`. |
| Acquire/release needed by losers to observe winner    | Provided by the CAS's `acq_rel`/`acquire`. A losing claim reads the winner index via `winner_.load(acquire)`. |
| Mapping to formal linearization point                | The CAS *is* `ContractLinearizeWinner(i)`'s linearization point — one source line, one atomic op. |

### 1.3 Why not C1 alone (plain field)

A plain field would make the linearization point implicit (the order of field
writes under the lock). The brief requires a fixed linearization point that
maps cleanly to the formal model, and requires that two competing winner
authorities do not coexist. The CAS is an unambiguous single source line; a
plain field under a lock is "whoever the scheduler ran first", which is harder
to test deterministically and harder to map to `ContractLinearizeWinner`.

### 1.4 Why not C2 alone (lock-free CAS without `global_mtx_`)

The existing primitives all serialize under `global_mtx_` for good reasons
(the Event serialization argument, the MW classification, the park-domain
discipline). Dropping `global_mtx_` from Select's offer paths would:

- race with Event `set()`/`reset()` admission (reopening
  `OLD_SET_WAKES_POST_RESET_WAITER`);
- require a separate publication lock anyway (publication still needs
  `make_runnable` + `route_runnable_locked`, which run under `global_mtx_`);
- complicate the timer pump's `global_mtx_` discipline.

The first scope does not need a lock-free Select. The hybrid keeps the CAS as
the linearization while letting `global_mtx_` do the serialization work.

### 1.5 Fixed central claim facts

```text
winner linearization source line:
    SelectGroup::winner_.compare_exchange_strong(kNoWinner, arm_index, acq_rel, acquire)

winner memory ordering:
    success: acq_rel    (publishes the winner + arm finalization)
    failure: acquire    (loser observes the winner's published state)

who may attempt claim:
    the Event scan (Phase 2, per affected group)
    the timer pump (select_timer_pump_entry, for an ACTIVE Select arm)
    the admission scan (inline, under the registration CS)

who processes losers:
    select_process_group_locked — the single function that, after a successful
    claim, iterates the group's arms and finalizes winner + losers

who publishes result:
    Scheduler::select_publish_locked(group) — the single publication function
```

There is exactly one linearization point, one loser processor, one publication
function. No competing winner authority exists.

---

## 2. Locking plan

### 2.1 The lock matrix

| Lock                              | Protects                                          | Used by Select                          |
|-----------------------------------|---------------------------------------------------|-----------------------------------------|
| `Scheduler::global_mtx_` (G)      | All Scheduler coordination state, the deadline heap, SelectGroup phase/winner, the Event Select registries | Every Select path                       |
| `Event`'s `waiters_.mtx_`         | Ordinary Event wait queue (sealed, unchanged)     | NOT used by Select                      |
| `Event`'s `select_port_`          | The Select registry intrusive list                | Protected by G (no separate mutex)      |
| `SelectGroup` mutex               | —                                                 | **None.** No group mutex exists.        |
| Timer heap domain                 | The deadline heap + pool                          | G (the existing domain)                 |
| `WaitQueue::mtx_`                 | Ordinary wait queues (sealed)                     | NOT used by Select                      |

### 2.2 Total order

```text
G  (global_mtx_)
    └── one primitive registry at a time
            └── (no further locks; SelectGroup has no mutex)
```

In detail:

```text
1. global_mtx_                 — always first
2. one Event's select_port_    — under G; not a separate mutex
3. (optional) the deadline heap — under G; the pump is already under G
```

The Event's ordinary `waiters_.mtx_` is acquired by the existing
`wake_wait_one_locked` during the ordinary drain (already under G, unchanged);
Select does not touch it.

### 2.3 Why no group mutex

A `SelectGroup` is reached by:

- the admission scan (in the registration CS, under G);
- the Event scan Phase 2 (under G);
- the timer pump (under G);
- the publication function (under G).

Every reach is under `global_mtx_`. A group mutex would add a lock-order edge
(`G → group_mtx_`) and a second acquisition site per path, with zero benefit:
no path accesses the group without already holding G. The brief permits
"prove no group mutex needed" — this is that proof.

### 2.4 Forbidden orders (from the brief, all satisfied)

| Forbidden                                                  | Status                                            |
|------------------------------------------------------------|---------------------------------------------------|
| `Event registry → global`                                  | The registry is under G; the edge does not exist. |
| `SelectGroup → Event registry while Event set path uses reverse` | Both are under G in a single CS; no reverse edge. |
| Two Event registry locks simultaneously                    | The scan visits one Event at a time; SelectPort has no separate mutex. |
| Two SelectGroup locks simultaneously                       | There is no group mutex.                          |
| Callback under a lock that may recursively reenter the same registry | Finalize does not re-enter any Event or registry. |

### 2.5 Per-action lock precondition

Every production action states its lock precondition. The mapping table in
`docs/e13-select-formal-production-mapping.md` carries a `Lock domain` column
for every formal action. Summary:

| Action class                | Lock precondition                       |
|-----------------------------|-----------------------------------------|
| Case validation             | no lock (before registration)           |
| Registration (link arm)     | `global_mtx_` held                      |
| Admission scan + claim      | `global_mtx_` held                      |
| Event scan Phase 1 + Phase 2| `global_mtx_` held (one continuous CS)  |
| Timer pump Select branch    | `global_mtx_` held                      |
| Finalize (winner + losers)  | `global_mtx_` held                      |
| Publication                 | `global_mtx_` held                      |
| Rollback                    | `global_mtx_` held                      |

---

## 3. Admission algorithm (task section L)

### 3.1 Pseudocode

```text
select_impl(scheduler, case_array):

    // -- outside any lock --
    1. validate case list and Scheduler identities
         (cross-Scheduler -> throw std::invalid_argument BEFORE any registration)
    2. allocate SelectTimerRegistration for each Timer arm (outside the lock)
         (std::bad_alloc here propagates directly; nothing to roll back)
    3. construct the SelectGroup + SelectArmRegistration array in this frame

    // -- under global_mtx_ --
    g.lock()
    4. group.phase_ = Building
    5. for each arm in index order:
         a. register the arm:
              Event arm: link into Event.select_port_  (SelectPort::link_locked)
              Timer arm: push its SelectTimerRegistration into deadline_heap_
         b. preserve rollback ability:
              - arm is linked but group.phase_ == Building, winner_ == kNoWinner
              - no caller suspension yet
              - no result publication yet
         c. on failure (none expected in first scope; defensive):
              -> rollback (section 4) and rethrow
    6. FinishRegistration: group.phase_ = Selecting

    7. admission snapshot: walk arms in index order, collect those whose
         readiness is already observable:
              Event arm: event.set_.load(acquire) == true
              Timer arm: deadline <= scheduler.monotonic_now()
         (these arms get arm.state = CandidateReady)
    8. if snapshot non-empty:
         a. choose lowest index
         b. inline claim: group.winner_.CAS(kNoWinner, lowest_index)
         c. commit winner: arm.state = Retired (winner classification)
         d. finalize losers: every other arm finalized as a loser
         e. close all authority (unlink Event arms, retire/consume Timer arms)
         f. select_publish_locked(group, mode=Inline)
         g. group.phase_ = Completed
         h. g.unlock()
         i. return SelectResult{index, kind, ...}  (no suspension)
    9. otherwise:
         a. me->make_waiting()
         b. group.phase_ = Armed
         c. g.unlock()
         d. fiber_ctx::context_switch(...)   // suspend
         // --- on resume ---
         e. g.lock()    // reacquire to read the result
         f. read SelectResult from group (winner is committed)
         g. verify all arm authority closed (defensive assert)
         h. g.unlock()
         i. return SelectResult
```

### 3.2 Rollback (registration failure)

On any failure between step 5 and step 6 (only `std::bad_alloc` from Timer
block allocation in the first scope, which is allocated *before* the lock — so
this path is defensive for future failures):

```text
under global_mtx_ (still held):
    for each arm already registered:
        Event arm: SelectPort::unlink_locked(arm); arm.state = Retired
        Timer arm: reg.state_.store(Retired); arm.state = Retired
    group.phase_ = Aborted
release global_mtx_
throw the original exception
```

The caller frame unwinds and destroys the group + arms. No runnable
publication, no result. This refines
`ContractBeginRollback`/`ContractRollbackRelease`/`ContractFinishRollback`.

### 3.3 No half-registered state observable

Steps 5–6 run under one continuous `global_mtx_` CS. An external Event setter
or the timer pump cannot acquire `global_mtx_` until step 6 completes; by then
the group is either `Building` (still inside the CS) or `Selecting`. There is
no window in which an external resolver sees a partially-registered group.

---

## 4. Winner/loser source order (task section O)

### 4.1 Timer winner

```text
group winner claim (SelectGroup::winner_ CAS: kNoWinner -> arm_index)
    <
SelectTimerRegistration: ACTIVE -> CONSUMED (try_claim_expiry CAS)
    <
arm terminal classification (arm.state = Retired; winner branch)
    <
arm unlink/detach (no-op for Timer arm: no Event registry membership)
    <
accounting close (--active_deadline_count_; group-internal pending counter)
    <
adapter authority close (the CONSUMED CAS above closed it)
    <
result/runnable publication (select_publish_locked)
```

### 4.2 Timer loser

```text
arm loser classification (arm.state = Retired; loser branch)
    <
arm cancel/terminal state (plain field write under global_mtx_)
    <
arm unlink/detach (no-op for Timer arm)
    <
SelectTimerRegistration: ACTIVE -> RETIRED (retire CAS)
    <
accounting close (--active_deadline_count_)
    <
adapter authority close (the RETIRED CAS above closed it)
```

No publication. The loser path never calls `make_runnable` /
`route_runnable_locked`.

### 4.3 Event winner

```text
group winner claim (SelectGroup::winner_ CAS: kNoWinner -> arm_index)
    <
Event arm resolve (targeted: EventSelectRegistration marked terminal-winner)
    <
arm unlink from Event.select_port_ (SelectPort::unlink_locked, under global_mtx_)
    <
arm terminal classification (arm.state = Retired; winner branch)
    <
accounting close (group-internal pending counter)
    <
adapter authority close (unlink closed the registry membership)
    <
result/runnable publication (select_publish_locked)
    <
(Event set_ flag is NOT cleared; SET remains persistent)
```

### 4.4 Event loser

```text
arm loser classification (arm.state = Retired; loser branch)
    <
arm unlink from Event.select_port_ (SelectPort::unlink_locked)
    <
arm terminal state
    <
accounting close
    <
adapter authority close (unlink closed the registry membership)
    <
(Event set_ flag is NOT cleared; SET remains persistent)
```

No publication. The Event's `set_` flag is never cleared by Select; the
persistent-readiness property is preserved.

### 4.5 The persistent-Event law

Across all four branches, Select never mutates `Event::set_`. An Event arm
that loses to a Timer does not "consume" the SET; a later ordinary `Event::wait`
still observes SET and returns Woken. This preserves the formal
`InvEventPersistentStateNotConsumed`.

---

## 5. Publication protocol (task section P)

### 5.1 The single publication function

```text
Scheduler::select_publish_locked(SelectGroup& group)

    PRE: global_mtx_ held
    PRE: group.winner_ != kNoWinner              (winner exists)
    PRE: the winner arm is committed              (arm.state == Retired, winner)
    PRE: every loser arm is finalized             (arm.state == Retired, loser)
    PRE: every arm's authority is closed:
              Event arms unlinked from their SelectPort
              Timer arms' SelectTimerRegistration in RETIRED or CONSUMED

    BODY:
        if group.caller_state_ == Running:
            // inline completion
            //   NO make_runnable
            //   NO route_runnable_locked
            group.completion_mode_ = Inline
        else:  // group.caller_state_ == Waiting
            // suspended completion
            Fiber* f = group.caller_
            if f->make_runnable():                 // exactly-once guard
                route_runnable_locked(f, group.caller_owner_)
            group.completion_mode_ = Suspended

        group.result_ = SelectResult{ winner index, kind, ... }   // written once
        group.phase_ = Completed

    POST:
        result_publication_count == 1
        runnable_publication_count == (Inline ? 0 : 1)
        arm_publication_count[winner] == 1
        arm_publication_count[loser] == 0  for every loser
```

### 5.2 Why a single function

The brief forbids each arm calling `make_runnable` / `route_runnable_locked`
on its own. The ordinary resolver does exactly that
(`wake_wait_one_locked`, `scheduler.cpp:1104-1105`); Select cannot, because
Select needs the claim to happen *before* publication. Centralizing
publication in one function:

- makes the exactly-once runnable publication mechanically obvious
  (one call site, guarded by `make_runnable`'s own return value);
- enforces the precondition ordering (winner + losers finalized + authority
  closed BEFORE publication);
- gives the formal `ContractPublishInline` / `ContractPublishSuspended`
  actions a single production counterpart.

### 5.3 Finding the caller Fiber and owner worker

`SelectGroup` records the caller Fiber and its owner worker at admission
(mirrors `WaitReg::fiber` + `WaitReg::owner` in the existing
`waiting_size_`/`waiting_void_`/`waiting_ready_` maps). For Select, these are
stored directly on the group:

```text
group.caller_         : Fiber*          // = g_worker->current at admission
group.caller_owner_   : WorkerState*    // = g_worker at admission
```

An external-thread Event `set()` finds the group via the Event scan; the
caller Fiber and owner are read from the group, not from `g_worker` (which is
null on an external thread). `route_runnable_locked` handles the
`owner == nullptr` case via `pending_spawn_` routing, exactly as for ordinary
external-thread wakes (`scheduler.cpp:910+`).

### 5.4 Inline vs suspended: the publication branch

The branch is on `group.caller_state_`, which is `Running` if the admission
scan found a ready arm (no suspension) and `Waiting` if the caller suspended.
There is no third state. The inline branch publishes no runnable; the
suspended branch publishes exactly one.

This maps cleanly to the formal `completion_mode ∈ {None, Inline, Suspended}`:

- inline admission winner → `Inline`
- post-suspension winner → `Suspended`
- (no other completion path exists in the first scope)

---

## 6. Multi-group shared Event

Two SelectGroups sharing one Event are handled by Phase 2's per-group
iteration. The Event scan collects *all* affected groups (deduplicated by
`SelectGroup*`); Phase 2 calls `select_process_group_locked` once per group.
Each call runs its own claim + finalize + publish under the same `global_mtx_`
CS. The two groups complete independently; one's winner/loser classification
does not affect the other.

A single group with two arms on the same Event is handled by Phase 1
deduplication: the group is collected once; Phase 2 picks the lowest-index
ready arm, makes it the winner, and finalizes the other arm as a loser.

---

## 7. The forbidden publication routes

The brief forbids:

```text
each arm calling make_runnable
each arm calling route_runnable_locked
```

This design has exactly **one** call to each, both inside
`select_publish_locked`, both guarded by `make_runnable`'s return value. No
arm-finalize code path, no Event scan path, no timer pump path calls either
directly.

The ordinary resolver (`wake_wait_one_locked`) is **not** used for Select
arms. Select arms are never linked into `waiters_`; they are linked into
`select_port_`, which has its own scan + finalize, not a `wake_one_locked`
resolver.

---

## 8. Summary of the locking+publication decisions

```text
central claim:     SelectGroup::winner_ CAS, under global_mtx_, acq_rel
lock order:        G -> (one registry, no mutex) -> (no group mutex)
group mutex:       none
publication:       single Scheduler::select_publish_locked(SelectGroup&)
inline:            no runnable publication
suspended:         make_runnable + route_runnable_locked, exactly once, guarded
caller routing:    group.caller_ + group.caller_owner_, external-thread safe
Event SET:         never cleared by Select
multi-group:       Phase 2 iterates deduplicated groups; each completes once
```
