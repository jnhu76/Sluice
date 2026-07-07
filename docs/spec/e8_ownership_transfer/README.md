# E8 Runnable Ownership-Transfer / Work-Stealing — TLA+ Model

Narrow TLA+ model of the E8 runnable-ownership-transfer protocol, extending
the E7 runnable-publication vocabulary (`docs/spec/e7_publication/`).

The load-bearing E8 question:

```text
At what exact abstract transition does a runnable Fiber stop belonging
to Worker W0 and start belonging to Worker W1?
```

Answer (Model B): the **`StealRunnable`** transition. It MOVES the
existing runnable ticket `W0Local -> W1Local` AND TRANSFERS
`owner: W0 -> W1`, as one atomic action. It does **not** publish a new
ticket (no `make_runnable`).

## Files

- `E8OwnershipTransfer.tla`           — the correct protocol (Model B).
- `E8OwnershipTransfer.cfg`           — TLC config (exhaustive).
- `E8OwnershipTransferBuggyOwner.tla` — deliberately broken Model A
  (ticket moves, owner stays → stale-owner routing).
- `E8OwnershipTransferBuggyOwner.cfg` — TLC config (expects counterexample).
- `README.md`                         — this file + refinement map.

## Running

```
java -cp /tmp/tla2tools.jar tlc2.TLC -config E8OwnershipTransfer.cfg E8OwnershipTransfer
java -cp /tmp/tla2tools.jar tlc2.TLC -config E8OwnershipTransferBuggyOwner.cfg E8OwnershipTransferBuggyOwner
```

## Model domain (finite, exhaustive TLC)

```
Workers = {W0, W1}
Fibers  = {F0, F1}
```

## State

- `fiberState[f]     ∈ {Created, Waiting, Runnable, Running, Done}`
- `ticketLocation[f] ∈ {None, PendingSpawn, W0Local, W1Local, W0Inbox, W1Inbox}`
  (the **single** abstract runnable publication token for a Fiber —
  inherited from E7)
- `waitReg[f]        ∈ {K, None}`  (active wait registration)
- `owner[f]          ∈ Workers`    (**mutable** by `StealRunnable`; the E8
  addition over E7, where `owner` was immutable)

`ticketLocation` remains single-valued: the protocol is still linear
(`no ticket → publish one → transport/consume`). E8 adds `StealRunnable`,
which is MOVE + OWNER TRANSFER — it never creates a ticket.

## Results

| model | distinct states | depth | result |
|-------|----------------:|------:|--------|
| `E8OwnershipTransfer` (correct, Model B) | 100 | 11 | **Inv1-Inv10 PASS** — no error found |
| `E8OwnershipTransferBuggyOwner` (Model A) | 10 | 4 | **counterexample** — `InvLocalMatchesOwner` violated |

### Correct model invariants (all PASS)

- **E8-Inv1** `ticketLocation[f] # None  => fiberState = Runnable`
- **E8-Inv2** `fiberState = Runnable     => ticketLocation # None`
- **E8-Inv3** `ticketLocation = W{0,1}Local => owner = W{0,1}` (load-bearing)
- **E8-Inv4** `ticketLocation = W{0,1}Inbox  => owner = W{0,1}`
- **E8-Inv5** `fiberState = Waiting  => ticketLocation = None` (not stealable)
- **E8-Inv6** `fiberState = Running  => ticketLocation = None` (not stealable)
- **E8-Inv7** `fiberState = Done     => ticketLocation = None ∧ waitReg = None`
- **E8-Inv8** Steal preserves ticket cardinality — structural
  (`StealRunnable` has no `make_runnable`; moves one NonNone loc to
  another NonNone loc).
- **E8-Inv9** Wake routes to current owner — structural
  (`WakeReady` publishes to `LocalOf(owner[f])`, and `owner` reflects any
  prior `StealRunnable`).
- **E8-Inv10** Pop executes only current-owner work — structural
  (`PopRunnable(w,f)` requires `owner[f] = w ∧ ticketLocation[f] = LocalOf(w)`).

Plus the E7 baseline invariants: `Waiting <=> registered`, `Runnable =>
unregistered` (the latter is what makes steal safe: a stealable runnable
fiber has no active wait registration whose owner could be stale).

### Buggy model counterexample (Model A)

