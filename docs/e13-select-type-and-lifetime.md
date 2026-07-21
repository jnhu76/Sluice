# E13 Select Type Graph and Lifetime

**Task:** `E13-SELECT-PRODUCTION-PREPARATION-1`
**Authority:** fixes the complete type graph and the destruction contract.
This is the load-bearing document for section E and section J of the task
brief: it answers every per-type question (owner, address stability, access
domain, locks, atomics, friends, visibility) and fixes the WaitNode reuse
decision.

---

## 1. WaitNode reuse decision — SELECTED: SEPARATE

### 1.1 The decision

Select arms do **not** reuse `WaitNode`. A new `SelectArmRegistration` type
replaces the role `WaitNode` plays for ordinary waits. There is no
`WaitNode::user_` reuse, no `WaitNodeUserKind` discriminator, no extension of
`WaitNode`'s responsibility.

### 1.2 Why SEPARATE

The task brief (section D, section J) explicitly forbids:

```text
using WaitNode::user as an undocumented global callback channel
reusing QueuePort's callback semantics without redesign
letting every arm independently wake the caller Fiber
```

The closed `e13-select-preparation.md` (sections 4, 5) proposed reusing
`WaitNode` by adding a `WaitNodeUserKind` tag and a `SelectArmMetadata*` in
`user_`. That route is now rejected for the production design. The reasons:

1. **Pollution of E10's narrow responsibility.** `WaitNode` (see its file
   banner, `include/sluice/async/wait_node.hpp`) is deliberately narrow: one
   wait lifecycle, one cancellation-safe queue protocol, one terminal seam.
   Its `user_` field is documented as **E12-E Queue per-op context only**
   (`wait_node.hpp:141-152`). Promoting it to a generic Select callback
   channel violates that documented boundary.
2. **Forgeable callback authority.** Any TU that can name `SelectArmMetadata`
   can `static_cast` a `user_` pointer and forge Select authority. The
   `ASYNC-TEST-SEAM-AUTHORITY-CORRECTIVE-1` discipline (see the
   `scheduler.hpp:39-58` banner) explicitly removed forgeable namespace-level
   friend types for exactly this reason. A `user_`-based discriminator is
   forgeable.
3. **Structural contamination of the ordinary resolver.** The ordinary
   resolver path (`wake_wait_one_locked`, `scheduler.cpp:1080`) would have to
   branch on every Select node. That branches a hot path for a feature that
   will exist in a small fraction of waits.
4. **Post-destruction raw pointer risk.** `WaitNode` is caller-frame storage.
   A Select arm registration lives in the same frame. The ordinary wait
   machinery expects the WaitNode to be the single object of interest; bolting
   Select metadata onto it creates two frame objects reachable through one
   pointer, doubling the use-after-free surface for the Event queue scan.

### 1.3 Consequence: a new intrusive membership structure

Select arms need a *membership* mechanism in the Event Select registry and the
Scheduler's timer subsystem. That mechanism is a new type
(`SelectArmSlot` and its Event/Timer payloads), with its own intrusive
link fields, not borrowed from `WaitNode`. The link fields are private and
friended to `Scheduler` only — the same sealed-authority pattern as
`WaitQueue` (`wait_queue.hpp:117-152`).

### 1.4 No re-invention of the winner state machine

The brief (section J) requires proof that a new node type does not re-invent:
winner state machine, timer retirement, intrusive membership, terminal
authority. This design reuses each mechanism **by delegation, not by
inheritance from `WaitNode`**:

| Mechanism                 | Reused from                    | How Select uses it                                          |
|---------------------------|--------------------------------|-------------------------------------------------------------|
| winner state machine      | `SelectGroup` atomic claim CAS | A *single* `winner.compare_exchange(NoWinner, arm_index)` on the group — not per-arm |
| timer retirement          | `TimerRegistration` ACTIVE/RETIRED/CONSUMED | Select introduces a *parallel* `SelectTimerRegistration` with the same atomic state pattern, not a reuse of `TimerRegistration` |
| intrusive membership      | `WaitQueue`'s intrusive-list pattern | New intrusive fields on `SelectArmSlot`, separate list head per Event Select registry |
| terminal authority        | The group CAS is the single terminal authority for the *group*; per-arm terminal classification is a plain field guarded by `global_mtx_`, not a CAS |

