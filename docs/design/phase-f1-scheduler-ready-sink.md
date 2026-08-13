# Phase F1 — Scheduler consumes identity-bearing reap (focused design)

**Status:** Design (implementation target for Issue #98 F1)
**Date:** 2026-08-13
**Authority:** ADR-explicit-io-request-contract (Accepted) Decisions 9/10;
AGENTS.md §4.1/§4.3/§4.4, §10, §13.1, §13.2; Architecture Constitution AC-13,
AC-14, AC-15.
**Scope:** Scheduler wait registration + wake routing; a production
Scheduler-owned ReadySink; the affected poll/wait_one sink wiring; production
waiter cancellation. F2 (Batch origin) and F3 (public RequestHandle) are NOT
implemented here.

---

## 1. Baseline as-built (source-traced)

### 1.1 The completion-progress path today (the gap)

```text
Fiber:
  RuntimeTaskContext::await_completion(c)                       application_runtime.cpp:60-71
    -> Scheduler::await_completion_size/void(c)                 scheduler.cpp:1241-1304
         { G: waiting_size_/waiting_void_[&c] = {fiber, owner}; // Completion*-keyed map
           if (c.ready()) erase + return;
           commit_suspend_locked; }                             // suspend

Worker (all four drains under G):                               worker_loop sites 678/742/806/854
  wake_ready_completions_locked()                               scheduler.cpp:1083-1119
    { G: ctx_.poll();                                           // G -> access_mtx_
      for each waiting_size_/waiting_void_ entry:
        if (c->ready()) { erase; make_runnable(f); route_runnable_locked(f, owner); } }
```

- `waiting_size_` / `waiting_void_` are `std::unordered_map<void*, WaitReg>`
  keyed by `&Completion` (scheduler.hpp:1195-1198). The wake path re-checks
  `c->ready()` on **every** registered waiter per drain — the O(N)
  `Completion::ready()` re-scan.
- The backend half of identity-bearing reap is complete: every backend reaps
  through `RequestArena::reap(SynchronousReadySink&)` and the arena invokes the
  sink with a by-value `ReadyEvent{RequestKey, OperationKind,
  OptionalWaiterDelivery}` **after** releasing the leaf slot-lifecycle domain
  (request_arena.hpp:441-506). All four production backends hold a stateless
  no-op `detail::ReferenceReadySink sink_` member (fake_backend.hpp:721,
  sync_backend.hpp:328, threadpool_backend.hpp:662, uring_backend.hpp:670) and
  call `arena_.reap(sink_)`.
- `RequestArena::register_waiter` / `cancel_waiter` (request_arena.hpp:588-626)
  are production arena API, but the only reachable callers today are
  `SLUICE_ASYNC_INTERNAL_TESTING` `*_for_test` seams on the backends.
  `WaiterToken{scheduler_identity, registration_slot, registration_generation}`
  and the move-only `RoutingLease` are "Phase B fake" per their headers
  (ready_sink.hpp:47-85); F1 makes them real Scheduler routing-record handles.

### 1.2 Scheduler wait inventory

| Wait kind | Registration | Wake path | F1 disposition |
|---|---|---|---|
| Completion wait (`Scheduler::await_completion_size/void`) | `waiting_size_`/`waiting_void_` (Completion\*-keyed) | scan `c->ready()` after `ctx_.poll()` | **Replaced by identity routing (this design)** |
| Ready-flag wait (`await_ready_flag`, `waiting_ready_`) | flag-address-keyed map | `wake_ready_flags_locked` | Unchanged (Future/EventedWaitPolicy are not arena requests; no RequestKey exists) |
| E10 WaitQueue waits (`waiting_waitq_count_`) | count | `wake_wait_one`/`cancel_wait` | Unchanged |
| E13 Select (`waiting_select_count_`) | count | `select_publish_locked` | Unchanged |

The 2ms MIXED-WAKE backstop (scheduler.cpp:1008-1009) is unchanged (removal is
Phase G).

---

## 2. Target architecture

```text
Backend worker / cancel winner / CQE handler
  -> record_terminal / terminal_won            (backend-ready, arena leaf)
  -> ready-ring push
  -> RequestArena::reap(sink)                  (reap path; Scheduler-owned sink installed)
        leaf: pin-ack check; registration close; token/lease extraction;
              terminal install; Completion-ready release-store  (I18)
        after leaf release: sink.on_ready(ReadyEvent{key, kind, waiter})   [exactly once]
  -> SchedulerReadySink (no Scheduler lock held by the arena)
        { wait_registry_mtx_: token -> WaitRecord; registered -> delivered;
          link into delivered list; consume lease }
  -> drain (under G, same worker_loop sites as today)
        ctx_.poll()            [sink marks records; no deadlock: sink takes only the leaf]
        pop delivered list; per record: make_runnable (E7-T2) + route_runnable_locked
  -> Fiber runnable exactly once on its owning worker

Waiter cancellation (production caller: RuntimeTaskContext::cancel_waiter):
  -> Scheduler::cancel_waiter(c) -> AsyncIoContext::cancel_waiter(c) -> arena.cancel_waiter(handle)
        lease won: { G: registry: registered -> cancelled; wake fiber exactly once
                     with wait-cancelled outcome (E10 cancel_wait precedent) }
        lease lost (reap won): nothing; the drain routes the delivery
```

The Scheduler keeps its role as the **sole Fiber-routing authority**: the sink
only marks records under a leaf domain; every `make_runnable` +
`route_runnable_locked` publication happens under `global_mtx_` in the drain,
reusing the existing E7-T2 exactly-once guard and the existing owner routing.

---

## 3. Authority model

| Authority | Owner |
|---|---|
| Request lifecycle / terminal winner | RequestArena (leaf slot-lifecycle domain) |
| Completion-ready publication | RequestArena reap (slot binding thunk, inside the leaf) |
| Waiter registration (single-waiter, provenance, generation) | RequestArena (leaf) |
| Scheduler wait record creation/retirement | Scheduler (wait registry, leaf) |
| Delivery token/lease extraction (reap vs cancel_waiter exactly-once) | RequestArena (leaf) |
| Identity route decision (registered -> delivered vs cancelled) | Scheduler wait registry (leaf) |
| Fiber runnable transition + route-to-worker | Scheduler (`global_mtx_` + `route_runnable_locked`) |
| Runnable queue insertion | Scheduler (WorkerState inbox, `global_mtx_ -> inbox_mtx`) |
| Waiter interest (token/lease pin lifetime) | Scheduler wait record (lease ack closes it) |

The RequestSlot never routes a Fiber; the Scheduler never touches a RequestSlot
after reap leaves the leaf (I16); the sink never holds a `Completion*` or
`RequestSlot*`.

---

## 4. New / changed types

### 4.1 `Scheduler::WaitRecord` (new, private)

```cpp
enum class WaitRecordState : std::uint8_t { free, registered, delivered, cancelled };

struct WaitRecord {
    std::uint32_t generation = 0;      // bumped on reuse before a new occupant is visible
    WaitRecordState state = WaitRecordState::free;
    Fiber* fiber = nullptr;            // caller-owned; valid while the fiber is suspended
    WorkerState* owner = nullptr;      // address-stable across runs (ensure_workers_locked)
    CompletionBase const* completion = nullptr;  // diagnostics / cancel identity
    WaitRecord* next_delivered = nullptr;        // intrusive delivered-list linkage
};
```

- Storage: `std::vector<std::unique_ptr<WaitRecord>>` + a free list of indices.
  Records are address-stable for the Scheduler lifetime; a freed record is
  reused with `generation++` (I6-style ABA guard for stale tokens).
- Growth: one allocation per newly concurrent waiter, on the registration path
  (pre-wait allocation — the existing `unordered_map` insert already allocated
  there; **no allocation is added to the accepted -> terminal -> reap -> route
  path** — see §8).

### 4.2 `WaiterToken` (ready_sink.hpp:52-58) — now real

`WaiterToken{scheduler_identity, registration_slot, registration_generation}`
already has the exact (SchedulerIdentity, RegistrationSlot, RegistrationGeneration)
shape the ADR Decision 10 requires. F1 fills it with real values:
`registration_slot` = record index, `registration_generation` = record
generation, `scheduler_identity` = per-Scheduler construction-time value. The
header comment is updated from "Phase B fake" to the current truth.

### 4.3 `RoutingLease` (ready_sink.hpp:64-85) — extended with the record pin

Additive change (the header already promises Phase F pinning):

```cpp
// New: the lease pins the Scheduler routing record it was created for.
static RoutingLease pinning(std::uint64_t id, std::uint32_t record_index,
                            std::uint32_t record_generation) noexcept;
std::uint32_t record_index() const noexcept;
std::uint32_t record_generation() const noexcept;
```

The arena treats the lease opaquely (stores/moves it); only the Scheduler
creates and reads the pin. The C2c transfer-chain tests construct leases via
`RoutingLease{id}` — that constructor remains.

### 4.4 `Scheduler::ReadyRoutingSink` (new, private nested)

```cpp
class ReadyRoutingSink final : public detail::SynchronousReadySink {
    Scheduler* scheduler_;
public:
    void on_ready(detail::ReadyEvent event) noexcept override;
};
```

`on_ready`: if `!event.waiter.has_waiter` -> return (no routing work; the
delivery has no token). Otherwise, under `wait_registry_mtx_` only:

1. look up `wait_records_[token.registration_slot]`; reject if out of range,
   `record->generation != token.registration_generation`, or
   `record->scheduler_identity != this Scheduler` — stale/cross-Scheduler
   tokens are dropped (lease destroyed = acknowledged) with no routing;
2. if `state == registered` -> `state = delivered`, link into the delivered
   list (intrusive `next_delivered`); the lease is destroyed under the same
   lock (acknowledged); a delivered record is never reused until the drain
   consumes it;
3. if `state == cancelled` (cancel_waiter won) -> nothing; the lease is
   destroyed (this is the loser path — the cancel path already routed).

`on_ready` acquires **no Scheduler lock and no backend-progress lock**; it
runs during `ctx_.poll()` / `ctx_.wait_one()` with `access_mtx_` held and
never blocks on `global_mtx_` (see §5).

### 4.5 Sink wiring (backends + context)

- `AsyncBackend` gains a non-virtual narrow setter mirroring `attach_stats`
  (async_io_context.hpp:142): `void attach_ready_sink(detail::SynchronousReadySink* s)`
  and a protected member `detail::SynchronousReadySink* routing_sink_ = nullptr;`.
- Each backend keeps its `ReferenceReadySink sink_` member as the no-op
  fallback and changes its reap call to
  `arena_.reap(routing_sink_ ? *routing_sink_ : sink_)` (4 call sites:
  fake_backend.hpp:590, sync_backend.hpp:286, threadpool_backend.cpp:703/716/731,
  uring_backend.cpp:1415/1449/1503). No backend reap/terminal/waiter authority
  changes.
- `AsyncIoContext` gains `set_ready_sink(detail::SynchronousReadySink* sink)`
  (serialized under `access_mtx_`), forwarding to the backend. The Scheduler
  installs `&ready_sink_` in its constructor and detaches (nullptr) in its
  destructor (Scheduler outlives any poll because ApplicationRuntime destroys
  `sched_` before `io_ctx_`; standalone Scheduler tests follow the same
  construction/destruction order).
- Standalone contexts (no Scheduler) keep the no-op sink: behavior unchanged.

### 4.6 Registration plumbing (backends + context)

`AsyncBackend` gains four source-compatible virtuals (precedent:
`wait_source()`, `wait_one_is_nonblocking()`), defaulting to `not_supported`:

```cpp
virtual Result<void> register_waiter(Completion<std::size_t>& c,
    detail::WaiterToken token, detail::RoutingLease lease);
virtual Result<void> register_waiter(Completion<void>& c, ...);
virtual Result<detail::RoutingLease> cancel_waiter(Completion<std::size_t>& c);
virtual Result<detail::RoutingLease> cancel_waiter(Completion<void>& c);
```

Each RequestArena-backed backend implements them by resolving the Completion
through the existing production `RequestArena::resolve_completion`
(request_arena.hpp:892) and forwarding verbatim to `arena_.register_waiter` /
`arena_.cancel_waiter` — the exact code the `*_for_test` seams already run,
promoted to production. Unresolvable Completion -> `invalid_state`
(provenance misuse, Decision 6 vocabulary). `AsyncIoContext` exposes
`register_waiter` / `cancel_waiter` (per type, under `access_mtx_`) forwarding
to the backend.

### 4.7 Scheduler primitive changes

- `await_completion_size` / `await_completion_void` return `Result<void>`
  (additive return channel; existing callers ignoring the Result still
  compile) and register through the arena instead of the Completion\*-keyed
  maps.
- New production `Scheduler::cancel_waiter(Completion&) -> Result<bool>`
  (mirrors the E10 `cancel_wait` family): true = this call removed the waiter;
  false = the delivery already won.
- `RuntimeTaskContext::await_completion` returns `Result<void>`; new
  `RuntimeTaskContext::cancel_waiter(Completion&) -> Result<bool>` — the
  production waiter-cancel caller (Issue #98 F1 item 4). Public API changes
  are documented in `docs/api-reference.md` and the compliance gate.

---

## 5. Lock protocol (AGENTS.md §13.1)

### 5.1 Lock inventory

| Lock | Domain | Guards |
|---|---|---|
| `global_mtx_` (G) | Scheduler coordination | wait registry membership ops, runnable routing, admission, `fiber_owner_`, `pending_spawn_` |
| `wait_registry_mtx_` (R, **new leaf**) | Scheduler wait registry | `wait_records_`, free list, delivered list, per-record state/fiber/owner |
| `access_mtx_` (A) | AsyncIoContext | backend serialization (submit/poll/wait_one/cancel/register/cancel_waiter) |
| arena leaf (L) | RequestArena | slot lifecycle, registration close, token extraction, ready-ring |
| `inbox_mtx` (I) | WorkerState | local runnable queue |
| `wake_mtx_` (W) | wake epoch | `wake_epoch_` + `wake_cv_` |

### 5.2 Allowed lock order (every edge that exists)

```text
G -> A        classify_locked: ctx_.outstanding()          (unchanged, scheduler.cpp:1228)
G -> A -> L   drain: ctx_.poll() -> backend poll -> arena.reap
G -> A        register_waiter/cancel_waiter via the context (new; same order as submit)
G -> R        registration, drain delivered-pop, cancel_waiter record retirement
G -> I        route_runnable_locked / classify_locked
G -> W        signal_wake_locked
A -> R        ReadyRoutingSink::on_ready (runs inside poll/wait_one)
A -> L        backend poll/reap, register_waiter, cancel_waiter
L -> (release) -> sink.on_ready                            (arena contract; never L -> R nested)
R -> (release) -> I / W                                    (the sink and drain never hold R
                                                             while acquiring I or W)
```

Forbidden / structurally absent:

- `R -> G`, `R -> A`: the sink holds R only while marking; it never acquires
  G or A. The drain acquires R only under G and releases it before
  `route_runnable_locked`.
- `L -> R`: the arena releases L **before** invoking the sink (arena contract,
  request_arena.hpp:426-428), so no path holds L and then R.
- `I -> G`, `I -> W`, `W -> G`: unchanged (documented inbox/wake discipline).

**Cycle argument:** G and A are the only "upper" domains; R and L are leaves
with two independent inbound edges each (G and A for R; G and A for L) and no
outbound edges. I and W are leaves below G. No directed cycle exists.

### 5.3 Why the sink cannot take G (and does not need to)

Today the drain calls `ctx_.poll()` **under G** (scheduler.cpp:1086) and
`classify_locked` calls `ctx_.outstanding()` **under G** (scheduler.cpp:1228).
If `on_ready` acquired G, a worker holding G in either path would deadlock
against a concurrent poll whose sink waits for G (`G -> A` vs `A -> G`). The
record-marking design keeps the sink on the R leaf, preserving both existing
G -> A edges unchanged — the MW-S2/MW-S3 classification keeps its accurate
`ctx_.outstanding()` under G, and the worker_loop drain structure is
unchanged. All routing authority stays in the G-protected drain.

### 5.4 Registration lock protocol (await_completion, under G)

```text
{G:
   R: acquire record (free-list pop or grow), generation++,
      state=registered, fiber/owner/completion stored
   (release R)
   A: backend->register_waiter(c, token, lease)     // L inside the backend
   outcome:
     success            -> commit_suspend_locked; suspend
     invalid_state && c.ready() -> retire record (R); return success inline
     invalid_state (else)      -> retire record (R); return invalid_state
                                    (duplicate waiter / provenance)
     not_found                 -> retire record (R); return invalid_state
                                    (completion not bound to this context)
}
```

Race A (completion/reap wins while registering) is closed by the arena leaf:
registration and reap's token-extraction serialize on L; a reaped request
rejects registration with `invalid_state` (registration closed), the
`c.ready()` recheck observes the I18 release-store and the fiber returns
without suspending. This is the arena-proven `register_waiter` vs `reap` race
(C2c `arena_register_waiter_vs_reap_race`), now on the production Scheduler
path.

### 5.5 Drain lock protocol (unchanged call sites, under G)

```text
{G:
   ctx_.poll();                       // sink marks records on R (no G)
   R: pop delivered list; extract fiber+owner; state=consumed; free record
   (release R)
   per record: if (fiber->make_runnable()) route_runnable_locked(fiber, owner);
               // E7-T2 exactly-once; then R: (nothing — record already freed)
   wake_ready_flags_locked(); pump_deadlines_locked(); classify_locked();
}
```

Work per drain is O(delivered), not O(registered) — the O(N) scan is removed.

### 5.6 Waiter-cancel lock protocol (Scheduler::cancel_waiter, under G)

```text
{G:
   A: backend->cancel_waiter(c) -> arena.cancel_waiter(handle)   // L inside
   Result<RoutingLease>:
     lease won:
       R: record by lease pin (record_index/generation);
          state=registered -> state=cancelled; extract fiber+owner
       (release R)
       if (fiber->make_runnable()) route_runnable_locked(fiber, owner);
       // await_completion resumes and returns IoError::canceled
       return true
     not_found (reap won):
       return false   // the delivery is already routed; nothing to remove
}
```

Race B (cancel_waiter vs reap) resolves exactly-once on the arena leaf (the
lease races: cancel XOR reap — C2c `arena_cancel_waiter_vs_reap_race`). The
Scheduler mirrors it on R: the record is retired exactly once (cancelled by
the cancel path, or delivered+consumed by the sink/drain). The fiber is made
runnable at most once by whichever path wins; the losing path observes the
winner's state and does nothing.

**Interpretation note (Issue #98 "cancel wins -> no Fiber wake"):** the issue's
Race B dichotomy refers to the **identity route** (reap -> ReadySink -> drain):
a cancel winner means no delivery exists, so the ReadySink route produces no
wake. The waiting fiber is nevertheless resumed by the cancel path itself with
the wait-cancelled outcome — exactly the E10 `cancel_wait` precedent
(scheduler.cpp:1422-1444: "Resolve `node` with Cancelled and route the
winner's fiber") and ADR Decision 10 ("routes the cancellation outcome"). A
suspended fiber must never be abandoned (liveness); the I/O, the borrow, and
the Completion are untouched (I5).

---

## 6. Race analyses

### 6.1 Race A — completion/reap wins while waiter is registering

Arena-leaf serialization (see §5.4). Three timings:

- reap **before** registration: registration observes `registration_closed`;
  `register_waiter` returns `invalid_state`; `c.ready()` (I18 acquire) is
  true; the fiber returns without suspending — no lost wake.
- reap **during** registration: impossible to observe a half state — L
  serializes the two critical sections.
- registration **before** reap: the token/lease are in the slot; reap closes
  registration, moves the delivery into the ReadyEvent, publishes ready, then
  invokes the sink; the drain routes the fiber exactly once.

### 6.2 Race B — cancel_waiter vs reap

See §5.6. Outcomes (both legal, exactly one):

- cancel wins: lease moved out; `state=registered -> cancelled`; the fiber is
  woken once with `canceled`; the I/O continues; the later reap delivers
  `has_waiter=false`; the sink does nothing.
- reap wins: `cancel_waiter` returns `not_found`; the record was marked
  delivered by the sink; the drain routes the fiber once; the cancel path
  touches nothing. No stale token route, no UAF (record address-stable and
  generation-guarded), no double route (E7-T2 `make_runnable` gate + R state).

### 6.3 Race C — slot generation reuse (stale event for an old request)

The arena delivers a token only from the **current** slot generation (the
leaf extracts `s.waiter_token_` at reap; release bumps the slot generation
before reuse). A stale event cannot exist at the arena level (C2c
`arena_lease_transfer_chain_reap_path`). Defense-in-depth on the Scheduler
side: the sink validates `record->generation == token.registration_generation`
and the record's `scheduler_identity` under R; a reused record (generation
bumped at registration) rejects the stale token and drops the lease. A new
occupant is never woken by an old request's event (I6).

### 6.4 Race D — Fiber teardown / shutdown

- Fibers are caller-owned raw pointers (scheduler.hpp:294-296); a suspended
  fiber cannot be destroyed while suspended: Group destruction fails fast if
  any Evented-task Future is still pending (group.cpp:110-119), and the fiber
  only leaves `waiting` via a routed runnable ticket. The record's `fiber`
  pointer is therefore valid from registration until the drain/cancel consumes
  the record (which happens before the fiber can be reaped as `done`).
- The sink never touches `Fiber*`; it only marks records. All fiber-pointer
  dereferences happen under G (drain/cancel) where the registration-owner
  invariant holds.
- Shutdown: sequencing unchanged (ApplicationRuntime destroys `group_` ->
  `sched_` -> `io_ctx_`). `~Scheduler` detaches the sink from the context
  (nullptr) and asserts the wait registry is empty (all records free). A
  control-plane interrupt wakes any parked backend wait; the driver re-entry
  loop drains remaining reaps; every registered waiter is delivered or
  cancelled before quiescent destruction (AC-4 + Group fail-fast).

### 6.5 Duplicate route protection

- One delivery per registration: the arena moves the token/lease out of the
  slot exactly once (single-waiter, I13; registration closed at reap). A
  second `ReadyEvent` for the same record is impossible.
- `make_runnable` is the E7-T2 exactly-once publication gate; the drain's
  record loop only processes records in `delivered` state, each popped from
  the delivered list exactly once.
- Duplicate waiter registration on the Scheduler path: the arena rejects the
  second `register_waiter` with `invalid_state` without overwriting the first
  (I13); the second fiber's `await_completion` returns `invalid_state`
  synchronously (F1 acceptance criterion 2).

---

## 7. Wake obligations (AGENTS.md §13.2)

| Persistent state | Producer | Sleeping consumer | Predicate | Commit-to-sleep closure | Worst-case latency |
|---|---|---|---|---|---|
| record `registered` (R) | registration (G) | the fiber, suspended in `await_completion` | delivery arrives | arena leaf serializes registration vs reap extraction; sink marks under R; drain routes under G; `make_runnable` before route | bounded by backend progress + drain |
| record `delivered` (R) | sink (A -> R) | the fiber | drain pop | the drain runs `ctx_.poll()` before checking the delivered list in the same G scope — a delivery produced by this poll is drained in this pass | one drain pass |
| runnable ticket (I) | drain (G -> I) | the owning worker | `local_runnable` non-empty | `route_runnable_locked` pushes + notifies `inbox_cv` and signals the wake source | one worker wake |
| backend ready epoch (backend) | terminal winner | MW-S2 participant in `ctx_.wait_one()` | ready epoch advanced | split-phase snapshot -> poll -> `wait_for_change(observed)` (D4-RM13/14) | one epoch advance |
| cancel outcome | cancel_waiter (G) | the fiber | record `cancelled` | cancel path routes the fiber under G | one worker wake |

The 2ms MIXED-WAKE backstop remains the observation-return path for backend
progress when no external wake is registered (unchanged; Phase G removes it).

---

## 8. Allocation / boundedness (AGENTS.md §12, I8, I9, AC-7)

| Path | Allocation | Classification |
|---|---|---|
| wait registration (record acquire) | **none** — the WaitRecord pool is preallocated at Scheduler construction to `wait_capacity` records (default 256); `acquire_wait_record_locked` pops from the free list or returns `nullptr` (synchronous `no_space`). No allocation on the hot path. | **pre-wait / caller-path** — bounded by explicit capacity, AC-7. Pool exhaustion returns a synchronous reportable result. |
| accepted -> terminal -> reap -> route | none | the sink marks under R (no alloc); the drain pops + routes (no alloc); the delivered list is intrusive through the records themselves (no separate container) |
| `register_waiter`/`cancel_waiter` | none (arena leaf) | unchanged |
| backend poll/wait_one | none (arena ready-ring, pre-reserved) | unchanged |

The delivered list cannot overflow: every record is delivered at most once
(single lease), the list is drained on every drain pass under G, and the
linkage is the record's own `next_delivered` field. No new long-lived
container grows by cumulative historical submissions.

---

## 9. What is deleted and what is retained as fallback

- `Scheduler::wake_ready_completions_locked` (scheduler.cpp) — the O(N)
  `Completion::ready()` re-scan. The drain call sites remain but call the
  identity drain.
- The four backends' no-op-only sink usage in production Scheduler runs
  (the `ReferenceReadySink` remains as the default for standalone contexts).
- `register_waiter`/`cancel_waiter` `*_for_test` seams on the backends become
  thin wrappers over the new production methods (they stay for arena-level
  tests but are no longer the only path).

Retained as fallback (non-arena backends returning `not_supported`):
- `waiting_size_` / `waiting_void_` maps and `WaitReg` — the
  Completion\*-keyed wait identity for backends without RequestArena waiter
  machinery. These maps are the exclusive fallback; each registration takes
  exactly ONE path (identity registry or legacy map, never both). The drain
  scans the legacy maps after the identity drain.

Kept: `waiting_ready_` flag waits, E10 WaitQueue waits, Select waits, the 2ms
backstop, `classify_locked`'s `ctx_.outstanding()` under G, the MW-S2
two-phase admission, D4-RM13/14 control-wake protocol.

---

## 10. Test plan (RED-first)

| # | Case (name TBD) | Proves | Determinism |
|---|---|---|---|
| T1 | production Scheduler routes via ReadySink (identity path), with a diagnostic counter distinguishing identity route vs legacy scan | F1 target shape; the sink's deliveries counter (internal-testing) increments; `waiting_count()` no longer includes Completion waits | phase seams around registration/suspend |
| T2 | completion ready before waiter registration -> await returns inline, no suspend, no lost wake | Race A | deterministic backend completion ordering |
| T3 | waiter registered before completion -> fiber resumes exactly once | exactly-once routing | E7-T2 assertion + sink counter |
| T4 | cancel_waiter before completion -> fiber resumes with `canceled`; I/O still terminal + reap; Completion publishes; sink delivers `has_waiter=false` | I5 (wait-cancel independence) | Fake/ThreadPool deterministic |
| T5 | cancel_waiter racing reap (high iteration) -> exactly one of cancel/delivery; no double route, no UAF | Race B | barrier/phase seams, 100x-500x loop |
| T6 | stale RequestKey generation -> old event cannot wake a new registration | Race C | arena generation reuse + scheduler record reuse |
| T7 | duplicate waiter registration -> synchronous `invalid_state`, first waiter intact | I13 on the Scheduler path | deterministic second await |
| T8 | duplicate reap / double ReadyEvent -> no double wake | I16/I4 | second reap delivers nothing |
| T9 | shutdown convergence: control wake + pending delivery + registry empty at destruction | Race D / shutdown | D4-RM13/14 seams |
| T10 | all four backends (Fake/Sync/ThreadPool/Uring) route through the same contract | backend-agnostic conformance | per-backend registration |

Existing suites re-run: full Clang Debug, backend conformance, negative
compile, TSan, ASan/UBSan, real-liburing suite.

---

## 11. Out of scope (F2/F3 and beyond)

- F2 Batch origin flag, F3 public RequestHandle: NOT implemented.
- No change to backend reap/terminal-winner/publication authority.
- No change to the synchronous public Reader/Writer contract.
- No `select()` redesign, no Future rework, no wake-bridge (Phase G), no 2ms
  backstop removal.
- ThreadPoolBackend Scheme-B watchdog (#101) is untouched; any F1 test finding
  against it is recorded separately.
