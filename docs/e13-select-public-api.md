# E13 Select Public API

**Task:** `E13-SELECT-PRODUCTION-PREPARATION-1`
**Authority:** selects the authoritative public surface for the first Select
production version. No header is created by this task.

This document compares three API shapes, fixes one, and pins every observable
behavior the formal model leaves open (result index, Event-vs-Timer outcome,
precedence rules, empty/duplicate handling, wrong-Scheduler detection).

---

## 1. Candidates

### 1.1 Candidate A — tagged case span

```cpp
SelectResult select(Scheduler& scheduler, std::span<SelectCase> cases);
```

A non-owning view over a caller-provided array of `SelectCase`. The array is
typically a stack temporary. Select reads it once at admission; the span does
not need to outlive the call beyond the registration critical section.

### 1.2 Candidate B — builder

```cpp
auto result = select(scheduler)
    .on(event_a)
    .on(event_b)
    .at(deadline)
    .wait();
```

A builder object accumulates cases and finalizes on `wait()`. The builder is
moved through each `.on()`/`.at()` and consumed by `wait()`.

### 1.3 Candidate C — fixed variadic

```cpp
SelectResult select(Scheduler& scheduler,
                    EventSelectCase c0, TimerSelectCase c1);
// general variadic form for 1..kSelectMaxArms cases
template <class... Cases>
SelectResult select(Scheduler& scheduler, Cases&&... cases);
```

Each case is a typed value (`EventSelectCase`, `TimerSelectCase`). The variadic
expands into a fixed-size caller-frame array at compile time.

---

## 2. Comparison matrix

| Dimension                          | A (span)              | B (builder)              | C (variadic)                |
|------------------------------------|-----------------------|--------------------------|-----------------------------|
| Allocation                         | caller array (1)      | builder object on stack  | fixed array, zero extra     |
| Exception behavior                 | throws after validate | `.wait()` throws         | throws after validate       |
| Case index stability               | caller must keep order| insertion order          | argument order (compile)    |
| Stack lifetime                     | caller manages span   | builder owns until wait  | automatic                   |
| Compile-time type safety           | runtime tag dispatch  | runtime tag dispatch     | typed per case              |
| Heterogeneous cases                | yes (tagged)          | yes                      | yes                         |
| Future adapter extensibility       | add tag value         | add `.on_x()` method     | add new case type           |
| Public header complexity           | low                   | medium (builder API)     | low (one template)          |
| Duplicate Event arms               | runtime-permitted     | runtime-permitted        | runtime-permitted           |
| Wrong-Scheduler detection          | runtime check         | runtime check            | runtime check               |
| Maximum arm count                  | runtime (cap asserts) | runtime (cap asserts)    | compile-time arity gate     |
| Empty case list                    | span.size()==0 runtime| builder with no `.on()`  | compile-time reject (>=1)   |
| Case-tag invalid                   | runtime reject        | runtime reject           | cannot be expressed         |

(1) "caller array" means Select needs the cases materialised somewhere outside
the variadic expansion; Candidate A pushes that onto the user.

---

## 3. Selected API: Candidate C — fixed variadic

### 3.1 Why C

1. **Zero extra allocation for case storage.** A variadic expansion materialises the cases into
   a fixed-size caller-frame array of `SelectArmSlot` (the variadic expansion
   materialises the cases into a `std::array<SelectArmSlot, sizeof...(Cases)>`).
   No builder object, no span indirection, no heap for case storage. (Timer arms
   additionally allocate one Scheduler-owned stable block each, allocated before
   the global critical section.)
2. **Compile-time type safety.** Each case is a typed value. An invalid tag or
   a missing Scheduler binding cannot be expressed; the type system rejects
   wrong case kinds at the call site.
3. **Compile-time arity gate.** `kSelectMaxArms` is enforced by a
   `requires` clause (`sizeof...(Cases) <= kSelectMaxArms`). The empty-list
   case is rejected by the same requires clause (`sizeof...(Cases) >= 1`).
   "Too many arms" and "zero arms" cannot reach runtime.
4. **Index stability.** Argument order *is* the index the formal model uses
   for the lowest-index tie-break. There is no builder reordering, no span
   reordering; the index is fixed at the call site and visible to the reader.
5. **Header simplicity.** One function template, two case constructors. No
   builder method surface to document, version, or accidentally overload.

### 3.2 Rejected alternatives