The split is clean: `WaitNode`/`WaitQueue` continue to handle ordinary waits
exactly as before; Select has its own parallel authority for its own arms.

---

## 2. The type graph

### 2.1 Public types (installed header `select.hpp`)

```cpp
namespace sluice::async {

class SelectResult;                 // see docs/e13-select-public-api.md §3.3
class EventSelectCase;              // a case value: holds Event& only
class TimerSelectCase;              // a case value: holds Scheduler& + deadline

template <class... Cases>
SelectResult select(Scheduler& scheduler, Cases&&... cases);

}  // namespace sluice::async
```

The type graph uses a **fixed-size union slot** representation, not inheritance.
A single `SelectArmSlot` type holds either an Event or Timer arm payload via a
plain union, avoiding the slicing problem of `std::array<Base, N>` with derived
types.

### 2.2 Detail types (header `detail/select_port.hpp`, `detail/select_registration.hpp`)

```cpp
namespace sluice::async::detail {

class SelectGroup;                          // one select() epoch
struct SelectArmSlot;                       // one arm slot: union of Event/Timer payload
struct EventArmPayload;                     // Event-specific arm fields
struct TimerArmPayload;                     // Timer-specific arm fields
class SelectTimerRegistration;              // Scheduler-owned stable timer block
class SelectPort;                           // Scheduler-side per-Event registry head

}  // namespace sluice::async::detail
```

### 2.3 Per-type property sheet

Each type below records the eight fields the brief requires.

#### 2.3.1 `SelectGroup`

| Property                  | Value                                                          |
|---------------------------|----------------------------------------------------------------|
| owner                     | the `select(...)` call frame (caller Fiber)                    |
| borrowed references       | `Scheduler&` (lifetime), `SelectArmSlot[]` (its own array) |
| address-stability         | required for the entire epoch; stable because stack-anchored   |
| construction location     | inside `select(...)` before taking `global_mtx_`               |
| destruction location      | `select(...)` frame unwind, AFTER all authority closed         |
| thread-access domain      | the calling Fiber + Scheduler worker(s) executing the broadcast/pump under `global_mtx_` |
| lock protecting structural state | none — all access is under `Scheduler::global_mtx_`   |
| atomic fields             | `std::atomic<uint32_t> winner_` (NoWinner sentinel + arm index); `std::atomic<GroupPhase> phase_` |
| plain fields under G      | `CompletionMode completion_mode_` (Inline/Suspended, plain enum, written once under G before runnable publication); `SelectGroup* broadcast_next_`; `std::uint64_t broadcast_epoch_` |
| friend authority          | `Scheduler` only                                              |
| public visibility         | none — `detail`                                               |

`winner_` is the **central claim CAS target** (section K, Option C1+C2 hybrid,
see `docs/e13-select-locking-and-publication.md`). The CAS is the single
linearization authority.

Additional fields for broadcast worklist (see
`docs/e13-select-event-adapter.md` §4.6):
```cpp
SelectGroup* broadcast_next_;             // intrusive worklist chain (under G only)
std::uint64_t broadcast_epoch_;           // deduplication generation counter
```

#### 2.3.2 `SelectArmSlot` (the single arm storage type — no inheritance)

| Property                  | Value                                                          |
|---------------------------|----------------------------------------------------------------|
| owner                     | the `SelectGroup` (embedded in a fixed `std::array<SelectArmSlot, N>`) |
| borrowed references       | `SelectGroup&`, the case's primitive (`Event&` or `Scheduler&` + deadline) |
| address-stability         | required for the epoch                                         |
| construction location     | `select(...)` frame, before `global_mtx_`                      |
| destruction location      | `select(...)` frame unwind, after authority closed             |
| thread-access domain      | calling Fiber + Scheduler under `global_mtx_`                  |
| lock protecting structural state | none (intrusive link fields under `global_mtx_`)          |
| atomic fields             | none — state is a plain `ArmState` field guarded by `global_mtx_` |
| friend authority          | `Scheduler`, `SelectGroup`                                     |
| public visibility         | none — `detail`                                               |

```cpp
struct SelectArmSlot {
    ArmKind kind;                           // Event or Timer (discriminator for the union)

    // common state
    ArmState state;                         // Registered, CandidateReady, Retired, etc.
    SelectGroup* group;                     // back-pointer to owning group

    // intrusive link fields (private, Scheduler-only access)
    SelectArmSlot* next_;
    SelectArmSlot* prev_;
    SelectPort* home_;                      // which Event SelectPort, or nullptr for Timer

    // discriminated payload
    union {
        EventArmPayload event;
        TimerArmPayload timer;
    };
};
```