```
State 1: <Initial predicate>
  owner = (F0:W0, F1:W0); ticketLocation=(F0:None,F1:None); fiberState=(F0:Created,F1:Created)
State 2: <SpawnPublish F0>
  ticketLocation = (F0:PendingSpawn, ...); fiberState = (F0:Runnable, ...)
State 3: <MovePendingToOwnerLocal F0>
  ticketLocation = (F0:W0Local, ...); owner = (F0:W0, ...)   \* F0 owned by W0, on W0's queue
State 4: <StealRunnableBuggy W0→W1, F0>
  ticketLocation = (F0:W1Local, ...); owner = (F0:W0, ...)   \* VIOLATION: ticket on W1's
                                                             \* queue but owner still W0.
```

`InvLocalMatchesOwner` is violated at State 4: `ticketLocation[F0]=W1Local`
requires `owner[F0]=W1`, but `owner[F0]=W0`.

**Full causal chain reachable** (same defect class; TLC reports the
shallowest violation): from State 4,
`PopRunnableBuggy(W1, F0)` → `SuspendFiber(F0)` (waitReg set, owner still W0)
→ `WakeReady(F0)` publishes to `LocalOf(owner=W0)=W0Local`. The fiber that
*ran on W1 and suspended on W1* is routed back to W0's queue — the
stale-owner routing defect. Semantically equivalent to the E8 spec §7
required trace:

```
F owned by W0 -> stolen to W1 without owner transfer -> runs W1 ->
waits -> wakes -> stale owner says W0 -> F routed back to W0.
```

## Refinement map (TLA+ action → production path)

| TLA+ action | Production path | lock / domain | publication semantics |
| --- | --- | --- | --- |
| `SpawnPublish(f)` | `Scheduler::spawn` → `fiber.make_runnable()` + push to `target->local_runnable` (or `pending_spawn_`) | `global_mtx_` + `inbox_mtx` | **PUBLISH** (created→runnable grants publication capability) |
| `MovePendingToOwnerLocal(f)` | `Scheduler::run` distribute `pending_spawn_` → `workers_[w]->local_runnable` | `global_mtx_` + `inbox_mtx` | **MOVE** (transport; ticket already published) |
| `MoveInboxToLocal(f)` | n/a (production `inbox` deque is dead storage — E8-0 audit O5/O6; kept for vocabulary) | — | MOVE |
| `SuspendFiber(f)` | `Scheduler::await_completion_*` / `await_ready_flag`: `make_waiting` + insert `WaitReg{fiber, owner=g_worker}` under `global_mtx_`, switch away | `global_mtx_` | state-only + wait registration |
| `WakeReady(f)` | `wake_ready_completions_locked` / `wake_ready_flags_locked`: erase reg, `make_runnable`, `route_runnable_locked(f, owner)` | `global_mtx_` (owner read from current owner record) | **PUBLISH** (waiting→runnable grants publication capability) |
| `StealRunnable(victim, thief, f)` | `Scheduler::try_steal_locked(thief)` (NEW in E8): under `global_mtx_`, verify `state==runnable && owner==victim`, remove from `victim->local_runnable`, set `owner[f]=thief`, push to `thief->local_runnable`, notify thief | `global_mtx_` | **MOVE + OWNER TRANSFER** (no publish; no `make_runnable`) |
| `PopRunnable(w, f)` | `worker_loop` pop `ws->local_runnable` → `run_next_on(ws, f)` (`make_running`) | `inbox_mtx` (queue) + owner is `ws` by construction | **CONSUME** |
| `FinishFiber(f)` | `fiber_entry_bridge` → `fiber.make_done()` + switch to `sched_ctx` | fiber-owned (running on its worker) | state-only |

### Publication-capability statement

```text
make_runnable() success grants publication capability.

StealRunnable does NOT acquire publication capability.
StealRunnable MOVES an existing capability and TRANSFERS owner.

StealRunnable calls no make_runnable.
```

This is the load-bearing E8 invariant: steal is transport, not
publication. Conflating the two risks reintroducing the E7-T2
duplicate-publication defect class (`docs/spec/e7_publication/`).

## What this model does NOT cover

- AsyncBackend internals, io_uring, ThreadPool (out of scope; E7 closed).
- MW-S2 blocking admission (closed by
  `docs/spec/e7_multiworker_progress/E7MultiWorkerProgress.tla`). E8 reuses
  that protocol unchanged; stealable work is MW-S1, and the E7 classifier
  already counts every `local_runnable` (E8-0 audit O7), so stealable work
  is automatically MW-S1-visible with no classifier change.
- Chase-Lev / lock-free deques (E16). E8 uses `global_mtx_`-serialized
  steal; the abstract `StealRunnable` action is identical under either
  implementation — only the data structure changes.
- Cancellation / multi-wait / timers (E10/E11/E13). Generation-tagged
  ownership (Model D) was evaluated and rejected for E8 as
  over-engineering; reserved for E10+ if a real epoch hazard appears.
