# E13 Select Event Adapter

**Task:** `E13-SELECT-PRODUCTION-PREPARATION-1`
**Authority:** fixes the Event adapter architecture (task section G) and the
two-phase Event broadcast (section H). No production code is changed.

---

## 1. The sealed authority of `Event::waiters_`

`Event` (see `include/sluice/async/event.hpp:35-47` banner) owns a private
`WaitQueue waiters_` whose membership authority is **sealed**:

- there is **no** `wait_queue()` accessor;
- there is **no** test friend that grants reach;
- the only RESOURCE_WAKE authorities are `Event::set()` and admission observing
  SET;
- the required bypass `scheduler.wake_wait_one(event.wait_queue())` **does not
  compile**.

Select must not break any of these. Specifically Select must not add:

```cpp
WaitQueue& Event::wait_queue();   // FORBIDDEN
```

nor reach `waiters_` via friend, nor extend the ordinary wake path to recognise
Select nodes. The ordinary wait queue continues to serve ordinary `Event::wait`
users, unchanged.

---

## 2. Three options

### 2.1 Option E1 — separate private Select registry

Add **one new** private intrusive list per Event: the Select registry. The
ordinary `waiters_` is untouched.

```text
Event
  ├── waiters_                  (ordinary WaitQueue, sealed, unchanged)
  └── select_port_              (SelectPort: intrusive list of Select arms)
```

`Event::set()` calls `Scheduler::event_set_broadcast`, which under
`Scheduler::global_mtx_`:

```text
1. set the persistent set_ flag
2. resolve ordinary Event waits via the existing drain
   (wake_wait_one_locked over waiters_)
3. scan select_port_ for Select arms
4. collect/deduplicate affected SelectGroups
5. release the Event registry iteration authority
6. process each unique SelectGroup (Central Claim)
```

### 2.2 Option E2 — tagged generic registration queue

Extend `WaitQueue`'s internal node so ordinary waits and Select arms share a
structure, distinguished by a resolver-kind tag. The claim is that one
underlying intrusive list serves both.

### 2.3 Option E3 — callback-bearing WaitNode extension

Extend `WaitNode` (or add an opaque hook) so a Select arm can install a
callback that the ordinary wake path invokes instead of resolving Woken.

---

## 3. Comparison and verdict

### 3.1 Why E1 is selected

E1 keeps the Select registry **physically separate** from `waiters_`. The
ordinary Event semantics — `event.hpp:26-33`'s serialization argument that
makes `OLD_SET_WAKES_POST_RESET_WAITER` "mechanically impossible" by serialising
set/reset/admission under `global_mtx_` — continues to hold exactly, because
the Select scan runs in the same serialized critical section. Nothing about
the ordinary queue changes.

E1 also matches the established sealed-authority pattern of every E12
primitive: a private registry, reachable only by Scheduler-friended seams, no
public accessor, no test friend.

### 3.2 Why E2 is rejected

E2 requires extending `WaitQueue` itself. `WaitQueue` is the canonical E10
primitive whose entire file banner (`wait_queue.hpp:1-107`) is dedicated to
its narrow responsibility and its sealed authority. Mixing Select nodes into
the same intrusive list:

- **Pollutes the ordinary FIFO discipline.** Ordinary `wake_one_locked`
  (`wait_queue.hpp:199-211`) assumes every node is resolvable via the
  `resolve_` CAS. Select arms are *not* resolvable via `resolve_`; they need
  CandidateReady + group claim. The resolver would have to branch.
- **Re-introduces the forgeable discriminator problem.** Distinguishing
  ordinary from Select needs a tag, which is the route the
  `ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1` banner explicitly removed.
- **Breaks the sealed-authority invariant.** E10-CORRECTIVE-2 R1+R2 sealed
  WaitQueue's structural operations to Scheduler-only friendship; opening the
  node type to Select re-opens that surface.

E2 is rejected.

### 3.3 Why E3 is rejected