This is a **fixed-size, non-slicing, indexable** representation. The variadic
expansion materialises:

```cpp
std::array<SelectArmSlot, sizeof...(Cases)>
```

inside the `select(...)` frame. Each slot is constructed in place; no per-arm
allocation, no derived-to-base slicing, no virtual dispatch. Arm `i` is
`slots[i]`, accessed by index.

#### 2.3.3 `EventArmPayload`

| Property                  | Value                                                          |
|---------------------------|----------------------------------------------------------------|
| owner                     | `SelectArmSlot` (embedded union member)                        |
| added fields              | `Event& event_` (the bound Event)                              |
| construction location     | `select(...)` frame, before `global_mtx_`                      |
| destruction location      | `select(...)` frame unwind, after authority closed             |

The registry node *is* the `SelectArmSlot` containing the `EventArmPayload` —
no separate node allocation. The slot's intrusive link fields connect it into
the Event's `SelectPort`.

#### 2.3.4 `TimerArmPayload`

| Property                  | Value                                                          |
|---------------------------|----------------------------------------------------------------|
| owner                     | `SelectArmSlot` (embedded union member)                        |
| added fields              | `select_deadline_t deadline_`; associated `SelectTimerRegistration*` |
| construction location     | `select(...)` frame, before `global_mtx_`                      |
| destruction location      | `select(...)` frame unwind, after authority closed             |

The Timer arm does **not** link into an Event Select registry; it is driven by
the Scheduler's timer pump through its associated `SelectTimerRegistration`
stable block.

#### 2.3.5 `SelectTimerRegistration` (Scheduler-owned stable block)

| Property                  | Value                                                          |
|---------------------------|----------------------------------------------------------------|
| owner                     | the **Scheduler** (pointer-stable container, mirroring `timer_pool_`) |
| borrowed references       | `SelectArmSlot*` (the caller-frame Timer arm slot), `Scheduler&` |
| address-stability         | **required and guaranteed**: stored by value in a temporary `std::list`, spliced into Scheduler pool under G |
| construction location     | `select(...)` frame, **before** `global_mtx_` is taken         |
| destruction location      | Scheduler timer subsystem, lazy-at-deadline (mirrors `TimerRegistration`) |
| thread-access domain      | Scheduler worker running the pump under `global_mtx_`; lifetime observed via atomic |
| lock protecting structural state | `Scheduler::global_mtx_` for heap membership                |
| atomic fields             | `std::atomic<State> state_` — `active/retired/consumed` (mirrors `TimerRegistration::state_`) |
| friend authority          | `Scheduler`                                                    |
| public visibility         | none — `detail`                                               |

This is the **only** Scheduler-owned object in the Select type graph. Its
atomic `state_` is the post-destruction safety boundary (I4): the pump reads
`state_` first; if not `active`, it skips without dereferencing the arm
pointer.

Ownership transfer: nodes are constructed in a temporary `std::list` outside G,
then each block is spliced individually under G during its arm's registration
step via `std::list::splice` (O(1), no allocation inside the lock). The
deadline heap stores `DeadlineHeapEntry` values, not raw `TimerRegistration*`
— see `docs/e13-select-timer-adapter.md` §4.

#### 2.3.6 `SelectPort` (Scheduler-side per-Event registry head)

| Property                  | Value                                                          |
|---------------------------|----------------------------------------------------------------|
| owner                     | the **Event** (one `SelectPort` embedded per Event)            |
| borrowed references       | the intrusive list of `SelectArmSlot*` currently linked        |
| address-stability         | stable for the Event's lifetime (embedded in Event)            |
| construction location     | Event construction                                             |
| destruction location      | Event destruction                                              |
| thread-access domain      | Scheduler worker running `event_set_broadcast` under `global_mtx_` |
| lock protecting structural state | `Scheduler::global_mtx_` (no separate registry mutex — see locking doc Option E1-final) |
| atomic fields             | none                                                           |
| friend authority          | `Scheduler`, `Event`                                           |
| public visibility         | none — `detail`, embedded private in Event                     |

`SelectPort` is the head pointer of the Event's Select registry intrusive
list. Adding a `SelectArmSlot` to it is `SelectPort::link_locked`,
called only by the Scheduler under `global_mtx_`.

