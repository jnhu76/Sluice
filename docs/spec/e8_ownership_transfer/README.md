# E8 Runnable Ownership-Transfer / Work-Stealing — TLA+ Model

Narrow TLA+ model of the E8 runnable-ownership-transfer protocol, extending
the E7 runnable-publication vocabulary (`docs/spec/e7_publication/`).

The load-bearing E8 question:

```text
At what exact abstract transition does a runnable Fiber stop belonging
to Worker W0 and start belonging to Worker W1?
```

Answer (Model B): the **`StealRunnable`** transition. It MOVES the
existing runnable ticket `W0Local -> W1Local` AND TRANSFERS the runnable
owner record `ownerRecord: W0 -> W1`, as one atomic action. It does **not**
publish a new ticket (no `make_runnable`).

## State-indexed authority (E8-FORMAL-CORRECTIVE)

The model was corrected to match as-built production authority, which is
**state-indexed**, not global. Three distinct TLA+ variables carry
ownership/execution/resume authority, one per Fiber phase, mapping to three
distinct production fields:

| TLA+ variable   | authority role                          | production representation                         |
| --------------- | --------------------------------------- | ------------------------------------------------- |
| `ownerRecord[f]`| RUNNABLE ownership / steal-consistency  | `Scheduler::fiber_owner_[F]`                      |
| `execWorker[f]` | RUNNING execution authority             | `g_worker` (TLS) / `WorkerState::current`         |
| `waitOwner[f]`  | WAITING wait-epoch resume authority     | `WaitReg.owner` (captured `g_worker` at suspend)  |

These agree at lifecycle transition boundaries (formal invariants Inv4/Inv5
bind `ownerRecord = execWorker` at Running and `ownerRecord = waitOwner` at
Waiting), but they are **different production fields**.

