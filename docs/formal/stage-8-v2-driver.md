# Stage 8V2.3 — Scheduler driver, replayed and re-adjudicated on the amended calculus

Campaign: FCB1-POST-V23-STACK-379-385 (Issue #375, §35). Calculus:
Stage-0-V2.3 as amended through Stage 5 (`CalcV2.lean`, unchanged by
this stage). Judgment layer: `JudgeV2.lean`. Chain: Stage-0 V2.3 →
Event → Semaphore (#378) → Mutex (#379) → Condition (#380) → RwLock
(#381) → Queue (#382) → Select (#383) → this scheduler driver (#384).
**RE-ADJUDICATED ON POST-#378 V2.3 AUTHORITY**: the pre-V2.3 stage-8
verdict is retired (§6); this stage's capability verdict is
RESEARCH/DEFER.

`BASE(run) = {}` — the Stage-0 §5 stage default (the driver reads no
substrate beyond the bare execution frame; the backlog, the worker
slot, and the driver state below are the run's own private state). No
base, no enlargement, no alternative.

## 1. Scope and subject

The scheduler driver: `src/async/scheduler.cpp` (the spawn entry
:131-159, `run_impl` :210-309, `worker_loop` :321-729, `run_next_on`
:731-745, the drain exit at one worker :642-663,
`classify_locked_impl` :1013-1039, the retire epilogue :708-714), and
the task fiber's bridge `src/async/fiber.cpp:25-34` (body, `make_done`,
final switch). Core surface modeled: **`run(1)` / `run_until_idle` at
one worker over plain tasks** — the frozen core instance. Outside the
core surface, recorded as extensions (§9): multi-worker `run`,
`run_live` (group.cpp:39), the classifier's non-quiescent legs, the
retire epilogue's wake scan, and the fiber-path spawn.

**The driver is not a `PrimLTS2`.** The frozen calculus's primitive
shape fits either the run-to-block fiber machine (one call, admit →
run/park/finish) or the fused external section (`extRun`); the drain
loop fits neither — external spawn sections interleave *inside* its
run-to-quiescence loop. The stage therefore models the driver as a
bespoke labeled transition system (`RunStep`, eight constructors, one
per code behavior) over the frozen observation model: the same
`Obs`/`Trace`/`SeqOK` language, the same `ApiSig` shape (`RunSig`:
`spawn`/`work`/`drain`; results `rSpawn`/`rDone`/`rReturned`), the
same run-relation discipline (`RunRuns` mirrors `PrimRuns2`'s
cons/split structure, `runRuns_cons`). The encoding side of the
capability judgments (`Encoding`, `SysStep`, `TracesEncS`) is reused
unchanged.

**The declared substrate extension (Stage-0 §9.3).** The driver state
the bare substrate does not carry is recorded, not smuggled:

```
DriverState { term : Bool, drainCaller : Option ExternalId, pending : List FiberId }
```

— the terminate flag (`global_terminate_`, committed at the drain
exit, cleared at `run_impl` entry), the in-flight drain record (the
single coordinated run), and the unclaimed-spawn queue
(`pending_spawn_`, scheduler.cpp:708-714). This is a recorded
extension of the substrate, sanctioned by Stage-0 §9.3, and carried by
both the Lean model and the TLA mirror.

Four structural facts shape the model:

* **Spawn's three-phase external shape.** Entry (the issue
  observation, no state effect, scheduler.cpp:131-134) → the fused
  `global_mtx_` section (gate-split: a run active routes the minted
  task to the backlog tail :142-148, no run leaves it unclaimed on
  `pending_spawn_` :157) → the physical return (the completion
  observation).
* **Dispatch is one driver-side unit.** The pop, `make_running`
  (fail-fast :732-734), and the worker count increment (:736) are one
  step emitting the task's issue observation (Stage-0 rule 2: a fresh
  dispatch emits the issue) — scheduler.cpp:332-347, :731-736.
* **The task completes through the bridge.** `workDone` is the entry
  bridge's `make_done` plus the final switch back (fiber.cpp:25-34);
  a plain task's driver-state effect is empty, so the slot goes
  straight from running to retired.
* **The drain boundary.** Entry issues the run observation, clears the
  terminate flag, and flushes `pending_spawn_` into the backlog
  (run_impl :229-247); the return commits the terminate flag and frees
  the drain, gated on quiescence — backlog empty, worker free (the
  classifier's quiescent exit at one worker, scheduler.cpp:1013-1039 +
  :642-663). The only drain completion carries `rReturned`.

## 2. Call-domain census (`extCap`/`extRun`)

| entry   | code                        | worker read | domain | model |
|---------|-----------------------------|-------------|--------|-------|
| `spawn` | scheduler.cpp:131-159 (entry :131-134; the fused section :141-158) | none (one `global_mtx_` section) | external-capable | `runExtCap spawn = true` |
| `drain` | scheduler.cpp:210-309 (`run_impl`), :642-663 (the exit) | issued from app threads | external-capable; its execution is the bespoke loop itself | `runExtCap drain = true` |
| `work`  | fiber.cpp:25-34 (the task body's own call) | the task fiber's own dispatch | fiber-bound (it IS the fiber's call) | `runExtCap work = false` |
| fiber-path spawn (a task body spawning) | scheduler spawn entry re-entered from a task | nested blocking call | outside the core surface | not modeled (§9) |
| `run_live`, multi-worker `run` | group.cpp:39, scheduler.cpp worker pool | the worker pool | outside the frozen core instance | not modeled (§9) |

Disclosures carried into the model: `spawn` and `drain` are the two
external call domains; the spawn's section is body-identical whether
the run is active or not except for the enqueue target (backlog tail
vs `pending_spawn_` — exactly scheduler.cpp:141-158's gate); the drain
is not a critical section but the run loop itself, so its "external
execution" is the disclosed bespoke loop.

## 3. The driver discipline

The whole driver discipline is one conjunction (the Lean `runSafe`,
the TLA invariants):

* **Task conservation**: every minted task is exactly one of queued,
  unclaimed (`pending_spawn_`), running, or retired — no lost or
  duplicated task (`runq.length + pending.length + occCount cur +
  retired.length = nextFiber`).
* **Fresh mints**: every recorded fiber id is below the mint counter.
* **Worker occupancy**: a task on the worker implies an active run.
* **Idle-backlog shape**: no active run ⇒ empty backlog — the backlog
  gains tasks only through in-run effects and the entry flush.
* **Terminate quiescence**: the terminate flag is committed only at
  the quiescent exit (`term → drainCaller = none ∧ runq = [] ∧ cur =
  none`).
* **Spawn-record shape**: in-flight spawn records carry `spawn` with
  result `none` (entered) or `rSpawn` (section ran).

Alongside stand the **silent-run shape lemmas** — the state-level
facts the separations ride on: with no in-flight spawn record a silent
run cannot move (`silent_run_nil`); with a completed record it cannot
move (`silent_run_some`); with an entered record and a run in flight
it is exactly the routed section (`silent_run_none` — the task is
minted onto the backlog tail or nothing happens).

## 4. Possession batteries (six, one per behavior class)

Explicit `RunRuns` step chains from `runInit` (batteries b3–b5
interleave the spawn section's silent step between observations — the
silent-run lemmas above are what make those chains provable):

* `battery_canonical` — from `runInit`: the empty drain (issue, return
  at quiescence; the terminate flag commits).
* `battery_fifo` — one drain, two tasks minted mid-run, dispatched and
  completed FIFO, then the quiescent return (the full run cycle).
* `battery_midDrain` — a spawn issued mid-run: its section lands on
  the backlog tail during the run, the spawn returns, the task runs to
  completion inside the same run.
* `battery_staleWindow` — the spawn's entry precedes the drain return
  and its section lands *after* it (the stale-classify window; the
  task is unclaimed by the returned-then-terminated run).
* `battery_seqDrains` — two sequential runs, with a post-terminate
  spawn going unclaimed between them (`pending_spawn_` accumulates
  while the terminate flag stands).
* `battery_empty` — the bare drain issue (the capability witness, §6).

## 5. Mutant battery (five independent fault classes)

* M1 `rsM1` — the drain-return quiescence gate dropped (SAFETY): the
  terminate flag commits with the worker occupied; `runM1_breaks`
  exhibits the safe start and the endpoint breaking the
  terminate-quiescence conjunct. TLA `MutQuiescence` killed by
  `InvTermQuiet`.
* M2 `rsM2` — the worker-free gate dropped from dispatch (SAFETY):
  a second task pops onto an occupied worker and the first vanishes
  from the conservation accounting; `runM2_breaks` exhibits the
  break. TLA `MutDuplicateDispatch` killed by `InvConservation`.
* M3 `runDrainM3` — the drain returns `rDone` instead of `rReturned`
  (RESULT-SEMANTICS): the state stays safe; the public result
  contradicts the outcome authority — the model's only drain
  completion carries `rReturned`. `runM3_wrong_result` exhibits the
  mutant step, both states safe, and the model-side unreachability of
  the wrong-result trace. TLA `MutDrainResult` killed by
  `InvDrainResult`.
* M4 `runDispatchM4` — the task's issue observation dropped, dispatch
  goes silent (TRACE-REMOVAL): every state invariant still holds
  (every other facet is the model's); `runM4_removes` exhibits the
  definitional removal (the model's dispatch emits the issue, the
  mutant's emits nothing). TLA `MutSilentDispatch` completes cleanly
  with `NotWorkIssue` — no step emits a `work` issue there — while
  `DriverCoreCovDispatch` certifies the issue reachable in the
  correct model. The completion discipline is excluded from that
  configuration: the removed issue orphans the completion, and that
  break IS the removal, not an independent fault.
* M5 `runEffectM5` — the spawn section fixes the result without
  minting the task (RESULT-BINDING): the returned `rSpawn` is
  unbacked — no task exists behind it — and the drain's quiescence
  gate is satisfied with the model's minted task vaporized.
  `runM5_unbacked` exhibits the definitional removal (the model's
  section mints, the mutant's does not) *and* the model-side
  separation: the four-observation unbacked trace (drain issue, spawn
  issue, spawn completion, drain completion) is unreachable in the
  model — the silent-run lemmas force the mint, and the mint blocks
  the quiescent return. TLA `MutUnbackedMint` is killed by
  `NotUnbacked` — the trace becomes reachable there.

## 6. Capability adjudication

* **OLD VERDICT — THEOREM B** (pre-V2.3 stage 8, branch
  `formal/fcb1-stage-8` at `6d02a9e3`): the scheduler driver is
  irreducible to the bare substrate; every encoding mismatches.
* **THE OLD PROOF IS REFUTED.** The proof rested on the
  completion-shadow projection applied to the drain's trace — the
  same device the Stage-6/7 re-adjudications retired:
  `tracesEnc_shadow_false` (CalcV2.lean) exhibits an encoding
  possessing a completion-shadowed trace, so no THEOREM-B-shaped
  conclusion can rest on it. The old artifact is additionally scoped
  to a pre-reset model shape; it has no inheritance right either way.
* **INVALID UNDER STAGE-0 V2.3.** With the shadow gone, the old
  universal mismatch proof has no premises to stand on; the verdict
  it carried is retired, not appealed.
* **NEW SEARCH RESULTS.** What the frozen method supports are two
  conditional, per-encoding-class statements (`run_capability_defer`):
  - Over-production for the class that declares the task's call
    externally callable: the driver never emits an external `work`
    issue (the only step emitting a `work` issue is `dispatch`, and it
    addresses the dispatched task's own fiber — `run_not_extWork`),
    while every such encoding emits the bare issue at its entry step
    (`run_over_produces_of_extCap`, separator `extWorkTrace`,
    non-vacuity instance `encExtWork_over`).
  - Under-production for the class that declares the run entry point
    internal: the driver emits the external drain issue plus its
    `rReturned` completion (`battery_empty` + `seqOK_b6`), while no
    such encoding can emit that issue at all
    (`run_under_produces_of_no_extCap`, the issue-step inversion
    `drainCap_true_of_drainIssue`, non-vacuity instance
    `encNoDrain_under`).
  Neither statement is named THEOREM B and neither closes the universal
  question. The encoding class is inhabited (`encRun`,
  `run_encoding_class_inhabited`).
* **VERDICT — RESEARCH/DEFER.** RESEARCH/DEFER does NOT mean
  reducible. RESEARCH/DEFER does NOT mean probably irreducible. The
  exact open boundary:

  > Does there exist an Encoding BaseOpsSig.none RunSig whose
  > disciplined trace language is observationally equivalent to the
  > driver core's disciplined language (`TracesRunS`) under Stage-0
  > V2.3? No such reduction is presently constructed. No universal
  > mismatch proof is presently known. The previous universal
  > mismatch proof is invalid because its completion-shadow lemma is
  > false under V2.3.

## 7. Model decisions recorded by this build

No production-vs-model fact conflict surfaced: the model was built
directly from the current sections. The decisions below are disclosed
modeling choices, not corrections.

* **Bespoke LTS, extension recorded.** The `PrimLTS2` shape is not
  forced onto the drain; `RunStep` is a parallel transition system
  over the frozen observation model, and the driver state is the
  declared Stage-0 §9.3 extension (`DriverState`, §1).
* **No clock in the core instance.** The core registers no timers;
  time advances only at idle points, which are extension steps here
  and carry no observations (Stage-0 rule 7). The TLA mirror has no
  environment actions for the same reason.
* **Dispatch is atomic at one worker.** The pop, `make_running`, and
  the count increment are one step; the fail-fast count ceiling is
  outside the frozen core (its breakage is the multi-worker
  extension's concern).
* **The spawn gate is the enqueue target.** The section's only
  run-dependence is where the minted task lands (backlog tail vs
  `pending_spawn_`); the section itself is body-identical, matching
  scheduler.cpp:141-158.
* **Per-caller blocking is folded into the TLA gates.** The raw Lean
  run language allows a caller to issue a spawn while its drain is in
  flight (the `SeqOK` trace filter removes such traces in
  `TracesRunS`); the TLA invariant is state-checkable, so the gates
  the C++ calling threads obey (a caller blocked in `run` cannot
  spawn; a caller with an un-returned spawn cannot run) are folded
  into the action guards. The TLA language is therefore the
  disciplined sub-language by construction.

## 8. The TLA mirror (`formal/tla/DriverCore.tla`)

One action per `RunStep` constructor, the census of §2 as comments,
`Boot` selecting two initial states (`"prim"` — the runInit-faithful
start; `"loaded"` — the drain already in flight over one queued task,
the b2 battery's mid-run start; relation-reachable, not
runInit-reachable, the documented battery disclosure). Fuel bounds:
two minted task fibers (`nextFiber < 2` at the mint sites) and
`MaxHistory` observations. Safety matrix: `TypeOK`,
`InvConservation`, `InvIdBounds`, `InvOccupied`, `InvIdleBacklog`,
`InvTermQuiet`, `InvExtShape`, `InvCompDiscipline`.

Configurations and outcomes: safety `DriverCore` (prim),
`DriverCoreLoaded` — both clean; coverage
`DriverCoreCov{EmptyDrain,FifoTwo,EffectIn,Flush,Stale,PostTerm,
Dispatch}` — each violated on its negated witness (the witness is
reachable); kills `DriverCoreMutQuiescence` → `InvTermQuiet`,
`DriverCoreMutDuplicateDispatch` → `InvConservation`,
`DriverCoreMutDrainResult` → `InvDrainResult`; separations
`DriverCoreMutSilentDispatch` — clean with `NotWorkIssue` asserted
(full state safety intact, the completion discipline excluded as
disclosed above) — and `DriverCoreMutUnbackedMint` — violated on
`NotUnbacked` (the state stays safe, the unbacked trace becomes
reachable; the model's unreachability is the Lean separation).

TLC lessons recorded by this mirror (mirror-implementation facts, not
model-vs-C++ disputes):

* **A variable listed in both the assignment and the UNCHANGED set
  silently disables the action.** `WorkDone` assigned `retired'` and
  also listed `retired` as unchanged — the action became unsatisfiable
  and TLC reported nothing wrong until the `CovFifoTwo` coverage
  configuration came back clean instead of violated. The state count
  stands at seventeen for every action: every primed variable is in
  exactly one of the assignment or the UNCHANGED set.
* **A sequence of ids has elements, not records.** `Head(runq)` is the
  fiber id itself — the first dispatch projection read `.fiber` off a
  string and TLC rejected the action outright (a loud failure, unlike
  the silent one above).
* **The coverage battery is the mutant battery's control group.** Each
  trace-removal or separation claim pairs with a coverage
  configuration that certifies the witness reachable in the correct
  model — the pairing caught the disabled `WorkDone` (the
  `CovFifoTwo` clean run) exactly as it caught nothing in earlier
  stages because nothing was disabled there.

## 9. Modeling disclosures

* **One worker.** The frozen core instance is the serialized
  single-worker run; the production surface's multi-worker `run` (the
  worker pool, the count ceiling, the cross-worker hand-offs) is a
  recorded extension, outside the model. Whether the multi-worker run
  can be adjudicated on the frozen method at all is a campaign-level
  question — deferred to #385 with the queue and select rows.
* **`run_live` is out of the surface.** The live-mode entry
  (group.cpp:39) shares the drain's loop but not its issue/return
  boundary; modeling it would double the drain surface without a
  current consumer.
* **The classifier's non-quiescent legs are outside the core.** The
  model carries the quiescent exit condition (backlog empty, worker
  free) as the drain-return gate; the mw_s1/mw_s2 legs and the
  terminate dance's intermediate classifications (:1013-1039,
  :642-663) collapse into that gate at one worker over plain tasks.
* **The retire epilogue's wake scan is out of the surface.** At one
  worker over plain tasks the epilogue's routing (scheduler.cpp
  :708-714) is exactly the EffectOut/EffectIn gate split; the wake
  scan over parked fibers is the Stage-4/5 cores' discipline, not the
  driver's.
* **Fiber-path spawn is excluded as a SeqOK conflict.** A task body
  calling spawn makes the task fiber a blocking caller with an
  in-flight call — a nested blocking call inside the dispatched task's
  own observation window; the frozen per-caller discipline has no
  shape for it, and the modeled surface excludes it (the C++ surface
  documents the same restriction for blocking calls in tasks).
* **No timers.** The core instance registers none; there is nothing
  for `expire`/`onTick` to carry (Stage-0 rule 7; §7).
* **The loaded boot is pre-Init** — the same prefix-scoped
  `InvCompDiscipline` exception the Stage-4/5/6/7 cards document: the
  boot's drain instance has no in-trace issue.
* **Fuel bounds are disclosed.** Two minted fibers and `MaxHistory`
  observations bound the TLA state space; the Lean model is
  unbounded.

## 10. Gates and verdict

* `scripts/verify_formal.sh` — **PASS** (lake build; no sorry/admit;
  every exported driver theorem depends only on the allowed standard
  principles — `[propext, Classical.choice, Quot.sound]` or fewer).
* `scripts/verify_tla.sh` — **PASS** (stages 1–7 re-run green; Stage
  8: 2 safety boots clean, 7 coverage witnesses violated, 3 mutants
  killed on their exact intended invariants, 1 trace-removal
  separation clean, 1 result-binding separation violated on its
  witness).
* `CalcV2.lean` / `JudgeV2.lean` — unchanged.
* Fresh-context adversarial review: **PENDING** (this section is
  updated with the reviewer's verdict before merge; the PR must not
  merge with PENDING standing).
* Verdict: **STAGE8_SEMANTICS_PASS / CAPABILITY_RESEARCH_DEFER /
  READY_FOR_STACK_CONTINUATION.**