---

## 3. The lifetime questions the brief requires answered

### 3.1 Is `SelectGroup` in the calling Fiber's stack frame?

**Yes.** It is a stack-local in the `select(...)` function frame. Every
`SelectArmSlot` is a fixed slot in the array embedded in the same
frame. The Scheduler reaches them via raw pointers that are valid only for the
epoch.

### 3.2 Is each arm registration in the same calling frame?

**Yes.** The variadic expansion materialises a fixed-size
`std::array<SelectArmSlot, N>` inside the `select(...)` frame. Each
slot is constructed in place; no per-arm allocation. The unified `SelectArmSlot`
type (with its `EventArmPayload`/`TimerArmPayload` union) avoids the slicing
problem of a derived-class array.

### 3.3 How late can an Event/Timer callback reach a Select arm?

An Event callback (`event_set_broadcast` Phase 1 scan) can reach a Select arm
**only while the arm is linked in the Event's `SelectPort`**. The arm is
unlinked under `global_mtx_` before completion (winner: during finalize; loser:
during finalize; rollback: during rollback). After unlink, no Event scan can
reach it.

A Timer callback (the pump) can reach a Select arm **only through its
`SelectTimerRegistration`, and only while that registration is `active`**. The
registration transitions `active → retired/consumed` under `global_mtx_`
before the caller is resumed. After that transition, a stale pump entry
observes the non-`active` state and skips.

### 3.4 After Fiber resume, which registrations are absolutely unreachable?

After the calling Fiber resumes from a suspended completion:

```text
every Event arm slot:    unlinked from its SelectPort (no Event scan reach)
every Timer arm slot:    state == retired or consumed (pump skips)
every SelectArmSlot:              arm state == retired; group phase == Completed
SelectGroup:                      phase == Completed → Consumed (after caller consumes result); winner committed
```

No callback of any kind can dereference any caller-frame object. The frame may
then unwind and destroy the group + arms.

### 3.5 Which stable control blocks MUST be Scheduler-owned?

Only **`SelectTimerRegistration`**. It mirrors `TimerRegistration`:
Scheduler-owned, pointer-stable, atomic retirement state, lazy physical
reclamation. Everything else is caller-frame.

---

## 4. Destruction contract

### 4.1 Per-object destruction conditions

| Object                          | Destroyed when                                  | Precondition enforced            |
|---------------------------------|-------------------------------------------------|----------------------------------|
| `SelectResult`                  | caller scope exit                               | trivially destructible           |
| `SelectGroup`                   | `select(...)` frame unwinds                     | phase == Consumed or Aborted; all arm authority closed |
| `SelectArmSlot`           | `select(...)` frame unwinds                     | arm state == retired; not linked in any registry |
| (Event arm slot)           | (same as arm)                                   | unlinked from its `SelectPort`   |
| (Timer arm slot)           | (same as arm)                                   | its `SelectTimerRegistration` is retired/consumed |
| `SelectTimerRegistration`       | Scheduler timer subsystem, lazy-at-deadline     | state != active                  |
| `SelectPort` (per-Event)        | Event destruction                               | intrusive list empty (caller contract: drain before destroying Event) |
| caller Fiber frame              | caller unwinds after `select(...)` returns      | all of the above already true    |

### 4.2 The load-bearing ordering

```text
caller resumes (suspended completion)
    happens-after
all arm callback authority closed
```

This is enforced by the publication protocol: the group phase advances to
`Completed` and `make_runnable` is called **only after** every arm is
finalized, every Event registry node is unlinked, and every Timer registration
is retired/consumed. The Fiber cannot observe a resume with any open
authority.

### 4.3 Stale Timer entry protocol

A `SelectTimerRegistration` whose deadline has not yet been reached may remain
physically in the Scheduler's timer heap even after the Select epoch has
retired it (lazy removal, mirroring `TimerRegistration`). The pump's entry
protocol is:

```text
select_timer_pump_entry(reg):
    PRE: global_mtx_ held
    if reg.state_.load(acquire) != active:
        return                         // skip; do NOT dereference reg.arm_
    // safe: while active, the arm is still registered (not yet retired)
    arm = reg.arm_
    ... // proceed with Select timer winner/loser path
```

