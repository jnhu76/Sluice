# Worker Retire Ticket Rescue — local_runnable → pending_spawn_ (MODEL-007e)

Focused TLA+ safety model of the G1 retirement epilogue: a worker leaving
the active run participant set must not strand tickets on its private
`local_runnable` queue — the epilogue moves them to the pre-run domain
(`pending_spawn_`), where a live worker's loop top (or the next
invocation's setup) redispatches them AND re-records the owner (a
retire-seeded ticket carries its DEAD original owner; the retire moves
the ticket, not the ownership record).

Issue #178 (child of umbrella #171). C++ is the fact source.

## C++ binding

| Model construct | C++ (baseline c1e93f9) |
|---|---|
| `RetireW0` (fused active-clear + rescue; guarded to the unconsumed-ticket epilogue state — fiber Runnable, ticket on W0Local; a worker mid-`RunFiber` switch cannot retire here, and the retire-with-empty-queue path has no topology to study) | the worker_loop epilogue (`src/async/scheduler.cpp:1230-1270`): one `global_mtx_` scope — `--live_loop_workers_`, `active := false`, move `local_runnable` → `pending_spawn_` under nested `inbox_mtx`, `signal_wake_locked()` unconditionally |
| owner NOT updated by the rescue | the C++ fact: the dead owner rides with the ticket into `pending_spawn_` |
| `RedispatchW1` + the re-record | worker loop top (`scheduler.cpp:485-545`): pop `pending_spawn_`, `fiber_owner_[f] = ws` — the adversarial-review route-to-dead-worker repair |
| `RunFiber(w)` active + Runnable guard | only a live participant pops/executes, and `make_running`'s Runnable precondition is a structural guard — a duplicate ticket (NEG-RT2) cannot run an already-Running fiber |
| no `make_runnable` anywhere in rescue/dispatch | rescue and dispatch are TRANSPORT; publication happened once at spawn (E7-T2) |
| `InvNoTicketOnRetiredWorker` | the G1 repair's headline invariant ("terminate path strands queued runnables") |
| `sawRescue` / `loop_exited` seam | the G1 causal worker-death evidence (`WorkerState::loop_exited`) |

## Boundary

Workers {W0 (retires with the unconsumed ticket), W1 (the survivor)}; one
Fiber F. `ticketAt` is a SET of locations — singleton as-built; the set
shape makes the duplication defect expressible without a second ticket
object. Safety only.