- **Candidate A (span).** Rejected because: (a) the caller must materialise
  the array and keep indices meaningful — easy to get wrong; (b) an empty span
  is a runtime check, not a compile-time gate; (c) no compile-time type
  safety — a mistyped tag is a runtime `std::invalid_argument`. The span form
  is useful only if cases come from runtime-counted sources, which the first
  scope does not require.
- **Candidate B (builder).** Rejected because: (a) the builder is an extra
  stack object that must be moved through every `.on()`; (b) `.wait()` is a
  second entry point, doubling the documented surface; (c) ordering is implicit
  (insertion order), which the reader has to trace; (d) the empty-builder case
  is a runtime error. The builder shines for very large case counts with
  optional arms, which is not the first scope.

Candidate C does not foreclose either A or B as future additions: both can be
layered later as wrappers that call the typed variadic core.

### 3.3 The authoritative surface

```cpp
namespace sluice::async {

// Maximum arms a single select() accepts. Compile-time gate.
inline constexpr std::size_t kSelectMaxArms = 8;

// A deadline. Mirrors Scheduler::deadline_t (== deadline_tick_t, monotonic
// absolute time point).
using select_deadline_t = Scheduler::deadline_t;

// Discriminator for the winning arm kind.
enum class SelectKind : std::uint8_t {
    event  = 0,   // an Event arm won (outcome: Woken)
    timer  = 1,   // a Timer arm won (outcome: Expired)
};

// Outcome of a Timer arm. Mirrors the E11 already-due / future-deadline cases.
enum class SelectTimerOutcome : std::uint8_t {
    fired = 0,   // the deadline elapsed (resource was not ready first)
};

// The result of one select() call.
class SelectResult {
public:
    constexpr SelectResult() noexcept = default;   // "no winner" sentinel

    constexpr bool has_winner() const noexcept;
    constexpr std::size_t index() const noexcept;          // 0..N-1
    constexpr SelectKind kind() const noexcept;            // event | timer
    constexpr SelectTimerOutcome timer_outcome() const noexcept; // valid iff kind==timer

private:
    /* stored fields */
};

// Case constructors. Each is a typed value; no tag field exposed.
// SelectCaseType<Case> is a concept satisfied by EventSelectCase and
// TimerSelectCase, used in the select() requires clause.
class EventSelectCase {
public:
    EventSelectCase(Event& event) noexcept;
    // (Scheduler binding is read from the Event; cross-Scheduler is rejected)
};
class TimerSelectCase {
public:
    TimerSelectCase(Scheduler& scheduler, select_deadline_t deadline) noexcept;
    // (absolute monotonic deadline; relative durations are caller-converted
    //  via scheduler.monotonic_now() + duration, matching E11)
};

// The entry point. 1 <= sizeof...(Cases) <= kSelectMaxArms.
// The requires clause rejects empty packs, too-large packs, and non-case
// types at compile time.
// SelectCaseType<Case> is a concept matching EventSelectCase and
// TimerSelectCase (defined in the same header), applied to the decayed type
// (std::remove_cvref_t) so that lvalue cases satisfy the constraint.
template <class... Cases>
    requires (
        sizeof...(Cases) >= 1 &&
        sizeof...(Cases) <= kSelectMaxArms &&
        (SelectCaseType<Cases> && ...)
    )
SelectResult select(Scheduler& scheduler, Cases&&... cases);

}  // namespace sluice::async
```

The implementation lives in `detail/select_port.hpp` (planned,
`docs/e13-select-production-architecture.md` §8). The variadic expands into a
fixed `std::array<SelectArmSlot, N>` of union-based slots inside the `select`
frame; no per-call heap, no derived-to-base slicing.

---

## 4. Pinned observable behaviors

These are the behaviors the formal model leaves open and this design fixes.

### 4.1 Result carries winning index

`SelectResult::index()` returns the position of the winning arm in the
argument list (0-based). Ties at admission go to the **lowest index** (formal:
`CentralClaimWinner` lowest-index tie-break). For a post-suspension winner,
the index is that of the arm whose claim succeeded. `has_winner()` is false
only on the default-constructed sentinel; a successful `select()` always has a
winner. Calling `index()` or `kind()` when `has_winner()` is false is a caller
bug — a debug assertion fires, and the returned value is `0` (defensive
sentinel, not a valid result).

### 4.2 Result identifies Event vs Timer

`SelectResult::kind()` returns `SelectKind::event` or `SelectKind::timer`. The
discriminator is needed because the two arm kinds carry different side-channel
information (Timer has an outcome; Event does not) and because caller code
typically switches on which alternative fired.

