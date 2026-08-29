# FE-1c — Type / Identity / Delivery Seam Design

Status: ARCHIVED DESIGN REPORT (stage FE-1c; design + implementation plan;
archived with the FE campaign PR).
Governing contract: FE-1B-FRONTEND-NEUTRAL-CONTRACT-FREEZE.md (same directory).
BASE_SHA: 5706a6ddfa23d7057a29e60be414139e4698e3e5.

---

## VERDICT

**FE-1c PASS — ONE NARROW TYPE/DELIVERY SEAM EARNED**

Exactly one representation change is mechanically earned by the FE-2 PoV:

- the **ResumeTarget token seam** — `WaitNode::fiber_` /
  `register_wait_locked(node, Fiber*)` / every winner publication tail are
  `Fiber*`-typed; a stackless continuation cannot pass through the SAME
  registration, winner, and publication authorities without typing that one
  edge. FE-1a independently identified it as the single most important
  leakage ("parameterizing that one edge collapses most of the F1 list").

plus the **delivery split** it enables at the existing publication seam
(commit-under-lock / discharge-after-locks). Nothing else changes:

- ActorIdentity separation is **designed but NOT implemented** — no
  primitive touched by the FE-2 Event PoV consults an owner identity. It
  becomes mechanically earned in FE-3 (RwLock writer ownership). Implemented
  only then.
- The suspension-commit seam needs **no production signature change**:
  `commit_suspend_locked` remains the stackful eligibility commit; the
  deferred frontend commits eligibility in its own record under the same
  critical section (contract L7), reached through the shared admission
  closure (below).
- No second admission ladder: the Event admission closure is refactored so
  both frontends execute the SAME textual ladder, differing only in the
  token they bind and the eligibility-commit tail.

---

## §10 — COMPLETE F1 SITE INVENTORY (current tree, mechanically enumerated)

Legend: AI = semantic role is ActorIdentity; RT = ResumeTarget;
FM = Fiber mechanism (physical stack/worker machinery); seam = does the FE-2+ PoV
need this site re-typed; stay = can remain Fiber-only without blocking a second
frontend.

