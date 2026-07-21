# E13 Select Timer Adapter

**Task:** `E13-SELECT-PRODUCTION-PREPARATION-1`
**Authority:** fixes the Timer adapter architecture (task section I). No
production code is changed.

---

## 1. The existing `TimerRegistration` is sealed to ordinary waits

`TimerRegistration` (`include/sluice/async/timer_registration.hpp`) is the
Scheduler-owned stable block for ordinary deadline waits. Its file banner and
field docstrings establish:

- the atomic `state` (`active/retired/consumed`) is the **independently-stable
  retirement authority**;
- the bound `{node, queue}` are the live-wait binding, read only while
  `active` is observed;
- the `OnResolveFn on_resolve_` + `void* owner_ctx_` pair is an **E12-E Queue
  per-port bookkeeping hook** (`timer_registration.hpp:71-87`), invoked by the
  Scheduler exactly once per `active → terminal` transition.

The brief explicitly forbids silently promoting that Queue hook to a Select
winner callback:

> 不能把 `owner_ctx_ + on_resolve_` 直接从 "Queue bookkeeping hook" 静默升级
> 为通用 Select winner callback。

A Select Timer arm therefore cannot bind to an ordinary `TimerRegistration`
through its `{node, queue}` target. The Select Timer arm has no `WaitNode`
(see `docs/e13-select-type-and-lifetime.md` §1 — WaitNode is not reused), so
there is no `node` to bind, and the arm does not park in any ordinary
`WaitQueue`, so there is no `queue`.

---

## 2. Three options

### 2.1 Option T1 — dedicated `SelectTimerRegistration`

A new stable control block dedicated to Select Timer arms. Its shape mirrors
`TimerRegistration`'s retirement discipline but binds to Select objects:

```text
states (atomic):
    ACTIVE   — registered; the pump may claim it
    RETIRED  — Select loser (or non-timer winner) closed callback authority
    CONSUMED — Select timer winner; callback authority closed

fields:
    std::atomic<State> state_
    SelectArmSlot* arm_       // the caller-frame Timer arm slot
    Scheduler* scheduler_             // owning scheduler (for publication)
    select_deadline_t deadline_
    std::size_t heap_index            // heap position back-reference
```

The pump's expiry protocol is:

```text
1. select_timer_pump_entry(reg):
       PRE: global_mtx_ held
       if reg.state_.load(acquire) != ACTIVE: return      // skip, no deref
       arm = reg.arm_                                     // safe: ACTIVE implies arm live
       ...                                                // proceed to CandidateReady + claim
```

A winner arm: `ACTIVE → CONSUMED` (CAS) **after** group claim succeeds.
A loser arm: `ACTIVE → RETIRED` (CAS) during finalize.

### 2.2 Option T2 — tagged generic `TimerRegistration` target

Make `TimerRegistration`'s target an internal tagged union:

```text
ordinary WaitNode target
Queue target
Select arm target
```

unifying the deadline heap and retirement protocol across all three.

### 2.3 Option T3 — general opaque resolution thunk

Generalize `owner_ctx_ + on_resolve_` into a stable callback target so a
Select arm installs a Select-resolution thunk.

---

## 3. Comparison and verdict

### 3.1 Why T1 is selected

T1 introduces a **parallel** stable block, leaving `TimerRegistration`
untouched. The deadline heap gains a second inhabitant kind, but each block
type owns its own binding. This:

- **Preserves the `TimerRegistration` contract verbatim.** Its banner, its
  `{node, queue}` semantics, its Queue hook all remain exactly as documented.
  No existing reader of `TimerRegistration` needs to learn a new tag.
- **Mirrors the E11 lifetime-closure proof (I4) directly.** `SelectTimerRegistration::state_`
  plays exactly the role `TimerRegistration::state_` plays: the independently-
  stable retirement identity that lets a stale pump entry skip without
  dereferencing the (possibly-destroyed) caller-frame arm. The I4 argument
  transfers mechanically.
- **Avoids the brief's T2/T3 failure modes** (see below).

### 3.2 Why T2 is rejected

T2 forces every `TimerRegistration` site (the pump, retire paths, every
ordinary `await_wait_deadline`, every Queue admit) to dispatch on the tag.
The brief explicitly lists the failure modes T2 must prevent:

```text
巨大 switch 扩散
错误 target tag
Queue callback 语义污染 Select
retired registration dereference
```

