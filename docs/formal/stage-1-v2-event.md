# Stage 1V2.2 — Event Core: Re-adjudicated on the V2.2 Execution Domains

Campaign: `FCB1-METHOD-CORRECTIVE-1` (repair of the FORMAL-CAPABILITY-BOUNDARY-1
stack, PRs #376–#385; charter issue #375).  This stage re-adjudicates the Event
core from zero after BRAKE-1 v2.2 invalidated the V2.1 verdict
(`docs/formal/stage-0-v2-base-calculus.md` §7.1).
Status: **ADJUDICATED (V2.2)** — the guarantee survives with its anchor moved
from the set *completion* to the set *issue*; the primitive now *possesses*
the trace the V2.1 countermodel certificated; the natural chained encoding
under-produces (certified); the capability verdict is **RESEARCH / DEFER** —
no universal separation is claimed, per the wording discipline of the
campaign freeze (§6/§15 of the corrective document): "THEOREM A fails" is
said only for a proved universal non-reducibility, which this stage does
*not* have.
Toolchain: Lean 4.33.1 (`formal/lean-toolchain`, no mathlib); TLC2 via a local
`formal/tla/tla2tools.jar` (gitignored, like the Lean toolchain itself).
Verification gates: `scripts/verify_formal.sh` (Lean, 21 audited theorems) and
`scripts/verify_tla.sh` (TLC, main model + negative mutant).
Current gate results: PASS / PASS (see §9).

## 1. Artifacts

| Artifact | Content |
| --- | --- |
| `formal/Sluice/Formal/EventV2.lean` | the Event primitive LTS, the V2.2 safety guarantee (issue-anchored), the fused carried invariant, possession batteries (drain + two-observation external witness), the natural chained encoding with its `extOpAllowed` blockade, the universal under-production of the fiber-only-`set` class, the RESEARCH/DEFER verdict, the negative mutant |
| `formal/tla/EventCore.tla` (+ `.cfg`, `EventCoreMutant.cfg`) | the TLC model of the primitive under V2.2: one action per `PrimStep2` constructor plus the three-phase external sequence, with `TypeOK` and the issue-anchored safety invariant and the mutant switch |
| this document | the stage card: census, anchors, verdict, ledger |

## 2. Call-domain census (from the code)

Public surface (`include/sluice/async/event.hpp`): `set()`, `reset()`,
`wait()` — all void-returning; the drain count returned by
`Scheduler::event_set_broadcast : std::size_t` is discarded at the `Event`
wrapper and is not part of the public API surface (the model keeps
`setWoke n` because the per-waiter publication it summarizes is the
load-bearing behavior).  `wait_until`, `cancel`, and the select integration
are outside the core model, as in V2.1.  The core model also covers the
**initially-clear** event only (`init` flag = false): `Event(Scheduler&,
bool initially_set = true)` (`include/sluice/async/event.hpp:16-17`) can
complete a `wait` with no set issue ever, so it is outside the modeled
domain; no in-tree consumer constructs it.

| Call | Domain | Code anchor |
| --- | --- | --- |
| `wait` | **fiber-only** | `await_event_wait` reads `g_worker` / `ws->current` and switches fibers (`src/async/scheduler_event.cpp:216-241`) |
| `set` | **external-capable** | `event_set_broadcast`: `LockGuard lk(global_mtx_)` only, no `g_worker` (`src/async/scheduler_event.cpp:21-37`) |
| `reset` | **external-capable** | `event_reset`: `LockGuard lk(global_mtx_)` only (`src/async/scheduler_event.cpp:39-42`) |

Consequences encoded in the calculus: an external `set`/`reset` never enters
the runnable FIFO (it may begin while fibers are queued or a fiber is between
its critical sections), and its physical return is unordered with respect to
fiber steps — `global_mtx_` serializes state effects, not physical returns.
Both sides declare the same domains (`eventPrim.extCap` / `Encoding.extCap`).

## 3. State and transition anchors

| Calculus | C++ counterpart |
| --- | --- |
| `EventState {flag, waitq}` | the `std::atomic<bool>` latch (`Event::set_`) and the wait queue |
| `eventRun` set-arm | `event_set_broadcast`: `set_.exchange(true)`, and if it was clear, drain the whole wait queue (`wake_wait_one_locked` loop), returning the drained count |
| `eventRun` reset-arm | `event_reset`: `store(false)`; waiters stay parked |
| `eventRun` wait-arm (flag set) | complete inline (`event_wait_admit_locked` already-set branch) |
| `eventPrim.park` (wait only) | `event_wait_admit_locked` / `await_event_wait`: append to the wait queue and suspend |
| `eventPrim.finish` (wait only, publishes nobody) | the one-shot completion of a resumed waiter |
| `admit`/`onTick`/`expire` (identity/identity/none) | no Event-specific environment behavior |

External three-phase mapping (V2.2; entry precedes `global_mtx_` in the
code — function body first, then `LockGuard` — so entry is never fused with
the critical section):

| Phase | Primitive | TLC action |
| --- | --- | --- |
| entry (`extApply`) | issue observation emitted, result-less record registered | `ExtApplySet` / `ExtApplyReset` |
| critical section (`extEffect`) | fused `extRun`: state effect + result fixed + waiters published; silent | `ExtEffectSet` / `ExtEffectReset` |
| physical return (`extDone`) | completion observation, unordered | `ExtDoneSet` / `ExtDoneReset` |

## 4. The guarantee (safety, V2.2 anchor)

`eventNoWaitBeforeSet`: every `wait` completion is preceded by a `set`
**issue** — a fiber dispatch or an external entry.  (V2.1 anchored on the
set *completion*; under V2.2 a drained waiter may complete before the
draining external `set` physically returns — its critical section has run —
so the issue is the observable anchor.)

* `eventPrim_guarantees` — one induction over the run with a fused carried
  invariant (`EventInv`): if any completion-enabling shape is present (set
  latch, in-flight external `set`, dispatched `set`, stale runnable entry,
  resumed `wait` in `cur`), then a set issue is already in the prefix;
  plus no external `wait` record can exist (`extCap` forbids it).  Every
  `PrimStep2` constructor preserves it (`eventStep_preserved`); a wait
  completion inverts to a latch or a resumed waiter
  (`eventStep_waitComp_inv`).
* Possession batteries (the trace language is not vacuous):
  - `eventPrim_possesses_drain` — the full external drain in the rule-10
    order: a parked waiter is drained by an *external* set and completes
    **before** the external set's own physical return (the very schedule
    V2.1 could not express, and the reason the anchor moved to the
    issue);
  - `eventPrim_possesses_ext` — the two-observation external sequence
    `[issue (ext 0) set, comp (ext 0) set (setWoke 0)]`, the trace the
    V2.1 countermodel rested on.