### 4.3 Timer result outcome

`SelectTimerOutcome::fired` is the only outcome in the first scope. The field
exists to leave room for future outcomes (e.g. a Timer arm that the runtime
*cancelled* under deferred post-suspension cancellation) without reshaping the
result type. Calling `timer_outcome()` when `kind() != timer` is a caller bug;
it returns `fired` defensively but a debug assert fires.

### 4.4 Inline vs suspended — not public

The `completion_mode` (`Inline` vs `Suspended` in the formal contract) is
**not** exposed on `SelectResult`. The two modes are observably identical to
the caller: the function returns the same `SelectResult`. The mode is an
internal accounting field used by the publication protocol
(`docs/e13-select-locking-and-publication.md`). Exposing it would invite
callers to depend on scheduler timing.

### 4.5 Empty case list

Compile-time reject. The variadic core requires `sizeof...(Cases) >= 1`
enforced by a `static_assert`. A defensive `assert(!"select requires >= 1 case")`
is also present for any future non-variadic entry point.

### 4.6 Duplicate arm behavior

| Pattern                                | Behavior                                                  |
|----------------------------------------|-----------------------------------------------------------|
| same Event appears twice               | **permitted.** Each arm gets a distinct Event registry node. Each can independently win. If both are CandidateReady at admission, lowest index wins; the other is a loser. |
| same Timer deadline appears twice      | **permitted.** Each arm gets a distinct `SelectTimerRegistration`. Same-dedline ties resolve by serialized claim under `global_mtx_`. |
| same Event + same deadline both ready  | lowest-index admission tie-break.                          |
| same `WaitNode` reused across cases     | caller contract violation; debug assert. A case never carries a WaitNode — they are internal. |
| cross-Scheduler Event                  | rejected (see 4.10).                                       |

### 4.7 Already-set Event precedence

At admission (under `global_mtx_`), each Event arm is registered, then the
Event's `set_` flag is observed. An already-set Event is a CandidateReady
offer. If multiple Event arms are already set, the lowest-index one wins the
admission claim (section 4.1). An already-set Event arm competes equally with
an already-due Timer arm; the admission scan observes all arms in index order
and picks the lowest-index ready arm (Event SET precedence over Timer-due is
**not** granted across arms — it is index-driven). See 4.9 for the within-arm
precedence.

### 4.8 Already-due Timer precedence

At admission, after registering a Timer arm and installing its
`SelectTimerRegistration`, the deadline is rechecked against
`scheduler.monotonic_now()`. An already-due Timer is a CandidateReady offer.
It competes with all other arms by index, exactly like an already-set Event.

### 4.9 Multiple ready arms tie-break

**Lowest index.** The admission scan walks the case array in index order; the
first arm observed ready is the admission winner. There is no fairness or
round-robin across arms. This matches `CentralClaimWinner` and the closed
formal safety layer.

### 4.10 All Events belong to same Scheduler

The first scope forbids cross-Scheduler Select. Each `EventSelectCase` reads
its Scheduler binding from the `Event` (which borrows `Scheduler&` for life).
Each `TimerSelectCase` takes an explicit `Scheduler&`. At admission, Select
verifies that every case's Scheduler matches the `select(scheduler, ...)`
first argument. A mismatch throws `std::invalid_argument` BEFORE any
registration begins.

### 4.11 Caller must be a running Fiber on the target Scheduler

`select()` may only be called by a currently running Fiber owned by the
Scheduler passed as the first argument. Calling from a plain OS thread, from a
worker of a different Scheduler, or from a worker with no current Fiber is a
caller contract violation. The implementation validates this before any
allocation or registration:

```text
Scheduler::validate_select_caller():
    // 1. verify this thread is a Scheduler worker
    if g_worker == nullptr:
        throw std::logic_error("select() called from non-worker thread")

    // 2. verify the worker belongs to this Scheduler
    //    (WorkerState stores an immutable owner Scheduler pointer)
    WorkerState* ws = g_worker
    if ws->owner_scheduler != this:
        throw std::logic_error("select() called on wrong Scheduler")

    // 3. verify the worker has a current Fiber
    if ws->current == nullptr:
        throw std::logic_error("select() called with no current Fiber")

    // capture caller + owner for publication
    caller_ = ws->current
    caller_owner_ = ws
```

The `WorkerState::owner_scheduler` pointer is set once at worker construction
and never modified. It is the single authoritative check: if the current
worker's `owner_scheduler` differs from the `select()` target Scheduler, the
call is rejected.

