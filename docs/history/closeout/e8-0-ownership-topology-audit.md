# E8-0 — Ownership Topology Audit (sluice-CORE-E8)

Mandatory pre-implementation audit of the E7 Scheduler's Fiber-execution-
ownership topology, performed before any E8 production change (per the E8
spec §2). Every answer below is backed by production evidence in
`src/async/scheduler.cpp`, `include/sluice/async/scheduler.hpp`, and
`src/async/fiber.cpp` / `include/sluice/async/fiber.hpp`.

References use `file:line` and are clickable.

---

## O1 — Where is Fiber ownership currently stored?

**Storage class: a `WorkerState*` tag carried inside the wait registration
only. There is NO `Fiber::owner` field and NO scheduler-side `Fiber* ->
Worker*` map.**

Production evidence:

- `include/sluice/async/scheduler.hpp:104-107` — `struct WaitReg { Fiber* fiber; WorkerState* owner; }`.
  The owner is captured *at wait-registration time* and lives only inside
  `WaitReg`.
- `include/sluice/async/scheduler.hpp:163-165` — the three wait maps
  (`waiting_size_`, `waiting_void_`, `waiting_ready_`) are keyed by the
  waitable identity and store `WaitReg`; that is the ONLY place `owner`
  is recorded.
- `include/sluice/async/fiber.hpp:114-127` — the `Fiber` fields are
  `state_`, `entry_`, `token_`, `cstate_`, `ctx`. No owner field. The ADR
  explicitly permits this: `docs/adr/ADR-execution-model.md:413-415`
  ("E7 does **not** require a public `Fiber::owner_worker` field").

So ownership is **implicit outside of a wait registration**: while a Fiber
is `runnable`/`running`, its owner is implied by which `WorkerState`'s
`local_runnable` it sits in / which `WorkerState::current` points at it.
While it is `waiting`, the owner is the `WaitReg.owner` captured at suspend
time. There is no single global owner record.

This matches the E7 audit expectation: ownership is recovered from the
ticket location / wait registration, not from a Fiber field.

---

## O2 — At what exact current action is initial ownership established?

**Initial ownership is established by `Scheduler::spawn`, which places the
first runnable ticket into a specific `WorkerState::local_runnable`.**

Production evidence:

- `src/async/scheduler.cpp:58-76` — `spawn`:
  - `fiber.make_runnable()` (the `created -> runnable` PUBLISH transition,
    exactly-once — `src/async/fiber.cpp:6-22`).
  - If workers exist, `target = next_spawn_worker_++ % workers_.size()`
    and `workers_[target]->local_runnable.push_back(&fiber)` under
    `inbox_mtx`. **The choice of `target` is the owner-establishing action.**
  - If no workers exist yet (pre-`run`), the ticket goes to
    `pending_spawn_`, and ownership is established at `run`-time distribute:
    `src/async/scheduler.cpp:112-123` — `workers_[w % worker_count]->local_runnable.push_back(f)`.
      Note that no `owner` value is written at spawn time — because there is
  no owner field. Ownership is *defined by* which `local_runnable` the
  ticket occupies. The owner becomes observable as a `WorkerState*` only
  when a wait registration is later created (O3).

---

## O3 — Which production functions read Fiber ownership?

Only **one** read site materializes the owner as a value, and it reads it
from `WaitReg.owner` (not from the Fiber):

- `wake_ready_completions_locked` — `src/async/scheduler.cpp:421` and `:438`:
  `WorkerState* owner = it->second.owner;` then `route_runnable_locked(f, owner)`.
- `wake_ready_flags_locked` — `src/async/scheduler.cpp:456`:
  `WorkerState* owner = it->second.owner;` then `route_runnable_locked(f, owner)`.

These are the *only* places a `WorkerState*` is recovered from ownership.
Everything else infers ownership structurally:

- `worker_loop` pops from `ws->local_runnable` (`src/async/scheduler.cpp:186-191`)
  — implicitly "this worker owns what's in its local queue."
- `run_next_on(ws, fiber)` (`:389-400`) sets `ws->current = fiber` — the
  running fiber is owned by `ws` by construction.
- `await_completion_*` / `await_ready_flag` capture `ws = g_worker`
  (`:523`, `:536`, `:551`) — the *current* worker becomes the `WaitReg.owner`.

**Implication for E8:** there is no `Fiber::owner` getter to call. E8 must
either (a) introduce an owner record readable on the runnable/running path,
or (b) keep ownership recoverable only from ticket location + wait reg.
Model B (ownership transfer) requires the wake path to read a *current*
owner, so E8 will need an explicit owner record that the steal updates.

---

## O4 — Which production functions route runnable work using ownership?