This is the I4 closure: a stale (retired/consumed) registration is never
dereferenced. The arm pointer is read **only** after observing `active`, and
`active` is lost in the same `global_mtx_` CS that finalizes the arm.

### 4.4 Event registry cleanup before caller resume

Before `make_runnable` (suspended) or before `select(...)` returns (inline),
every `SelectArmSlot` with Event payload for the group is unlinked from its Event's
`SelectPort`. The cleanup is part of the finalize step, under `global_mtx_`,
in the same critical section that closes the arm's authority. After this, an
Event scan that runs after resume cannot observe the group's nodes.

### 4.5 No "Fiber probably still alive" reasoning

The brief forbids relying on Fiber lifetime as a safety guarantee. This design
honors that: **the publication protocol is the safety boundary, not Fiber
lifetime.** The Scheduler never relies on the caller Fiber being alive to
protect a callback; it relies on the arm authority being closed before
publication.

---

## 5. Exception-rollback lifetime

### 5.1 Rollback domain

Registration rollback is permitted only in the `Building` phase — before
`FinishRegistration`, before caller suspension, before any claim. The
preconditions mirror `ContractRollbackEnabledDomain`:

```text
phase == Building
winner == NoWinner
```

(The phase expresses the caller disposition: `Building`/`Selecting` imply the
caller is still running; `Armed` implies the caller was suspended. There is no
separate `caller_state_` field.)

### 5.2 Rollback sequence

On a registration failure (in the first scope, exercised through a synthetic
injection seam — no natural allocation failure occurs mid-registration):

```text
under global_mtx_ (still held from the registration CS):
    for each arm a in 0..last_registered:
        if a is an Event arm and a is linked in its SelectPort:
            SelectPort::unlink_locked(a)        // structural removal
            a.state = retired                   // terminal classification
        if a is a Timer arm and a.stable_reg is active:
            a.stable_reg.state_.store(retired)  // retire the stable block
            a.state = retired
    group.phase = Aborted
release global_mtx_
throw (the synthetic SelectRegistrationError, or the original exception)
```

No runnable publication occurs. No result is written. The caller frame unwinds
and destroys the group + arms. The rollback is safe because each Timer block is
spliced individually during registration: only already-registered Timer arms
have their blocks in the Scheduler pool (and are retired by the loop above);
not-yet-registered Timer blocks remain in the local `tmp_pool` and are
destroyed with the frame, never having been observable by any other thread.

### 5.3 No half-registered state observable

Because rollback runs under the same `global_mtx_` CS that began registration,
no external resolver (Event setter or timer pump) can observe a half-
registered group. The Event setter blocks on `global_mtx_` until rollback
finishes; the pump runs only under `global_mtx_`.

---

## 6. Address-stability discipline

| Object                       | Address stable for           | Why                                  |
|------------------------------|------------------------------|--------------------------------------|
| `SelectGroup`                | the epoch                    | referenced by Event scan + pump      |
| `SelectArmSlot`      | the epoch                    | referenced via `SelectTimerRegistration::arm_` and via intrusive links |
| `SelectTimerRegistration`    | until pump reclamation       | referenced by the pump after the frame may be gone |
| `EventSelectCase`/`TimerSelectCase` | the `select()` call    | trivially stable (stack temps)       |

`SelectGroup` and the arm array are non-copyable and non-movable (their
address is their identity for the intrusive lists), exactly mirroring
`WaitNode` and `Event`.

---

## 7. Summary of the type-and-lifetime decisions

```text
SelectGroup:                  caller-frame, embedded arm array, no mutex
SelectArmSlot:                caller-frame, plain state under global_mtx_, unified union
EventArmPayload:              Event-specific fields inside SelectArmSlot union
TimerArmPayload:              Timer-specific fields inside SelectArmSlot union
SelectTimerRegistration:      Scheduler-owned stable block (the ONLY one)
SelectPort:                   per-Event, Scheduler-only access
SelectResult:                 caller-returned, trivially movable

winner authority:             SelectGroup::winner_ CAS (single point)
arm terminal authority:       plain field under global_mtx_ (no per-arm CAS)
post-destruction safety:      SelectTimerRegistration::state_ atomic (I4)
Event cleanup:                unlink every node before publication
resume ordering:              all authority closed before make_runnable
```

Every question in the brief's section E ("owner / borrowed / address-stable /
construction / destruction / thread-access / lock / atomic / friend /
visibility") is answered in §2.3 for each type.
