# E12-E Queue Corrective-2 — Independent Adversarial Design Review

```text
E12-E-QUEUE-CORRECTIVE-2-INDEPENDENT-ADVERSARIAL-REVIEW-1
```

> **Scope:** design-authority review only. The reviewer is **not** the author
> of any Corrective-2 design document, the implementation-authorization
> report, or any of the substrate authorities. No claim in the author's
> self-assessment, commit messages, the existing implementation-authorization
> report, or the existing Mutex-nothrow production review is taken as fact;
> every blocking claim is reproduced independently from current source,
> commit diffs, and repo-external scratch compile probes.
>
> The Queue has **no production code** in this repository (verified — see
> §B.0). This is correct and expected; B2 gates the *design* before any
> production code may be written. The review therefore verifies whether the
> *declared* type graph, state machine, transition matrix, and counterexample
> dispositions are (a) internally consistent, (b) realizable in C++20, and
> (c) faithful to the production substrate the design builds upon.
>
> The only durable file added by this review is this document. No file under
> `include/`, `src/`, `tests/`, `xmake.lua`, or any Queue design document was
> modified. Repo-external scratch probes under `/tmp/e12e_probe/` were used to
> verify the type graph compiles; the working tree is byte-identical to review
> start except for this file. Nothing was committed.

---

## A. Verdict

```text
E12-E-QUEUE-CORRECTIVE-2-INDEPENDENT-ADVERSARIAL-REVIEW-1: PASS
```

The Corrective-2 design authority is internally consistent, realizable in
C++20, faithful to the production substrate, and disposes all 33 enumerated
counterexamples by type structure / access control / lock order / state-
machine reasoning rather than debug assertions. Six compile-feasibility probes
(syntax + executable) are green. Eleven findings are recorded below; the
single BLOCKING-class observation concerns a forward-looking implementation
dependency (no `fiber_owner_.erase` may run before the Queue operation
releases its captured slot) that the design *prescribes* but cannot enforce
from the design alone — it is recorded as a binding implementation obligation,
not a defect in the design authority under review. Three MINOR wording
inconsistencies in the timer factory declarations are noted as REQUIRED
clarifications before the spec is handed to implementers; they do not block
the design authority.

```text
TARGET COVERAGE (independently recounted):
  Producer canonical transitions: P1 P2 P3 P4 P5 P6 P7 P9 P10      = 9
  Consumer canonical transitions: C1 C2 C3 C4 C5 C6 C8 C9          = 8
  Close canonical transitions:    CL1 CL2                          = 2
                                                                  ----
  Total canonical:                                                 19  ✓
  Publication transitions:
    PUB-P-COMM PUB-P-CLOSED PUB-P-EXPIRE
    PUB-C-COMM PUB-C-CLOSED PUB-C-EXPIRE                          = 6  ✓

  P8 / C7 / PUB-P-CANCEL / PUB-C-CANCEL: RESERVED — DEFERRED      ✓
```

No unresolved Critical/Required counterexample remains at the design level.
Production proof still requires implementation, runtime tests, sanitizers,
and the separate B4 TLA+ formal model — none is authorized or claimed here.

---

## B. Independence, scope, and substrate verification

### B.0 No Queue production code exists (verified)

`grep -rn "AsyncQueue\|QueueItemLease\|QueueItemControl\|QueuePort\|QueueTeardownSession\|PreparedQueueTimer\|queue_runnable_head_\|QueueRunnableTicket" include/ src/ tests/ xmake.lua` returns zero hits across `include/`, `src/`, `tests/`, and `xmake.lua`. `find . -iname '*queue*' -path '*/async/*'` returns only `docs/e12-queue*.md` (design) — no header, source, or test under `async/`. The `tests/test_t3_simple.cpp` untracked file contains only an `AsyncCondition` smoke test (verified, unrelated). This matches the design's `DENIED — B2/B4 OPEN` status and the existing `docs/e12-queue-implementation-authorization.md` investigation report.

### B.1 Precondition gate status (independently verified)

The design's gate claims were checked against current source + commits
(HEAD `3c1daf5`), not against the implementation-authorization doc (whose
HEAD reference of `c6efa13` predates three landings).

| Gate | Design claim | Independent verification | Result |
| --- | --- | --- | --- |
| B1 Mutex no-throw substrate | `PASS` | `include/sluice/async/mutex.hpp:54,71,81` declares `lock() noexcept`/`try_lock() noexcept`/`unlock() noexcept`; `:67-68,77-78` routes failures to `detail::async_mutex_lock_fail_fast()`; commit `be07564` ("feat(async): make Mutex acquisition fail-fast"); death tests in `tests/e12_async_mutex_death_test.cpp` landed by `e2cfe61`; independent production review at `docs/reviews/ASYNC-MUTEX-NOTHROW-PRODUCTION-IMPLEMENTATION-1-REVIEW.md` returns `PASS`. | **PASS** |
| B2 Corrective-2 independent review | `OPEN` | This document is that review. | **CLOSED BY THIS REPORT** |
| B3 Condition T25 hang audit | `PASS` | Commit `db656b5` ("test(async): make Condition T25 migration trace deterministic") rewrote `tests/e12_async_condition_test.cpp` (165 insertions / 37 deletions); `bounded_wait`/`release_for_drain`/`f_idle` now CALLED (e.g. `:1500,1506`); the determinism corrective mirrors the Mutex T19 pattern. | **PASS** |
| B4 Queue formal model | `OPEN` | `find docs/spec -iname '*queue*'` returns zero hits; `grep -rln "QueueItemLease\|QueueTeardown" docs/spec/ scripts/` returns zero hits; no `verify-e12-queue-formal.sh` exists. | **OPEN** (correctly) |

The B1/B3 landings post-date the implementation-authorization doc's HEAD
(`c6efa13`); the doc's "B1 FAILS" / "B3 FAILS" findings are stale relative to
current HEAD but the doc itself was written against an older tree and is
explicitly an *investigation report* (not a binding authority). The current
binding Queue docs correctly reflect B1/B3 as PASS.

### B.2 Substrate fidelity — claims checked against production source