- `route_runnable(Fiber*, WorkerState*)` — `src/async/scheduler.cpp:402-411`:
  if `owner`, push to `owner->local_runnable` under `inbox_mtx` and notify;
  else push to `pending_spawn_`.
- `route_runnable_locked(Fiber*, WorkerState*)` — `:469-492`: same, plus
  demotes a pending MW-S2 admission candidate (`:481-484`) and clears
  `global_terminate_`/`idle_workers_` (`:473-474`). Called under
  `global_mtx_`.
- Both are invoked from the wake path with `owner = WaitReg.owner`
  (`:427`, `:442`, `:459`).

So routing is **owner-preserving by construction**: it routes to whatever
`WorkerState*` the caller supplies. E7 correctness rests on that pointer
being the *original* owning worker (it is, because `WaitReg.owner` was set
at suspend time and E7 never migrates). E8's hazard is exactly here: if a
steal changes ownership but the wake path still reads the stale
`WaitReg.owner`, routing goes to the wrong worker. That is the
stale-owner defect the negative TLA+ model must expose.

---

## O5 — Can a runnable Fiber exist in `pending_spawn_` / `owner.local_runnable` / `inbox`?

Three physical locations are *declared*; only two are *used*:

| location | declared | ever pushed? | ever popped? |
|---|---|---|---|
| `pending_spawn_` | `scheduler.hpp:169` | yes — `spawn` pre-`run` (`:74`); `route_runnable[_locked]` with null owner (`:410`, `:491`) | yes — `run` distribute (`:115-122`); `worker_loop` top (`:194-197`) |
| `WorkerState::local_runnable` | `scheduler.hpp:41` | yes — `spawn` post-`run` (`:71`); `run` distribute (`:119`); `route_runnable[_locked]` (`:406`, `:487`) | yes — `worker_loop` (`:187-190`) |
| `WorkerState::inbox` | `scheduler.hpp:47` | **NO** — grep finds zero `inbox.push_*` calls | n/a |

**Critical finding:** the `inbox` `std::deque<Fiber*>` is **dead storage**.
Every routed/transported runnable ticket is placed directly into
`local_runnable` under `inbox_mtx`. The `inbox_cv` is used purely as a
wake primitive for `local_runnable` mutations; the `inbox` deque itself
is never read or written. So in production there is effectively **one
stealable location per worker**: `WorkerState::local_runnable`.

This collapses O6 (below) considerably: there is no separate inbox vs
local ticket-transport distinction to model. E8 may treat `local_runnable`
as the single stealable ticket location and `pending_spawn_` as the
pre-`run` assignment buffer (not stealable — see O6).

Legal runnable locations:
- `pending_spawn_` — legal ONLY pre-`run` (initial assignment), and as a
  fallback when `owner == nullptr` in `route_runnable`. In a well-formed
  multi-worker run, `owner` is always non-null after the first suspend,
  so `pending_spawn_` receives no routed tickets during a run.
- `owner->local_runnable` — the canonical runnable ticket location.
- `inbox` — declared but unused; effectively a no-op transport.

---

## O6 — Is `inbox` a runnable ticket transport location or a publication location? Classify every path.

Using the E7 publication vocabulary (PUBLISH / MOVE / CONSUME):

| path | code | classification |
|---|---|---|
| `spawn` (workers exist) | `:67-72` | PUBLISH (`make_runnable` success) + MOVE (pending→target `local_runnable`) — combined under one critical section |
| `spawn` (no workers) | `:73-74` | PUBLISH (`make_runnable`) + placement into `pending_spawn_` (transport to a deferred owner) |
| `run` distribute `pending_spawn_`→`local_runnable` | `:112-123` | MOVE (transport; ticket already published) |
| `wake_ready_completions_locked` → `route_runnable_locked` | `:421,427` / `:438,442` | PUBLISH (`make_runnable` from `waiting`) + MOVE (to owner `local_runnable`) |
| `wake_ready_flags_locked` → `route_runnable_locked` | `:456,459` | PUBLISH + MOVE |
| `route_runnable` (null owner → `pending_spawn_`) | `:409-410` | MOVE (fallback transport) |
| `worker_loop` pop `local_runnable` | `:186-191` | CONSUME (ticket removed; `run_next_on` will `make_running`) |
| `worker_loop` pop `pending_spawn_` | `:193-197` | CONSUME |

`inbox` is **neither** — it is never the source or destination of any
ticket. It is a transport-location-in-name-only. The `inbox_mtx` is the
lock guarding `local_runnable`; the `inbox_cv` is the wake for
`local_runnable` mutation. The naming is historical (E7-A's plan was to
use `inbox` as a separate cross-worker transport; E7-B/E7-C collapsed it
into `local_runnable` and the deque was left in place).