This is the guarantee the architecture question asks about: **no waiter
completes before some caller has issued a `set`**.  Woken-while-set is
deliberately *not* claimed.

## 5. The counter-evidence (what is certified)

**The natural chained encoding under-produces** (`encChained_underProduces`).
`encChained` implements `set` over the bare substrate as
`wakeOne(flagQ) → attach(flagQ) → wakeOne(waitQ)` — the marker write is
unavoidable for a latch.  Its external `set` enters (`extCap set = true`),
takes its first step, and is then permanently blocked: the continuation's
`attach` requires `g_worker` (`await_wait*`), and `extOpAllowed` forbids it.
The record never reaches `pure`, so the external `set` never completes —
`encChained_not_extWitness` refutes the two-observation external witness.
`encChained_not_reduction` closes the reduction route.
(The Stage-0 v2.2.1 repair — §7 below — guarantees this blockade is the
*only* reason: the encoding machine does advance external programs.)

**Every encoding declaring `set` fiber-only under-produces**
(`enc_no_ext_cap_under`, universal over the closed `Encoding` class): no
step can emit an external `set` issue at all (`extCap_true_of_extIssue`).

## 6. The verdict: RESEARCH / DEFER

`event_capability_defer` packages the two certified halves:

1. universal under-production of the `extCap set = false` class (above);
2. **no universal separation**: `encPure` — a degenerate encoding whose
   external `set` program is already `pure` — *produces* the external
   witness (`encPure_possesses_ext`), so `SeparatedBy` fails for it.

Whether some `extCap set = true` encoding matches `eventPrim` on all traces
(THEOREM A) is neither proved nor refuted here.  Per the freeze's wording
discipline this stage does **not** say "THEOREM A fails"; it says the
natural encoding is not a reduction (certified) and the general question is
deferred to a dedicated research item on the charter issue (#375).

## 7. BRAKE-1 consequences found by this stage

* **v2.2.1 (Stage-0 repair, upstream).**  While proving the chained
  encoding's blockade this stage found that `extSubOpStep`/`extSubOpWake`
  discarded the external program's continuation — any external program with
  at least one substrate operation could never return.  The blockade would
  have been a machine artifact instead of the code-real `extOpAllowed`
  rule.  Repaired upstream with a ledger entry
  (`docs/formal/stage-0-v2-base-calculus.md` §7.1 v2.2.1); no merged
  certificate was affected.

## 8. Negative mutant

`eventMutant` (`wait` completing inline on a clear latch, mirroring the
code's admission check inverted): `eventMutant_not_guarantees` — the
two-observation inline wait has no set issue at all.  TLC mirror: the
`WaitInlineOnClear` switch; `scripts/verify_tla.sh` requires the mutant run
to violate `NoWaitBeforeSet`.

## 9. Verification gates

| Gate | Result |
| --- | --- |
| `scripts/verify_formal.sh` | **PASS** — build clean, no sorry/admit, 21 audited theorems depend only on `{propext, Quot.sound}` |
| `scripts/verify_tla.sh` | **PASS** — `EventCore` (main model, `MaxFiber=1 MaxExt=1 MaxHistory=5`; covers the full external drain, both domains' set/reset flows, the already-set branch): `TypeOK` + `NoWaitBeforeSet` hold; `EventCoreMutant` (`WaitInlineOnClear=TRUE`): `NoWaitBeforeSet` violated |

## 10. Downstream obligations

* Stage 2 (Semaphore, PR #378) inherits the V2.2 calculus and the same
  three-phase external discipline (`release`/`try_acquire`/`cancel` are
  external-capable, `acquire` is fiber-bound — census to be re-verified on
  the Semaphore code); its stage card must repeat the call-domain census
  and the possession battery before any verdict.
* The THEOREM-A question for the `extCap set = true` Event class is
  recorded as RESEARCH/DEFER on the charter issue (#375); the campaign
  stays OPEN.
