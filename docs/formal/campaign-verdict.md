# FCB1-POST-V23-STACK-379-385 — campaign verdict

Re-adjudicated on the Stage-0-V2.3 authority — post-#378, as amended
by the declared Stage-4 calculus brake (provenance in the frozen-core
summary below). This document
is the campaign's final synthesis record (PR #385, stacked on #384);
it replaces the pre-reset verdict document, which carried nine
THEOREM-B verdicts and one THEOREM-A on the V1 authority. Nothing on
this page is inherited from that document: every row below was
re-derived on the current stack (#378 → #379 → #380 → #381 → #382 →
#383 → #384), each row's stage card carries the full evidence, and
the #375 ledger (§28–§35) records each stage's closure.

Naming: the repair campaign over PRs #376–#385 runs under
FCB1-METHOD-CORRECTIVE-1 (the label the Stage-0/1/2 cards carry);
the stack re-adjudicating on V2.3 (#379–#385) is
FCB1-POST-V23-STACK-379-385 (this document's label, and the stage-4
through stage-8 cards'); the #375 ledger §36 closes the stack under
FCB1-FINAL-SYNTHESIS-CORRECTIVE-1.

## The correction of the campaign

The V1 campaign carried nine THEOREM-B verdicts and one THEOREM-A
(lock_guard reducible). The nine THEOREM-B rows were Event, Semaphore,
AsyncMutex, AsyncCondition, AsyncRwLock, AsyncQueue, select, and
`Scheduler::run` and `Scheduler::run_until_idle` — the latter two as
separate rows of the V1 table over one modeled drain instance — so
every primitive row was adjudicated irreducible. The Stage-0
V2.3 authority invalidated the device all nine THEOREM-B proofs
rested on: `tracesEnc_shadow_false` (`CalcV2.lean`) exhibits an
encoding possessing a completion-shadowed trace, so the
completion-shadow projection is not a valid separator under V2.3.

Four of the old THEOREM-B rows — AsyncQueue, select, and the two
driver rows `Scheduler::run` and `Scheduler::run_until_idle` (one
modeled stage) — had exactly that proof shape and are
**retired, not appealed**: their stages were re-adjudicated
RESEARCH/DEFER on the amended authority, with the old branches
preserved fetchable as historical evidence, not deleted (the old
heads are recorded on the stage cards and in the ledger: select at
`7d03e1f6`, the driver at `6d02a9e3`). The THEOREM-B rows for
AsyncMutex, AsyncCondition, and AsyncRwLock had shadow-shaped V1
proofs as well, but each was **re-proved on V2.3 by a different,
shadow-free argument** (the owner-gated admission refusal — see the
table), so their verdicts stand on new premises.

This correction is the campaign's headline result: the
re-adjudication did not lose the old conclusions by sloppiness; it
found the old conclusions unsound and replaced them with what the
frozen method actually supports.

## Per-primitive verdicts (this stack, post-V2.3)

| primitive | verdict | the argument that carries it | carrier |
|---|---|---|---|
| Event | **RESEARCH/DEFER** | per-encoding-class conditionals only (`event_capability_defer`) | #378, stage-1 card §7 |
| Semaphore | **RESEARCH/DEFER** | per-encoding-class conditionals only (stage-2 card §5) | #378, stage-2 card §5 |
| AsyncMutex | **THEOREM B** — re-proved, new argument | the owner-gated admission of `unlock` (`assert`, scheduler_mutex.cpp:188): the primitive never issues a non-owner `unlock` (`mutex_not_unlockIssue`), every encoding dispatches any submitted call unconditionally; `mutex_irreducible` over `BASE(AsyncMutex) = {Semaphore}` | #379, stage-3 card §5 |
| lock_guard | **THEOREM-A question OPEN** | the charter's nonempty-`BASE(P)` re-export test; "a charter obligation still open; this stage neither relies on it nor delivers it" (stage-3 card §5) | #379, stage-3 card §5 |
| AsyncCondition | **THEOREM B** — re-verified on the amended base | the owner-gated admission of `wait` (scheduler_condition.cpp:79-80); `cond_irreducible` | #380, stage-4 card §5 |
| AsyncRwLock | **THEOREM B** — re-verified on the amended base | the owner-gated admission of `unlock_write` (:372-377); `rw_irreducible` | #381, stage-5 card §5 |
| AsyncQueue | **RESEARCH/DEFER** — old B retired | per-encoding-class conditionals only (`q_capability_defer`) | #382, stage-6 card |
| select | **RESEARCH/DEFER** — old B retired | per-encoding-class conditionals only (`sel_capability_defer`) | #383, stage-7 card §6 |
| Scheduler::run / run_until_idle | **RESEARCH/DEFER** — old B retired | per-encoding-class conditionals only (`run_capability_defer`); bespoke driver LTS over the frozen observation model, the declared substrate extension per Stage-0 §9.3 | #384, stage-8 card §6 |

For every RESEARCH/DEFER row: the stage proved per-encoding-class
conditionals — over-production for the encoding class that declares a
fiber-bound call externally callable, under-production for the class
that declares a produced external call internal — each with explicit
non-vacuity instances of the encoding class, **neither conditional
named THEOREM B**, and recorded the exact open boundary. RESEARCH/DEFER
does NOT mean reducible. RESEARCH/DEFER does NOT mean probably
irreducible.

## The exact open boundaries (verbatim from the stage cards)

> **AsyncQueue** — Does there exist an Encoding QueueBase QSig whose
> disciplined trace language is observationally equivalent to
> `queuePrim` under Stage-0 V2.3? No such reduction is presently
> constructed. No universal mismatch proof is presently known. The
> previous universal mismatch proof is invalid because its
> completion-shadow lemma is false under V2.3. (stage-6 card)

> **Select** — Does there exist an Encoding BaseOpsSig.none SelSig
> whose disciplined trace language is observationally equivalent to
> `selPrim` under Stage-0 V2.3? No such reduction is presently
> constructed. No universal mismatch proof is presently known. The
> previous universal mismatch proof is invalid because its
> completion-shadow lemma is false under V2.3. (stage-7 card)

> **Scheduler::run / run_until_idle** — Does there exist an Encoding
> BaseOpsSig.none RunSig whose disciplined trace language is
> observationally equivalent to the driver core's disciplined
> language (`TracesRunS`) under Stage-0 V2.3? No such reduction is
> presently constructed. No universal mismatch proof is presently
> known. The previous universal mismatch proof is invalid because its
> completion-shadow lemma is false under V2.3. (stage-8 card)

The Event and Semaphore rows carry the same RESEARCH/DEFER
disposition with their own recorded boundaries (stage-1 card §7,
stage-2 card §5); their open questions predate this stack's
refutation and are not shadow-resting.

## Multi-worker run adjudication (Stage-0 §9, item 4 — explicit)

Stage-0's open-assumptions register requires this document to address
the multi-worker run explicitly: "Multi-worker `run` remains outside
the serialized single-worker discipline above; its adjudication
status is a campaign-level question the FINAL VERDICT must address
explicitly (RESEARCH is acceptable)."

The disposition is **RESEARCH (deferred)**:

* The stage-8 frozen core instance is `run(1)` / `run_until_idle` at
  ONE worker over plain tasks. The production surface's multi-worker
  `run` — the worker pool, the count ceiling, the cross-worker
  hand-offs, the idle dance over `live_loop_workers_` — is a recorded
  extension outside every modeled surface in this stack.
* Modeling it requires either a multi-worker observation discipline
  the frozen single-worker method does not provide, or a serialized
  reduction argument that has not been constructed. Neither exists
  today; inventing one ad hoc is what the Stage-0 discipline forbids.
* Accordingly: no THEOREM-B-shaped claim is made about the
  multi-worker run, in either direction, and the single-worker rows
  above do not pretend to cover it. The per-worker accounting
  facts the single-worker model carries (task conservation, the
  quiescent-exit gate) are the recorded single-worker discipline;
  extending them to the pool is future work with a recorded starting
  point (stage-8 card §1, §9).

## What the stack established (frozen-core summary)

* **Stage 0 V2.3** — the completion shadow is not valid under the
  V2.3 discipline (`tracesEnc_shadow_false`). Calculus provenance:
  **CalcV2** — Stage-0 V2.3 established through #378; Stage 4 (#380)
  then fired a declared calculus BRAKE (park publishes wakes from the
  pre-suspension section; section-ending wakes use the
  order-preserving `Wakes` relation), and Stages 0–3 were replayed
  after that amendment — the charter rule that no post-freeze
  amendment goes without replaying the adjudicated stages.
  **JudgeV2** — unchanged by the Stage-4 brake and by Stages 5–8.
  The final authority is therefore post-#378 V2.3 + the declared
  Stage-4 calculus amendment + the required replay.
* **The BASE discipline** — each `BASE(P)` was frozen at Stage 0 and
  neither enlarged nor shrank thereafter; the frozen matrix
  (stage-0 card §5) is `BASE(Event) = BASE(Semaphore) = {}`,
  `BASE(AsyncMutex) = {Semaphore}`, `BASE(AsyncCondition) =
  {AsyncMutex}`, `BASE(AsyncRwLock) = {AsyncMutex}`,
  `BASE(AsyncQueue) = {Semaphore}`, `BASE(select) = {}`,
  `BASE(Scheduler::run) = {}`; lock_guard's declared base is the
  synchronous `Mutex`. Nonempty bases are part of the method, not
  exceptions: AsyncMutex's THEOREM B is over its frozen `{Semaphore}`
  base. No stage enlarged or shrank its frozen BASE merely to recover
  a verdict.
* **Eight modeled surfaces rebuilt from current C++** — Event,
  Semaphore, Mutex, Condition, RwLock, Queue, Select, and the
  scheduler driver — each with a state discipline preserved per
  constructor, possession batteries, a mutant battery, a TLA mirror,
  and gate wiring.
* **Every stage closed under fresh-context adversarial review** —
  verdicts on the stage cards, closures in the #375 ledger (§28–§35).

## Gates at close

* `scripts/verify_formal.sh` — **PASS**: lake build clean; no
  sorry/admit anywhere; the full axiom audit within
  {propext, Classical.choice, Quot.sound}.
* `scripts/verify_tla.sh` — **PASS**: all stages 1–8 green (116 TLC
  configurations: safety boots clean, coverage witnesses violated,
  mutants killed on their exact intended invariants, trace-removal
  separations clean).
* CalcV2.lean was amended exactly once after the V2.3 authority was
  established (through #378) — the declared Stage-4 brake in #380
  (commit `3eba6fe5`), after which Stages 0–3 were replayed;
  JudgeV2.lean is unchanged by #379–#385.
* No production C++ was changed anywhere in the stack.