E3 (callback-bearing WaitNode) is the route `e13-select-preparation.md` §4
proposed via `WaitNodeUserKind`. The brief (section D) explicitly forbids it:

```text
using WaitNode::user as an undocumented global callback channel
```

Concretely E3 fails on every axis the brief requires reviewed:

- **Pollutes E10's narrow responsibility** — `WaitNode::user_` is documented
  (`wait_node.hpp:141-152`) as E12-E Queue per-op context only.
- **Forms a forgeable callback authority** — any TU that can name the callback
  type can `static_cast<…*>(node.user_pointer())`.
- **Forces the ordinary primitive to branch** — `wake_wait_one_locked` would
  need a kind check on every head.
- **Increases post-destruction raw pointer risk** — Select metadata becomes
  reachable through the same `user_` pointer a Queue context uses, doubling
  the cast-then-dereference surface.

E3 is rejected. (See also `docs/e13-select-type-and-lifetime.md` §1 for the
parallel WaitNode-reuse decision.)

### 3.4 Verdict

```text
SELECTED: Option E1 — separate sealed Select registry per Event
```

This is also the brief's stated default preference. It is selected on source
and lock-order evidence, not preference alone:

- the Event serialization domain (`global_mtx_`) already makes a per-Event
  scan atomic with set/reset/admission;
- the ordinary `wake_wait_one_locked` path is left untouched, satisfying the
  D-law "no ordinary wake path becomes a Select arm resolver";
- the Select registry is a brand-new structure with no ordinary consumer, so
  no existing primitive's branch count grows.

---

## 4. Two-phase Event broadcast (task section H)

### 4.1 The phase boundary

```text
Phase 1 — under Event registry authority:
    scan all Select arms linked in select_port_
    mark offered readiness (arm.state = CandidateReady)
    deduplicate SelectGroup identities (one group may have several arms here)
    DO NOT claim
    DO NOT commit
    DO NOT publish

authority boundary:
    release Event-specific registry iteration authority

Phase 2 — group processing:
    for each unique SelectGroup:
        attempt Central Claim (SelectGroup::winner_ CAS)
        finalize winner + losers
        publish the group at most once
```

### 4.2 Lock order for the two phases

The brief requires precise answers to:

> `Scheduler::global_mtx_` 在 Phase 1 与 Phase 2 之间是否继续持有；
> Event registry 是否有独立 mutex；锁顺序是什么；
> 是否允许 Phase 2 递归进入另一个 Event；
> set() 外部线程如何处理 caller Fiber routing；
> 同一 SelectGroup 具有两个相同 Event arms 时如何去重 group 处理；
> 两个 SelectGroups 共享同一 Event 时如何分别完成。

Answers (selected):

