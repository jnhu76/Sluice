# E8-FORMAL-CORRECTIVE — Production Refinement Trace & Consistency Gate

This document closes §11 of the corrective brief: after correcting the
model/ADR/README, re-read current production code and produce a refinement
trace showing the corrected authority model is consistent with as-built
production across one real E8-T3 execution.

## Corrected authority model (recap)

| TLA+ variable   | production field                       | role                                    |
| --------------- | -------------------------------------- | --------------------------------------- |
| `ownerRecord[f]`| `Scheduler::fiber_owner_[F]`           | RUNNABLE ownership / steal-consistency  |
| `execWorker[f]` | `g_worker` (TLS) / `WorkerState::current` | RUNNING execution authority          |
| `waitOwner[f]`  | `WaitReg.owner` (captured `g_worker` at suspend) | WAITING wait-epoch resume authority |

`WakeReady` routes by `waitOwner`, NOT `ownerRecord`.

## Refinement trace for one E8-T3 execution

E8-T3 (`tests/e8_steal_test.cpp`, case `e8_t3_steal_run_suspend_wake_resume_on_thief`)
constructs the load-bearing path: F2 is spawned onto F1's worker's queue
(victim), the OTHER worker steals F2 (thief), F2 runs on the thief and
suspends (`await_ready_flag(flag_x)`), a setter makes `flag_x` ready, and
F2 must RESUME on the thief (not route back to the victim).

Setup: `run(2)` → `spawn(f0)`→W0, `spawn(f1)`→W1. Inside f1's body,
`spawn_on(f2, me=1)` places F2 on W1's `local_runnable`.

```
RUNNABLE phase:
  spawn(f1):       fiber_owner_[f1] = W1        ticket: W1->local_runnable  (sched.cpp:73)
  spawn_on(f2,1):  fiber_owner_[f2] = W1        ticket: W1->local_runnable  (sched.cpp:99)
  authority:       ownerRecord[f2] = W1 (=victim); ticketLocation[f2] = W1Local
  execWorker[f2] = NA, waitOwner[f2] = NA        (model Inv2)

STEAL (W0 steals F2 from W1):
  try_steal(W0):
    verify f2.state()==runnable && fiber_owner_[f2]==W1   (sched.cpp:655-657)
    remove f2 from W1->local_runnable                     (sched.cpp:659)
    fiber_owner_[f2] = W0   (thief)                       (sched.cpp:665)  <-- OWNER TRANSFER
    push f2 to W0->local_runnable                         (sched.cpp:668)
  authority:       ownerRecord[f2] = W0 (=thief); ticketLocation[f2] = W0Local
  execWorker/waitOwner unchanged (both NA)               (model Inv3 holds: ticket matches ownerRecord)

POP / RUNNING phase:
  W0 worker_loop pops f2 from its local_runnable          (sched.cpp:216-218)
  run_next_on(W0, f2): ws->current = f2; g_worker = W0    (sched.cpp:430)
  authority:       execWorker[f2] = W0 (=thief, the executor)
                   ownerRecord[f2] = execWorker[f2] = W0   (model Inv4 holds)

SUSPEND phase:
  f2 body calls sched.await_ready_flag(flag_x):
    ws = g_worker = W0;  me = ws->current = f2             (sched.cpp:591-592)
    make_waiting()                                         (sched.cpp:603)
    waiting_ready_[&flag_x] = {me, ws}                     (sched.cpp:596)  <-- WaitReg.owner = W0
    switch to sched_ctx                                    (sched.cpp:607)
  authority:       waitOwner[f2] = W0 (= g_worker at suspend = thief)
                   execWorker[f2] = NA;  ownerRecord[f2] = waitOwner[f2] = W0   (model Inv5 holds)
                   NOTE: fiber_owner_ is NOT written by await_*; it remains W0
                   (last runnable owner), which happens to equal the captured
                   resume owner — the consistency relation, not the routing source.

WAKE phase (setter makes flag_x ready; readiness drain runs on W0):
  wake_ready_flags_locked():
    flag_x loads true                                       (sched.cpp:494)
    owner = it->second.owner = W0    (WaitReg.owner)        (sched.cpp:496)  <-- READS WAITREG.OWNER, NOT fiber_owner_
    erase registration                                      (sched.cpp:497)
    f2->make_runnable() succeeds                            (sched.cpp:498)
    route_runnable_locked(f2, owner=W0)                     (sched.cpp:499)
      -> pushes f2 to W0->local_runnable                    (sched.cpp:527)  <-- ROUTE DESTINATION = WaitReg.owner = W0 = thief
  authority:       ticketLocation[f2] = W0Local; waitOwner[f2] consumed (NA)
                   the route destination was waitOwner[f2] = W0 (the thief)
  OBSERVABLE RESULT: F2 is runnable on W0's queue; W0 resumes it.
                     wid_f2_pre == wid_f2_post == W0  (resumed on thief)
                     wid_f2_post != W1  (did NOT route back to victim)
```

## Production consistency gate (verdict)

The trace confirms the corrected model's lifecycle transitions all hold
against as-built production for one real E8-T3 execution:

```
RUNNABLE:  ownerRecord + local ticket agree            (Inv2/Inv3)   ✓
STEAL:     ownerRecord victim -> thief; ticket victim-local -> thief-local   ✓
POP:       owner-local ticket consumed; execWorker = Worker      (Inv4)   ✓
SUSPEND:   WaitReg.owner = current Worker (= g_worker)           (Inv10)  ✓
WAKE:      route destination = WaitReg.owner (NOT fiber_owner_)  (Inv6)   ✓
```

The load-bearing observation, asserted by T3 and reproduced 1000/1000
(release) and 1000/1000 (debug): `wid_f2_pre == wid_f2_post` (F2 resumed on
the thief). This is consistent with `WakeReady` routing by `waitOwner`
because `waitOwner` was captured as `g_worker = W0` (thief) at suspend.

**Wake does NOT read `fiber_owner_`.** The correct statement (per the
brief's §11) is: the lifecycle transition protocol preserves consistency
between the runnable owner record, the execution Worker, and the captured
wait resume owner. `ownerRecord` and `waitOwner` are invariant-equal in
reachable Waiting states (Inv5), but the routing source in production is
`WaitReg.owner` (`waitOwner`).

## Production C++ diff (expected)

```
NONE (behavioral).
```

The only production-source changes are comment corrections
(`include/sluice/async/scheduler.hpp` fiber_owner_ comment block;
`src/async/scheduler.cpp` two inline comments at spawn/spawn_on). Stripping
comments, `git show HEAD:...` vs working tree shows zero code differences
for both files. This was verified by rebuild + 100× T3 re-run after the
comment edits.