Non-goals (explicit): the #161 idle dance / `live_loop_workers_` /
`idle_workers_` terms (their own suite, `spec/tla/e12_rwlock_scheduler_liveness/`
— composition boundary only; if they were needed to prove ticket topology,
the boundary would be too wide); the post-retire route onto a dead inbox
and its steal backstop (E8/#115 transport domain); the next-invocation
redistribution (isomorphic to the loop-top redispatch); timers, backend,
the suspend-switch window (MODEL-007(a)'s domain).

Memory-model boundary: `active` is a release/acquire atomic, but every
ticket consumer (rescue, dispatch, run) needs `global_mtx_` or the owning
`inbox_mtx_` — both SC mutex domains. The fused retire+rescue action is a
faithful SC abstraction. No C++ weak-memory claim.

## Laws (positive cfg, all PASS)

| invariant | meaning |
|---|---|
| `InvNoTicketOnRetiredWorker` | a retired worker's queue never holds the ticket (the rescue emptied it in the same critical section) |
| `InvSingleTicket` | one live ticket per Fiber — rescue/dispatch/steal are transport, never publication |
| `InvRunnableHasRecoverableTicket` | a runnable Fiber's ticket is in the pre-run domain or on an ACTIVE worker's queue |
| `InvOwnerLocationConsistency` | owner/location agreement for worker queues, per the REAL owner semantics — no assumption that Pending implies any owner (the pending ticket carries its dead original owner until the dispatch re-record) |

## Negative controls (one-rule cfg flips)

| gate | defect | named CEX | specificity exclusions |
|---|---|---|---|
| NegNoRescue | retire strands the queue (the pre-G1 defect) | `InvNoTicketOnRetiredWorker` | entailed co-victim `InvRunnableHasRecoverableTicket` |
| NegRescueCopies | rescue copies without clearing | `InvSingleTicket` | entailed co-victims `InvNoTicketOnRetiredWorker` (the stale copy stays on the retired worker) and `InvOwnerLocationConsistency` (after a redispatch the re-recorded owner no longer matches the stale copy) |
| NegNoRerecord | dispatch drops the owner re-record (the adversarial-review route-to-dead-worker class) | `InvOwnerLocationConsistency` | none — all other laws PASS |

## Reachability (5 witnesses, each a NoReach* CEX)

ER1 initial ticket on the active owner · ER2 (strengthened) retired worker
with the fiber still Runnable and the ticket AT PENDING — the rescue
itself; subsumes the old standalone Pending witness (Pending is reachable
only through the retire rescue, so the two targets coincided and the
duplicate gate was merged away) · ER4 the survivor's queue holds the
ticket with the re-recorded owner · ER5 (strengthened) the SURVIVOR
resumed the rescued ticket: W0 retired ∧ owner = W1 (only the dispatch
re-record can set that) ∧ ticket consumed ∧ Running — reachable only via
the full retire → rescue → pending → redispatch → re-record → W1-consume
chain; the old generic Running∨Done form was vacuous (Init → RunFiber(W0)
satisfied it with no retirement at all) · plus the rescue-move ghost
witness.

## Results

TLC 2.19 (tla2tools 1.7.4), exhaustive, 1 worker:

- positive: 14 states generated, 7 distinct, depth 5, no error (after
  the review-fix tightening: retire requires the unconsumed-ticket
  epilogue state, RunFiber requires Runnable).
- all 3 negatives: exact named CEX; all 3 specificity gates PASS; all 5
  reachability gates CEX as expected (the standalone ReachPending merged
  into the strengthened RetiredTicketPending; Resumed tightened into the
  survivor-resume chain).
- `bash scripts/formal/verify-worker-retire-rescue.sh` → 13/13, PASS.

## C++ bridge (classified honestly: causal seam + adjacent bridges, NOT an exact realization)

No existing test deterministically asserts the rescue-chain TOPOLOGY
(retire with an unconsumed local ticket -> pending_spawn_ -> survivor
loop-top redispatch -> owner re-record). The available evidence, per the
review-fix audit:

- **Causal worker-death seam**: `WorkerState::loop_exited` (stored right
  after the retire epilogue; after the store the thread never touches
  WorkerState). `tests/phase_g_backend_progress_wake_test.cpp` observes
  it (mw_s2 no-progress terminate) — an ADJACENT bridge: its stranded
  ticket is recovered by steal (E8/#115 transport domain), not by the
  G1 rescue.
- **The retire epilogue executes incidentally** in every worker-loop
  exit (scheduler.cpp:1232-1262 runs on ALL loop exits), so the multi-
  worker lifecycle tests (`issue161_idle_dance_orphan_test.cpp`,
  `issue161_pub_erase_orphan_test.cpp`) traverse the rescue code path —
  but they belong to the idle-dance/publication-erase domains (e12) and
  assert no ticket topology.
- **D2 setup redistribution** (run() setup distributing pending_spawn_
  across fresh participants + owner re-record, the loop-top D1's
  isomorph): observed in `issue161_idle_dance_orphan_test.cpp` (:214,
  "Pre-run spawn -> pending_spawn_; run(2) distributes the first
  ticket").

A dedicated deterministic retire-rescue topology test (seam-park a fiber
on the retiring worker, retire, assert pending_spawn_ membership then
survivor consumption with the re-recorded owner) would be the exact C++
realization — recorded as a coverage note, not a defect (the epilogue is
a straight-line critical section; the model covers its protocol shape).

## Verdict

**AS-BUILT MODELED.** No C++ defect candidate. Allowed claims: the SC
abstraction satisfies the four laws; the three studied defect behaviors
(pre-G1 strand, copy-not-move, dropped owner re-record) each violate the
named law; the as-built epilogue excludes all three. Forbidden claims:
C++ weak-memory correctness; implementation bug-freedom; liveness under
adversarial scheduling; and "TLC green ⇒ C++ correct".