**E8 consequence:** the spec's §4.2 question "is inbox stealable?" is
moot for the current implementation — there is no inbox ticket to steal.
E8 steals from `local_runnable`. If a future revision revives `inbox` as
a real transport queue, E8's stealable-location decision must be
revisited. The ADR will record this.

---

## O7 — Does current quiescence/progress classification count all runnable ticket locations?

`classify_locked` (`src/async/scheduler.cpp:494-520`) counts:

| location | counted in `classify_locked`? | evidence |
|---|---|---|
| `pending_spawn_` | YES | `:496` `bool any_runnable = !pending_spawn_.empty();` |
| `local_runnable` (each worker) | YES | `:498-503` locks each `inbox_mtx`, sums `local_runnable.size()`, sets `any_runnable` |
| `inbox` | n/a | unused (O5/O6) |
| `running_fiber_count_` | YES (MW-S1) | `:505-507` |
| `ctx_.outstanding()` | YES (MW-S2) | `:512` |
| wait registrations | YES (MW-S3) | `:515-516` |

So all *actually-populated* runnable ticket locations are counted.
`runnable_count()` (`:570-581`) likewise sums `pending_spawn_` + every
`local_runnable`. The dead `inbox` deque is not counted, but it is also
never populated, so this is not a defect — only a latent trap if anyone
later pushes to `inbox` without updating `classify_locked` /
`runnable_count`.

**E8 consequence:** a steal that moves a ticket `W0.local_runnable →
W1.local_runnable` preserves the global runnable count, so
`classify_locked` and `runnable_count` remain correct *for free*. This is
a strong argument for Model B (move-the-ticket) over any model that
introduces a transient location. It also means stealable work is
automatically MW-S1-visible (E8-Inv / §4.6) with no classifier change.

---

## O8 — Can any current code read Fiber owner without `global_mtx_`?

The owner is read **only** from `WaitReg.owner` inside
`wake_ready_completions_locked` / `wake_ready_flags_locked`
(`:421,438,456`), both of which are called **only** with `global_mtx_`
held:

- `worker_loop` readiness drain: `:209-213` — `lock_guard(global_mtx_)`
  then calls both wake functions.
- MW-S2 Phase-B re-drain: `:266-275` under `global_mtx_`.
- Phase-D reacquire: `:303-309` under `global_mtx_`.

`route_runnable` (the unlocked variant, `:402-411`) takes `owner` as a
parameter and does not itself read a `WaitReg`; it is only called from…
actually it is **never called in production**. Grep shows `route_runnable`
(non-`_locked`) is defined but has zero call sites — every routing path
goes through `route_runnable_locked`. So in practice all owner reads are
under `global_mtx_`.

**E8 consequence:** the existing owner-read domain is `global_mtx_`. If
E8 stores the owner in a new field (e.g. `Fiber::owner_` or a scheduler
map) and updates it under `global_mtx_`, the wake path's owner read stays
in the same domain as the steal's owner write. This is the basis for
Model B's claim that owner transfer linearizes under one coordination
domain (the E8 spec §4.4 candidate).

---

## O9 — Can any current code mutate Fiber owner?

**No.** There is no owner field on `Fiber` to mutate, and `WaitReg.owner`
is write-once (set at suspend time in `await_completion_*` /
`await_ready_flag`: `:528`, `:542`, `:556`) and never updated thereafter.
The only "ownership change" in E7 is *implicit* — when a ticket moves
between `local_runnable` queues, but E7 never moves a ticket between
workers' queues: `route_runnable_locked` always routes to the *same*
`owner` that was captured at suspend time, which is the worker the Fiber
first ran on. Pinned ownership holds by construction.

This matches the E7 expected answer: **no migration after initial
ownership** (`docs/adr/ADR-execution-model.md:376-389`).

**E8 consequence:** E8 will introduce the *first* code that mutates
ownership. That mutation must happen under `global_mtx_` (the existing
owner-read domain, O8) and must atomically relocate the ticket, so no
intermediate state is observable where `owner` and ticket location
disagree. This is precisely Model B's single-transition linearization.

---

## O10 — What exact invariants currently depend on pinned ownership?

