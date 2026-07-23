# E13 Select Production Architecture

**Task:** `E13-SELECT-PRODUCTION-PREPARATION-1`
**Base commit:** `6ff8cb3dfd411b78259a0fec0801f8b617fa7572`
**Authority:** maps the CLOSED formal semantics (PR #17, PR #18) onto the
existing production code (E10–E12). **No production implementation is performed
or authorized by this document.** Implementation is
`E13-SELECT-EVENT-TIMER-PRODUCTION-IMPLEMENTATION-1`, denied pending an
independent review of this preparation.

This is the master document. Each non-negotiable production decision is stated
once here with its selected option and pointer to the focused sub-document that
carries the comparison and rejected alternatives.

---

## 0. Authority and scope

### 0.1 Files this task writes

```text
docs/e13-select-production-architecture.md          (this file)
docs/e13-select-public-api.md
docs/e13-select-type-and-lifetime.md
docs/e13-select-event-adapter.md
docs/e13-select-timer-adapter.md
docs/e13-select-locking-and-publication.md
docs/e13-select-production-test-plan.md
docs/e13-select-formal-production-mapping.md
docs/reviews/E13-SELECT-PRODUCTION-PREPARATION-1-REVIEW-REQUEST.md
docs/spec/e13_select/README.md                     (status update only)
```

No modification to `include/**`, `src/**`, `tests/**`, `examples/**`,
`benchmarks/**`, `CMakeLists.txt`, CI workflows, or public installed headers is
performed or proposed by this task. The file layout in section 8 is the
**planned** target for the *next* task; this task does not create those files.

### 0.2 Frozen production scope

The first Select production version supports only:

```text
Central Claim strategy                         (Candidate A, PR #17)
Event arm                                      (E12-A persistent Event)
independent absolute-deadline Timer arm        (E11 TimerRegistration)
inline completion                              (admission-ready winner)
suspended completion                           (post-arming winner)
lowest-index admission tie-break
post-suspension first-valid-claim winner
registration-failure rollback                  (Building phase only)
same-Event multiple arms                       (one group, several arms)
multiple SelectGroups sharing one Event
```

Explicitly **not** supported (deferred or forbidden):

```text
whole-Select cancellation / post-suspension cancellation
shutdown cancellation / task cancellation propagation
Semaphore / AsyncMutex / Condition / Queue / I/O arms
wait-all / cross-Scheduler Select
reusable SelectGroup / multi-epoch SelectGroup
alternative selection strategy
liveness / starvation guarantees
```

### 0.3 Mapping to closed formal layers

```text
E13SelectContract.tla            externally observable lifecycle (PR #17)
        ^  TLC-checked refinement
E13SelectCentralClaim.tla        Candidate A: CandidateReady + claim       (PR #17)
        ^  TLC-checked refinement
E13SelectEventTimer.tla          Event + Timer adapter concrete layer      (PR #17)
        +  PR #18 layered safety invariants + negative models + non-vacuity
```

This architecture document is the bridge from `E13SelectEventTimer` to C++. The
formal-to-production action mapping lives in
`docs/e13-select-formal-production-mapping.md`.

---

## 1. Non-negotiable production laws

The following are mechanical invariants of the closed formal model. The
production design preserves every one; any review finding that contradicts one
of these laws must be resolved by changing the production design, never by
relaxing the law.

```text
one SelectGroup winner linearization            (ContractLinearizeWinner)
one irreversible winner commit                  (ContractCommitWinner)
one result publication                          (result_publication_count <= 1)
at most one runnable publication                 (runnable_publication_count <= 1)
losers never publish                            (C_InvOnlyWinnerPublishes)
no irreversible primitive effect before claim   (C_InvNoIrreversibleEffectBeforeLinearization)
all loser authority closed before completion    (C_InvAllLosersAbortedBeforeCompletion)
all timer authority retired or consumed         (timer terminal after completion)
stale Timer registration never dereferences     (TimerPumpSkip / I4)
Event SET remains persistent                    (no Select consumes the flag)
registration rollback only before completion    (ContractRegistrationRollbackDisabledAfterSuspension)
```

### 1.1 Forbidden implementation routes

These are the routes the task brief (section D) explicitly forbids. They are
restated here as load-bearing production laws, not stylistic preferences:

```text
one ordinary Event::wait per arm + cancelling losers      FORBIDDEN
one ordinary deadline wait per Timer arm                  FORBIDDEN
letting every arm independently wake the caller Fiber     FORBIDDEN
exposing Event::waiters_                                  FORBIDDEN
calling generic wake_wait_one for Select arms            FORBIDDEN
using WaitNode::user as an undocumented Select channel   FORBIDDEN
reusing QueuePort's callback semantics without redesign  FORBIDDEN
```

The ordinary wait resolver (`Scheduler::wake_wait_one_locked`,
`src/async/scheduler.cpp:1080`) performs runnable publication *directly* —
`make_runnable` + `route_runnable_locked` inside the resolve critical section.
Select requires the indirection:

```text
arm offers readiness
        ↓
SelectGroup claim            (single linearization authority)
        ↓
only the winning group publishes once
```

Therefore ordinary WaitQueue wake paths cannot be the Select arm resolver.
Select arms need a **separate registration authority** (section 4) that maps a
primitive readiness event into a group CandidateReady offer, not a runnable
publication.

---

## 2. Summary of selected decisions

| Decision                       | Selected option                                              | Detail doc                              |
|--------------------------------|--------------------------------------------------------------|-----------------------------------------|
| Public API                     | **Candidate C** fixed variadic `select(sched, case, case, …)`| `docs/e13-select-public-api.md`         |
| SelectGroup ownership          | **caller stack frame** (embedded in the select call frame)   | `docs/e13-select-type-and-lifetime.md`  |
| Arm registration ownership     | **caller stack frame** (one per case slot, fixed array of `SelectArmSlot`) | type-and-lifetime                       |
| Event registry strategy        | **E1 — separate private Select registry per Event**          | `docs/e13-select-event-adapter.md`      |
| Timer registration strategy    | **T1 — dedicated SelectTimerRegistration stable block**      | `docs/e13-select-timer-adapter.md`      |
| WaitNode relation              | **SEPARATE — new `SelectArmSlot`, no WaitNode reuse**  | `docs/e13-select-type-and-lifetime.md`  |
| Winner authority               | **C1+C2 hybrid — CAS on SelectGroup, under global_mtx_**     | `docs/e13-select-locking-and-publication.md` |
| Lock order                     | **G → one primitive registry → one SelectGroup (no group m.)**| locking-and-publication                 |
| Allocation policy              | **stack-anchored, per-Timer-arm stable block, no lock alloc**| section 7 / type-and-lifetime           |
| Publication                    | **single `Scheduler::select_publish_locked(SelectGroup&)`**  | locking-and-publication                 |
| Wrong-Scheduler behavior       | **compile-time tag + debug assert + safe rejection**         | section 9                               |
| Destruction contract           | **all arm authority closed before caller resume**            | `docs/e13-select-type-and-lifetime.md`  |
| Implementation file layout     | planned in section 8 (NOT created by this task)              | section 8                               |
| Test seam strategy             | **deterministic PhaseTag under `sluice_async_internal_testing`**| `docs/e13-select-production-test-plan.md`|

---

## 3. Baseline: the seams Select must build on

These are the *existing* production seams the Select implementation will invoke.
Cited line numbers are from base `6ff8cb3`.

| Seam                                                  | Role in Select                                           |
|-------------------------------------------------------|----------------------------------------------------------|
| `Scheduler::global_mtx_`                              | Single coordination domain; claim + publication happen here |
| `Scheduler::wake_wait_one_locked` (`scheduler.cpp:1080`)| The ordinary resolver pattern to *mirror*, not call. Shows `resolve → retire timer → dec count → make_runnable → route` order |
| `Scheduler::route_runnable_locked` (`scheduler.cpp:910`)| The single canonical runnable-enqueue seam; suspended Select publication calls it exactly once |
| `Fiber::make_runnable` / `make_waiting`               | The waiting/runnable transition guards                    |
| `Event::waiters_` (private)                           | Sealed ordinary Event authority. Select does NOT reach into it. |
| `Event::set()` → `Scheduler::event_set_broadcast` (`scheduler.cpp:1303`)| The point Select injects its Phase 1 scan via a **separate** registry |
| `TimerRegistration` (`timer_registration.hpp`)        | The active/retired/consumed stable-block pattern Select mirrors with its own block |
| `Scheduler::pump_deadlines_locked`                    | The timer pump Select hooks with a Select-aware branch     |
| `WaitNode` / `WaitQueue`                              | Ordinary wait authority; Select does **not** reuse `WaitNode::user_` |

The pattern for every E12 primitive was: *Scheduler is the sole executor; the
primitive passes its private `WaitQueue&` by reference into narrow Scheduler
seams; no public accessor exposes the queue; no test friend grants authority.*
Select follows the exact same discipline.

---

## 4. Core type graph (summary)

The full type graph with per-field ownership, address stability, and access
domain lives in `docs/e13-select-type-and-lifetime.md`. The shape is:

```text
public:
    EventSelectCase
    TimerSelectCase
    SelectResult                    (winning index + kind + timer outcome)
    select(Scheduler&, Case0, Case1, ...)   (variadic, 1..kSelectMaxArms)

detail:
    SelectGroup                     (caller-frame control block)
    SelectArmSlot                   (one per case; caller-frame, union of Event/Timer payload)
    EventArmPayload                 (Event-specific arm fields inside SelectArmSlot)
    TimerArmPayload                 (Timer-specific arm fields inside SelectArmSlot)
    SelectPort                      (Scheduler-side per-Event registry head)
    SelectTimerRegistration         (Scheduler-owned stable timer block)
```

### 4.1 SelectGroup is caller-frame, not Scheduler-owned

`SelectGroup` lives inside the C++ stack frame of the `select(...)` call. Every
`SelectArmSlot` is a fixed-array slot inside the same frame. The
Scheduler never owns these objects; it borrows raw pointers to them for the
duration of one Select epoch. This is the same caller-owned, address-stable
discipline as `WaitNode` and `Completion<T>`.

Consequence: **every pointer the Scheduler may reach into a SelectGroup or arm
registration MUST be invalidated before the `select(...)` frame returns.** That
is enforced by closing all arm authority before inline completion (so the frame
can return) and before the caller is resumed after suspended completion (so the
frame cannot be destroyed while a callback still references it).

### 4.2 The stable control block the Scheduler owns

Only one object is Scheduler-owned and outlives the caller frame: a
**`SelectTimerRegistration`** per Timer arm, stored in a pointer-stable
container mirroring `timer_pool_`. Nodes are constructed in a temporary
`std::list<SelectTimerRegistration>` outside G, then **each block is spliced
individually** under G during its arm's registration step via
`std::list::splice` (O(1), no allocation inside the lock). The deadline heap
is migrated to `DeadlineHeapEntry` to hold both `TimerRegistration*` and
`SelectTimerRegistration*` (see `docs/e13-select-timer-adapter.md` §4). Its
atomic `active/retired/consumed` state is the post-destruction safety boundary
(I4): a stale pump entry observes retirement and skips without dereferencing
the caller-frame arm.

---

## 5. Authority seam: separate Select registries

Because Select must not reach into `Event::waiters_` and must not reuse the
ordinary wait resolver, it introduces two **separate, sealed** registration
authorities:

```text
Event ordinary WaitQueue        : ordinary Event::wait users (unchanged, sealed)
Event Select registry (new)     : Event Select arms only       (Option E1)
SelectGroup                     : the claim + publication authority (Scheduler-side)
SelectTimerRegistration (new)   : Timer Select arms only       (Option T1)
```

Each new authority is a *new* intrusive structure on the owning object (Event
or the Scheduler's timer subsystem), with its own lifetime closed before
completion. The ordinary structures are untouched.

---

## 6. Cancellation model

The first production version implements exactly one cancellation path:
**registration-failure rollback** during the `Building` phase, before
`FinishRegistration`, before caller suspension, before any claim. This refines
`ContractBeginRollback` / `ContractRollbackRelease(i)` / `ContractFinishRollback`.

All other cancellation — whole-Select, post-suspension, shutdown, task
propagation — is **deferred**. Registration rollback is the only allowed
loser-side mutation before completion that is driven by the caller rather than
by the claim+finalize protocol.

---

## 7. Allocation and exception policy (summary)

| Concern                        | Decision                                                      |
|--------------------------------|---------------------------------------------------------------|
| Dynamic allocation             | one optional Scheduler-owned stable block per Timer arm only  |
| Allocation under locks         | **deadline heap `reserve` only.** `reserve` is called before any registration mutation; if it throws, nothing is registered. No other allocation under G. |
| Arm count cap                  | compile-time `kSelectMaxArms` (see public-api doc)            |
| `select(...)` exception spec   | **not noexcept**; may throw on validation/allocation failure  |
| Case validation failure        | `std::invalid_argument` thrown BEFORE any registration        |
| Registration failure mid-loop  | rollback every already-registered arm, then rethrow           |
| Timer block alloc failure      | rollback arms, throw `std::bad_alloc`                         |

The variadic `select(...)` evaluates all cases into a fixed-size caller-frame
array (`std::array<SelectArmSlot, N>`) before taking `global_mtx_`. Allocation
happens only in the Timer-arm construction step, which is performed *before*
the global critical section so that a `bad_alloc` cannot leave the Scheduler
with a partially-registered group under lock. Timer blocks are constructed in a
temporary `std::list<SelectTimerRegistration>` outside G, then **each block is
spliced individually** under G during its arm's registration step. Under G, the
deadline heap `reserve`s capacity for all Timer arms before any registration
mutation; if `reserve` throws, no arm is registered and no splice has occurred.
No other allocation occurs inside the lock.

Full detail: `docs/e13-select-public-api.md` §allocation and
`docs/e13-select-type-and-lifetime.md` §exception-rollback.

---

## 8. Planned implementation file layout (NOT created by this task)

The production implementation task may use this layout. This task does **not**
create any of these files; the list exists so reviewers can see the proposed
surface:

```text
include/sluice/async/select.hpp                       (public: SelectCase, SelectResult, select())
include/sluice/async/detail/select_port.hpp           (SelectGroup, SelectArmSlot, SelectPort)
include/sluice/async/detail/select_registration.hpp   (SelectTimerRegistration)

src/async/select.cpp                                  (admission + select() entry)
src/async/select_event.cpp                            (Event Select registry + Phase 1/2)
src/async/select_timer.cpp                            (Timer Select registration + pump branch)

tests/select_inline_test.cpp
tests/select_suspended_test.cpp
tests/e13_select_timer_adapter.cpp
tests/select_registration_rollback_test.cpp
tests/e13_select_multi_group.cpp
tests/e13_select_negative.cpp
```

Each `.cpp` corresponds to one review stage in
`docs/e13-select-production-test-plan.md` §production-implementation-split.

---

## 9. Wrong-object and wrong-Scheduler behavior (summary)

| Misuse                              | Handling                                                  |
|-------------------------------------|-----------------------------------------------------------|
| Event belongs to another Scheduler  | debug assert + safe reject (`std::invalid_argument`)      |
| same Event appears twice            | **permitted** (section C scope); each arm distinct registry node |
| same Timer deadline appears twice   | **permitted**; distinct SelectTimerRegistration blocks    |
| empty case list                     | compile-time reject (variadic min-1 case) + runtime assert|
| invalid case tag                    | compile-time reject (typed cases only)                    |
| too many arms                       | compile-time reject (variadic arity > `kSelectMaxArms`)   |
| Event destroyed with active Select reg | caller contract violation; debug assert               |
| Scheduler destroyed with active SelectGroup | caller contract violation; debug assert            |

Cross-Scheduler Select is **forbidden**. Every case carries an implicit
Scheduler binding (via the Event handle or the explicit Scheduler argument);
mismatch is a debug assert + `std::invalid_argument`.

Full detail: `docs/e13-select-public-api.md` §wrong-input.

---

## 10. Final author verdict

```text
E13-SELECT-PRODUCTION-PREPARATION-1:
PASS — AUTHOR SELF-ASSESSMENT

BASE:
6ff8cb3dfd411b78259a0fec0801f8b617fa7572

PUBLIC API:              SELECTED   (Candidate C, fixed variadic)
TYPE GRAPH:              SELECTED   (SelectGroup caller-frame + SelectTimerReg stable)
EVENT ADAPTER:           SELECTED   (Option E1 separate sealed registry)
TIMER ADAPTER:           SELECTED   (Option T1 dedicated SelectTimerRegistration)
WAITNODE RELATION:       SELECTED   (SEPARATE — no WaitNode::user_ reuse)
CENTRAL CLAIM AUTHORITY: SELECTED   (C1+C2 hybrid CAS under global_mtx_)
LOCK ORDER:              SELECTED   (G → one registry → one group, no group mutex)
LIFETIME:                SELECTED   (all authority closed before caller resume)
PUBLICATION:             SELECTED   (single select_publish_locked)
FORMAL-PRODUCTION MAPPING: COMPLETE (docs/e13-select-formal-production-mapping.md)
TEST MATRIX:             COMPLETE   (docs/e13-select-production-test-plan.md)

PRODUCTION IMPLEMENTATION:
DENIED PENDING INDEPENDENT REVIEW
```

Only an independent review PASS authorizes
`E13-SELECT-EVENT-TIMER-PRODUCTION-IMPLEMENTATION-1`.