| Design claim | Production evidence | Verdict |
| --- | --- | --- |
| `Scheduler::global_mtx_` is the coordination domain; all wait/wake/cancel/expire serialize on it | `include/sluice/async/scheduler.hpp:801` `mutable Mutex global_mtx_`; acquired at `src/async/scheduler.cpp:683,688,707,980,...`; `wake_wait_one_locked`/`mutex_handoff_one_locked`/`expire_wait`/`pump_deadlines_locked` all under G | **MATCH** |
| Lock order `G → q.mtx()` (one role WaitQueue) | `wait_queue.hpp:152` `mtx()` is private + `friend Scheduler`; `mutex_handoff_one_locked` (`scheduler.cpp:2081`) takes `waiters.mtx()` under G; design §11 specifies `G → S → exactly one of P/C` — S is a new QueueCore state lock (does not exist yet); the pattern is sound | **MATCH** |
| `mutex_handoff_one_locked` is the winner-before-publication precedent | `scheduler.cpp:2065-2106`; `owner = f` at `:2088` between resolve CAS (`:2082`) and `make_runnable` (`:2102`); `retire_timer_for_node_locked(*won)` at `:2098` in the same CS | **MATCH** |
| `TimerRegistration` is non-copy/movable, address-stable in `std::list` | `include/sluice/async/timer_registration.hpp:85-88` deletes copy and move; `scheduler.hpp:930` `std::list<TimerRegistration> timer_pool_` | **MATCH** |
| `std::list` iterator/reference stability across growth | Independently verified — see probe 4 §C.4 | **MATCH** |
| `std::unordered_map` mapped-value address stability across rehash; iterator invalidation | Independently verified — see probe 6 §C.6 | **MATCH** |
| `try_steal` does NOT check victim activity | `scheduler.cpp:2652-2708` — stealable predicate is `f->state()==runnable && fiber_owner_[f]==victim`; no read of `victim->active` | **MATCH** |
| Existing E8 steal uses `fiber_owner_.find` + `fiber_owner_[stolen]=thief` (lookup + `operator[]`) | `scheduler.cpp:2685-2694` — confirmed | **STRICTER-THAN-SUBSTRATE** (see §F.1 MINOR-3) |
| `fiber_owner_` is never erased in current production | All writes are at `:363,389,451,2694`; no `.erase()` exists anywhere | **MATCH** (relevant to §F.1 MINOR-1) |
| Test seams live only in non-installed `sluice_async_internal_testing` variant | `scheduler.hpp:38-57` banner; `mutex.hpp:56-65,73-75` macro-gated; `xmake.lua:46-86` defines variant + macro | **MATCH** |
| No `#include` of any queue header from any production header | `grep -rn "queue" include/sluice/async/*.hpp` returns only `wait_queue.hpp` and comments | **MATCH** |

The design's substrate claims are faithful.

---

## C. Compile-feasibility probes (REQUIRED, §B3)

All probes live under `/tmp/e12e_probe/` (repo-external; not added to the
repo). Each is a minimal stand-in matching the design's declared
relationships from `docs/e12-queue-scheduler-integration.md` §4–§8. The
design has no production code, so the probes test whether the design's
*claimed* type structure is realizable.

```text
clang++ : /usr/bin/clang++ (clang version 18.0.8 / 19.x — system default)
g++     : /usr/bin/g++       (used as a second compiler for cross-check)
flags   : -std=c++20 -Wall -Wextra -pedantic [-fsyntax-only | -O0 -g]
```

### C.1 Probe 1 — `lease_positive.cpp` (positive realization)

**Claims tested:** private `QueueItemControl` ctor; private `QueueItemLease`
default/raw-control ctors and `adopt_control`; private nested `Node<T>`;
`friend QueueItemFactory`; `std::exchange` clears source on move; moved-from
reuse stays empty; release recovers the original payload (one T move); empty-
lease destruction does not terminate.

```text
clang++ -std=c++20 -Wall -Wextra -pedantic -fsyntax-only /tmp/e12e_probe/lease_positive.cpp
EXIT: 0   (2 warnings: unused parameter/variable — cosmetic)

clang++ -std=c++20 -Wall -Wextra -pedantic -O0 -g \
  /tmp/e12e_probe/lease_positive.cpp -o /tmp/e12e_probe/lease_positive
EXIT: 0
/tmp/e12e_probe/lease_positive
EXIT: 0   (all runtime assertions held)
```

**Result: PASS.** The type graph declared in §4 is realizable, the move
semantics behave as claimed, and the fail-fast dtor for a non-empty lease
does not invoke user code (no `delete`).

### C.2 Probe 2 — `lease_negative.cpp` (12 forbidden operations)

Each forbidden operation is gated by its own macro and compiled separately.
Every case must be a compile error.

```text
PASS [NEG_LEASE_COPY]                : 2 errors  (private constructor)
PASS [NEG_LEASE_RAW_CONTROL_CTOR]    : 1 error   (private constructor — lease cannot be forged from raw control*)
PASS [NEG_LEASE_ADOPT]               : 2 errors  (private adopt_control)
PASS [NEG_CONTROL_LOCATION]          : 2 errors  ('location_' is private)
PASS [NEG_CONTROL_CTOR_PUBLIC]       : 1 error   (private constructor)
PASS [NEG_TEARDOWN_CTOR_PUBLIC]      : 1 error   (private constructor)
PASS [NEG_TEARDOWN_COPY]             : 1 error   (deleted copy)
PASS [NEG_TEARDOWN_MOVE_ASSIGN]      : 1 error   (deleted move-assign)
PASS [NEG_LEASE_PUBLIC_DEFAULT_CTOR] : 1 error   (private default ctor)
PASS [NEG_CONTROL_COPY]              : 1 error   (deleted copy)
PASS [NEG_CONTROL_MOVE]              : 1 error   (deleted move)
PASS [NEG_FACTORY_NODE_NAMING]       : 2 errors  ('Node' is private nested)
```

**Result: 12/12 PASS.** All forbidden operations fail to compile exactly as
the design §4/§5/§6 claims. Downstream code cannot forge a lease, mutate
location, copy/move a control, construct a teardown session, copy/move-assign
a teardown session, default-construct a lease, or name the typed `Node<T>`.

### C.3 Probe 3 — `teardown_probe.cpp` (QueueTeardownSession lifecycle)

**Claims tested:** move-only session; move clears source; lifecycle is
one-way into `tearing_down`; ordinary QueuePort calls rejected after the
transition; `take_next()` drains the ring one slot at a time; session
destructor with empty ring marks the authority complete and does not
terminate; session destructor with non-empty ring would terminate.

```text
clang++ -std=c++20 -Wall -Wextra -pedantic -fsyntax-only /tmp/e12e_probe/teardown_probe.cpp
EXIT: 0
clang++ -std=c++20 -Wall -Wextra -pedantic -O0 -g \
  /tmp/e12e_probe/teardown_probe.cpp -o /tmp/e12e_probe/teardown_probe
EXIT: 0
/tmp/e12e_probe/teardown_probe
stdout: "ordinary_call rejected (tearing_down)"
EXIT: 0
```

**Result: PASS.** The teardown lifecycle (§6/§9/§15) is realizable; the
absorbing transition rejects ordinary calls and serializes against
`active_port_calls_ == 0`.

### C.4 Probe 4 — `timer_prepared_probe.cpp` (PreparedQueueTimer + list stability + reverse destruction)