**The load-bearing correction:** `WakeReady` routes by `waitOwner[f]`
(production `WaitReg.owner`), NOT by `ownerRecord[f]` (production
`fiber_owner_`). The earlier single-`owner` model read `owner[f]` in
`WakeReady`, silently claiming wake routing reads `fiber_owner_` — a
refinement ambiguity. Production `wake_ready_*_locked` read
`it->second.owner` (the registration's captured `WaitReg.owner`) and call
`route_runnable_locked(f, owner)`; no wake path references `fiber_owner_`.
See the audit at `docs/e8-formal-corrective/audit.md` (A4) for file/line
evidence.

`ownerRecord` and `waitOwner` are invariant-equal in all reachable Waiting
states (Inv5), but `WakeReady` routes from `waitOwner` in production. This
distinction is load-bearing: it keeps the routing authority and the
consistency relation separate in the model.

## Files

- `E8OwnershipTransfer.tla`           — the correct protocol (Model B).
- `E8OwnershipTransfer.cfg`           — TLC config (exhaustive).
- `E8OwnershipTransferBuggyOwner.tla` — deliberately broken Model A
  (ticket moves, owner record not transferred → runnable ticket/owner-record
  split).
- `E8OwnershipTransferBuggyOwner.cfg` — TLC config (expects counterexample).
- `README.md`                         — this file + refinement map.

## Running

```
java -cp /tmp/tla2tools.jar tlc2.TLC \
  -config docs/spec/e8_ownership_transfer/E8OwnershipTransfer.cfg \
  docs/spec/e8_ownership_transfer/E8OwnershipTransfer
java -cp /tmp/tla2tools.jar tlc2.TLC \
  -config docs/spec/e8_ownership_transfer/E8OwnershipTransferBuggyOwner.cfg \
  docs/spec/e8_ownership_transfer/E8OwnershipTransferBuggyOwner
```

## Model domain (finite, exhaustive TLC)

```
Workers = {W0, W1}
Fibers  = {F0, F1}
```

## State

- `fiberState[f]   ∈ {Created, Waiting, Runnable, Running, Done}`
- `ticketLocation[f] ∈ {None, PendingSpawn, W0Local, W1Local, W0Inbox, W1Inbox}`
  (the **single** abstract runnable publication token for a Fiber —
  inherited from E7)
- `waitReg[f]      ∈ {K, None}`  (active wait registration)
- `ownerRecord[f]  ∈ Workers ∪ {NA}`  (**mutable** by `StealRunnable`;
  the runnable owner record ↔ `fiber_owner_`. NA = no runnable owner yet)
- `execWorker[f]   ∈ Workers ∪ {NA}`  (set by `PopRunnable`; cleared by
  `SuspendFiber` ↔ `g_worker`/`WorkerState::current`)
- `waitOwner[f]    ∈ Workers ∪ {NA}`  (set by `SuspendFiber` from
  `execWorker`; read by `WakeReady` ↔ `WaitReg.owner`)

`ticketLocation` remains single-valued: the protocol is still linear
(`no ticket → publish one → transport/consume`). E8 adds `StealRunnable`,
which is MOVE + OWNER-RECORD TRANSFER — it never creates a ticket.

## Results

| model | distinct states | depth | result |
|-------|----------------:|------:|--------|
| `E8OwnershipTransfer` (correct, Model B) | 100 | 11 | **all configured invariants PASS** — no error found (fp collision prob 1.3e-15) |
| `E8OwnershipTransferBuggyOwner` (Model A) | 10 | 4 | **counterexample** — `InvLocalMatchesOwner` violated by the single steal defect |

### Correct model invariants (all PASS)

- **E8-AUTH-Inv1** `ticketLocation[f] # None  => fiberState = Runnable`
- **E8-AUTH-Inv2** `fiberState = Runnable    => ticketLocation # None ∧
  ownerRecord ∈ Workers ∧ execWorker = NA ∧ waitOwner = NA`
- **E8-AUTH-Inv3** `ticketLocation = W{0,1}Local => ownerRecord = W{0,1}`
  (load-bearing — this is what the negative model violates)
- **E8-AUTH-Inv3b** `ticketLocation = W{0,1}Inbox => ownerRecord = W{0,1}`
- **E8-AUTH-Inv4** `fiberState = Running => execWorker ∈ Workers ∧
  ticketLocation = None ∧ waitOwner = NA ∧ waitReg = None ∧
  ownerRecord = execWorker`
- **E8-AUTH-Inv5** `fiberState = Waiting => waitReg # None ∧
  ticketLocation = None ∧ execWorker = NA ∧ waitOwner ∈ Workers ∧
  ownerRecord = waitOwner`  (the formal refinement obligation for
  `WaitReg.owner = g_worker` at suspend; invariant-equal, not routing source)
- **E8-AUTH-Inv6** `WakeReady` routes by `waitOwner` — structural
  (`ticketLocation' = LocalOf(waitOwner[f])`); no abstract rule routes via
  `ownerRecord`, even though `ownerRecord = waitOwner` in valid Waiting states
- **E8-AUTH-Inv7** `fiberState = Done => ticketLocation = None ∧ waitReg = None`
- **E8-AUTH-Inv8** Steal preserves ticket cardinality — structural
  (`StealRunnable` has no `make_runnable`; moves one NonNone loc to another)
- **E8-AUTH-Inv9** Steal cannot race a live wait epoch — `StealRunnable`
  requires `fiberState = Runnable ∧ waitOwner = NA` (E7
  `InvRunnableUnregistered`)
- **E8-AUTH-Inv10** Suspend captures the current executor — structural
  (`waitOwner' = execWorker`; production `WaitReg.owner = g_worker`)

Plus the E7 baseline invariants: `Waiting <=> registered`, `Runnable =>
unregistered` (the latter is what makes steal safe: a stealable runnable
fiber has no active wait registration whose owner could be stale).

### Buggy model counterexample (Model A — single defect)

```
State 1: <Initial predicate>
  ownerRecord=(F0:NA,F1:NA); ticketLocation=(F0:None,F1:None); fiberState=(F0:Created,F1:Created)
State 2: <SpawnPublish F0>
  ticketLocation=(F0:PendingSpawn,...); ownerRecord=(F0:W0,...); fiberState=(F0:Runnable,...)
State 3: <MovePendingToOwnerLocal F0>
  ticketLocation=(F0:W0Local,...); ownerRecord=(F0:W0,...)   \* F0 owned by W0, on W0's queue
State 4: <StealRunnableBuggy W0→W1, F0>
  ticketLocation=(F0:W1Local,...); ownerRecord=(F0:W0,...)   \* VIOLATION: ticket on W1's
                                                             \* queue but owner record still W0.
```

`InvLocalMatchesOwner` is violated at State 4: `ticketLocation[F0]=W1Local`
requires `ownerRecord[F0]=W1`, but `ownerRecord[F0]=W0`.

This is the **single intended defect** (Model A): the steal moves the
ticket without transferring `ownerRecord`. Because `PopRunnable` (kept
identical to the correct model — no `PopRunnableBuggy`) requires
`ownerRecord[f] = w`, the thief can never pop the stolen fiber; the ticket
is stranded on the wrong owner's queue. The defect class is the **runnable
ticket / runnable owner-record split**, not a stale `WaitReg` wake route
(production wake reads `WaitReg.owner`, so a stale-route counterexample is
neither producible by this model nor representative of production).

## Correct vs buggy — semantic diff

The comment-stripped, name-normalized diff between
`E8OwnershipTransfer.tla` and `E8OwnershipTransferBuggyOwner.tla` shows
**exactly one** behavioral difference:

```
StealRunnable:
-    /\ ownerRecord' = [ownerRecord EXCEPT ![f] = thief]     \* correct (Model B)
+    /\ ownerRecord' = ownerRecord                           \* buggy  (Model A: no transfer)
```

Every other action, `Init`, `Spec`, and `Next` is byte-identical. The buggy
model checks a smaller invariant set (only those it must violate); that is
a verification-configuration difference, not a behavioral one. No FORMAL
BLOCKER.

## Refinement map (TLA+ → production, state-indexed authority)

| TLA+ item        | Production representation                                  | source of truth / authority role             | lock / execution domain              |
| ---------------- | --------------------------------------------------------- | -------------------------------------------- | ------------------------------------ |
| `ownerRecord[f]` | `Scheduler::fiber_owner_[F]`                              | RUNNABLE ownership / steal-consistency       | `global_mtx_`                        |
| `execWorker[f]`  | `g_worker` (TLS) / `WorkerState::current`                 | RUNNING execution authority                  | TLS (per Worker thread); set in `run_next_on` |
| `waitOwner[f]`   | `WaitReg.owner` (in `waiting_size_/void_/ready_` maps)    | WAITING wait-epoch resume authority          | `global_mtx_`                        |
| `SpawnPublish(f)`| `Scheduler::spawn` → `fiber.make_runnable()` + push to `target->local_runnable` (or `pending_spawn_`) | **PUBLISH** (created→runnable); sets `ownerRecord` | `global_mtx_` + `inbox_mtx` |
| `MovePendingToOwnerLocal(f)` | `Scheduler::run` distribute `pending_spawn_` → `workers_[w]->local_runnable` | **MOVE** (transport)         | `global_mtx_` + `inbox_mtx`          |
| `MoveInboxToLocal(f)` | n/a (production `inbox` deque is dead storage — E8-0 audit O5/O6; kept for vocabulary) | MOVE                                  | —                                    |
| `SuspendFiber(f)`| `Scheduler::await_completion_*` / `await_ready_flag`: `make_waiting` + insert `WaitReg{me, ws}` with `ws = g_worker` under `global_mtx`, switch away. **Captures `waitOwner = execWorker`** (=`g_worker`). | state-only + wait registration | `global_mtx_` |
| `WakeReady(f)`   | `wake_ready_completions_locked` / `wake_ready_flags_locked`: erase reg, `make_runnable`, `route_runnable_locked(f, it->second.owner)`. **Routes by `WaitReg.owner`** (= `waitOwner`), NOT `fiber_owner_`. | **PUBLISH** (waiting→runnable)        | `global_mtx_`                        |
| `Route`          | `route_runnable_locked(f, owner)` — pushes `f` to `owner->local_runnable` | transport after wake                | `global_mtx_` + `inbox_mtx`          |
| `StealRunnable(victim, thief, f)` | `Scheduler::try_steal(thief)`: under `global_mtx_`, verify `state==runnable && fiber_owner_==victim`, remove from `victim->local_runnable`, set `fiber_owner_[f]=thief`, push to `thief->local_runnable`, notify | **MOVE + OWNER-RECORD TRANSFER** (no publish; no `make_runnable`) | `global_mtx_` |
| `PopRunnable(w, f)` | `worker_loop` pop `ws->local_runnable` → `run_next_on(ws, f)` (`make_running`, sets `ws->current=f`; `g_worker=ws` on the worker thread). **Sets `execWorker = w`.** | **CONSUME**                     | `inbox_mtx` (queue); `ws->current`/`g_worker` per-thread |
| `FinishFiber(f)` | `fiber_entry_bridge` → `fiber.make_done()` + switch to `sched_ctx` | state-only                                   | fiber-owned (running on its worker)  |

### Routing authority vs consistency relation (load-bearing)

```text
ownerRecord[f] and waitOwner[f] are invariant-equal in all reachable
Waiting states (Inv5 binds ownerRecord = waitOwner when fiberState=Waiting).

BUT WakeReady routes from waitOwner in production (wake_ready_*_locked
read it->second.owner = WaitReg.owner), NOT from ownerRecord (fiber_owner_).

The model preserves this distinction: WakeReady publishes the ticket to
LocalOf(waitOwner[f]), with no abstract rule routing via ownerRecord.
```

### Publication-capability statement

```text
make_runnable() success grants publication capability.

StealRunnable does NOT acquire publication capability.
StealRunnable MOVES an existing capability and TRANSFERS ownerRecord.

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

## Reproducible verification

See `scripts/verify-e8-stability.sh` for the committed repeat runner and
the exact stress-gate commands (E8-T3/T11, E7 dup/worker). The runner uses
`SLUICE_TEST_FILTER` to select individual cases (precise tokens like
`e8_t3`, `e8_t11`, not whole-suite runs).
