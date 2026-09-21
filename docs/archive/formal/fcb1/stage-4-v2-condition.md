> **HISTORICAL — ARCHIVED.** This record belongs to the FCB1 formal-capability
> research campaign on the superseded async-primitive architecture. The
> [v1 Architecture and Contract Reference](../../../explicit-io-v1-final-decision.md)
> is the sole normative root; formal evidence for v1 is being re-mapped by
> issue #391, not inherited from this campaign. Nothing below establishes v1
> conformance, architecture ownership, or current authority.

# Stage 4V2.3 — AsyncCondition core, replayed on the amended calculus

Campaign: FCB1-POST-V23-STACK-379-385 (Issue #375, §30–§31). Calculus:
Stage-0-V2.3 as amended by the Stage-4 brake (`CalcV2.lean`: `park`
publishes the wakes its pre-suspension section readied, and the
section-ending steps take those wakes as an order-preserving `Wakes`
sublist of the parked set — the amendment that the Stage-4 replay's
finding motivated).
Judgment layer: `JudgeV2.lean`. Chain: Stage-0 V2.3 → Event → Semaphore
(#378) → Mutex (#379) → this condition replay (#380).

`BASE(AsyncCondition) = {AsyncMutex}` — the Stage-0 §5 stage default,
unchanged by this stage. No enlargement, no shrinking, no alternative
base.

## 1. Scope and subject

AsyncCondition: `include/sluice/async/condition.hpp`,
`src/async/scheduler_condition.cpp`. Core surface modeled: `wait` (bool:
the `WaitNode` outcome the caller inspects after the call returns —
`true` woken, `false` cancelled), `notify_one` (void), `notify_all`
(void, returning the drained count to the accounting), and `cancel`
(bool, external-capable). The deadline extension (`wait_until`) is
outside the core surface, exactly as `acquire_until`/`lock_until` were
at Stages 2–3.

## 2. Call-domain census (`extCap`/`extRun`)

| entry        | code                       | worker read | domain           | model                                  |
|--------------|----------------------------|-------------|------------------|----------------------------------------|
| `wait`       | condition.hpp:63-78 + scheduler_condition.cpp:73-109 | `g_worker` (the prepare/finish pair), the `assert(owner == me)` :79-80 | fiber-bound, owner-gated | `extCap = false`, `extRun = none` |
| `notify_one` | scheduler_condition.cpp:145-148 | none (`global_mtx_` only) | external-capable | `extCap = true`, `extRun = condExtRun` |
| `notify_all` | scheduler_condition.cpp:150-162 | none (`global_mtx_` only) | external-capable | `extCap = true`, `extRun = condExtRun` |
| `cancel`     | scheduler_condition.cpp:164-174 | none (`global_mtx_` + queue mutex) | external-capable | `extCap = true`, `extRun = condExtRun` |
| `wait_until` | timed extension            | `g_worker`  | fiber-bound      | extension, outside the core surface    |

The modeled sections mirror the C++ shapes: `notify_one` wakes at most
the head; `notify_all` drains the whole queue and its result carries the
drained count; `cancel` publishes `cancelled` into the named queued node
or reports not-found. The embedded mutex slot (`owner_`) is modeled only
for the wait-reacquire discipline: it transfers at a `wait`-entry
release (handoff to a reacquire-blocked waiter, else freed —
`mutex_handoff_one_locked` + `owner = nullptr` at
scheduler_condition.cpp:63-65) and at a reacquire (take of a free slot
at the resumed finish).

## 3. The interleaving windows and the claim

Window A (external effect/return): an external `notify_one`/
`notify_all`/`cancel` issues at `extApply`, effects its section at
`extEffect` (waking its targets there), and its completion observation
lands only at `extDone` — fiber completions legitimately interleave
between all three. The batteries exhibit the full straddle
(`cond_battery_fast_path` completes the waiter between the notify's
section and its return).

Window B (the Mesa reacquire, the condition-specific seam): a woken
waiter's resume is a *second suspension of the same call*. A resumed
`cwWaiting` waiter whose reacquire blocks re-parks on the mutex queue
(`condPark`'s `cwWaiting` branch, phase → `cwReacq`); the per-fiber
phase makes the two park sites distinguishable. The model
over-approximates here: it re-parks regardless of the slot's state,
where the C++ `mutex_.lock()` over a free slot would take it inline —
a strict superset of behaviors, safe in both directions (the safety
invariants hold over the superset; a witness unreachable in it is
unreachable in the C++-faithful subset). The slot comes back to
the waiter either free (its finish takes it — `condFinish`'s
`cwWaiting` branch) or handed off by another waiter's release (the
`cwReacq` finish, which consumes the outcome without a take). The
handoff chain is witnessed end-to-end by `CovHandoff` (TLA) and by the
model-level runs in the batteries.

The guarantee, restated over both windows — this stage's safety split:

* **`condWakeBacked`** (direction A, no spurious completion): at every
  prefix, completed `wait` calls are covered by the wakes their
  resolvers can have published. The credit ledger is the frozen one: a
  `notify_one` completion credits at most one wake (the unit result
  cannot distinguish 0 from 1, so the safe upper bound is taken), a
  `notify_all` completion credits exactly its drained count (which its
  result observation carries), a `cancel` completion credits one iff it
  hit (result `true`), a cancel miss credits nothing. Credit accrues
  only where the effect has happened — never at entry (`extApply`),
  never at issue, never at dispatch — and a pending-to-completed
  transition only *moves* credit between locations (the returning
  worker's fixed result; external records past their section), never
  double-counts.
* **`cond_mirror`** — the conservation ledger behind it, by induction
  over the `PrimStep2` constructors in the accumulator form: for every
  run endpoint, `acc + trace completions ≤ acr + trace wake credit +
  in-window credit (the returning slot's resolver result + external
  records past their section)`, carried with the queue-possession
  invariants (a returning slot is never a `cwait`; no external record
  is a `cwait`).
* Release-side and reacquire-side ownership (a `cwWaiting` completion
  leaves the caller owning the slot; a completed `wait` consumed a
  resolution record whose outcome is the completed bit; a fresh
  `wait`'s release over an empty mutex queue frees the slot) are the
  TLA mirror's half of the split (`InvFinishOwned`, `InvFinishBacked`,
  `InvQueueOwnership` below) — and are proved outright at the facet
  level in Lean (`condFinish_takes_owner`, `condFinish_consumed`,
  `condPark_frees_slot`).

## 4. Direction B: notify_all completeness

`condWakeBacked` is an upper bound; it cannot catch an under-producing
resolver. The under-wake seam is closed by two defequations — the drain
facets state exactly that the wake list *is* the whole queue and the
queue is left empty (`condRun_notifyAll_drains`,
`condExtRun_notifyAll_drains`) — plus the battery-B run showing the
readied waiters actually complete. A mutant `notify_all` failing either
equation is killed outright (`condM2_drains_refuted`); one keeping the
facet but readying fewer waiters is killed by the witness separation
(`cond_battery_broadcast`'s endpoint is unreachable for it — the TLA
`CondCoreMutDrainOne` certificate below).

## 5. What is proved (`formal/Sluice/Formal/ConditionV2.lean`)

* `cond_irreducible` — THEOREM B, unchanged from the pre-#380
  adjudication and re-verified on the amended base: the owner-gated
  *admission* of `wait` (`assert(owner == me)`,
  scheduler_condition.cpp:79-80) means the primitive never issues a
  `wait` from a non-owner (`condAdmit` refuses the dispatch) while
  every encoding's fiber machine dispatches any submitted call
  unconditionally. Witness: `waitIssueTrace`, produced by every encoding
  (`cond_over_produces`) and by no run of the primitive
  (`cond_not_waitIssue`).
* `cond_mirror` — the conservation ledger (§3), accumulator form. The
  state-slack form used by the mutex stage is *not* inductive here: a
  resolver's effect grows the window credit before its completion
  contributes the wakes that pay for it, so slack cannot be transferred
  step-by-step; the accumulator premise
  `acc + resolved ≤ acr + window-credit` is carried instead and
  discharged per constructor.
* `condWakeBacked` — direction A, from `primInit` via the mirror with
  zero accumulators.
* `condRun_notifyAll_drains` / `condExtRun_notifyAll_drains` —
  direction B's facet equations (§4).
* Possession batteries (reachability witnesses for the model's own
  transition relation):
  - `cond_battery_fast_path` — a slot-holder parks (the release frees
    the slot), an external `notify_one` resolves its node (the waiter
    is readied *between* the notify's section and its return), and the
    resume takes the free slot and completes `rout true`.
  - `cond_battery_cancel` — same shape with an external `cancel`: the
    cancel caller's own completion is `rout true` (a waiter was
    resolved) while the waiter's outcome bit is `false`.
  - `cond_battery_broadcast` — three parked waiters (the instrumented
    start; see §7), one `notify_all`: the drain readies all three (all
    three surface in the run queue), the result observation carries
    `rall 3`, and the first resume takes the slot and completes `rout
    true` while the other two sit in the Mesa window with their
    resolutions unconsumed.
* Mutant battery (four independent fault classes, each killed by the
  evidence shape its class demands):
  - M1 `condFinishM1` (spurious completion — the resolution record is
    not required): killed by the backing discipline `condFinish_consumed`
    (completion ⇒ consumed record with the completed bit) —
    `condM1_spurious` completes with an empty record list. Safety-kill
    shape.
  - M2 `condExtRunM2` (`notify_all` readies only the head while still
    publishing the full count): under-production, so no safety
    invariant can catch it — killed by refuting the drain theorem
    (`condM2_drains_refuted`) and by the credit overclaim
    (`condM2_overclaims`: published wakes < the count the result
    carries). Theorem-refutation + separation shape.
  - M3 `condFinishM3` (the reacquire is skipped — the slot is never
    taken): killed by `condFinish_takes_owner` — `condM3_reacquire_skipped`
    completes with the slot free. Safety-kill shape.
  - M4 `condParkM4` (the wait section is not atomic — the register
    happens, the release is skipped, the slot stays held):
    under-production (lost wakeup), killed by witness separation —
    `condM4_keeps_slot` + `condM4_lost_wakeup` (the notified waiter's
    resume deadlocks) against `cond_battery_fast_path`, whose
    completion the model reaches through the same shape.

Axiom audit (`scripts/verify_formal.sh`): all of the above —
`cond_irreducible`, `cond_mirror`, `condWakeBacked`, both drains, the
three batteries, the three disciplines, and all six mutant kills —
depend on exactly `[propext, Quot.sound]` (or fewer).

## 6. Model correction recorded by this replay

The replay itself caught a model defect before any proof was attempted:
`condPark`'s `cwFresh` branch over an empty mutex queue kept
`owner := some f` instead of freeing the slot — deadlocking every
fast-path waiter (`condFinish` requires a free slot). The C++
(`scheduler_condition.cpp:63-65`: the handoff, else
`owner = nullptr`) forces `owner := none`, and that is what the model
now says. The M4 mutant is exactly this defect, retained as a
fault-class lesson: the TLA `MutParkHolds` separation proves the
fast-path witness unreachable under it.

## 7. Modeling disclosures

* **Multi-waiter configurations are relation-reachable but not
  `primInit`-reachable.** A `wait` is admitted only from the slot's
  holder, and acquiring the slot is the caller side's mutex traffic
  (the Stage-3 surface). The batteries that need several waiters start
  from instrumented configurations (pinned `cwaitq`/`resolved`); the
  TLA mirror exposes the same freedom as the `Boot` constant
  (`"prim"` | `"own0"` | `"wait3"`).
* **The wait node is per-call.** The C++ inline-resolution corners —
  `resolved_inline_released` (`condition_wait_admit_locked` :67-69, a
  caller-supplied node already terminal at entry) and its sibling
  `rejected_retain` (:34-39, `register_wait_locked` rejecting a
  non-detached node) — both require a node shared across calls, which
  the one-in-flight-call discipline excludes; the model has no such
  trace and neither can its discipline produce one.
* **Plain lock/unlock traffic is the mutex primitive's business.** The
  condition's embedded slot is modeled only where this surface touches
  it: the wait-entry release (handoff-or-free) and the reacquire take.
* **A `notify_one` credits at most one.** The unit result cannot
  distinguish "queue was empty" from "one waiter woken"; the frozen
  rule takes the safe upper bound (1), and no claim consumes the
  difference.
* **The cancel caller's result vs the waiter's outcome.** `cancel`
  completes `rout true` iff it resolved a waiter; the waiter's own
  outcome bit is `false` (cancelled). Both are modeled; the credit rule
  (cancel true → 1, false → 0) keys on the caller's result.
* **Fiber-origin notify/cancel.** `condAdmit` admits them from fibers
  (the entry points serve any caller holding `global_mtx_`); the TLA
  machine includes them. The batteries exercise the external-capable
  surfaces.

## 8. The TLA mirror (`formal/tla/CondCore.tla`)

A TLC-executable mirror of the same action set. Constants: fixed caller
sets `Fibers = {"f0","f1"}`, `Exts = {"e0"}` (one external caller: the
external window is witnessed by fiber-vs-ext ordering; a second caller
only multiplies the bounded state space); observation fuel `MaxHistory`
3–7 per cfg (submits are silent, so depth decouples from the
observation budget). `Spec == Init /\ [][Next]_vars` — **no fairness is
assumed anywhere**; every claim below is safety. Four constant mutant
switches (`MutSpurious`, `MutDrainOne`, `MutNoTake`, `MutParkHolds`),
all `FALSE` in the reference configuration. Two state constraints
(`WitConstraint`, `CancelConstraint`) prune the witness cfgs to the
witness's own call mix; a constraint can only shrink the explored
space, so a violation found under it is still a witness, and the PASS
cfgs run unconstrained.

### 8.1 Action crosswalk (Lean `PrimStep2` ↔ TLA)

| Lean constructor (`CalcV2.lean`) | CondCore.tla action(s) | observable |
|----------------------------------|------------------------|------------|
| `submit` | `FiberSubmit(f,c,w)` | silent |
| `dispatchFresh` | `FiberDispatch` | fiber issue; `Admits(c,f)` gate: `wait` needs `owner = f` |
| `runPark` (`cwFresh`) | `WaitPark` | silent; registers at the condition-queue tail and releases the slot — handoff to the mutex-queue head, else free |
| `runPark` (`cwWaiting`, resumed) | `WaitReacqPark` | silent; the Mesa re-park on the mutex queue (the resumed second suspension) |
| `fiberEffect` (`notify_one`) | `NotifyOneEmpty` / `NotifyOneTake` | silent; take readies the head with a `true` record |
| `fiberEffect` (`notify_all`) | `NotifyAllEmpty` / `NotifyAllTake` | silent; drains, publishes the count |
| `fiberEffect` (`cancel`) | `CancelHit` / `CancelMiss` | silent; hit readies the target with a `false` record |
| `fiberDone` | `FiberDone` | fiber completion |
| `dispatchResumed` | `FiberResume` | silent dispatch of a published waiter |
| `finishDone` (`cwReacq`) | `FinishReacq` | the handed-off completion; consumes the record, no take |
| `finishDone` (`cwWaiting`) | `FinishTake` | the reacquire; consumes the record and takes the free slot |
| `extApply` | `ExtIssue(x,c,w)` | external issue |
| `extEffect` | `ExtNotifyOne` / `ExtNotifyAll` / `ExtCancelHit` / `ExtCancelMiss` | silent sections |
| `extDone` | `ExtDone(x)` | external completion |
| `envTime` / `expire` | omitted | the condition has no timers; `onTick` is the identity, `expire` is none |

### 8.2 Safety invariants

| TLA invariant | content | Lean analogue |
|---------------|---------|---------------|
| `TypeOK` | every variable in its state domain | type discipline |
| `InvWakeBacked` | completed waits ≤ completed-resolver credit + in-window credit (returning result, external `eff` records; nothing at `ent`/issue) | `condWakeBacked` + the frozen credit rules |
| `InvFinishBacked` | ghost: every completed `wait` consumed its record | `condFinish_consumed` |
| `InvFinishOwned` | ghost: every `cwWaiting` completion left the caller owning the slot | `condFinish_takes_owner` |
| `InvQueueNoDup` | no fiber in `cwaitq`/`mwaitq`/`runq` twice | the possession invariants |
| `InvQueueOwnership` | parked/mutex-blocked/dispatched disjoint; the slot holder never parked or queued; stale runq entries are waits; parked phase = `waiting`, mutex-blocked phase = `reacq` | `condPark_resolved` + the handoff shape |
| `InvCompDiscipline` | every completion matched by a prior same-caller same-call issue with no intervening completion — except a boot waiter's single pre-Init instance (no in-trace issue up to the completion; prefix-scoped, so the invariant is monotone) | no completion before issue |

Reference results (`scripts/verify_tla.sh`, Stage 4V2.3 block):

* **Safety matrix**: `cond-main` (primInit start), `cond-safety-boot`
  (slot-holder start), `cond-safety-wait3` (parked-pair start) — all
  seven invariants, all complete cleanly (379k / 459k / 8k distinct
  states).
* **Coverage** (negated conjunctions; each must be violated):
  `CovFastPath` (release freed the slot, resumed waiter completed
  true), `CovBroadcast` (a drain readied ≥ 2 waiters and a resume
  completed — the Mesa window), `CovCancel` (a queued waiter cancelled,
  resume completed false), `CovHandoff` (a release handed the slot to a
  mutex-blocked waiter and that waiter completed through its `cwReacq`
  finish), `CovNotifyEmpty`, `CovCancelMiss`. All six violated as
  expected.
* **Safety mutants**: `MutSpurious` (M1) dies on `InvFinishBacked`;
  `MutNoTake` (M3) dies on `InvFinishOwned`; `MutParkHoldsOwn` (M4's
  release skipped, full safety matrix) dies on `InvQueueOwnership` (the
  slot holder is parked).
* **Under-production witness separations** (must complete CLEANLY with
  the witness invariants asserted): `MutDrainOne` (M2) exhausts its
  space with `NotBcastWitness` holding — the broadcast witness is
  unreachable; `MutParkHolds` (M4) exhausts 2.49M states with
  `NotFastPathWitness` holding — the fast-path witness is unreachable.
  The coverage cfgs certify both witnesses ARE reachable in the correct
  model; that two-direction pair is the separation certificate.

## 9. Verification bugs the mirror caught

* `WaitReacqPark` originally guarded on a *fresh* worker slot (copied
  from the mutex's `LockPark`, where parks only happen on fresh
  dispatches). With that guard the Mesa re-park — the resumed waiter's
  second suspension — was unreachable and `CovHandoff` could not fire;
  the calculus's `runPark` has no freshness premise, and the resumed
  slot is exactly how the model re-parks a `cwWaiting` waiter whose
  finish is blocked. The guard now requires the resumed bit.
* The first `InvWakeBacked` draft credited only *completed* resolver
  observations; the drain's wakes are already published between the
  effect and the return, so the invariant failed on legal traces. The
  in-window credit terms (returning result, external `eff` records) are
  the frozen accounting, not a patch: no credit at entry, none at
  issue; a pending-to-completed transition only moves credit.

## 10. Disposition

AsyncCondition — **THEOREM B**, re-verified on the amended base with the
wake-credit ledger added. Stage-4 closes with both evidence directions
the adjudication demanded (A: no-spurious-wake; B: drain completeness),
a four-class mutant battery, and the TLA mirror's safety matrix,
coverage, and separations.