T2 does not mechanically prevent any of them; it merely asks each site to be
careful. The Queue hook semantics (`timer_registration.hpp:159-167`) are
already load-bearing for Queue counter bookkeeping; mixing them with Select
winner/loser semantics through the same target tag risks the exact
"Queue callback semantics contaminate Select" failure the brief names. T2 is
rejected.

### 3.3 Why T3 is rejected

T3 is the route the brief most directly forbids. The `owner_ctx_ + on_resolve_`
pair is documented as **E12-E Queue per-port bookkeeping only**
(`timer_registration.hpp:71-87`). Promoting it to a general Select winner
callback:

- **silently upgrades a narrow documented hook**, violating the
  sealed-authority discipline;
- **creates a forgeable callback authority** — any TU that can name the thunk
  type can install one;
- **risks callback lifetime bugs** the existing Queue hook does not have,
  because the Queue `owner_ctx_` is a `QueuePort*` that outlives every wait,
  while a Select thunk would need to reach a caller-frame `SelectGroup`.

T3 is rejected.

### 3.4 Verdict

```text
SELECTED: Option T1 — dedicated SelectTimerRegistration stable block
```

This adds a second stable-block type to the timer subsystem. The heap and the
pump learn to handle both kinds; ordinary `TimerRegistration` and its
consumers are unchanged.

---

## 4. Heap integration

`SelectTimerRegistration` joins the deadline heap alongside ordinary
`TimerRegistration`. The existing `deadline_heap_` is a
`std::vector<TimerRegistration*>`, which cannot hold a second pointer type
without change. The heap is therefore migrated to a unified entry type:

```cpp
// in detail/, not installed
struct DeadlineHeapEntry {
    deadline_tick_t deadline;   // cached for heap_less

    enum Kind : uint8_t { Ordinary, Select };
    Kind kind;

    union {
        TimerRegistration* ordinary;
        SelectTimerRegistration* sel;
    };
};
```

The deadline heap becomes:

```cpp
std::vector<DeadlineHeapEntry> deadline_heap_;
```

All heap helpers (`heap_less`, `bubble_up`, `bubble_down`, `pop_min`, etc.)
operate on `DeadlineHeapEntry` by deadline, unchanged in logic. The ordinary
`TimerRegistration*` vector is replaced; the ordinary pump branch simply reads
`entry.ordinary` instead of a raw pointer.

### 4.1 Ownership transfer for SelectTimerRegistration

`SelectTimerRegistration` nodes are constructed in a temporary
`std::list<SelectTimerRegistration>` outside G (before any lock acquisition).
Under G, the list is spliced into the Scheduler-owned pool:

```cpp
// outside G:
std::list<SelectTimerRegistration> tmp_pool;
for each Timer arm:
    tmp_pool.emplace_back(arm, deadline, scheduler);

// under G, before any registration mutation:
// reserve heap capacity to guarantee push_back does not allocate
deadline_heap_.reserve(deadline_heap_.size() + timer_arm_count);

// splice is O(1) and allocation-free
scheduler.select_timer_pool_.splice(end, tmp_pool);

// push DeadlineHeapEntry values — no allocation after reserve
for each Timer arm:
    deadline_heap_.push_back(DeadlineHeapEntry{...});
```

If `reserve` throws `std::bad_alloc`:

```text
no arm registered
no list spliced
no authority opened
release G
rethrow
```

Key properties:

- **No allocation under G after reserve.** `std::list::splice` is O(1) and
  allocation-free. `push_back` does not allocate if capacity is sufficient.
- **`reserve` is the only allocation under G.** It happens strictly before
  any Select registration mutation (no arm linked, no timer registered). If
  reserve fails, the Scheduler state is untouched.
- **Pointer stability.** `std::list` nodes are stable after splice; the
  `SelectTimerRegistration*` stored in the heap entry remains valid for the
  lifetime of the registration.
- **Lazy reclamation.** Mirroring `TimerRegistration`, retired/consumed
  `SelectTimerRegistration` blocks are physically removed from the pool when
  the pump pops their deadline entry (lazy-at-deadline). The pool is bounded by
  `(concurrent ACTIVE Select timer arms) + (retired/consumed entries whose
  deadlines have not been reached)`, matching the bound for ordinary timers.

### 4.2 Pump integration