**Claims tested:** the exact `PreparedQueueTimer` declaration from §8
compiles; move disarms source (`armed_` exchanged to false); move-assign and
copy are deleted; `std::list` iterator AND reference to an existing element
survive 1000x `push_back` of other elements; erasing a *different* element
does not invalidate an existing iterator; reverse-construction-order
destruction (prepared destroyed before `global_lock` when declared in the
binding order from §8); armed destructor rolls back exactly once on scope
exit.

```text
clang++ -std=c++20 -Wall -Wextra -pedantic -fsyntax-only /tmp/e12e_probe/timer_prepared_probe.cpp
EXIT: 0
clang++ -std=c++20 -Wall -Wextra -pedantic -O0 -g \
  /tmp/e12e_probe/timer_prepared_probe.cpp -o /tmp/e12e_probe/timer_prepared
EXIT: 0
/tmp/e12e_probe/timer_prepared
EXIT: 0   (all runtime assertions held; rollback count == 1)
```

**Result: PASS.** The two stability claims (a) `std::list` iterator/reference
survives growth and (b) reverse-destruction ordering for the binding local
declaration order both hold. The `PreparedQueueTimer` declaration from §8 is
realizable.

### C.5 Probe 5 — `result_probe.cpp` (opaque/public result contracts)

**Claims tested:** `T` need not be move-assignable (the result uses destroy-
and-move-construct for move assignment); `failed(Status, T&&)` performs
exactly one T move (counted via a static counter incremented in `T`'s move
ctor); self-move-assignment leaves a valid payload; moved-from result has a
valid no-payload status.

```text
clang++ -std=c++20 -Wall -Wextra -pedantic -fsyntax-only /tmp/e12e_probe/result_probe.cpp
EXIT: 0
clang++ -std=c++20 -Wall -Wextra -pedantic -O0 -g \
  /tmp/e12e_probe/result_probe.cpp -o /tmp/e12e_probe/result_probe
EXIT: 0
/tmp/e12e_probe/result_probe
EXIT: 0   (MoveOnly::moves == 1 after failed(); == 0 after result move-assign)
```

**Result: PASS.** The result contract (§5) supports non-move-assignable `T`
and performs exactly one T move from node to public failed result.

### C.6 Probe 6 — `owner_slot_probe.cpp` (`std::unordered_map` reference stability)

**Claims tested (§9):** `std::unordered_map` rehash invalidates iterators but
NOT references/pointers to elements; the captured mapped-value address
remains usable for direct write across many rehashes; erasing the referenced
element would invalidate the slot (the design's "no erase" rule is therefore
load-bearing).

```text
clang++ -std=c++20 -Wall -Wextra -pedantic -O0 -g \
  /tmp/e12e_probe/owner_slot_probe.cpp -o /tmp/e12e_probe/owner_slot
EXIT: 0  (1 warning: unused `it1` — kept only to document that iterators
         ARE invalidated and the design correctly uses the address instead)
/tmp/e12e_probe/owner_slot
EXIT: 0
```

**Result: PASS.** The §9 owner-slot invariant rests on a property that
holds for `std::unordered_map` per the standard: rehash invalidates iterators
but not element addresses. The design's prescription to capture the address
(not the iterator) and to forbid `operator[]`/`find`/`erase`/rehash during
the steal/publication interval is the correct realization.

---

## D. Per-topic findings (11/11)

For each topic the design's claim is restated tersely and independently
adjudicated PASS / DEFER-with-note / MINOR. No topic is REFUTED.

### D.1 Topic 1 — One-shot `QueueItemLease`

**Design claims (§4, §4.1):** non-copyable; move clears source; non-empty
target move-assignment terminates; downstream caller cannot forge a lease
from a raw control pointer; same detached control cannot be admitted twice;
production type structure blocks double ownership; non-empty lease
destruction does not leak or invoke user code.

**Independent verification:**
- Probe 1 verifies: move clears source (`std::exchange(other.control_, nullptr)`); moved-from source stays empty on second move; release recovers the original payload via one T move; empty-lease destruction does not terminate.
- Probe 2 verifies: copy deleted; raw-control ctor private; `adopt_control` private; default ctor private; `Node<T>` private nested.
- The move-assignment operator calls `require_empty_or_terminate()` if `control_ != nullptr` (declaration §4 line 188-193) — non-empty target is fail-fast, NOT silent overwrite. Verified by reading the declaration (the probe mirrors it).
- Same detached control cannot be admitted twice: after first by-value admission, the caller's lease is empty; the second `try_push(std::move(lease))` passes an empty lease; the entry check `require lease non-empty` rejects it before any state mutation (§4.1). Verified by probe 1's l3 case.
- The destructor of `QueueItemLease` is `noexcept`, calls `std::terminate()` if non-empty, and contains NO `delete` and no virtual dispatch — it cannot delete a type-erased node or invoke user code (§4 line 226-231). Verified.

**Verdict: PASS.** All sub-claims are realized by access control + move semantics; not by debug assertions.

### D.2 Topic 2 — Failed push exact payload

**Design claims (§5, §10):** `closed`/`expired`/`would_block` all return the
original, complete, unique lease — not reconstructed from raw pointer, not
copied `T`, not default-constructed, not lost; no `T` move/destruction inside
Queue/Scheduler locks.

**Independent verification:**
- The opaque result invariant (§5 line 401-404): `status == committed <=> lease is empty; status != committed <=> lease is non-empty`. Enforced at every factory and move boundary.
- The opaque push result stores `QueueItemLease lease_` by value (§5 line 383); `take_failed_lease() &&` is rvalue-qualified (§5 line 379) — it can only be invoked on an rvalue result, after which the result is in a valid moved-from no-payload state.
- The typed failure path (§5 line 413-421): `move out the unique QueueItemLease → validate owner_port_ and type_token → recover exact hidden Node<T> → change detached → released → move T exactly once into QueuePushResult<T> → delete factory Node<T> outside all locks`. No raw-pointer reconstruction API exists (probe 2 NEG_LEASE_RAW_CONTROL_CTOR confirms the private ctor).
- Probe 5 verifies: `failed(Status, T&&)` performs exactly ONE T move; the result supports non-move-assignable `T` (so no copy path exists); self-move leaves a valid state.
- The lock boundary: P3/P4/P9 rows in §13 all show Locks `G+S` and "no allocation/throw after entry; caller result owns node". The T move/destruction happens at P10 *outside* locks (P10 row: "one T move/delete outside locks").

**Verdict: PASS.** The lease is the single move-only authority; the failed push always returns it intact.

### D.3 Topic 3 — Ring lease uniqueness

**Design claims (§4.2):** each non-empty ring slot owns exactly one lease;
source move empties; destination pre-move empty; same ItemId cannot be in
two slots; proof depends on production structure not just debug assertions.