| Site (file:line, current) | Current type | Semantic role | AI | RT | FM | Seam needed? | Can stay Fiber-only? |
|---|---|---|---|---|---|---|---|
| `WaitNode::fiber_` (wait_node.hpp:252) | `Fiber*` | admission-bound continuation token | no | YES | no | **YES — the token seam** | no |
| `WaitNode::register_(q, Fiber*)` (wait_node.hpp:218) | `Fiber*` | binds token at epoch establishment | no | YES | no | **YES** | no |
| `WaitQueue::register_wait_locked(WaitNode&, Fiber*)` (wait_queue.hpp:185) | `Fiber*` | same edge, queue layer | no | YES | no | **YES** | no |
| Winner tails: `won->fiber()` → publication (park_wake.cpp:1210/1264; event:210; queue:434/467/503; timer:186/291; mutex:258/286; rwlock:120/199/426/461; condition:258; semaphore:253) | `Fiber*` | ResumeTarget consumption at publication | no | YES | delivery | **YES — switch on token kind** | no |
| `publish_waiting_fiber_runnable_locked(Fiber*)` (scheduler.hpp:1309, scheduler.cpp:1642) | `Fiber*` | fiber-kind publication (owner lookup + exactly-once + route) | no | YES | YES | fiber branch keeps this verbatim | fiber branch: yes |
| `route_runnable_locked(Fiber*, WorkerState*)` (scheduler.hpp:1293, scheduler.cpp:1559) | `Fiber*` | worker delivery topology | no | no | YES | no — fiber-kind only | yes |
| `commit_suspend_locked(WorkerState*, Fiber*)` (scheduler.hpp:1404, scheduler.cpp:1360) | `Fiber*` | stackful eligibility commit (L7 representation) | no | no | YES | no — stays the fiber-kind tail | yes |
| `g_worker` / `ws->current` capture at every admit seam (event:242/327; timer:102; park_wake:1152; sem:81/159; cond:49/133; rwlock:227/305/325/397/487/585; queue admit fns; select.cpp:819) | TLS | token SOURCE for the fiber frontend | no | (source) | YES | no — the fiber entry keeps capturing; the deferred entry supplies its own token | yes |
| `AsyncMutex::owner_` + `Fiber*& owner` params (scheduler.hpp:655-746; scheduler_mutex.cpp:47/82/165/328) | `Fiber*` | **ActorIdentity** (recursive detection, handoff commit) | YES | no | no | not for FE-2 (Event has no owner) | **yes until FE-3** |
| RwLock `writer_owner` `Fiber*&` family (scheduler.hpp:986-1033, 1344) | `Fiber*` | ActorIdentity (writer ownership) | YES | no | no | not for FE-2 | **yes until FE-3** |
| `fiber_owner_` map (scheduler.hpp:1560) + `owner_for_fiber_locked` (scheduler.cpp:1631) | `unordered_map<Fiber*, WorkerState*>` | fiber-kind owner routing input | no | no | YES | no | yes |
| `WaitReg{Fiber*, WorkerState*}` (scheduler.hpp:1177-1180) | `Fiber*` | ready-flag/legacy registry token | no | YES | YES | no — legacy registries are fiber-entry-only (FE-2 does not touch ready-flag waits) | yes |
| `WaitRecord{Fiber* fiber, WorkerState* owner, ...}` (scheduler.hpp:1203-1212) | `Fiber*` | Completion-wait routing record | no | YES | YES | no — Completion waits stay a fiber-frontend surface (public API deferred) | yes |
| `waiting_ready_` / `waiting_size_` / `waiting_void_` (scheduler.hpp:1487-1489) | maps to `WaitReg` | legacy flag-wait registries | no | YES | YES | no | yes |
| `QueueWaitCtx` stack result storage (`&lease`/`&out`, queue_detail.hpp:21, scheduler_queue.cpp:60/139/214/326) | raw stack pointers | per-epoch result storage (L2 binding) | no | no | storage-location | no for FE-2; FE-3 Queue slice moves ctx+lease into the coroutine frame (same rule, different storage) | yes for FE-2 |
| `RwWaitCtx{mode}` (scheduler_rwlock.cpp:33) | trivial struct | mode payload | no | no | no | no | yes |
| DST seam `Run(F1)` typing (test layer, PR #241) | `Fiber*` | EM observation of runnable tickets | no | no | YES | no — test infrastructure, low pressure (FE-1a DST connection) | yes |

Inventory conclusion: exactly TWO edges must change for FE-2 — the token
(WaitNode + register + winner tails) and the delivery split behind
publication. Every other site either serves only the fiber entry (EM) or is
deferred to the FE-3 slice that first exercises it.

---

## §11 — TYPE STRATEGY EVALUATION

Strategies: **A** tagged POD token; **B** compact pair of opaque ids/pointers;
**C** frontend-specific record behind opaque handle; **D** template
WaitNode/WaitQueue; **E** virtual interface; **F** std::function.

| Criterion | A tagged POD | B compact pair | C handle→record | D templates | E virtual | F std::function |
|---|---|---|---|---|---|---|
| per-wait allocation | none | none | record is frame-embedded → none | none | none | per-op alloc risk |
| branch cost | one kind switch | one kind switch | pointer deref + vtable-free switch | none (monomorphized) | vtable per publication | indirection + possible alloc |
| indirection | 0 | 0 | 1 | 0 | 1 | 1+ |
| ABI/layout impact | WaitNode +8B (ptr+tag) | WaitNode +8B | WaitNode +8B | splits types per frontend: queue/node types multiply | header-wide vtable | header-wide |
| copyability/equality | trivial | trivial | trivial handle; record owned elsewhere | trivial | non-copyable | non-copyable |
| lifetime risk | none (data) | none | record must outlive epoch — SAME rule as WaitNode itself (L2), enforced by frame discipline | none | sink lifetime | capture lifetime traps |
| testability | plain | plain | plain | ok | mock-heavy | ok |
| stackful hot-path cost | one predictable load+cmp | same | deref | zero | vcall | worst |
| semantic clarity | explicit kind = the two known mechanisms | same but looser | opaque: cannot switch without registry | both frontends real, but type explosion in installed headers | forbidden shape (framework) | forbidden shape |
| future frontend compat | add a kind | add a kind | add a record kind | new instantiation | new subclass | any (too weak) |

Decision: **A (tagged POD token), concretely `WaitResume {void* ptr; Kind kind;}`** —
A and B are the same shape at this size; A names the kinds, which is the
whole point (the publication tail must switch on mechanism). Rejected:
D (type explosion across WaitQueue + all Scheduler seams for zero semantic
gain — the queue is token-agnostic already); E/F (non-goals; also AGENTS.md
§12 forbids per-op callable records); C (adds a dereference layer without
adding information — the record address IS the token payload).

Shape (no new installed header needed — lives in wait_node.hpp next to its
only consumer):

```cpp
// ResumeTarget role (FE-1b L11). Plain data: no behavior, no ownership, no
// allocation. The winner publication tail switches on kind() to select the
// delivery mechanism; the Core never dereferences ptr for semantic decisions.
class WaitResume {
public:
    enum class Kind : std::uint8_t { none = 0, fiber = 1, deferred = 2 };
    static const WaitResume kNone;
    static WaitResume fiber(Fiber* f) noexcept;          // stackful frontend
    static WaitResume deferred(void* delivery_record) noexcept; // stackless frontend
    Kind kind() const noexcept;
    Fiber* as_fiber() const noexcept;      // pre: kind()==fiber
    void*  as_deferred() const noexcept;   // pre: kind()==deferred
private:
    void* ptr_ = nullptr;
    Kind  kind_ = Kind::none;
};
```

- `deferred` payload = address of the frontend's continuation record
  (FE-2: the awaiter's delivery record, frame-embedded, subject to the SAME
  L2 address-stability rule as the WaitNode itself).
- Layout cost: WaitNode grows 8→16 bytes for the token field. Explicitly
  accepted and documented (AGENTS.md §15 discipline: documented, not silent).
- Stackful hot-path delta: winner tails change `Fiber* f = won->fiber()` to
  `const WaitResume& r = won->resume()` + kind switch; the fiber branch is
  the existing code path. One extra predictable compare on a cache line the
  tail already touches. No new atomic, no new lock, no allocation, no virtual.

---

## §12 — ACTOR IDENTITY DESIGN (designed now; implemented at FE-3 when earned)

Minimum semantics: equality-comparable plain token; stable for one logical
execution actor across the ownership decisions that consult it; no behavior;
no ownership of continuation execution.

Consumers census (answers §12's questions from source):
- Mutex: YES — recursive detection `owner == me` (scheduler_mutex.cpp:47),
  non-owner unlock assert (:328), handoff commit `owner = winner`
  (:286 region). Recursive detection compares the token; the winner's token
  is read from its WaitNode (token-as-AI coincidence today).
- RwLock: YES — `writer_owner` comparisons (scheduler.hpp:986-1033).
- Condition: NO direct comparison; it inherits Mutex choreography.
- Queue: NO — no owner concept.
- Must identity survive suspension? For Mutex/RwLock: the OWNER identity
  must remain comparable while the owner is suspended elsewhere — yes, it is
  a stable token, not a stack address.
- Can two coroutine frames share one ActorIdentity? YES — that is the point
  (a logical actor may run one frame at a time across sequential
  suspensions, or an actor object may serialize its own frames). The test
  FE-3 constructs (same AI, different RT) relies on this.

FE-3 shape (deferred until earned): `ActorId {void* ptr; Kind kind}` with
`Kind {none, fiber, actor}` where `actor` = address of a stable per-logical-
actor record supplied by the frontend. Mutex/RwLock owner fields and the
`me` source of comparisons are re-typed. NOT `std::coroutine_handle` (never
hard-code the handle as identity — a handle is a ResumeTarget).

## §13 — RESUME TARGET DESIGN (implemented at FE-2)

`WaitResume` (above). Answers to §13's questions:
- One pointer + kind tag is enough: delivery needs {mechanism, target}.
- Frontend tag: YES — it IS the switch (§14/§15), no registry needed.
- Scheduler inspects it only at the publication tail (switch) and the drain
  (raw record addresses); no semantic decision uses the payload.
- Adapter record: enough — the deferred payload is the record address.
- Publication path switch-based, no virtual calls: YES (kind switch).
- No general callback object: none introduced.

## §14 — PUBLICATION GUARD DESIGN (implemented at FE-2)

The semantic guard lives in the **frontend continuation record** (candidate
2), NOT in a new Core state machine:

```text
record state: unarmed ──arm(inside admission CS, under G)──> armed
              armed ──try_consume(acq_rel CAS, at drain)──> consumed
```

- `arm` is the PublicationEligibilityCommit (L7): executed inside the SAME
  resolver-excluded admission critical section, after authorization. For the
  fiber frontend the same role is played by `make_waiting()`; no change.
- `try_consume` grants the right to resume exactly once. It is SUBORDINATE to
  the terminal winner: the node's `resolve_` CAS has already decided the
  outcome before any tail runs; the record CAS never decides Woken /
  Cancelled / Expired — it dedups delivery of an already-decided terminal.
  No second terminal winner is created (checked against FE-1b L4).
- A `try_consume` on an unarmed record is a contract violation (fail-fast in
  the PoV): it would mean publication-before-eligibility (L8 violated).
- The fiber-kind guard home remains `FiberState` (created/waiting → runnable,
  fiber.cpp:6-22) — unchanged, same role, different frontend.

## §15 — NO-REENTRANT-RESUME CONTRACT (delivery split; implemented at FE-2)

Split (names indicative; the seams are `defer_publication_locked` /
`drain_deferred_publications`):

1. **Commit under lock** — a winner tail, under G, for a `deferred` token:
   move the record address onto the Scheduler-owned transient transit list
   (`deferred_publications_`, `SLUICE_GUARDED_BY(global_mtx_)`). No user
   code runs. Record must be `armed` (§14), checked by the record's own
   consume CAS at discharge time.
2. **Discharge after locks** — after the authoritative critical section that
   created the obligation has released, the drain takes a bounded chunk of
   records under G (move-out, no allocation), releases G, and for each
   record `try_consume()`s it (acq_rel CAS; exactly once) and — on success —
   resumes the continuation. The resume call happens with NO Scheduler,
   WaitQueue, port, or role lock held.

Lost-wake closure: the list is persistent state under G; a record pushed by
a winner is visible to the next chunk-take regardless of thread timing
(the commit-to-drain race is closed by G; the discharge races nothing —
exactly-once comes from the record CAS). Chunked loop termination: entries
are added only by winner tails; a resume() that re-enters a primitive can
push new entries; the loop drains until a chunk-take returns empty.

Reentrancy proof obligation for tests (§22): a resumed coroutine that
immediately re-acquires an authoritative lock must never execute while the
resolver CS is still held — the test re-enters G from the resumed body and
must observe the drain's G release first (deterministic phase assertion,
not sleep).

Teardown extension (L13): Scheduler/primitive teardown requires
`deferred_publications_` empty (no published-but-unconsumed winners).

---

## IMPLEMENTATION PLAN (FE-2, minimal production change)

Commit 2 (refactor, behavior-preserving for stackful):
1. `WaitResume` in wait_node.hpp; `WaitNode::fiber_` → `resume_`;
   `fiber()` → `resume()` (+ `as_fiber()` at existing tails).
2. `register_wait_locked(WaitNode&, WaitResume)`; wait_node `register_`
   binds the token (still CAS-then-bind under the authoritative CS — L2).
3. All winner tails: kind switch; fiber branch unchanged behavior.
4. Scheduler: `deferred_publications_` + `defer_publication_locked` +
   chunked `drain_deferred_publications(void**, n)` + teardown gate.
5. Event admission closure (`await_event_wait` / `..._deadline`) refactored
   into shared `_locked` ladders parameterized by {WaitResume, kind,
   Fiber* / WorkerState*}; fiber entry passes g_worker-captured values;
   eligibility tail: fiber → `commit_suspend_locked`, deferred → record
   `arm()`. Zero textual duplication of the ladder.

Commit 3 (test PoV, internal-testing only):
- tiny coroutine task + `EventWaitAwaiter` + delivery record (frame-embedded,
  {state: unarmed/armed/consumed, std::coroutine_handle<>, outcome slot});
  `await_suspend` runs the shared ladder through the seam access header,
  returns false on inline resolution (L6), else arms + returns true.
- Drain driver: takes chunks, try_consume + resume outside locks.
- Tests: §21 race matrix A–F, §22 no-user-code-under-lock witness,
  §23 mutation sensitivity.

Non-goals honored: no Task library (the PoV task is test-local), no promise
hierarchy, no executor, no second timer/cancel/admission/resource authority,
no public API.

---

## FE-1c EXIT CHECK

- New allocation per wait: NONE (token is 16 bytes inside the node; record is
  frame-embedded; transit list transient, drained per resolver call).
- New virtual dispatch: NONE.
- New atomic: NONE in the Core (record CAS is frontend-record state,
  FE-2 test-owned; fiber path untouched).
- New lock: NONE (transit list lives under existing G).
- Stackful hot-path delta: one kind compare per winner tail; admission
  closure identical instructions after inlining of the kind tail.