| Question                                           | Answer                                                  |
|----------------------------------------------------|---------------------------------------------------------|
| Is `global_mtx_` held across Phase 1 → Phase 2?    | **Yes.** The entire broadcast is one `global_mtx_` CS. |
| Does the Event registry have its own mutex?        | **No.** `select_port_` is protected by `global_mtx_`. There is no separate registry mutex. |
| Lock order                                         | `global_mtx_` → (the Event's `waiters_.mtx()` for the ordinary drain, already under G) → the Select registry is under G directly (no extra mutex) |
| May Phase 2 recursively re-enter another Event?    | **No.** Finalization does not call `set()` or any Event mutator. The winner resolve is on the **already-scanned** Select arm via a *targeted* `wake_arm_locked`, not a queue scan. |
| How does an external-thread `set()` route the caller Fiber? | Same path as today: `route_runnable_locked(f, g_worker)` with `g_worker == nullptr` on an external thread, falling back to `pending_spawn_` routing (mirrors `event_set_broadcast`'s existing external-thread handling). |
| One group with two arms on the same Event: dedup?  | The Phase 1 scan collects the group identity once (dedup by `SelectGroup*`); Phase 2 processes it once, picking the lowest-index ready arm. |
| Two groups sharing one Event: separate completion? | Yes. Phase 2 iterates the deduplicated set; each group runs its own claim + finalize + publish, independently. |

### 4.3 Why no separate registry mutex

A separate per-Event registry mutex would introduce a new lock order edge
(`registry_mtx_ → ...`). Because the scan and the group processing both run
under `global_mtx_` (the existing coordination domain), adding a registry
mutex buys nothing and creates a new deadlock surface. The brief's prohibition
on "two Event registry locks simultaneously" and "callback under a lock that
may recursively reenter the same registry" is satisfied trivially: there is
only `global_mtx_`.

### 4.4 Why no recursive Event re-entry in Phase 2

The Phase 2 winner finalization resolves the winning Select arm via a
**targeted** resolve on that specific `SelectArmSlot`, not via a new
scan of any `waiters_` or `select_port_`. It does not call `set()`/`reset()`
on any Event. Therefore no Event method is re-entered while a broadcast is in
flight, and the prohibition on recursive re-entry is satisfied.

### 4.5 The forbidden pattern

The brief forbids:

> executing group finalization that may modify the same intrusive list while
> holding Event intrusive-list authority

The two-phase split exists to enforce exactly this. Phase 1 holds the scan
authority and produces a deduplicated group list; Phase 2 finalizes groups
**after** the scan snapshot is taken. Finalize may unlink arms from the
registry (closing their authority), but the scan has already built the
deduplicated group set, so finalize-driven list mutation cannot affect the
Phase 2 iteration.

### 4.6 Intrusive affected-group worklist (no caller-local array)

A fixed-size caller-local array cannot hold an unbounded number of
`SelectGroup*` entries (one Event may be shared by any number of
SelectGroups). Instead, Phase 1 builds an **intrusive worklist chain** using
temporary fields on `SelectGroup` that are live only under `global_mtx_`:

```cpp
struct SelectGroup {
    // ... permanent fields ...

    // temporary fields, used only inside the event_set_broadcast CS
    // (protected by global_mtx_; no concurrent reuse)
    SelectGroup* broadcast_next_;             // intrusive worklist next pointer
    std::uint64_t broadcast_epoch_;           // deduplication generation counter
};
```

Phase 1 protocol:

```text
for arm in event.select_port_:
    if arm.group.phase != Arming and arm.group.phase != Armed:
        continue
    if arm.state != Registered:
        continue
    arm.state = CandidateReady

    // deduplicate: if this group's broadcast_epoch_ != current_epoch,
    // it hasn't been linked yet
    if arm.group.broadcast_epoch_ != current_broadcast_epoch_:
        arm.group.broadcast_epoch_ = current_broadcast_epoch_
        arm.group.broadcast_next_ = worklist_head_
        worklist_head_ = &arm.group
```

Phase 2 walks the chain:

```text
grp = worklist_head_
while grp != nullptr:
    next = grp->broadcast_next_     // save before processing
    select_process_group_locked(*grp)
    grp = next
```

This has no capacity limit, no allocation, and no truncation. The entire
broadcast runs under one `global_mtx_` CS, so the temporary fields are never
concurrently reused.

Key properties:

- **No allocation.** The intrusive chain uses existing `SelectGroup` storage.
- **No truncation.** There is no fixed-size array to overflow. Every affected
  group is linked.
- **No dedup failure.** The epoch counter prevents duplicate processing.
- **Phase 2 mutation-safe.** `broadcast_next_` is saved before the group is
  processed (which may finalize the group and leave its arms). The saved
  pointer is valid for the remainder of the walk because the chain is
  built and consumed as a single-linked list under one CS.

---

## 5. The Select-aware Event broadcast — pseudocode

This is the body of `Scheduler::event_set_broadcast` extended for Select. The
existing ordinary-drain loop is unchanged; the Select handling is appended
under the same `global_mtx_` CS.

```text
event_set_broadcast_select_aware(waiters, set_flag):

    PRE: caller will hold global_mtx_ for the whole body

    g.lock()                                          // global_mtx_
    if set_flag already SET:
        g.unlock(); return 0                          // idempotent
    set_flag.store(true, release)

    // -- ordinary drain (UNCHANGED from production) --
    while wake_wait_one_locked(waiters) != nullptr:
        ;                                             // ordinary Event waits

    // -- Phase 1: scan select_port_ for THIS Event, build intrusive worklist --
    worklist_head = nullptr
    current_epoch = ++global_broadcast_epoch_          // monotonically increasing
    for arm in event.select_port_:
        if arm.group.phase != Arming and arm.group.phase != Armed:
            continue                                  // not live; skip
        if arm.state != Registered:
            continue                                  // already CandidateReady or terminal
        arm.state = CandidateReady                    // offered readiness

        // deduplicate group using epoch counter (no caller-local array)
        if arm.group.broadcast_epoch_ != current_epoch:
            arm.group.broadcast_epoch_ = current_epoch
            arm.group.broadcast_next_ = worklist_head
            worklist_head = &arm.group

    // -- Phase 2: process each affected group once via intrusive chain --
    grp = worklist_head
    while grp != nullptr:
        next = grp->broadcast_next_                   // save before processing
        select_process_group_locked(*grp)
        grp = next

    g.unlock()
    signal_wake_locked()                              // wake any parked worker
    return |worklist_head|                            // count not needed; exists for symmetry
```

### 5.1 Properties preserved

- **Event SET is persistent.** `set_flag` is set once and never cleared by
  Select; `reset()` is unaffected.
- **Ordinary Event broadcast semantics are unchanged.** The ordinary drain
  runs first, exactly as today.
- **One group processed once.** Dedup by epoch counter ensures a group with
  two arms on the same Event completes once.
- **No infinite loop.** Phase 1 walks a stable intrusive list snapshot; arms
  registered after the scan are not visited (they are linked under
  `global_mtx_`, so they cannot appear mid-scan).
- **No recursive mutex acquisition.** `select_process_group_locked` does not
  lock any Event structure; it operates on group/registration state under the
  already-held `global_mtx_`.

---

## 6. External-thread `set()`

`Event::set()` is documented as safe from an external OS thread
(`event.hpp:99-105`). The Select-aware broadcast preserves this. The routing
path on an external thread uses `group.caller_owner_` (stored at admission),
not `g_worker`. If the group was admitted from a valid Fiber (required by the
caller contract, `docs/e13-select-public-api.md` §4.11), `caller_owner_` is
non-null and `route_runnable_locked` routes to the correct worker. If
`caller_owner_` is null, it falls back to `pending_spawn_` routing
(`scheduler.cpp:910+`), exactly as today. The cross-worker routing concern is
already solved by the existing E7-B/E8 machinery.

---

## 7. What this does NOT change

- `Event::waiters_` remains sealed and ordinary-only.
- `Event::wait()` and `Event::wait_until()` are unchanged.
- Ordinary `wake_wait_one_locked` is unchanged; it is never called on Select
  arms.
- The ordinary Event drain runs first, before any Select handling, in the same
  `global_mtx_` CS — so ordinary Event waiters continue to wake with the same
  latency and ordering.

---

## 8. Test seams for the Event adapter

The deterministic PhaseTag seams (compiled only into
`sluice_async_internal_testing`, never into the production target) are:

```text
E13PhaseEventScanDone       // pause after Phase 1, before Phase 2 (intrusive worklist built)
E13PhaseEventGroupClaimed   // pause after one group's claim, before finalize
E13PhaseEventArmUnlinked    // pause after winner arm unlink, before publish
E13PhaseEventWorklistWalk   // pause mid-worklist walk (test multi-group chain)
```

These mirror the existing internal-testing discipline
(`scheduler.hpp:39-58`, `tests/async_test_control_internal.hpp`). They drive
causal proofs without sleep. Full plan:
`docs/e13-select-production-test-plan.md` §seams.
