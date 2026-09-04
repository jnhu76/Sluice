# Async Explicit-I/O Request Lifecycle — As-Built Walkthrough

**Authority:** narrative companion to the governing contracts. This document does
not create rules; it maps where each already-governing rule lives in the code so
a reader can follow ONE request from creation to reclamation without reverse-
engineering eight files. Baseline: master, 2026-08-20 (issue #139). Code anchors
are FILE + SYMBOL (grep-stable); line numbers are cited only for enum
definitions, which move rarely.

**Governing contracts (in authority order):**

- [AGENTS.md §3.2](../../AGENTS.md) — explicit request lifecycle invariants
- [ADR-explicit-io-request-contract](../adr/ADR-explicit-io-request-contract.md)
  (Accepted) — the five-stage submission transaction, identity, capacity, cancel
- [ADR-explicit-io-completion-authority](../adr/ADR-explicit-io-completion-authority.md)
  (Accepted) — Completion binding/publication authority
- [as-built-async-architecture](../history/closeout/as-built-async-architecture.md) — component
  topology, Scheduler progress model, shutdown paths
- [architecture-constitution](architecture-constitution.md) — AC-3 (transactional
  submission), AC-14 (provenance/generation)

> **What this document is NOT:** a proposal to merge authority domains. The
> four-domain separation (slot-lifecycle / backend-progress / Completion /
> Scheduler) is intentional and correct (issue #139 core principle). This
> document exists because the *knowledge* of one request's story is spread
> across files; the *authority* is not moved by documenting it.

---

## 1. The state machines involved

Three separate state machines participate in every explicit-I/O request. They
are NEVER merged; a request's story is the interleaving of all three, tied
together by the RequestKey and the publication binding.

### 1.1 RequestState — the slot lifecycle (authority: RequestArena)

`include/sluice/async/detail/request_slot.hpp` (`enum class RequestState`,
lines 48-57 at baseline):

```text
free → reserved → prepared → pending → enqueued → running
                                  │                   │
                                  │        (cancel wins before dispatch)
                                  │                   │
                                  └──────→ backend_ready ←────────────┘
                                             │
                                             │
                                             │ reap — progress drivers only
                                             │ (poll()/wait_one(); a Scheduler
                                             │  drain reaches the SAME reap
                                             │  through ctx_.poll())
                                             ▼
                                      completion_ready
                                             │
                                             │ caller reset()/destroy
                                             ▼
                             free (generation++ BEFORE slot is visible)
```

Every transition is validated under the single arena leaf mutex
(`RequestArena::mutex_`); an illegal transition fails fast rather than
best-effort recovering. `pending` is the state between commit and enqueue —
it exists so that pending-cancel and enqueue can arbitrate under one state
authority (AGENTS.md §3.2).

### 1.2 Completion::State — the caller-owned publication object

`include/sluice/async/completion.hpp` (`enum class State`, lines 422-425 /
631-634 at baseline — size and void twins):

```text
idle → binding → outstanding → publishing → ready → resetting → idle
        ↑          (submit-success LP is idle→…→outstanding)
        └ rollback (binding→idle, pre-acceptance only)
```

The `backend-only` capabilities are `idle→binding`, `binding→outstanding`,
`binding→idle` (rollback). The `caller-only` capabilities are `ready→resetting`
and `resetting→idle`. Reset or destruction in any other state is a checked
contract violation (fail-fast in Debug AND Release).

### 1.3 WaiterRegistration — waiter handoff state (authority: slot)

`request_slot.hpp` (`enum class WaiterRegistration`): `open_no_waiter →
open_registered → closed`. The slot closes registration at reap; exactly-once
waiter delivery is arbitrated by the same leaf mutex that arbitrates reap vs
`register_waiter` vs `cancel_waiter`.

---

## 2. Full lifecycle diagram (annotated)

Legend: **[T]** logical state transition (arena-validated) · **[O]** ownership
transfer only (no state change) · **[C]** cross-thread observable publication
(release-store) · **[F]** failure entry point (terminal or rollback).

```text
CALLER                 BACKEND (submission)            SLOT-LIFECYCLE DOMAIN      BACKEND (execution)
──────                 ────────────────────            ────────────────────       ───────────────────
Completion idle
     │
     │  submit_*(op, c)
     ▼
[AsyncIoContext::submit_*  ─ holds access_mtx_ only; NO claim authority]
     │
     ▼
backend submit (under backend admission discipline¹)
     │
     ├─ arena_.reserve() ──────────────► [T] free → reserved          [F] full → would_block
     │                                                            [F] closed → invalid_state
     ├─ validate_op() (real backends)      [F] malformed descriptor → invalid_argument
     │                                    (rollback: slot only; Completion still idle)
     ├─ arena_.prepare() ────────────────► [T] reserved → prepared
     ├─ (backend scratch write: PreparedBlockingOp / PreparedUringOp)
     ├─ arena_.install_publication_binding()   (publish thunk + requested bytes live IN the slot)
     ├─ c.begin_binding() ──────────────► [T] idle → binding (CAS)   [F] loser → slot rollback only
     ├─ arena_.commit() ────────────────► [T] prepared → pending
     │                                    (pin set; accepted_outstanding++; borrow begins)
     ├─ c.install_binding() + c.commit_binding()
     │                                    [C] binding → outstanding = SUBMIT-SUCCESS
     │                                        LINEARIZATION POINT — after this, submit
     │                                        MUST NOT return a rejection
     │
     │  (admission lock released)
     ▼
enqueue_after_commit (noexcept, allocation-free)
     ├─ arena_.enqueue() ───────────────► [T] pending → enqueued      (pin acknowledged = FINAL
     │                                                                slot access by submit)
     │                                    [T] backend_ready observed → terminal_noop (Scheme B:
     │                                        pending-cancel already won; enqueue no-ops)
     └─ dispatch-ring push + wake (backend domain)                    [F] injected dispatch failure
                                                                        (test-only) → backend_error
                                                                        terminal AFTER commit
     │
     ▼  [O] queue pop + mark_running = ONE coordinated transfer under the
     │      backend work/dispatch lock (no visible pop-before-running gap)
arena_.mark_running() ───────────────────► [T] enqueued → running      (or back off: cancel won)
     │
     ▼  [O] execution ownership: syscall runs with NO lock held
     │      (blocking pread/pwrite/fsync; or kernel-owned SQE for io_uring)
     │                                        [F] syscall error → terminal VERBATIM
     │                                        [F] confirmed interruption → canceled terminal
     ▼
arena_.record_terminal() ────────────────► [T] → backend_ready        (terminal winner = first
     │                                        valid transition; losers never overwrite)
     │                                    [C] ready-ring push (arena mutex only)
     │
     ▼  signal_ready_progress (backend wait-source; outside arena domain)
     │
poll()/wait_one() driver (AsyncIoContext caller or Scheduler drain)
     │
arena_.reap(sink) ──────────────────────► [T] backend_ready → completion_ready
     │                                    (per slot, under ONE arena-mutex acquisition:
     │                                     binding validation → pin acknowledgement check →
     │                                     registration closed → waiter delivery extracted →
     │                                     borrow ends → accounting decremented)
     │                                    [C] Completion publish_from_reap: outstanding →
     │                                        publishing → ready  (THE ONLY publication under
     │                                        the arena mutex; the release-store is the LP)
     ▼
sink.on_ready(ReadyEvent{key, kind, waiter delivery})   ← invoked OUTSIDE the arena mutex
     │  (Scheduler: validate identity → mark record delivered under registry leaf)
     ▼
Scheduler drain → route_runnable_locked → fiber runnable → worker resumes caller
     │
     ▼
caller: c.result()  (terminal value) ── then ── c.reset()
     │
     ▼
Completion::reset() ────────────────────► [T] ready → resetting → idle
     │                                    slot: release_completed_binding → free_slot_locked_
     │                                    [T] completion_ready → free; generation++ BEFORE the
     │                                        slot is visible to a new reserve
     ▼
slot reusable by a LATER request (stale keys can never address it)
```

¹ Backend admission disciplines differ by design (see §5): SyncBackend relies on
the caller-side `access_mtx_` serialization; FakeAsyncBackend and
ThreadPoolBackend hold `admission_mtx_`; UringAsyncBackend serializes
submission under `dispatch_mtx_` with an additional Stage-0 poison/admission
check.

### 2.1 How each failure enters the terminal path

| Failure | Where it lands | Completion state | Caller sees |
|---|---|---|---|
| Arena full / admission closed (`reserve`) | synchronous submit error | stays `idle` | `would_block` / `invalid_state` |
| Malformed descriptor (Stage 1.5) | synchronous submit error after slot rollback | stays `idle` | `invalid_argument` |
| Binding-CAS loser (`begin_binding`) | synchronous submit error after slot rollback | stays `idle` (loser never touches the winner's slot) | `invalid_state` |
| Commit-stage arena failure | `rollback_binding_before_accept` FIRST, then slot rollback | `binding → idle` | synchronous error |
| Anything after `commit_binding` | NEVER a submit rejection — terminal path only | `outstanding` | terminal result via reap |
| Cancellation winning at pending/enqueued | canceled terminal → `backend_ready`; enqueue no-ops (Scheme B); reap defers until pin acknowledged | `outstanding` | `canceled` terminal |
| Running-syscall failure | terminal VERBATIM (cancel intent never rewrites it) | `outstanding` | real error code |
| Shutdown (close_admission) | only gates NEW acceptance (`reserve`); accepted requests must still be reaped and reset before destruction | — | quiescent-destruction contract (AGENTS.md §3.7) |

### 2.2 Ownership transfers that are NOT state transitions

Documented to prevent "why didn't the state change" confusion:

- **Dispatch-queue push/pop** — intrusive ring linkage inside the backend work
  domain (`ThreadPoolBackend::BoundedDispatchQueue::push_back` / `pop_front`,
  `src/async/threadpool_backend.cpp`). The logical transitions are
  `pending→enqueued` (arena) and `enqueued→running` (arena); the pop→running
  pair is one coordinated ownership transfer so cancel cannot terminalize a
  popped-but-not-running request.
- **Ready-ring push/pop** (`RequestArena::push_ready_locked_` /
  `pop_ready_front_locked_`) — linkage only; the logical transitions are
  `→backend_ready` and `→completion_ready`.
- **Prepared-op scratch copy** (worker copies `PreparedBlockingOp` after
  `mark_running`) — submit-thread → worker data handoff made safe by the same
  work-lock critical section as `mark_running`.
- **RoutingLease movement** — slot → by-value `ReadyEvent` → sink → drain;
  acknowledgement is lease destruction (`Scheduler::ReadyRoutingSink::on_ready`
  scope exit in `src/async/scheduler.cpp`).

---

## 3. Authority table

Who owns what. Different concerns deliberately belong to different authorities;
**do not merge them** (issue #139 explicit prohibition).

| Concern | Authority | Code location | Invariant |
|---|---|---|---|
| Request identity (`RequestKey = ctx+slot+generation`) | RequestSlot / RequestArena | `include/sluice/async/detail/request_key.hpp`, `include/sluice/async/detail/request_slot.hpp` | Generation increments before slot reuse; stale key cannot cancel/dispatch/complete/wait/mutate a later occupant |
| Accepted vs not-accepted | Arena `commit` + Completion `commit_binding` (dual halves of one LP) | `include/sluice/async/detail/request_arena.hpp` `RequestArena::commit`, `completion.hpp` `commit_binding_to_outstanding` | Before LP: complete rollback, Completion idle, no borrow/accounting. After LP: never a rejection |
| Backend submission (stages 1–3c) | The concrete backend, under its admission discipline | `include/sluice/async/sync_backend.hpp` (`submit_size`/`submit_void`), `include/sluice/async/fake_backend.hpp` (same), `src/async/threadpool_backend.cpp` (`submit_size`/`submit_void`), `src/async/uring_backend.cpp` (same) | reserve→validate→prepare→binding→commit ordering; rollback ladder before acceptance (ADR Decision 5) |
| Execution ownership (worker/ring/SQE) | The concrete backend progress mechanism | `threadpool_backend.cpp` `worker_loop`/`run_syscall`, `uring_backend.cpp` (SQ/CQ) | Workers may publish ONLY `backend_ready`; never a Completion |
| Terminal-result publication (first winner) | RequestArena terminal-winner arbitration | `request_arena.hpp` `record_terminal` / `cancel` | State validated BEFORE terminal write; losers no-op; exactly one terminal per generation |
| Enqueue/cancel arbitration (pending window) | Arena state authority under leaf mutex | `request_arena.hpp` `enqueue`, `cancel` | `pending→enqueued` vs `pending→backend_ready(canceled)` compete under one authority; enqueue pin is submit's FINAL slot access |
| Completion-ready publication | Reap ONLY, through the slot's publication binding | `request_arena.hpp` `reap`, `completion.hpp` `publish_from_reap` | Workers/CQE handlers/cancel/Scheduler never publish directly; the ready release-store under the arena mutex is the LP |
| Runnable routing / wake | Scheduler (registry + canonical routing) | `scheduler.cpp` `ReadyRoutingSink::on_ready`, `drain_routed_completion_waits_locked`, `route_runnable_locked` | Sink invoked outside arena/backend locks with a by-value event; Scheduler never chooses terminal results |
| Waiter registration/delivery | Slot registration state (arena leaf) | `request_arena.hpp` `register_waiter`/`cancel_waiter`, `request_slot.hpp` `WaiterRegistration` | Exactly-once delivery; reap closes registration; cancel_waiter removes only the waiter, never the I/O |
| Cancellation semantics (layered) | Task: CancelToken · wait: Scheduler · I/O op: arena cancel + backend interlock | `threadpool_backend.cpp` `ThreadPoolBackend::cancel`, `request_arena.hpp` `cancel` | Running cancel records intent only; ordinary results win verbatim; kernel-owned races resolve through the same slot winner |
| Completion consumption | Caller (`result()`) | `completion.hpp` public queries | Caller-owned, address-stable for the documented lifetime |
| Reclamation / reset → free | Caller reset/destruction → arena release | `completion.hpp` `reset`, `request_arena.hpp` `release_completed_binding` / `free_slot_locked_` | Release allocates nothing, waits for nothing, calls no Scheduler/user code; fail-fast on any precondition violation |

---

## 4. One concrete walkthrough (ThreadPoolBackend, normal + failure)

A real operation: **write 4 KiB to a file through the async runtime**. Every
name below is the real symbol; paths are repo-relative.

### 4.1 Normal path, API entry to completion consumed

1. **Entry.** `AsyncIoContext::submit_write(WriteOp, Completion<std::size_t>&)`
   (`src/async/async_io_context.cpp`) locks `access_mtx_`, forwards to
   `backend_->submit_write`, tallies stats. It has NO claim authority of its own.
2. **Backend submission transaction.**
   `ThreadPoolBackend::submit_write` (`src/async/threadpool_backend.cpp`)
   forwards to the shared `submit_size` template: acquires `admission_mtx_` →
   `arena_.reserve()` (arena checks admission-closed then capacity) →
   `validate_write` (fd / null-buffer / offset / `SSIZE_MAX` bounds, via
   `ThreadPoolBackend::validate_op`) → `arena_.prepare()` → writes a
   `PreparedBlockingOp` into the per-slot scratch →
   `arena_.install_publication_binding()` (the `publish_size_ready` thunk lives
   IN the slot) → `c.begin_binding()` (CAS idle→binding) → `arena_.commit()`
   (→ `pending`, pin set, `accepted_outstanding_++`, borrow active) →
   `c.install_binding()` + `c.commit_binding()` — **the submit-success
   linearization point**: the Completion becomes observably `outstanding`.
3. **Enqueue.** Admission lock released; `enqueue_after_commit(h)`
   (`noexcept`) takes `work_mtx_`, `arena_.enqueue(h)` (→ `enqueued`, pin
   acknowledged as submit's final slot access), `dispatch_.push_back(h)` in
   the SAME critical section (closes the pin-clear→ring-push gap), then
   `work_cv_.notify_one()`.
4. **Execution.** A persistent worker in `worker_loop` wakes, pops the handle,
   and `arena_.mark_running(h)` — pop + mark_running are ONE coordinated
   transfer under `work_mtx_`, so an enqueued cancel cannot strike a
   popped-but-not-running request. The worker copies the prepared op, releases
   `work_mtx_`, and runs the real blocking syscall with no lock held
   (`run_syscall`: EINTR-retried `pwrite`).
5. **Terminal.** `arena_.record_terminal(h, terminal)` — the arena leaf mutex
   ONLY (explicitly no `work_mtx_`): validates the transition, stores the
   result, → `backend_ready`, pushes the ready-ring. First valid transition
   wins; any loser no-ops. Then `signal_ready_progress()` advances the ready
   epoch and wakes a parked `wait_one` participant.
6. **Reap.** Whoever drives progress — `ThreadPoolBackend::poll()` /
   `wait_one()`, ultimately from `AsyncIoContext::poll/wait_one` or the
   Scheduler's `drain_routed_completion_waits_locked` (`ctx_.poll()`) — calls
   `arena_.reap(sink)` (`include/sluice/async/detail/request_arena.hpp`). Per slot under ONE mutex
   acquisition: binding present (else fail-fast), pin acknowledged (else defer
   at ring head), registration closed, waiter delivery extracted, borrow ends,
   → `completion_ready`, counters decremented, then the slot's publication
   binding runs `publish_size_ready` → `Completion::publish_from_reap`:
   `outstanding→publishing→ready` release-store. This is the ONLY
   Completion-ready publication path in the repository.
7. **Wake/route.** AFTER leaving the arena mutex, reap invokes
   `sink.on_ready(ReadyEvent{...})` — a by-value event, no slot/Completion
   pointers. With a Scheduler attached this is
   `Scheduler::ReadyRoutingSink::on_ready` (`src/async/scheduler.cpp`): it
   validates scheduler identity + record generation, marks the wait record
   `delivered` under the registry leaf (`wait_registry_mtx_`). The Scheduler
   drain (`drain_routed_completion_waits_locked`) pops delivered records under
   `global_mtx_`, freezes the `CompletionWaitOutcome::completed`, and routes
   the fiber exactly once via `route_runnable_locked` (inbox push + wake
   epoch). Without a Scheduler (bare context use), the sink is the stateless
   `ReferenceReadySink` and the caller observes `c.ready()` itself.
8. **Consume.** The resumed fiber runs `await_take` → `c.result()` →
   `c.reset()` (`completion.hpp`): CAS ready→resetting, calls
   `arena_.release_completed_binding(h)`, which fail-fasts on stale handle /
   live pin / open registration / wrong state, then `free_slot_locked_`:
   → `free`, **generation++ before the slot re-enters the free list**, all
   per-request fields cleared, `--slot_in_use_`. Back in `reset()`, the
   Completion clears its binding and release-stores `idle`. A later `reserve()`
   hands out the slot with the new generation; every stale key from the
   previous occupant is now `not_found`.

### 4.2 Failure path: submit rejected before acceptance

Same entry, but the arena is at capacity. `RequestArena::reserve()` returns
`would_block` (after bumping `capacity_rejections_`). `submit_size` returns it
synchronously: the Completion never left `idle`, no slot is in use, no borrow
exists, nothing was enqueued, `accepted_outstanding_` is unchanged. The caller
may retry; nothing leaks. The deeper rollback ladder (descriptor invalid after
reserve; binding-CAS loser; commit-stage failure →
`rollback_binding_before_accept` then slot rollback) all land in the same
place: **zero residue, Completion idle** (see the rollback call sites inside
`submit_size`/`submit_void`; ADR Decision 5's pre-LP rollback obligation).

### 4.3 Failure path: cancellation racing the queue

`ThreadPoolBackend::cancel` resolves the key
(`RequestArena::resolve_completion`, bounded O(capacity) slot scan) and, under
ONE `work_mtx_` hold, removes the dispatch-ring entry
(`BoundedDispatchQueue::remove_exact`) and calls `arena_.cancel(h)`:

- request still `pending`/`enqueued` and ring removal succeeded → cancel wins:
  canceled terminal stored, → `backend_ready`, ready-ring push. If the submit
  path has not yet run `enqueue()`, the slot keeps its enqueue pin, and reap
  defers it at the ring head until `arena_.enqueue()` observes `backend_ready`
  and acknowledges the pin as its final slot access (Scheme B — see the
  pin-deferral comment inside `RequestArena::reap`).
- request `running` → `intent_recorded` only. The worker's syscall finishes
  and `record_terminal` stores the REAL result verbatim; a cancel intent never
  rewrites an ordinary success or error.
- already terminal → `already_terminal` (loser no-ops).

In every case the canceled/error result reaches the caller through the SAME
reap/publication path as a success — cancellation never publishes a Completion
directly.

---

## 5. Where backends differ (and why that is correct)

The submission transaction's *shape* is shared (ADR Decision 5); its
*serialization and execution* are backend property (the ADR deliberately leaves
the admission lock and dispatch mechanism to each backend). As of this
baseline (#137 tracks the duplication itself):

| Stage | SyncBackend | FakeAsyncBackend | ThreadPoolBackend | UringAsyncBackend |
|---|---|---|---|---|
| Admission serialization | external (`access_mtx_`) | `admission_mtx_` | `admission_mtx_` | `dispatch_mtx_` (one lock for admission + dispatch) |
| Stage-0 gate | none | none | none | poison (`fatal_error_`) + local `admission_closed_` checked in-lock before reserve |
| Descriptor validation | deferred (reference divergence) | deferred | after reserve, `SSIZE_MAX` bound | after reserve, `UINT_MAX` native-length bound |
| Post-commit enqueue | immediate (no queue) | immediate (test staging) | `work_mtx_` + bounded ring + cv | `dispatch_mtx_` + ring + inline SQE submit drain |
| Execution | synthetic at poll | test-controlled | persistent workers, blocking syscalls | kernel-owned SQEs/CQEs |

These differences are authority, not drift — EXCEPT the copy-pasted
transaction core (forwarders, rollback ladder, publish thunks, waiter surface),
which is the #137 design topic. This document records the as-built split; it
does not presuppose #137's outcome.

---

## 6. Reading map (which file owns which part of the story)

| File | Owns |
|---|---|
| `include/sluice/async/detail/request_key.hpp` | request identity vocabulary (ContextIdentity/SlotIndex/Generation) |
| `include/sluice/async/detail/request_slot.hpp` | RequestState enum, per-slot record (terminal, pin, registration, binding, borrow) |
| `include/sluice/async/detail/request_arena.hpp` | every slot-state transition: reserve/prepare/commit/enqueue/mark_running/record_terminal/cancel/reap/rollback/release |
| `include/sluice/async/completion.hpp` | Completion state machine, binding mutators, `publish_from_reap`, `reset()` |
| `include/sluice/async/async_io_context.hpp` | protected binding helpers (`begin_binding`/`commit_binding`/…), backend contract |
| `src/async/async_io_context.cpp` | submit forwarding + stats, poll/wait_one drivers, waiter forwarding |
| `src/async/threadpool_backend.cpp` | bounded submission transaction + worker execution (the production portable path this document walks) |
| `src/async/uring_backend.cpp` | ring-based equivalent (Stage-0 poison, SQE/CQE, kernel-owned phase) |
| `include/sluice/async/sync_backend.hpp`, `fake_backend.hpp` | reference/test twins of the same transaction |
| `src/async/scheduler.cpp` (+ `scheduler_park_wake.cpp`) | waiter registration, `ReadyRoutingSink`, delivered-record drain, fiber routing |
| `include/sluice/async/detail/ready_sink.hpp` | the by-value event contract that decouples reap from Scheduler |

---

## 7. Common misreadings (adversarial notes)

- **Accepted ≠ completed.** A submit that returns success has committed the
  request (`outstanding`), but nothing has executed yet. `accepted_outstanding`
  counts accepted-but-not-yet-reaped requests; completion is observable only at
  `ready`.
- **Terminal publication ≠ wake.** `backend_ready` (result stored) is arena
  state; the Completion becomes `ready` only at reap; the WAITER wakes only
  after the Scheduler drain routes the fiber. Three different events with three
  different owners — a registered waiter can even be delivered before the
  submitting thread returns (legal; the terminal-winner protocol keeps it
  exactly-once).
- **Request lifetime ≠ backend operation lifetime.** The slot leaves the
  backend's world at `backend_ready`, but the REQUEST lives until the caller
  resets the ready Completion (slot → `free`, generation++). Conversely the
  Completion object is caller-owned and may outlive the slot by any margin.
- **Cancel is not synchronous revocation.** Only pending/enqueued cancels can
  win outright; a running blocking syscall is intent-only; kernel-owned
  io_uring cancellation resolves through the same terminal-winner arbitration
  as the original CQE.
- **Reset cannot precede the last consumer.** `reset()` fail-fasts unless the
  Completion is `ready` (or `idle` no-op); the reap-already-ran guarantee plus
  the by-value ReadyEvent means no sink or Scheduler path retains a pointer
  that an early reset could dangle.
- **The unhappy paths are first-class.** §2.1 tabulates every failure entry;
  the reap path is identical for success, error, and cancellation — there is no
  separate "error fast path" that skips publication.