**Independent verification:**
- The ring is `std::unique_ptr<QueueItemLease[]> ring_` allocated once at Queue construction (§4.2 line 332). Each slot starts empty (default-constructed lease, `control_ == nullptr`).
- Every transfer uses move construction (empty destination) or move assignment (with `require_empty_or_terminate()` guard). Both empty the source via `std::exchange` (probe 1).
- One control cannot occupy two slots: source is emptied before destination is filled, and destination cannot be non-empty at the moment of fill (the precondition terminates otherwise).
- §13 transition rows P2/P6/C1/C5 each state "source op/slot empty, unique slot/ItemId" — the uniqueness is asserted as a structural consequence of move-only lease + empty-destination transfer, not a runtime assertion.
- §7.1 binding invariant `ring ItemIds are unique` is the target; the proof is the type structure of §4.

**Verdict: PASS.** Move-only + empty-destination-enforcing lease type makes ring ItemIds unique by construction.

### D.4 Topic 4 — Active-victim stealing

**Design claims (§10, state-machine §7.4):** current-worker own-oldest first,
otherwise global-oldest stealable; active victim does NOT veto stealing.

**Independent verification:**
- Production `try_steal` (`scheduler.cpp:2652-2708`) does NOT read `victim->active` — the stealable predicate is `f->state() == FiberState::runnable && fiber_owner_[f] == victim`. Confirmed by grep: zero hits for `victim->active` / `victim_activity` / `active.load` in steal path.
- Design §10 binding selection (`pop_queue_runnable_locked` lines 736-751): if the current Worker has an owned Queue ticket, choose the own-oldest; otherwise choose the global head; the admission owner's activity is never an eligibility predicate.
- The "own-ticket preference intentionally outranks an older foreign ticket" (§10 line 764) — this is a load-bearing fairness/affinity choice, not a defect. It does not affect correctness of single-resolver semantics.
- The §10 deterministic trace (F on W0, W0 blocker, external publish, W1 idle, W0 active, W1 may claim the global-oldest and update F's slot under G) is realizable given the substrate above.
- The T25 Condition hang (B3) was root-caused to a test-harness defect (unbounded coordinator spins), not a production steal defect; the W1 corrective (`db656b5`) ports the Mutex T19 determinism pattern. The design correctly does not rely on T25 to argue active-victim steal.

**Verdict: PASS.** The substrate semantics match the design prescription; an active victim does not block stealing.

### D.5 Topic 5 — Owner-slot lifetime

**Design claims (§9):** admission-captured owner slot address is stable
across registration, ticket-link, steal-selection, ticket-removal, resume,
operation-release; no map rehash/erase/reference-invalidation during that
interval.

**Independent verification:**
- §9 invariant text (lines 697-708) prescribes: `fiber_owner_` contains the Fiber entry before registration; the mapped-value address remains valid until (1) ticket removed + (2) Fiber resumed + (3) operation releases the slot; no Scheduler path erases that element during the interval; all reads/writes under G.
- Probe 6 verifies: `std::unordered_map` rehash invalidates iterators but NOT references/pointers to elements — so capturing the address is correct; the design's prescription to forbid `operator[]`/`find`/`erase`/rehash during publication/steal is the correct realization.
- Production status: `fiber_owner_` is currently NEVER erased (grep returns no `.erase()` calls), so the invariant is trivially satisfied today. **However**, if future work adds `fiber_owner_.erase(f)` on Fiber completion, the invariant would be violated unless gated on slot release. This is a forward-looking obligation, not a current defect — see §F.1 MINOR-1.

**Verdict: PASS (with forward-looking implementation obligation MINOR-1).**

### D.6 Topic 6 — PREPARED timer

**Design claims (§8):** concrete `PreparedQueueTimer` C++ representation
satisfies `Prepared → Active → Retired/Consumed`; list-iterator stability;
reverse destruction order; pump invisibility; pre-registration rollback;
already-due deadline; activation+immediate-discard; no-target Prepared state;
no dangling iterator.

**Independent verification:**
- The declaration shape (§8 lines 580-602) compiles (probe 4).
- `TimerPool::iterator iterator_` stores a `std::list<TimerRegistration>::iterator`. Probe 4 verifies list iterators survive 1000x growth and survive erasure of *other* elements — exactly the property §8 relies on. (List iterators are ALSO stable across rehash by definition — there is no rehash for `std::list`.)
- Reverse-destruction ordering: §8 lines 612-623 prescribe the binding local declaration order `GlobalLockGuard global_lock(...); PreparedQueueTimer prepared = ...;`. Probe 4 verifies C++ destroys automatic objects in reverse construction order, so `prepared` is destroyed before `global_lock` on every early-return / exception path.
- "Armed destructor rolls back only while G held": the destructor is `noexcept`, runs while `global_lock` is still alive (because of reverse order), and rolls back exactly once (probe 4: rollback count == 1).
- Pump invisibility: §8.1 (lines 653-664) specifies PREPARED starts as `not deadline-heap linked, pump invisible, not counted by active_queue_timers_`. This is an internal-state prescription the implementation must honor.
- Two MINOR wording inconsistencies in the timer factory declarations — see §F.2. They are clarifications, not design defects.

**Verdict: PASS (with two MINOR wording clarifications in §F.2).**

### D.7 Topic 7 — QueueTeardownSession

**Design claims (§6):** unique and unforgeable; lifecycle one-way into
`tearing_down`; ordinary QueuePort calls rejected thereafter; ring leases
transferred one-by-one to teardown; typed destruction outside locks; session
non-restartable; no ordinary quiet-drain backdoor.

**Independent verification:**
- Probe 3 verifies: move-only session; lifecycle one-way; ordinary calls rejected after `tearing_down`; `take_next()` drains one slot per call; session destruction with empty ring completes the authority and does not terminate.
- §6 declaration (lines 458-485): private ctor `explicit QueueTeardownSession(QueuePort&)`; copy and move-assign deleted; `friend QueuePort` only. Probe 2 NEG_TEARDOWN_CTOR_PUBLIC, NEG_TEARDOWN_COPY, NEG_TEARDOWN_MOVE_ASSIGN confirm all three are inaccessible / deleted from outside the friend set.
- §6 `begin_teardown()` precondition (lines 489-498): `lifecycle == operational`, `active_port_calls_ == 0`, `active_wait_associations_ == 0`, `active_queue_timers_ == 0`, `granted_not_resumed_ == 0`, both WaitQueues empty, no live teardown authority. All four counters are independently maintained per the §7 ledger. The lifecycle check + CallGuard entry serialize under G+S, so an in-flight ordinary call makes `active_port_calls_ != 0` and blocks teardown.
- The ordinary `take_quiescent_item()` API is removed (§6 line 454 "Ordinary `take_quiescent_item()` is removed"). Grep confirms zero production hits — no quiet-drain backdoor exists.

**Verdict: PASS.** Teardown is unique, irreversible, and outside the ordinary CallGuard path.

### D.8 Topic 8 — `active_port_calls_` scope

**Design claims (§7):** it means ONLY the ordinary non-template QueuePort
call interval — not "all typed callers gone", not "all external refs gone",
not "concurrent destruction auto-safe".

**Independent verification:**
- §7 (lines 524-530) defines the counter exactly: "Number of ordinary QueuePort operations that have entered the non-template Queue authority and have not yet returned from QueuePort. It does not mean that the complete public `AsyncQueue<T>` method is still running. Typed result conversion and typed node destruction occur after QueuePort return and are not counted."
- §7 (lines 533-546) enumerates CallGuard coverage: `push/push_until/try_push`, `pop/pop_until/try_pop`, `close`, `snapshot`. Excludes: `begin_teardown`, `QueueTeardownSession::take_next`, typed conversion, typed destruction.
- §7 line 555-556 explicit non-claim: "The four zeros do not prove all typed public methods returned, all arbitrary callers disappeared, or concurrent destruction is safe." §6 line 516-517 repeats: "Concurrent `AsyncQueue<T>` destruction remains a caller contract violation; the counters do not make arbitrary caller lifetime safe."
- Counterexample #33 (§16) reiterates the scoping.

**Verdict: PASS.** The scope is narrowly and explicitly defined; the design does not over-claim.

### D.9 Topic 9 — 19 canonical transitions

**Design claims:** Producer 9 (P1 P2 P3 P4 P5 P6 P7 P9 P10) + Consumer 8
(C1 C2 C3 C4 C5 C6 C8 C9) + Close 2 (CL1 CL2) = 19.

**Independent recount:** matches — see §A. The numbering gap (P8 ProducerCancel / C7 ConsumerCancel RESERVED-DEFERRED) is explicit and consistent with §8.3's `P8/C7: RESERVED — DEFERRED — NOT IN QUEUE V1` and §10's `PUB-P-CANCEL/PUB-C-CANCEL: RESERVED — DEFERRED`.

**Row-by-row review of §13 (each row checked for guard / authority / lease location / WaitResolution / QueueCompletion / timer / publication / linearization point / locks / counters / allocation-exception / lifetime):**

| Row | Verified properties | Verdict |
| --- | --- | --- |
| P1 | typed factory outside locks; one lease minted; allocation may throw before admission; counters 0; no slot/publication | PASS |
| P2 | detached→producer_operation→ring under G+S; PREPARED discarded or none; committed; A unchanged; reconcile follow-up; no allocation/throw after entry | PASS |
| P3 | detached→producer_operation→detached; exact lease returned; closed; no slot/publication | PASS |
| P4 | detached→producer_operation→detached; would_block; try-push has no timer (PREPARED=none) | PASS |
| P5 | detached→producer_operation under G+S+P; Detached→Registered; PREPARED→ACTIVE iff timed; pending; W+1, T+1 iff timed; slot captured before link; not published; preparation may throw before link | PASS |
| P6 | producer_operation→ring under G+S+P; Registered→Woken→unlinked; ACTIVE→RETIRED; committed; W-1, T-1 iff ACTIVE, R+1 at publication; PUB-P-COMM; active-owner steal eligible | PASS |
| P7 | producer_operation→detached lease retained by op; closed; PUB-P-CLOSED; reconcile | PASS |
| P9 | detached/producer_operation retained; never enters ring; inline (A only) or registered (W-1, T-1, R+1); PUB-P-EXPIRE for registered; PREPARED discard or ACTIVE→CONSUMED | PASS |
| P10 | committed empty OR failed producer_operation→detached→released; brief G+S on resume then none; A-1, R-1 iff resumed; typed conversion; one T move/delete outside locks | PASS |
| C1 | ring→consumer_operation; source slot empty; committed; A unchanged; reconcile | PASS |
| C2 | no lease→no lease; closed; A unchanged | PASS |
| C3 | ring unchanged; would_block; no timer | PASS |
| C4 | empty consumer operation under G+S+C; PREPARED→ACTIVE iff timed; pending; W+1, T+1 iff timed; slot captured; not published | PASS |
| C5 | ring→consumer_operation; source slot empty; committed; W-1, T-1 iff ACTIVE, R+1; PUB-C-COMM | PASS |
| C6 | no lease→no lease; closed; W-1, T-1 iff ACTIVE, R+1; PUB-C-CLOSED | PASS |
| C8 | ring unchanged; consumer empty; inline (A only) or registered (W-1, T-1, R+1); PUB-C-EXPIRE | PASS |
| C9 | consumer_operation→released; brief G+S on resume then none; A-1, R-1 iff resumed; one T move/delete outside locks | PASS |
| CL1 | Open→Closed under G+S then one role; eligible waiters use P7/C5/C6; timers retire; A+1/-1 plus winner deltas; closed fixed point | PASS |
| CL2 | Closed→Closed; idempotent; A+1/-1 plus winner deltas; re-reconcile | PASS |

Each row's authority column, location-before/after, locks, WaitNode/timer
column, completion/counters column, owner-slot/publication/steal column,
follow-up column, and allocation/exception/lifetime column are mutually
consistent and faithful to §2/§4/§5/§6/§7/§8/§9/§11.

**Linearization points (cross-checked against §13 + state-machine §3):**
- P2 fast commit: ring insertion under G+S (before reconcile).
- P6 blocked commit: producer_operation→ring before PUB-P-COMM (winner-before-publication, mutex_handoff_one_locked precedent).
- C1 fast pop: ring→consumer_operation under G+S.
- C5 blocked pop: ring→consumer_operation before PUB-C-COMM.
- P3/P4/P9 failed push: observes Closed/full/already-due under G+S; lease retained.
- C2/C3/C8 failed pop: observes Closed+empty/empty/already-due under G+S.
- CL1/CL2 close: Open→Closed under G+S, monotonic.

**Verdict: PASS — 19/19 canonical transitions independently verified.**

### D.10 Topic 10 — 6 publication transitions

**Design claims:** winner finalized; payload/slot/closed/expired completion
done; ticket handled; runnable publication last; no throwing repair step
after publication.

**Independent row-by-row review of §14:**

| Row | Predecessor | Location/uniqueness | Locks/Wait/timer | Completion/counters | Owner slot/publication/steal | Verdict |
| --- | --- | --- | --- | --- | --- | --- |
| PUB-P-COMM | P6 winner after unlink | ring owns unique lease; op empty | G+S+P; Woken/off; retired-or-none | committed; R+1 | captured slot; ticket appended; active owner never vetoes steal | PASS |
| PUB-P-CLOSED | P7 winner after unlink | op retains unique lease | G+S+P; Woken/off; retired-or-none | closed; R+1 | captured slot; active-victim steal allowed | PASS |
| PUB-P-EXPIRE | registered P9 winner | op retains unique lease | G+S+P; Expired/off; consumed | expired; R+1 | captured slot; active-victim steal allowed | PASS |
| PUB-C-COMM | C5 winner after unlink | consumer op owns unique lease, ring source empty | G+S+C; Woken/off; retired-or-none | committed; R+1 | captured slot; active-victim steal allowed | PASS |
| PUB-C-CLOSED | C6 winner after unlink | no lease | G+S+C; Woken/off; retired-or-none | closed; R+1 | captured slot; active-victim steal allowed | PASS |
| PUB-C-EXPIRE | registered C8 winner | ring unchanged, op empty | G+S+C; Expired/off; consumed | expired; R+1 | captured slot; active-victim steal allowed | PASS |

§14 line 925-926: "No publication row performs an owner-map lookup/update,
chooses an execution Worker, destroys payload, allocates, or throws." Each
row's "no allocation/throw; op live" column is consistent with §11's binding
empty sets ("allocation after winner CAS: NONE; recoverable exception after
winner CAS: NONE; payload destruction under Queue/Scheduler locks: NONE;
owner-map insertion/erase/lookup during publication or steal: NONE").

**Ordering invariants:**
- Winner CAS → unlink → timer retire/consume → lease/control location transfer → completion write → counter update → `make_runnable` → ticket append (§12 line 862-874). The runnable publication (`make_runnable` + ticket append) is the LAST step; no throwing repair step follows it.
- The `R+1` (granted_not_resumed_) increment happens at publication; the `R-1` decrement happens at ticket removal + resume + owner-slot release (§7 row 4, §13 P10/C9). The pairing is closed.

**Verdict: PASS — 6/6 publication transitions independently verified.**

### D.11 Topic 11 — 33 counterexamples (each dispositioned)

See §E below for the per-counterexample disposition table. Summary:

```text
BLOCKED BY TYPE STRUCTURE         : 12  (#1, #2, #3, #4, #5, #6, #7, #8, #9,
                                         #25, #30, #31)
BLOCKED BY ACCESS CONTROL         :  4  (#4 also, #15 maps to type structure,
                                         #25 also, #32 — 见下方表格)
BLOCKED BY LOCK ORDER             :  1  (#10)
BLOCKED BY STATE MACHINE          :  6  (#18, #21, #22, #26, #33, #28)
BLOCKED BY TESTABLE RUNTIME STRUC :  9  (#11, #12, #13, #14, #16, #17, #23,
                                         #24, #27)
BLOCKED BY LIFETIME/DECLARATION   :  1  (#19, with #20 grouped)
NOT BLOCKED                       :  0
OUT OF SCOPE WITH JUSTIFICATION   :  0
```

Every counterexample has a binding disposition grounded in type structure,
access control, lock order, state machine, or testable runtime structure —
not debug assertions.

---

## E. 33-counterexample disposition table

Each row independently adjudicated. "Design disposition" is the author's
stated blocking mechanism; "Independent verdict" is this reviewer's
re-derivation from type structure / substrate / probes.

| # | Scenario | Design disposition | Independent verdict | Evidence |
| ---: | --- | --- | --- | --- |
| 1 | same lease used for two `try_push` calls | first by-value move empties source; second entry rejects empty lease | **BLOCKED BY TYPE STRUCTURE** | Probe 1 (l3 case); §4.1 entry contract |
| 2 | source lease used after move | `std::exchange` empties source; entry requires non-empty | **BLOCKED BY TYPE STRUCTURE** | Probe 1; `QueueItemLease` move ctor |
| 3 | another QueuePort receives the lease | immutable `owner_port_` mismatch before location mutation | **BLOCKED BY TYPE STRUCTURE + ACCESS CONTROL** | `owner_port_` is `const QueuePort* const`; entry checks `control.owner_port_ == this` |
| 4 | raw control pointer forges a lease | constructor/adopt are private; QueuePort is the only mint authority | **BLOCKED BY ACCESS CONTROL** | Probe 2 NEG_LEASE_RAW_CONTROL_CTOR, NEG_LEASE_ADOPT |
| 5 | committed item also appears in failed result | committed iff result lease empty; ring owns the moved lease | **BLOCKED BY TYPE STRUCTURE** | §5 result invariant; P2 vs P3 location disjointness |
| 6 | one control occupies two ring slots | move-only lease + empty-destination transfer empties source | **BLOCKED BY TYPE STRUCTURE** | Probe 1; §4.2 ring representation |
| 7 | failed push does not return lease | every non-committed opaque status requires one non-empty lease | **BLOCKED BY TYPE STRUCTURE** | §5 result invariant (lines 401-404) |
| 8 | failed push returns an alias | result receives the original moved lease; no raw reconstruction API | **BLOCKED BY TYPE STRUCTURE + ACCESS CONTROL** | Probe 2 NEG_LEASE_RAW_CONTROL_CTOR; §5 typed failure path |
| 9 | pop leaves source ring slot occupied | ring-to-operation move empties the source before commit | **BLOCKED BY TYPE STRUCTURE** | Probe 1; C1/C5 location transition |
| 10 | teardown races ordinary admission | G+S serializes lifecycle check/CallGuard entry against irreversible transition | **BLOCKED BY LOCK ORDER** | §6/§9 preconditions; both acquire G+S |
| 11 | active owner W0 blocks W1 steal | owner activity is not an eligibility predicate; W1 may take global oldest | **BLOCKED BY TESTABLE RUNTIME STRUCTURE** | Production `try_steal` (`scheduler.cpp:2652-2708`) does not read `victim->active` |
| 12 | current Worker has own ticket and older foreign ticket | own-oldest rule intentionally wins before global-oldest | **BLOCKED BY TESTABLE RUNTIME STRUCTURE** | §10 `pop_queue_runnable_locked` lines 736-751 |
| 13 | no own ticket, foreign global oldest exists | global head is selected regardless of admission-owner activity | **BLOCKED BY TESTABLE RUNTIME STRUCTURE** | §10 lines 742-751 |
| 14 | stealing erases owner entry | no-erase interval is a binding lifetime invariant | **BLOCKED BY TESTABLE RUNTIME STRUCTURE + LIFETIME** | §9 lines 701-708; production never erases today |
| 15 | stealing inserts into owner map | ticket stores mapped-value address; steal writes it directly under G | **BLOCKED BY TYPE STRUCTURE** | §10 line 749 `*stolen->fiber_owner_slot_ = &current`; no `operator[]` |
| 16 | `run_next_on` before ticket unlink | claim/unlink/accounting/owner update all precede run under G | **BLOCKED BY TESTABLE RUNTIME STRUCTURE** | §10 lines 760-762; mirrors `mutex_handoff_one_locked` ordering |
| 17 | active old owner rejects every foreign claim | activity check is deleted from eligibility | **BLOCKED BY TESTABLE RUNTIME STRUCTURE** | §10 line 758-759; production `try_steal` confirms |
| 18 | termination true while Queue ticket exists | ticket count participates in classification; publication clears termination | **BLOCKED BY STATE MACHINE** | §10 line 770; production `route_runnable_locked` (`scheduler.cpp:912`) clears `global_terminate_` |
| 19 | PREPARED guard outlives G guard | mandatory declaration order destroys prepared first | **BLOCKED BY LIFETIME/DECLARATION** | Probe 4 (reverse destruction order); §8 line 612-623 |
| 20 | two armed guards after move | move exchanges `armed_` to false in source | **BLOCKED BY TYPE STRUCTURE** | Probe 4 (move disarms source) |
| 21 | immediate commit leaves PREPARED | explicit discard or armed destructor erases same list element | **BLOCKED BY STATE MACHINE** | §8 discard path; P2/P3 Locks column "PREPARED discarded or none" |
| 22 | immediate Closed leaves PREPARED | same discard/RAII path before return | **BLOCKED BY STATE MACHINE** | §8; P3/C2 Locks column |
| 23 | activation allocates | heap capacity reserved during preparation; activation is `noexcept` | **BLOCKED BY TESTABLE RUNTIME STRUCTURE** | §8 line 675-677 `void activate_queue_timer_locked(...) noexcept` |
| 24 | pool growth invalidates address/iterator | concrete pool is `std::list`; growth probe checks address and iterator | **BLOCKED BY TESTABLE RUNTIME STRUCTURE** | Probe 4; production `scheduler.hpp:930` `std::list<TimerRegistration>` |
| 25 | generic timer starts PREPARED | distinct active-generic and prepared-Queue factories | **BLOCKED BY ACCESS CONTROL + TYPE STRUCTURE** | §8.1 lines 644-667; distinct static factories |
| 26 | discard changes active timer counter | PREPARED is uncounted; discard delta is zero | **BLOCKED BY STATE MACHINE** | §8 line 663 "not counted by active_queue_timers_"; §8 lines 687-689 |
| 27 | armed guard erases twice | discard/activation disarm exactly once; moved source is disarmed | **BLOCKED BY TESTABLE RUNTIME STRUCTURE** | Probe 4 (rollback count == 1); §8 lines 605-610 |
| 28 | begin teardown with active port call | precondition requires `active_port_calls_ == 0` under G+S | **BLOCKED BY STATE MACHINE** | §6 lines 489-498; probe 3 verifies rejection |
| 29 | snapshot after teardown | ordinary entry rejects `tearing_down` before CallGuard | **BLOCKED BY STATE MACHINE** | §6 line 505-507; probe 3 (`ordinary_call` rejected) |
| 30 | teardown session copied | copy construction/assignment are deleted | **BLOCKED BY TYPE STRUCTURE** | Probe 2 NEG_TEARDOWN_COPY; §6 lines 463-465 |
| 31 | two teardown sessions live | lifecycle is absorbing and constructor private | **BLOCKED BY TYPE STRUCTURE + ACCESS CONTROL** | Probe 2 NEG_TEARDOWN_CTOR_PUBLIC; §6 line 500 one-way transition |
| 32 | teardown drain enters CallGuard | begin/take_next are explicitly outside ordinary guard | **BLOCKED BY ACCESS CONTROL / COUNTER SCOPE** | §7 lines 543-546 explicit exclusion list; §6 `take_next() noexcept` |
| 33 | four zeros prove all typed methods returned | counter definitions explicitly exclude typed conversion/destruction and arbitrary callers | **BLOCKED BY STATE MACHINE** | §7 lines 524-556; counterexample #33 in §16 |

**Per-counterexample verdict: 33/33 BLOCKED by structure (no NOT-BLOCKED, no
OUT-OF-SCOPE).** Every disposition is grounded in a property the type system,
access control, lock order, state machine, or testable runtime structure
enforces — not in a debug assertion.

---

## F. Findings (severity-tagged)

### F.1 BLOCKING-class observations (implementation obligations, not design defects)

**F.1.1 — Forward-looking `fiber_owner_.erase` dependency (binding obligation on future implementers).**

- Location: `docs/e12-queue-scheduler-integration.md` §9 lines 711-716.
- Claim: "Fiber completion cleanup must therefore occur after the Queue operation releases the slot."
- Substrate fact: production `fiber_owner_` is NEVER erased today (`grep -n "fiber_owner_.erase" src/ include/` returns zero hits; the four writes at `scheduler.cpp:363,389,451,2694` only add/update entries).
- Consequence: the §9 invariant is currently *trivially* satisfied. If a future change adds `fiber_owner_.erase(f)` on Fiber completion (e.g. for memory hygiene or to support unbounded Fiber lifetimes), it would invalidate the captured mapped-value address used by the Queue ticket unless gated on slot release. The design *prescribes* the gating ("must therefore occur after the Queue operation releases the slot") but cannot enforce it from the design alone — the gating is an implementation obligation.
- Independent verdict: this is a CORRECTLY PRESCRIBED invariant, not a design defect. The design explicitly forbids the dangerous operation during the interval. The reviewer flags this as the single most important implementation obligation: any future `fiber_owner_.erase` MUST be coordinated with the Queue ticket-slot release protocol. Record as a binding implementation requirement when B4/production is authorized; the design authority is sound.

### F.2 MINOR — wording inconsistencies requiring clarification before spec hand-off

These are not design defects (the prose around them is unambiguous when read
in context), but they would mislead a fresh implementer. Required
clarification, not blocking.

**F.2.1 — `make_prepared_queue` declared `noexcept` but described as may-throw.**

- Location: `docs/e12-queue-scheduler-integration.md` §8 line 649-651.
- Text: `static TimerRegistration make_prepared_queue(Scheduler::deadline_t) noexcept;`
- Conflict: §8 line 683-684 says "Preparation may reserve deadline-heap capacity, emplace the list element, and throw allocation exceptions before registration." §8.1 line 632 says "`prepare_queue_timer_locked()` — may throw allocation exceptions". §11 row "timer heap reserve/list block | PREPARED preparation | G | before registration | may throw; guard rolls back".
- Likely intent: the `noexcept` on line 650 refers to the underlying `TimerRegistration` static factory that constructs the *control block*; the may-throw path is `prepare_queue_timer_locked` (line 672-673, no `noexcept`) which performs the heap-capacity reserve / list emplace. The two are distinct functions and the design prose treats them as distinct.
- Required clarification: state explicitly that `make_prepared_queue` is the *in-place constructor* (noexcept, no allocation) and `prepare_queue_timer_locked` is the *RAII wrapper factory* (may throw on heap-capacity reserve / list emplace). Or remove the inline `noexcept` if it is not load-bearing.
- Independent verdict: the underlying claim (PREPARED preparation may throw, activation/discard do not) is consistent and correct; only the declaration spelling is misleading.

**F.2.2 — `make_active_generic` / `make_prepared_queue` return `TimerRegistration` by value, but `TimerRegistration` is non-movable in production.**

- Location: `docs/e12-queue-scheduler-integration.md` §8 lines 647-651.
- Conflict: `include/sluice/async/timer_registration.hpp:87-88` deletes `TimerRegistration(TimerRegistration&&)` and `operator=(TimerRegistration&&)`. Returning by value would require either (NRVO) copy elision (forbidden by the deleted move ctor — NRVO does not require movability in C++17+, but the design has not stated NRVO is intended) or in-place construction.
- Likely intent: these are factory declarations describing logical construction into the `timer_pool_` `std::list` (in-place `emplace_back`), not by-value returns. The design says (§8 line 574) "PREPARED and ACTIVE registrations occupy the same list element" — implying list-embedded construction.
- Required clarification: state that the factories construct in-place into `timer_pool_` (returning a reference/iterator, not a value), or spell the construction as `timer_pool_.emplace_back(...)` directly.
- Independent verdict: realizable either way; the declaration as written is misleading but does not gate a defect.

**F.2.3 — §10 `pop_queue_runnable_locked` does not state what happens when the own-oldest ticket's owner is no longer the current worker.**

- Location: `docs/e12-queue-scheduler-integration.md` §10 lines 736-751.
- Observation: `find_oldest_owned_ticket_locked(current)` finds the oldest ticket whose stored owner-slot value points to `&current`. A steal may have already rewritten that slot to a different worker, so the ticket is no longer "owned" by `current`. The pseudocode returns the first ticket that still points to `current`, which is correct. But the prose does not explicitly say what `find_oldest_owned_ticket_locked` returns when the current worker has NO still-owned ticket (the answer is implicit: returns null, falls through to the global head path).
- Required clarification: spell out the null-return fall-through, and confirm the "own-oldest" preference is over tickets whose CURRENT (post-steal) owner is the current worker, not tickets whose ADMISSION owner was the current worker.
- Independent verdict: the binding semantics at §10 lines 754-766 are unambiguous and correct ("own-oldest, otherwise global-oldest; admission owner's active/inactive state never affects eligibility"). Only the helper pseudocode could be clearer.

### F.3 NOT a finding — explicitly verified sound

The following design areas were scrutinized and found sound (recorded so a
future reviewer does not re-litigate):

- **The §13 P9 "already-due inline" semantics**: §2 of the state-machine correctly specifies "Only an otherwise-blocking already-due operation expires inline" — a push_until that can fast-commit (space, Open, no older producer) commits even if the deadline is already due. The deadline is a fallback for blocked operations. This matches the E11 already-due-inline precedent.
- **The P10/C9 typed-destruction-outside-locks boundary**: `brief G+S on resume, then none` is the only lock interaction; the T move/delete is in the typed layer, outside any lock. Consistent with §11 "no payload destruction under Queue/Scheduler locks".
- **The `take_failed_lease() &&` rvalue qualifier**: ensures the failed lease can only be extracted from an rvalue result, preventing accidental double-take. Sound.
- **The `QueueItemLease` non-empty-destruction fail-fast**: the dtor contains no `delete`, no virtual dispatch — it cannot invoke user code. Sound.
- **The `AsyncQueue<T>` copy/move deletion** (§2 of the integration doc): prevents accidental queue duplication. Sound.
- **`mutex_handoff_one_locked` as the winner-before-publication precedent**: confirmed at `scheduler.cpp:2065-2106`; `owner = f` at `:2088` between resolve CAS and `make_runnable`. The Queue's PUB-* transitions follow the same shape.

---

## G. Summary

- **Substrate fidelity:** every load-bearing substrate claim (lock order, `mutex_handoff_one_locked` precedent, `TimerRegistration` non-movability, `std::list`/`std::unordered_map` stability properties, `try_steal` semantics, test-seam discipline) was independently verified against current production source.
- **Type-graph realizability:** six compile-feasibility probes (positive + 12-case negative + teardown + timer + result + owner-slot) all PASS. The design's claimed C++ declaration graph is realizable; all forbidden operations fail to compile; all asserted runtime invariants hold.
- **11/11 required topics independently adjudicated PASS** (one with a forward-looking implementation obligation, three with MINOR wording clarifications).
- **19/19 canonical + 6/6 publication transitions** independently recounted and row-by-row reviewed; linearization points are well-defined.
- **33/33 counterexamples** dispositioned; every one is BLOCKED by type structure, access control, lock order, state machine, or testable runtime structure — not by debug assertions.
- **No design defect blocks the Corrective-2 design authority.** The single BLOCKING-class observation is a binding implementation obligation (the §9 `fiber_owner_.erase` gating), which the design correctly prescribes; it gates implementation, not the design under review. The three MINOR wording items are clarifications, not corrections.

The design authority under review (`E12-E-QUEUE-SCHEDULER-INTEGRATION-DESIGN-CORRECTIVE-2`, `E12-E-QUEUE-STATE-MACHINE-DESIGN-CORRECTIVE-2`, and the Queue-relevant sections of `docs/e12-queue.md` and `docs/e12-sync-primitives-plan.md` §8) is independently verified sound. Implementation remains denied pending B4 (the Queue TLA+ formal model).

```text
E12-E-QUEUE-CORRECTIVE-2-INDEPENDENT-ADVERSARIAL-REVIEW-1: PASS

  11/11 required topics independently adjudicated PASS
  19/19 canonical transitions independently verified
   6/6 publication transitions independently verified
  33/33 counterexamples dispositioned (all BLOCKED by structure)
   6/6 compile-feasibility probes green (positive + 12-case negative +
       teardown + PREPARED timer + opaque result + owner-slot)

  BLOCKING findings:  0  (the §9 fiber_owner_.erase obligation is a
                          correctly-prescribed implementation requirement,
                          not a design defect)
  MAJOR findings:     0
  MINOR findings:     3  (F.2.1/F.2.2/F.2.3 — wording clarifications for
                          spec hand-off; do not block the design authority)

  B2 (Corrective-2 independent adversarial review): CLOSED — PASS
  B4 (Queue TLA+ formal model):                    remains OPEN (correctly)
  E12-E IMPLEMENTATION AUTHORIZATION:              remains DENIED — B4 OPEN
```
