# E7 Runnable-Publication Protocol — TLA+ Model

Narrow TLA+ model of the E7 pinned-Fiber runnable-publication protocol,
created during the root-cause investigation of the E7-T2 flake (see commit
`2265f1f` "async: enforce exactly-once runnable publication").

The purpose is **NOT** to prove the C++ implementation bug-free. It verifies
that the abstract scheduling protocol preserves the required safety invariants
across all modeled interleavings, and that the proven old-producer defect
admits a counterexample matching the observed causal chain.

## Files

- `E7Publication.tla` — the correct protocol spec (Inv1-Inv8).
- `E7Correct.cfg`    — TLC config for the correct model (exhaustive).
- `E7Buggy.tla`       — deliberately broken variant with `DefectDuplicatePublish`.
- `E7Buggy.cfg`       — TLC config for the buggy model (expects counterexample).
- `README.md`         — this file + the refinement map.

## Running

```
java -cp /tmp/tla2tools.jar tlc2.TLC -config E7Correct.cfg E7Publication
java -cp /tmp/tla2tools.jar tlc2.TLC -config E7Buggy.cfg    E7Buggy
```

## Model domain (finite, exhaustive TLC)

```
Fibers   = {F0, F1, F2}
Workers  = {W0, W1}
WaitKeys = {K0, K1}
```

## State

- `fiberState[f]    ∈ {Created, Waiting, Runnable, Running, Done}`
- `ticketLocation[f]∈ {None, PendingSpawn, W0Local, W1Local, W0Inbox, W1Inbox}`
  (the **single** abstract runnable publication token for a Fiber)
- `waitReg[f]       ∈ WaitKeys ∪ {None}`
- `owner[f]         ∈ Workers` (immutable per fiber)

`ticketLocation` is single-valued: the intended protocol is linear
(`no ticket → publish one → transport moves it → pop consumes`). A transport
action must NOT create another publication.

## Results

| model | distinct states | depth | result |
|-------|----------------:|------:|--------|
| `E7Correct` | 343 | 13 | **Inv1-Inv8 PASS** — no error found |
| `E7Buggy`   | 1000 | 10 | **counterexample** — `InvDoneNoTicket` violated |

### Correct model invariants (all PASS)

- **Inv1** `ticketLocation[f] # None  => fiberState = Runnable`
- **Inv2** `fiberState = Runnable     => ticketLocation # None`
- **Inv3** `fiberState = Running      => ticketLocation = None`
- **Inv4** `fiberState = Done         => ticketLocation = None ∧ waitReg = None`
- **Inv5** `fiberState = Waiting  <=> waitReg # None`
- **Inv6** `fiberState = Runnable     => waitReg = None`
- **Inv7** encoded structurally — no action has the shape
  "Runnable → Runnable plus publish". `WakeReady` requires `Waiting`.
- **Inv8** pinned routing — a ticket at `WxLocal`/`WxInbox` ⇒ `owner = Wx`.

### Buggy model counterexample (matches the flight recorder)

```
State 1: Created, ticketCount=0
State 2: SpawnPublish            -> Runnable, ticketCount=1   (ticket A)
State 3: DefectDuplicatePublish  -> ticketCount=2             (ticket B - defect)
State 4: PopRunnable             -> Running, ticketCount=1    (consumed A)
State 5: FinishFiber             -> Done, ticketCount=1        ← VIOLATION
```

This is exactly: ticket A published, ticket B published by the defect, A
consumed, fiber runs to done, **B remains live** → Done with a live ticket.
Matches the Phase-5 flight-recorder trace (two PUBLISH events to the same
queue, two POPs, the second on a Done fiber).

## Refinement map (TLA+ action → production path)

| TLA+ action            | production function / path                          | linearization point / lock domain |
|------------------------|-----------------------------------------------------|-----------------------------------|
| `SpawnPublish(f)`      | `Scheduler::spawn` → `make_runnable` + enqueue      | `global_mtx_`                     |
| `SuspendFiber(f,key)`  | `await_ready_flag`/`await_completion_*` (register + `make_waiting` + switch away) | `global_mtx_` (register); the register+suspend window is **atomic** in the model — production has a transient (documented) |
| `WakeReady(f)`         | `wake_ready_flags_locked` / `wake_ready_completions_locked` | `global_mtx_`               |
| `MoveInboxToLocal(f)`  | `route_runnable_locked` transport                   | `global_mtx_` + `owner->inbox_mtx`|
| `MovePendingToOwnerLocal(f)` | `run()` distribute loop                       | `global_mtx_` + `inbox_mtx`       |
| `PopRunnable(f)`       | `worker_loop` pop + `run_next_on` → `make_running` | `inbox_mtx` (local) / `global_mtx_` (pending) |
| `FinishFiber(f)`       | `fiber_entry_bridge` → `make_done`                 | (single-threaded on the worker)   |

### The proven buggy producer

```
old C++ behavior:
    make_runnable() returned void (silent no-op on already-Runnable);
    wake_ready_*_locked called route_runnable_locked UNCONDITIONALLY.
    => a second runnable ticket published for a fiber whose spawn ticket
       was still unconsumed (or whose original ticket was consumed but the
       fiber was re-published during the register/suspend transient).

    VIOLATES: Inv7 (no action may publish without owning a transition)
              and, downstream, Inv4 (Done => no live ticket).

fixed C++ behavior (commit 2265f1f):
    make_runnable() returns bool; route_runnable_locked is called ONLY when
    make_runnable() returns true (a real waiting->runnable transition).
    => REFINES the checked WakeReady transition.
```

## Conclusion (per the protocol doc)

```
The abstract E7 runnable-publication protocol preserves Inv1-Inv8 for the
checked finite model.

The old producer rule admits a counterexample matching the observed
duplicate-publication causal chain.

The production repair is required to refine the checked protocol.
```

This does **not** prove the C++ implementation bug-free. The C++ repair
(commit `2265f1f`) is validated empirically (T2 5000/5000, full suite GREEN,
TSan 0 races); the TLA+ model independently confirms the abstract protocol's
safety and demonstrates that the defect's abstract form violates it.

## Scope (explicit non-goals)

- Does NOT model E8 work-stealing or Fiber migration.
- Does NOT model AsyncBackend internals, context-switch asm, stack layout,
  ThreadPool syscalls, timers, cancellation redesign, or performance.
- Does NOT model the register→suspend transient as a separate visible state
  in the CORRECT model (collapsed to one atomic `SuspendFiber`); the buggy
  model encodes the defect directly.