| Invariant | Current code dependency | E8 impact |
|---|---|---|
| wake routes to owner | `wake_ready_*_locked` reads `WaitReg.owner` (set at suspend) and routes there (`:421-427, 438-442, 456-459`). Pinned ownership guarantees `WaitReg.owner` == the worker that will resume the fiber. | **Load-bearing.** If E8 transfers ownership after suspend, the wake must read the *current* owner, not the stale `WaitReg.owner`. Requires an owner record updated by steal. |
| local queue ownership | `worker_loop` pops `ws->local_runnable` and runs it (`:186-202`); `run_next_on(ws, fiber)` sets `ws->current` (`:390`). The fiber is assumed to belong to `ws`. | Steal moves a ticket out of `victim->local_runnable`; the thief must own what it pops. Preserved if steal transfers owner atomically with the ticket move. |
| sched_ctx selection | `run_next_on` switches `ws->sched_ctx ↔ fiber.ctx` (`:393-396`); `await_*` switches back to `g_worker->sched_ctx` (`:531, 547, 566`). Each worker's `sched_ctx` is its own native-stack continuation. | Unchanged by E8: a stolen fiber resumes on the thief's `sched_ctx`. The fiber's own `ctx` is stack-content, not owner-bound. (Cross-thread resume validity was investigated in E7-T2; the spec §1 notes sequential cross-thread resume is *not* inherently invalid, but E8 must still transfer ownership explicitly.) |
| current Fiber state | `ws->current` (`:390, 398`) — the running fiber is recorded on its worker. | Unchanged: a stolen fiber, once popped by the thief, sets `thief->current`. |
| wait registration | `WaitReg { fiber, owner }` captures the *current* worker at suspend (`:528, 542, 556`). Owner is the worker that will resume. | **Load-bearing.** After a steal, a subsequent suspend on the thief must capture `thief` as `WaitReg.owner` — which happens automatically because `await_*` uses `g_worker` (the thief, now running the fiber). The hazard is only for *pre-steal* wait registrations, which cannot exist (a runnable fiber has no active reg — see O1/E7-Inv). |
| quiescence | `classify_locked` sums `local_runnable` across all workers (`:498-503`). Pinned ownership means a ticket in `W0.local_runnable` is owned by W0. | Preserved: steal moves a ticket between queues; the global count is unchanged (O7). |
| termination | `worker_loop` final recheck (`:339-375`) uses `classify_locked`. | Preserved: same as quiescence. Stealable work remains MW-S1 (§4.6), so an idle thief cannot commit MW-S2 admission while stealable work exists. |

**Summary of pinned-ownership dependencies:** the *only* invariant that
genuinely depends on immutability is **wake routing** (row 1). Everything
else depends on "the fiber is in the queue of its owner," which is
preserved by Model B's atomic ticket+owner move. The E7 publication
invariant (a runnable fiber has no active wait registration —
`docs/spec/e7_publication/E7Publication.tla:186-188`, `InvRunnableUnregistered`)
is what makes steal safe: a stealable runnable fiber has *no* `WaitReg`
whose `owner` could be stale. The stale-owner hazard is exclusively about
a *future* registration created after the steal, and that registration
will capture the *current* (thief) owner via `g_worker`. So the only way
to get a stale owner is to **not** transfer ownership at steal time —
exactly the negative model E8 must build.

---

## Audit conclusion

The E7 topology is **compatible with Model B (ownership-transfer steal)**
with no E7 contradiction:

1. Ownership is stored as `WaitReg.owner` (write-once at suspend) and
   implied by ticket location otherwise. There is no `Fiber::owner` field
   to break.
2. The `inbox` deque is dead storage; the single stealable location is
   `WorkerState::local_runnable`, guarded by `inbox_mtx`. This collapses
   the stealable-location decision (§4.2).
3. All owner reads occur under `global_mtx_` (O8). E8 owner-transfer
   under `global_mtx_` (which already guards routing) linearizes in one
   existing coordination domain — satisfying §4.4 without inventing
   IN_TRANSIT/STEALING/MIGRATING states.
4. `classify_locked` / `runnable_count` already count every populated
   runnable location; a steal that moves a ticket between `local_runnable`
   queues keeps the global count invariant for free (O7), so stealable
   work is automatically MW-S1 (§4.6) with no classifier change.
5. The pinned-ownership invariant that E8 must *replace* is wake routing
   (O10 row 1): the wake path must read the *current* owner. Because a
   stealable runnable fiber has no active `WaitReg` (E7
   `InvRunnableUnregistered`), there is no stale-reg race on the *stolen*
   epoch; the hazard is only the negative model's "ticket moves, owner
   stays," which produces a stale routing on a *post-steal* suspend/wake.

**No E7 contradiction found. E8 may proceed to the protocol decision (§3)
and ADR (§4).**

One implementation note for E8-B: the `inbox` deque is dead. E8 should
either (a) leave it dead and steal from `local_runnable` (smallest
change), or (b) remove `inbox` entirely as a cleanup. Option (a) is
preferred for the E8 baseline — do not expand the diff. Record this in
the ADR.