The pump's per-entry branch is exactly two cases:

```text
pump_deadlines_locked():
    PRE: global_mtx_ held
    while heap not empty and heap_min.deadline <= now:
        entry = pop_min()
        if entry.kind == Ordinary:
            ... existing ordinary path (entry.ordinary, UNCHANGED in logic) ...
        else:  // Select
            select_timer_pump_entry(*entry.sel)
            // pool block is reclaimed here (mirrors erase_popped_registration_locked)
```

This adds **one branch** to the pump's hot loop. The ordinary branch is
logically byte-for-byte identical to the current code (it reads the same
deadline, pops the same min, and processes the same `TimerRegistration*`).

The earliest-deadline cache `earliest_active_deadline_` continues to be
maintained by scanning both block kinds; `SelectTimerRegistration::state_ ==
active` participates exactly like `TimerRegistration::state_ == active`.

### 4.3 Key invariant: no concurrent heap ownership

The `std::list::splice` transfer is safe because:

1. The temporary `tmp_pool` is local to the `select(...)` call frame.
2. No other thread can observe the `SelectTimerRegistration` until it is in
   the Scheduler-owned pool (under G).
3. The heap entry referencing the `SelectTimerRegistration` is pushed only
   under G, after the splice.

---

## 5. The Select timer expiry protocol (mirrors `e13-select-preparation.md` §15.2)

```text
select_timer_pump_entry(SelectTimerRegistration& reg):

    PRE: global_mtx_ held
    PRE: the pump has popped reg and observed now >= reg.deadline_

    1. if reg.state_.load(acquire) != ACTIVE:
            return                                  // stale; skip (TimerPumpSkip)
               — does NOT dereference reg.arm_
               — does NOT increment timer_node_deref (formal action TimerPumpSkip)

    2. arm = reg.arm_                              // safe: ACTIVE implies arm live
    3. group = arm->group

    4. if group->phase_ != Armed:
            // INVARIANT: ACTIVE + non-Armed is unreachable in a correct protocol.
            // Arming cannot interleave with the pump (both run under G).
            // Completed groups have retired/consumed all registrations.
            // If this branch fires, either the pump observed a stale entry
            // before the state_ CAS completed, or the registration protocol
            // has a bug. Fail fast rather than silently returning with an
            // ACTIVE registration still in the heap.
            select_fail_fast("ACTIVE SelectTimerRegistration with non-Armed group")

    5. arm->state = CandidateReady                 // offered readiness
    6. select_process_group_locked(group)          // see locking-and-publication doc
       — this may make this arm the winner or a loser
       — if winner: reg.state_ CAS ACTIVE -> CONSUMED
       — if loser:  reg.state_ CAS ACTIVE -> RETIRED
```

The order enforced by `select_process_group_locked` for a Timer winner is:

```text
group winner claim (SelectGroup::winner_ CAS)
    <
SelectTimerRegistration ACTIVE -> CONSUMED
    <
arm terminal classification (arm.state = Retired, classified as winner)
    <
arm unlink/detach (no Event registry for a Timer arm; nothing to unlink here)
    <
accounting close (--active_deadline_count_, --waiting-equivalent counter)
    <
adapter authority close (the CONSUMED CAS above already closed it)
    <
result/runnable publication (select_publish_locked)
```

For a Timer loser:

```text
arm loser classification (arm.state = Retired, classified as loser)
    <
arm terminal state
    <
(no unlink — Timer arm has no Event registry membership)
    <
SelectTimerRegistration ACTIVE -> RETIRED
    <
accounting close
    <
adapter authority close
```

These sequences are the production counterpart of the formal
`E13SelectEventTimer.tla` actions `ClaimTimerWinner`, `ConsumeTimerWinner`,
`FinalizeTimerWinner`, `CancelTimerLoser`, `RetireTimerLoser`,
`CloseAdapterAuthority`, `TimerPumpSkip`. The full action-by-action mapping is
in `docs/e13-select-formal-production-mapping.md`.

---

## 6. Stale-timer safety (I4 closure)

The retirement identity of a `SelectTimerRegistration` is its atomic `state_`.
The arm pointer is read **only after** observing `active`, and `active` is
lost in the same `global_mtx_` CS that finalizes the arm. Therefore:

```text
pump observes state_ != active  ⟹  arm pointer is NOT dereferenced
arm is finalized               ⟺  state_ transitioned out of active under global_mtx_
caller resume                  happens-after  arm finalized
caller frame destroyed         happens-after  caller resume
```

So a pump entry that fires after the caller frame is destroyed necessarily
observes `retired`/`consumed` and skips. This is the I4 closure, transferred
verbatim from `TimerRegistration` (`timer_registration.hpp:23-32`).

### 6.1 Lazy physical reclamation

Mirroring `TimerRegistration`'s lazy-at-deadline reclamation
(`scheduler.hpp:1077-1094`, the `erase_popped_registration_locked` path), a
`SelectTimerRegistration` whose deadline has not yet been reached remains
physically in the heap+pool even after retirement. The pump reclaims the pool
block only when `now >= its deadline`. An inert retired block consumes one
pool slot until its deadline elapses; the pool is bounded by
`(concurrent ACTIVE Select timer arms) + (retired/consumed entries whose
deadlines have not been reached)`, exactly the bound stated for ordinary
timers.

### 6.2 No active timer after completion

For a completed SelectGroup, every Timer arm's `SelectTimerRegistration` is in
`retired` or `consumed` state. This is the formal invariant
`InvNoActiveTimerAfterCompletion` made mechanical: finalize transitions every
Timer registration out of `active` under `global_mtx_` before the group
reaches `Completed`.

---

## 7. Already-due Timer at admission

At admission (inside the registration CS, under `global_mtx_`), after a Timer
arm's `SelectTimerRegistration` is created and pushed into the heap, the
deadline is rechecked against `scheduler.monotonic_now()`. If already due:

```text
arm.state = CandidateReady        // offered readiness, no suspension
// the registration stays ACTIVE for now; the inline winner path will CONSUME it
// if this arm wins the admission claim, or RETIRE it if it loses
```

The group's inline admission scan then picks the lowest-index ready arm. If a
non-Timer arm wins, the Timer arm's registration is retired
(`ACTIVE → RETIRED`) in the same CS. If the Timer arm wins, its registration
is consumed (`ACTIVE → CONSUMED`).

This matches the formal `AdmissionObserveReady` / `ClaimAdmissionWinner` /
`ConsumeTimerWinner` / `RetireTimerLoser` actions.

---

## 8. No `WaitNode`, no `WaitQueue`

A Timer arm has no `WaitNode` (WaitNode is not reused,
`docs/e13-select-type-and-lifetime.md` §1) and parks in no `WaitQueue`. The
`SelectTimerRegistration` is the sole registration object; the caller-frame
`TimerArmPayload` arm is reached only via the stable block's `arm_`
pointer while `active`. There is no queue to unlink from; the only cleanup is
the `state_` transition.

This avoids the `timer_waiters_` private queue the
`e13-select-preparation.md` design proposed. That queue existed only because
the design reused `WaitNode`, which needed a queue. With no `WaitNode`, no
queue is needed.

---

## 9. What this does NOT change

- `TimerRegistration` is unchanged. Its banner, its `{node, queue}` binding,
  its Queue hook all continue to serve ordinary + Queue deadline waits.
- `await_wait_deadline`, `sem_acquire_until`, `mutex_lock_until`,
  `condition_wait_prepare_until`, `queue_push/pop_admit_until` are unchanged.
- The ordinary pump branch (the `entry.kind == Ordinary` case) is the existing
  code, byte-for-byte.
- The earliest-deadline park cache (`earliest_active_deadline_`) continues to
  bound worker park time; Select Timer deadlines participate in it like
  ordinary deadlines.

---

## 10. Test seams for the Timer adapter

Deterministic PhaseTag seams (compiled only into
`sluice_async_internal_testing`):

```text
E13PhaseTimerPumpActive     // pause after ACTIVE check, before CandidateReady
E13PhaseTimerConsumed       // pause after ACTIVE->CONSUMED, before resolve
E13PhaseTimerRetired        // pause after ACTIVE->RETIRED on a loser
E13PhaseTimerPumpSkip       // pause to observe a stale (non-active) entry skip
```

`E13PhaseTimerPumpSkip` is the deterministic proof of the I4 closure: it lets
a test force the pump to observe a `retired`/`consumed` entry and assert that
`arm_` is never read. Full plan:
`docs/e13-select-production-test-plan.md` §seams and §negative.
