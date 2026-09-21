> **HISTORICAL — ARCHIVED.** This record belongs to the FCB1 formal-capability
> research campaign on the superseded async-primitive architecture. The
> [v1 Architecture and Contract Reference](../../../explicit-io-v1-final-decision.md)
> is the sole normative root; formal evidence for v1 is being re-mapped by
> issue #391, not inherited from this campaign. Nothing below establishes v1
> conformance, architecture ownership, or current authority.

# Stage 3V2.3 — AsyncMutex core, replayed on the split return window

Campaign: FCB1-METHOD-CORRECTIVE-1 (Issue #375). Calculus: Stage-0-V2.3
(`formal/Sluice/Formal/CalcV2.lean`, `Caller := fiber | ext`, three-phase
external calls, per-stage `extCap`/`extRun` census, and the V2.3 split of a
fiber call's critical section (`fiberEffect`) from its physical return
(`fiberDone`)). Judgment layer: `JudgeV2.lean`. Chain: Stage-0 V2.3 →
Event replay → Semaphore replay (#378) → this mutex replay.

## 1. Scope and subject

AsyncMutex: `include/sluice/async/async_mutex.hpp`,
`src/async/scheduler_mutex.cpp`. Core surface modeled: `lock` (bool: the
`WaitNode` outcome the caller inspects after the call returns), `unlock`
(void, owner-gated), `try_lock` (bool, non-suspending), and `cancel`
(bool, external-capable: cancels a queued locker). The deadline extension
(`lock_until`) is outside the core surface, exactly as Semaphore's
`acquire_until` was at Stage 2.

## 2. Call-domain census (`extCap`/`extRun`)

| entry      | code                            | worker read | domain           | model                                |
|------------|---------------------------------|-------------|------------------|--------------------------------------|
| `lock`     | scheduler_mutex.cpp:38-40       | `g_worker` :38-40, `commit_suspend_locked` :70, `context_switch` :76-79 | fiber-bound | `extCap = false`, `extRun = none` |
| `unlock`   | scheduler_mutex.cpp:183-197     | `g_worker` :184-186 | fiber-bound (owner-gated :188) | `extCap = false`, `extRun = none` |
| `try_lock` | scheduler_mutex.cpp:22-24       | `g_worker`/`ws->current` :22-24 | fiber-bound | `extCap = false`, `extRun = none` |
| `cancel`   | scheduler_mutex.cpp:145-156     | none (`global_mtx_` + queue mutex only) | external-capable | `extCap = true`, `extRun = mutexExtRun` |
| `lock_until` | timed extension               | `g_worker`  | fiber-bound      | extension, outside the core surface |

The modeled `mutexRun` mirrors the C++ section shapes: `lock`'s inline
grant runs exactly when the fresh node lands at the queue head and the
owner slot is free (:51-58); `unlock` hands off to the queue head
(`mutex_handoff_one_locked` :158-181) or leaves the slot free; `cancel`
publishes a `false` outcome into the queued node or reports not-found.

## 3. The interleaving windows and the claim

Window A (external effect/return): an external `cancel` issues at
`extApply`, effects the dequeue-and-publish at `extEffect`, and its
completion observation lands only at `extDone` — fiber completions
legitimately interleave between all three. `mutex_possesses_cancel`
exhibits the full straddle; `CovCancelChain`/`CovCancelMiss`/
`CovExtWindow` are the TLA-side certificates of the same windows.

Window B (fiber effect/return, the V2.3 seam): the fiber's critical
section fixes its result at `fiberEffect` while the fiber keeps the worker
baton; `extApply`, `extEffect`, `extDone`, and the fiber's own `fiberDone`
are exactly the steps legal in that window. For the mutex the seam's load
is carried by the unlock handoff: the handed-off winner's `true` outcome
sits in `resolved` (the one-shot `WaitNode::resolve_` state) from the
unlocker's `fiberEffect` until the winner's own resumed finish, and the
owner slot transfers in that same window — the winner owns the mutex
before it is running. `CovQ` is the TLA certificate (handoff, external
entry, resume, resume-completion all witnessed); the ghost machine's
`wseq = 5` is the V2.3 seam witness.

The guarantee, restated over both windows:

* **`mutexExclHonored`**: every completed grant completes only when the
  completed releases have caught up — at every completion observation of
  a `lock`/`try_lock` returning `true`, the completed releases so far
  cover all earlier grants. Mutual exclusion follows as the global prefix
  bound `grantsAll ≤ releasesAll + 1`: the counts move only at
  completions, and the bound makes a second concurrent holder's grant
  unstatable.
* **`mutexPrim_guarantees`**: `Guarantees mutexPrim mutexExclHonored`.
* Release-side ownership (no `unlock` completes without a backing grant;
  at most one owner; a delivered record belongs to the owner) is the TLA
  mirror's half of the safety split (`MutexCore.tla` invariants below) —
  the Lean calculus dispatches `unlock` only by the owner
  (`mutexAdmit`), so the guarantee side never sees a phantom release to
  refute.

Accounting detail carried from the Stage-0 result-semantics corrective: a
returning fiber still holds the baton, so the in-window release credit
(`retUnlock`, `retUnlockAll`) counts only a returning `munlock` whose
result is already `runit` — a `try_lock`'s or a refused call's return
window contributes nothing, mirroring the `isRelease` trace predicate
exactly (the C++ `unlock` is void, and every modeled `unlock` section
fixes `runit` at `fiberEffect`).

## 4. What is proved (`formal/Sluice/Formal/MutexV2.lean`)

* `mutex_mirror` — the conservation ledger, by induction over the
  `PrimStep2` constructors, per fiber and globally: trace grants plus
  in-window grant credit (the returning slot, the delivered true records)
  equal trace releases plus in-window release credit and the owner slot,
  at every run endpoint. Six carried facts: `hnd` (the wait queue has no
  duplicates — one registration per fiber), `hown` (no queued fiber is
  the recorded owner), `hM3` (a running `lock` is admitted only against a
  free owner slot; a running `unlock` only by the owner), `hstale`
  (stale runq entries are locks — the handed-off winners), `hparked`
  (parked calls are locks), and `hext` (an external entry whose result is
  fixed is a `cancel` — the only external critical section — so an
  external completion never adds grant or release credit). `hext` is the
  sixth fact the semaphore's mirror did not need: without it `extDone`
  breaks the identity.
* `mutexRuns_split` — every run splits at an arbitrary observation cut.
* `mutexStep_grant_credit` — a grant-completing step leaves at least one
  release's worth of accounting behind (the owner slot, or the in-window
  `retUnlock` credit of the unlocker whose handoff is being consumed).
* `mutexPrim_guarantees` — the guarantee, by splitting the run at the
  completing grant and applying the mirror from `primInit`.
* Possession batteries: `mutex_possesses_grant` (inline grant and its
  completion) and `mutex_possesses_cancel` (cancel of a queued locker:
  park, unlock handoff, external cancel straddling the window, the
  cancelled resume completing `false`, then the owner's unlock) — the
  windows are inhabited, not vacuous.
* `mutexMutant_not_guarantees` — a mutant whose `lock` succeeds whenever
  the wait queue is empty, ignoring the owner slot, violates
  `mutexExclHonored` (two grants, no intervening release); the guarantee
  has teeth.

Axiom audit (`scripts/verify_formal.sh`): all of the above —
`mutexPrim_guarantees`, `mutex_mirror`, `mutexRuns_split`,
`mutexStep_grant_credit`, both possessions, the THEOREM-B quartet below,
and `mutexMutant_not_guarantees` — depend on exactly
`[propext, Quot.sound]`.

## 5. Disposition of the THEOREM-B question

AsyncMutex — **THEOREM B** (`mutex_irreducible`): irreducible to
`BASE(AsyncMutex) = {Semaphore}`. The separating fact is the owner-gated
*admission* of `unlock`: a non-owner `unlock` is a caller precondition
violation (the `assert` at scheduler_mutex.cpp:188), so the primitive
never issues it — `mutexAdmit` refuses the dispatch — while every
encoding's fiber machine dispatches any submitted call unconditionally.
The witness `unlockIssueTrace` (a bare `unlock` issue by a fresh fiber,
trace-well-formed per `seqOK_unlockIssue`) is produced by every encoding
over the semaphore substrate (`mutex_over_produces`, via the closed
`BaseOpsSig`/`Encoding` class) and by no run of the primitive
(`mutex_not_unlockIssue`, with `mutex_silent_prefix` lifting the
dispatch-level refusal to the trace level). `mutex_irreducible` closes
via `irreducible_of_always_over`.

This replaces the old branch's argument entirely: the pre-reset THEOREM B
rested on a V1 completion shadow, which the V2 calculus refutes; nothing
is inherited from it. LockGuard's THEOREM A over the
synchronous `Mutex` (the freeze's mandated first nonempty-`BASE(P)`
re-export test) is a charter obligation still open; this stage neither
relies on it nor delivers it.

Non-vacuity: the closed encoding class is inhabited —
`encSemMutex` is the natural binary-semaphore encoding (`lock` →
`acquire`, `unlock` → `release`; `try_lock`/`cancel` have no completing
program on this substrate), and `encoding_class_inhabited` records the
inhabitance in the axiom audit. The over-production fires at the encoding
machine's dispatch step, before any program runs, so it binds this
encoding — the closest an implementation can get to a mutex over the
base — exactly as it binds every other element of the class: the
separator is the discipline itself, not any particular encoding's bug.

## 6. Redelivered from the old stack

The old branch (`origin/formal/fcb1-stage-3`, head `1d453e7f`) carried
`formal/Sluice/Formal/AsyncMutex.lean` and
`docs/formal/stage-3-async-mutex.md`, both built on the pre-reset
calculus and superseded models. Per the campaign charter they have no
inheritance right; this stage is rebuilt from the current C++ as
`formal/Sluice/Formal/MutexV2.lean` and this card. Nothing was ported
from the old branch.

## 7. The TLA mirror (`formal/tla/MutexCore.tla`)

A TLC-executable mirror of the same action set, written from this card's
census and the V2.3 Lean machine (not copied from any pre-reset model).
Constants: fixed caller sets `Fibers = {"f0","f1"}`, `Exts =
{"e0","e1"}` (Lean's `nextFiber` minting is dropped: a caller re-enters
once its previous call is no longer in flight); observation fuel
`MaxHistory = 6`, raised to `7` in the three cfgs whose certificate
needs one more observed step (the witness machine `MutexCoreWitness`
and the two handoff-resume coverages `MutexCoreCovQ`,
`MutexCoreCovCancelChain`). `Spec == Init /\ [][Next]_vars` — **no
fairness is assumed anywhere**; every claim below is safety. Four
constant mutant switches (`MutGrantHeld`, `MutHandoffOwner`,
`MutCancelTrue`, `MutFusedReturn`), all `FALSE` in the reference
configuration.

### 7.1 Action crosswalk (Lean `PrimStep2` ↔ TLA)

| Lean constructor (`CalcV2.lean`) | MutexCore.tla action(s) | observable |
|----------------------------------|--------------------------|------------|
| `submit` | `FiberSubmit(f,c)` | silent |
| `dispatchFresh` | `FiberDispatch` | fiber issue; `Admits(c,f)` gate: `lock`/`try` need `owner # f`, `unlock` needs `owner = f` |
| `fiberEffect` (`lock` inline grant) | `LockTake` | silent; owner slot set, slot → returning **with the baton** |
| `fiberEffect` (`try_lock` success/refusal) | `TryTake` / `TryRefuse` | silent; result fixed, owner taken only on success |
| `runPark` (`P.run` = none) | `LockPark` | silent; appends to `waitq`, slot freed with the suspension |
| `fiberEffect` (`unlock` handoff/free) | `UnlockHandoff` / `UnlockFree` | silent; owner cleared, the winner's `true` record published to `resolved` on handoff |
| `fiberDone` | `FiberDone` | fiber completion; baton freed |
| `dispatchResumed` | `FiberResume` | silent dispatch of a published winner |
| `finishDone` | `FinishResumed` | resumed call's completion; consumes the delivered record |
| `extApply` | `ExtIssue(x,w)` | external cancel issue; names its target waiter |
| `extEffect` (`cancel` hit/miss) | `ExtCancelHit` / `ExtCancelMiss` | silent; hit dequeues the waiter and publishes `false` into `resolved` |
| `extDone` | `ExtDone(x)` | external completion |
| `envTime` / `expire` | omitted | the mutex has no timers; `onTick` is the identity |

The worker-slot encoding: Lean `FSlot.running d b` ↔ `cur` with
`phase = "run"` (`resumed` ↔ `b`); Lean `FSlot.returning d r` ↔ `cur`
with `phase = "ret"` (`result` ↔ `r`, always-record — the sentinel-free
form TLC requires). The critical V2.3 property — a returning fiber still
holds the baton — is `FiberDispatch`/`FiberResume` requiring
`cur = NoCur`: no second fiber dispatches while any fiber sits in
`phase = "ret"`, while `ExtIssue`/`ExtEffect`/`ExtDone` and `FiberDone`
itself are legal in that window. `Admits` is the THEOREM-B separator in
executable form: `unlock` may not even be *issued* by a non-owner, so
`unlockIssueTrace` is unreachable in this machine, matching
`mutex_not_unlockIssue`.

### 7.2 Safety invariants

| TLA invariant | content | Lean analogue |
|---------------|---------|---------------|
| `TypeOK` | every variable in its state domain | type discipline |
| `InvGrantBound` | completed grants `≤` completed releases `+ 1` | `mutexExclHonored` prefix bound |
| `InvReleaseBacked` | completed releases `≤` completed grants (no phantom unlock) | the guarantee's release side |
| `InvBalance` | exact conservation: trace grants + in-window grant credit = trace releases + in-window release credit + owner slot | `mutex_mirror` (equality: the TLA machine omits the resumed-repark over-approximation, see below) |
| `InvQueueNoDup` | no fiber parked twice; no fiber queued twice in the runnable queue | `hnd` |
| `InvQueueOwnership` | parked/queued/disjoint from current and owner; stale runq entries are locks | `hown` + `hstale` |
| `InvRecordOwner` | at most one delivered true record, and it belongs to the owner | `hM3` + handoff shape |
| `InvCompDiscipline` | every completion has a same-caller same-call prior issue with no intervening completion | no completion before issue, no double completion |

Reference results (`scripts/verify_tla.sh`, Stage 3V2.3 block):

* **Safety matrix**: `mutex-main` (all switches off, all eight invariants
  above) completes cleanly — 1,714,569 states generated, 922,931
  distinct, depth 20.
* **V2.3 seam witness**: the ghost machine (`wseq` 0→5, `wf`/`wx`/
  `wlive` anchoring steps 1 and 5 to the same unlock instance) reaches
  step 5 — `InvWitness` is violated in the correct model
  (`mutex-witness`), certifying the straddled return window is inhabited.
  Under the fused-return mutant the ghost cannot pass step 4→5 (a fused
  handoff completes the call in one transition; no `FiberDone` exists to
  take step 5).
* **Coverage** (each a negated conjunction; each cfg checks it as an
  invariant and TLC must violate it): `CovW1` (one grant + one release),
  `CovQ` (handoff → external entry → resume → resumed completion),
  `CovTryBoth` (try_lock success and refusal both observed), `CovQ`'s
  cancel-side sibling `CovCancelChain` (cancel of the parked head, resume
  completes `false`), `CovCancelMiss` (cancel of a non-waiter), and
  `CovExtWindow` (a cancel issue/completion straddling a fiber
  completion). All six are violated as expected.
* **Safety mutants** — three independent fault classes, each killed on
  its expected distinct invariant: `MutGrantHeld` (lock grants without
  checking the owner slot) dies on `InvBalance`; `MutHandoffOwner`
  (unlock handoff skips the owner-slot transfer) dies on
  `InvRecordOwner`; `MutCancelTrue` (a cancel hit publishes `true` — a
  grant — instead of `false`) dies on `InvBalance`.
* **Fused-return mutant** (`MutFusedReturn`, `FiberEffect` + `FiberDone`
  fused into one step, the pre-V2.3 shape): all eight safety invariants
  hold cleanly — and the seam witness becomes unreachable. That
  unreachability is the separation certificate for the V2.3 split.

## 8. Modeling disclosures

* **The `WaitNode` outcome bit.** The modeled `lock` result is the
  outcome the caller can inspect after the call returns
  (`node.was_woken()` / `was_cancelled()`, wait_node.hpp): `true` =
  granted (inline or handed off), `false` = cancelled. Without that bit
  the completion surface cannot distinguish a granted `lock` from a
  cancelled one and the ownership accounting is not statable.
* **Duplicate registration / self-relock.** `mutexPark` refuses a
  duplicate registration and `mutexRun` routes a lock whose caller
  already owns the mutex to the completion path (the handed-off winner's
  resume). Both guards sit on transitions the frozen machine cannot reach
  for this primitive (one in-flight call per fiber; a winner's owner is
  already recorded), so no reachable trace changes; they make `waitq`
  exactly the set the code's `WaitQueue` is (one registration per fiber).
* **Resumed repark (Lean-only over-approximation).** As in the semaphore
  stage, the calculus's `runPark` is unguarded on the resumed bit, so a
  resumed *cancelled* locker may re-run its inline paths in the Lean
  model — an over-approximation confined to that corner. The TLA mirror
  has no such step (`LockPark` requires a fresh call), which is why
  `InvBalance` there is an exact equality while the Lean mirror is
  stated to tolerate the same corner. The over-approximation is in the
  C++'s favor: more traces admitted, none removed.
* **External surface.** `cancel` is the only external critical section
  (`mutex_cancel` takes no worker), hence `hext`'s shape: a fixed-result
  external entry is a cancel. The `assert`-gated owner precondition of
  `unlock` is modeled as admission (`mutexAdmit`/`Admits`), not as a
  reachable failure step.
* **Fiber-issued `cancel` stalls.** `mutexAdmit` admits an `mcancel`
  from a fiber, but the fiber-side run functions have no section for it
  (in the C++ `AsyncMutex::cancel` is callable from any context; a fiber
  calling it would complete inline, as the semaphore's fiber-origin
  `release` does). In the Lean model such an issue is a stuck prefix: no
  step applies to it and no observation follows. No proved claim depends
  on this corner — the guarantee constrains only grant completions and
  the mirror conserves across the stuck prefix — and the TLA machine
  omits fiber-origin cancel entirely. The honest shape is a
  non-faithfulness confined to an issue with no completion, disclosed
  rather than silently repaired; the campaign's `cancel` census domain
  (external-capable) is unaffected.