These checks run before any `SelectTimerRegistration` allocation, so no
rollback is needed. The error form is `std::logic_error` (not a debug assert)
because the caller is a user of the public API, not an internal invariant
failure. A debug assertion may additionally fire in debug builds, but the
`std::logic_error` is the load-bearing rejection mechanism.

---

## 5. Allocation policy

| Step                                | Allocates?                          | Where               |
|-------------------------------------|-------------------------------------|---------------------|
| Variadic → caller-frame array       | no (stack)                          | outside any lock    |
| EventSelectCase ctor                | no                                  | outside any lock    |
| TimerSelectCase ctor                | no                                  | outside any lock    |
| `SelectTimerRegistration` per Timer arm | **yes** — Scheduler-owned stable block | **outside `global_mtx_`** |
| Event registry node                 | no (intrusive into SelectArmSlot) | inside `global_mtx_` CS, but no allocation |

The Timer stable block is allocated *before* the `global_mtx_` critical section
begins, so `std::bad_alloc` cannot leave the Scheduler with a partially-
registered group. If the block allocation fails, no registration has begun;
the exception propagates directly.

For a call with `T` Timer arms, Select performs `T` allocations outside the
lock. For an all-Event call, Select performs zero allocations.

---

## 6. Exception behavior

### 6.1 `select(...)` is NOT `noexcept`

It may throw:
- `std::invalid_argument` — validation failure (wrong Scheduler, malformed
  case). Thrown before any registration; nothing to roll back.
- `std::bad_alloc` — Timer block allocation failure. Thrown before the
  global critical section if all Timer blocks are allocated up front, or
  thrown mid-registration if allocation is interleaved (the design forbids the
  interleaved form; see §6.2).

### 6.2 Registration failure rollback

If a failure occurs *after* the first arm is registered but *before*
`FinishRegistration`, the registration critical section performs rollback:

```text
under global_mtx_ (still held):
    for each arm already Registered:
        unlink its Event registry node (Event arm)
          OR retire its SelectTimerRegistration (Timer arm)
        close the arm's authority
    group -> Aborted
release global_mtx_
rethrow
```

This refines `ContractBeginRollback` / `ContractRollbackRelease(i)` /
`ContractFinishRollback`. The rollback is permitted only in the `Building`
phase; once `FinishRegistration` runs the caller is committed to either inline
or suspended completion.

### 6.3 No exception escapes after suspension

Once the caller is `Waiting`, no failure can throw out of `select(...)`. All
post-suspension paths (claim, finalize, publish) are `noexcept` Scheduler
operations. If a fatal invariant is violated post-suspension, the runtime
fails fast (debug assert / `queue_lease_fail_fast`-equivalent), it does not
throw across a `context_switch` boundary.

---

## 7. Wrong-input behavior matrix

| Input                                  | Mechanism                  | Phase      |
|----------------------------------------|----------------------------|------------|
| 0 cases                                | `requires` clause          | compile    |
| > `kSelectMaxArms` cases               | `requires` clause          | compile    |
| wrong case type                        | `requires` clause (`SelectCaseType` concept) | compile |
| Event bound to another Scheduler       | `std::invalid_argument`    | admission  |
| Timer case Scheduler ≠ select() sched  | `std::invalid_argument`    | admission  |
| caller not a Fiber on target Scheduler | debug assert + `std::logic_error` | admission |
| Event destroyed mid-select             | caller contract violation  | runtime (UB in release) |
| Scheduler destroyed mid-select         | caller contract violation  | runtime (UB in release) |

The compile-time rejects are the load-bearing safety: the most common mistakes
(empty, too many, wrong type) cannot reach the runtime at all.

---

## 8. Why this is not "future-adjustable"

The brief explicitly forbids a "we can change it later" verdict. This section
records the irreversibilities:

- **`SelectResult` shape is frozen.** Callers will pattern-match on `kind()`
  and `index()`. Adding a third kind or removing the index is a breaking
  change.
- **The variadic entry point is frozen.** Adding Candidate A (span) or B
  (builder) later is additive; changing the variadic signature is breaking.
- **`kSelectMaxArms = 8` is a public constant.** Raising it is safe; lowering
  it is breaking. The value is chosen to cover realistic `select` use (a
  handful of alternatives) without bloating the caller-frame array.
- **Index semantics is frozen.** Argument order is the winner index. This
  cannot be reordered without breaking every caller.

Everything else (case constructors, internal detail types, the `detail/`
headers) is not part of the stable surface and may evolve.
