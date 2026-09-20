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

---

# Architecture-owner adjudication — final repository disposition (#375 closure)

Label: FCB1-ARCH-DISPOSITION-375. Adjudicated on master
`515d1d13e4e475496018011445d4bc24e8e4b594` with the merged formal stack
(#378–#385) unchanged. This section is the second, independent half of the
#375 question: the capability table above stays exactly as the campaign
delivered it (frozen input, not re-adjudicated); what follows adds the
architecture-owner result per surface and derives the final repository
disposition from the two together. CAPABILITY and ARCHITECTURE OWNER are
kept as separate columns throughout; DISPOSITION is derived from both and
never collapses them.

## Adjudication provenance and census freshness

* Start state: master `515d1d13…` containing #378–#385; PRs #379–#385
  MERGED; #373/#374/#375 OPEN.
* Freshness: production C++ is byte-identical to the #373 audit baseline
  `253fabe79c425ad5473e1bf4efc30896023762bc` —
  `git diff --stat 253fabe7..515d1d13 -- src include apps tests xmake.lua xmake`
  is empty, and #379–#385 changed no production C++. The census facts below
  were nevertheless re-derived on current master (per-symbol sweeps over
  `include/ src/ apps/ tests/` plus per-header `#include` sweeps), not
  inherited from the 2026-09-13 audit.

## ARCHITECTURE OWNER TEST (frozen before any candidate was assigned a result)

The question, per #375 and the #373 canonical addendum §D:

> Does Sluice, as the retained Explicit I/O library/runtime described by
> `docs/mission.md`, ADR-0001 and ADR-0002, need to own this abstraction
> boundary?

Not: is the abstraction interesting, is it mathematically distinct, is the
implementation already written, could somebody use it someday.

Eight independent dimensions, evaluated per surface:

| dim | question |
|---|---|
| O1 | Normative requirement: is the abstraction or its semantics required by mission, an accepted ADR, the canonical architecture, or an explicitly retained product/runtime contract? (`REQUIRED / SUPPORTED / NOT NAMED / CONTRADICTS SCOPE`; a generic possibility is not a normative requirement) |
| O2 | Retained production dependency: does current retained production code depend on this abstraction — not merely on a lower-level mechanism that happens to also implement it? (`public abstraction required / internal abstraction required / implementation exists but unused / no retained dependency`) |
| O3 | Boundary ownership: would removal force retained Sluice code to duplicate or leak a semantic/correctness/resource boundary the architecture says Sluice should hide (ownership transfer, park/wake protocol, permit semantics, wait-set semantics, selection commitment, queue-item ownership, driver quiescence)? |
| O4 | Public-surface necessity: even if the mechanism is needed internally, must it be exposed publicly? Mechanism owner and public API owner are answered separately. |
| O5 | Product-domain fit: core Explicit-I/O concept / runtime implementation mechanism / general concurrency utility / generic container-synchronization abstraction / composition helper. |
| O6 | Evidence of actual use (real consumers, internal callers, tests) — supporting evidence only; `zero consumer != DELETE` stands frozen. |
| O7 | Replacement/convergence path: if the surface vanished from its current form, is there a retained abstraction that owns the required semantics? `BASE(P)` is proof language and never automatically the repository replacement. |
| O8 | Maintenance authority cost (compatibility, tests, formal obligation, docs, complexity, invariant interaction) — recorded, never the deciding factor. |

Owner verdict vocabulary and rules:

```text
OWNER_YES         requires a positive retained-architecture reason:
                  normative architecture requires the boundary, or retained
                  production architecture genuinely depends on owning it.
                  THEOREM B is never the reason.
OWNER_NO          requires more than the zero-consumer fact, supported by:
                  no normative requirement + no retained abstraction
                  dependency + surface general-purpose / outside retained
                  mission + no required Sluice boundary leaks on removal or
                  internalization.
OWNER_UNRESOLVED  the architecture authority itself is ambiguous; no
                  product strategy is invented.
```

Frozen capability input (verbatim from the stack; not re-adjudicated):
Event RESEARCH/DEFER; Semaphore RESEARCH/DEFER; AsyncMutex THEOREM B;
lock_guard THEOREM-A question OPEN; AsyncCondition THEOREM B; AsyncRwLock
THEOREM B; AsyncQueue RESEARCH/DEFER; select RESEARCH/DEFER;
`Scheduler::run` / `run_until_idle` RESEARCH/DEFER; multi-worker run
RESEARCH/DEFER outside the Stage-8 single-worker core.

Disposition mapping (the #375 matrix, extended honestly for unresolved
capability):

```text
THEOREM B           x OWNER_YES -> KEEP (CONVERGE only if another retained
                                   abstraction should own the same boundary
                                   while preserving it)
THEOREM B           x OWNER_NO  -> RESEARCH: independent capability exists,
                                   Sluice Core has not earned ownership; a
                                   distinct capability may belong outside
                                   Core. Never silently turned into DELETE.
THEOREM A / proved  x OWNER_YES -> CONVERGE or INTERNALIZE (public product
                                   concept warranted or not)
THEOREM A / proved  x OWNER_NO  -> DELETE or INTERNALIZE (per actual
                                   implementation dependencies)
RESEARCH/DEFER/OPEN x any       -> RESEARCH, recording both the capability
                                   open boundary and the owner result;
                                   uncertainty is never upgraded to KEEP or
                                   DELETE. RESEARCH rows satisfy the #375
                                   stop-state B by naming the exact open
                                   boundary.
```

Separations enforced throughout (never conflated):

1. Scheduler implementation vs the `Scheduler::run`/`run_until_idle` public
   driver surfaces: the runtime needing scheduler internals does not make
   these specific observable driver APIs owned.
2. Async primitives vs the park/wake substrate: wait nodes, park, wake,
   runnable publication and `global_mtx_` are MC-* mechanism ownership, not
   C4 abstraction ownership.
3. `BASE(P)` relations are part of the capability proof language; they are
   not implementation-replacement or ownership relations.
4. lock_guard's capability stays OPEN: no GuardV2 theorem was delivered and
   the V1 THEOREM A is not inherited.

## Current-master census (Phase-A evidence)

```text
Event            include/sluice/async/event.hpp; impl scheduler_event.cpp,
                 select_event.cpp. Retained internal callers as this
                 abstraction: the select machinery only — EventSelectCase
                 binds Event* (select.hpp), Event embeds
                 detail::SelectPort (event.hpp), select_event.cpp drives
                 its scheduler ops; scheduler.cpp references are select
                 assertions and test access only. apps: 0, tests: 0.
Semaphore        include/sluice/async/semaphore.hpp; impl
                 scheduler_semaphore.cpp. semaphore.hpp is included
                 nowhere; the Semaphore class has zero references outside
                 its own header (the scheduler-side sem_* ops are
                 reachable only through it). apps: 0, tests: 0.
AsyncMutex       include/sluice/async/async_mutex.hpp; impl
                 scheduler_mutex.cpp. Retained internal callers: only
                 AsyncCondition (condition.hpp holds AsyncMutex&; friend
                 declaration in async_mutex.hpp). No retained product
                 dependency. apps: 0, tests: 0.
lock_guard       include/sluice/async/lock_guard.hpp (LockGuard over the
                 synchronous Mutex, mutex.hpp). Retained internal callers:
                 the scheduler mechanism itself — LockGuard guards
                 global_mtx_ (declared `mutable Mutex`, scheduler.hpp) and
                 waiter mutexes across 13 src TUs (244 references). The
                 public surface has zero apps and zero tests.
AsyncCondition   include/sluice/async/condition.hpp; impl
                 scheduler_condition.cpp. condition.hpp is included
                 nowhere; zero callers. Depends on AsyncMutex; nothing
                 depends on it. apps: 0, tests: 0.
AsyncRwLock      include/sluice/async/async_rwlock.hpp; impl
                 scheduler_rwlock.cpp. Zero callers as an abstraction.
                 Mechanism-level coupling only: the shared timer-expiry
                 path reuses AsyncRwLock::ExpireCtx
                 (scheduler_timer.cpp:155); other references are test
                 access. apps: 0, tests: 0.
AsyncQueue       include/sluice/async/async_queue.hpp; impl
                 scheduler_queue.cpp + queue_port.cpp + detail/queue_*.
                 async_queue.hpp is included nowhere; AsyncQueue<T> has
                 zero callers. The internal QueuePort substrate is
                 retained, but its only instantiation lives inside
                 AsyncQueue<T> (async_queue.hpp) and the scheduler-side
                 queue_* ops are exercised only through that surface —
                 mechanism, not the public abstraction, and not an
                 independent retained user. apps: 0, tests: 0.
select           include/sluice/async/select.hpp + select_fwd.hpp; impl
                 select.cpp, select_event.cpp, select_timer.cpp,
                 detail/select_*. Zero callers of the public select()
                 surface. The Scheduler core retains dormant select
                 machinery — the friend declaration, the select
                 admission/commit/finalize helpers, waiting_select_count_
                 and select_timer_pool_ (scheduler.hpp, scheduler.cpp,
                 scheduler_timer.cpp) — reachable only through that
                 surface; any future disposition implementation must
                 untangle it first. Depends on Event and the Scheduler
                 timer. apps: 0, tests: 0.
Scheduler::run   scheduler.hpp:145, impl scheduler.cpp:194. Exactly one
                 caller: the inline body of run_until_idle()
                 (scheduler.hpp:151). No other caller in src, apps or
                 tests. The retained driver consumers — ApplicationRuntime
                 (application_runtime.cpp:464) and Group (group.cpp:39) —
                 drive run_live, not run.
run_until_idle   scheduler.hpp:151, inline run(1). Zero callers anywhere.
multi-worker run exists in production via RuntimeBuilder::workers(n) and
                 ApplicationRuntime driving run_live(worker_count); it is
                 outside the Stage-8 single-worker modeled core.
```

Historical/docs-only references (stage cards, audit, campaign records)
exist for every row and carry no runtime dependency.

## Final matrix

CAPABILITY is the frozen formal input. ARCHITECTURE OWNER is derived from
the owner test on current authority. DISPOSITION is derived from both per
the mapping above.

| surface | current public | retained internal callers | real consumers | tests | normative anchor (O1) | domain fit (O5) | CAPABILITY (frozen) | ARCHITECTURE OWNER | final DISPOSITION | follow-up |
|---|---|---|---|---|---|---|---|---|---|---|
| Event | yes | select machinery only | 0 | 0 | NOT NAMED | runtime mechanism for select / general latch utility | RESEARCH/DEFER | OWNER_NO (public surface) | RESEARCH | none authorized by #375 |
| Semaphore | yes | zero | 0 | 0 | NOT NAMED | general concurrency utility | RESEARCH/DEFER | OWNER_NO | RESEARCH | none authorized by #375 |
| AsyncMutex | yes | AsyncCondition only | 0 | 0 | NOT NAMED | general concurrency utility | THEOREM B | OWNER_NO | RESEARCH | none authorized by #375 |
| lock_guard | yes | scheduler mechanism (244 refs, 13 TUs) | 0 | 0 | NOT NAMED as product surface | runtime implementation mechanism | THEOREM-A question OPEN | OWNER_NO (public surface); mechanism owner YES | RESEARCH | none authorized by #375 |
| AsyncCondition | yes | zero | 0 | 0 | NOT NAMED | general concurrency utility | THEOREM B | OWNER_NO | RESEARCH | none authorized by #375 |
| AsyncRwLock | yes | zero (ExpireCtx mechanism coupling only) | 0 | 0 | NOT NAMED | general concurrency utility | THEOREM B | OWNER_NO | RESEARCH | none authorized by #375 |
| AsyncQueue | yes | zero (QueuePort substrate is separate) | 0 | 0 | NOT NAMED | generic container/sync abstraction | RESEARCH/DEFER | OWNER_NO | RESEARCH | none authorized by #375 |
| select | yes | zero (dormant scheduler-side select machinery retained) | 0 | 0 | NOT NAMED | composition helper | RESEARCH/DEFER | OWNER_NO | RESEARCH | none authorized by #375 |
| Scheduler::run(unsigned) | yes | run_until_idle inline body only | 0 | 0 | NOT NAMED (ADR-0002 §8.1 frames Scheduler/Fiber as machinery — supportive, indirect) | driver entry point / execution policy | RESEARCH/DEFER | OWNER_NO (these entry points; mechanism owned via run_live + ApplicationRuntime) | RESEARCH | none authorized by #375 |
| Scheduler::run_until_idle() | yes | zero | 0 | 0 | NOT NAMED | driver entry point / execution policy | RESEARCH/DEFER | OWNER_NO | RESEARCH | none authorized by #375 |
| multi-worker run | via ApplicationRuntime workers(n) | ApplicationRuntime | sluice-tail only (workers knob) | 0 | NOT NAMED as Scheduler public API | execution policy | RESEARCH/DEFER (outside Stage-8 core) | OWNER_NO (not a Scheduler public-API question) | RESEARCH | none authorized by #375 |

## Evidence summary (each row cites code + architecture authority + formal result)

* **Event** — code: census above (select-only internal dependency; apps/tests
  zero); the park/wake and wait-set boundary beneath it is owned by the
  Scheduler substrate (WaitQueue/WaitNode/wake machinery), not by the Event
  class boundary. Authority: mission/ADR-0001/ADR-0002 and
  `docs/architecture.md` name no public event primitive — the runtime box
  names only the Scheduler/Fiber wait/wake/deadline/cancellation mechanism;
  `audit` DC-27 classifies the family EXTRA_MECHANISM pending this round.
  Formal: RESEARCH/DEFER, per-encoding-class conditionals only (stage-1
  card §7; THEOREM-A question for the `extCap(set)=true` class recorded on
  #375). Owner: public ownership not required; the only dependent (select)
  is itself an unadjudicated zero-consumer surface and can keep consuming an
  internalized mechanism without changing any retained caller-visible
  contract. Disposition: RESEARCH (capability DEFER × any owner).
* **Semaphore** — code: zero includers, zero callers; scheduler-side ops
  unreachable except through the unused wrapper. Authority: no normative
  naming anywhere; no retained dependency. Formal: RESEARCH/DEFER, the
  THEOREM-B question open in both directions (stage-2 card §5). Owner:
  OWNER_NO on all four evidence pillars. Disposition: RESEARCH.
* **AsyncMutex** — code: only internal dependent is AsyncCondition, itself
  zero-consumer; retained product architecture (apps, File ops, runtime
  driver) has zero dependency. Authority: NOT NAMED normatively. Formal:
  THEOREM B re-proved on V2.3 (owner-gated admission of `unlock`,
  `mutex_irreducible` over the frozen `BASE = {Semaphore}`) — capability
  independence is a property of the semantics, not a product-ownership
  grant; `BASE(P)` is not an implementation or ownership relation.
  Owner: OWNER_NO — an independent capability can belong outside Core.
  Disposition: RESEARCH (THEOREM B × OWNER_NO; never silently DELETE).
* **lock_guard** — code: the one family member with a heavy retained
  mechanism dependency (scheduler internals lock `global_mtx_`/waiter
  mutexes through LockGuard/Mutex). Authority: NOT NAMED as product
  surface; the runtime's own thread-safety is a substrate correctness
  requirement, not a public-boundary requirement; scheduler.hpp includes
  lock_guard.hpp only for its private `Mutex` members (`global_mtx_`,
  `wait_registry_mtx_`). Formal:
  THEOREM-A question OPEN — the charter's nonempty-`BASE` re-export test
  against the synchronous `Mutex`; no GuardV2 theorem; the V1 THEOREM A is
  not inherited. Owner: mechanism owner YES, public API owner NO — the
  recorded candidate path is internalization, expressly not implemented
  here. Disposition: RESEARCH (capability OPEN; §7.4 rule — no
  already-proved mapping exists that would force another class without
  pretending the capability is settled).
* **AsyncCondition** — code: zero includers, zero callers; depends on
  AsyncMutex, nothing depends on it. Authority: NOT NAMED. Formal: THEOREM
  B re-verified on the amended base (owner-gated admission of `wait`,
  `cond_irreducible`). Owner: OWNER_NO on all four pillars. Disposition:
  RESEARCH (THEOREM B × OWNER_NO).
* **AsyncRwLock** — code: zero abstraction callers; one mechanism coupling
  (timer-expiry path reuses `AsyncRwLock::ExpireCtx`), which any future
  implementation work must untangle first. Authority: NOT NAMED. Formal:
  THEOREM B re-verified (owner-gated admission of `unlock_write`,
  `rw_irreducible`). Owner: OWNER_NO on all four pillars. Disposition:
  RESEARCH (THEOREM B × OWNER_NO).
* **AsyncQueue** — code: zero includers/callers of `AsyncQueue<T>`; the
  retained QueuePort machinery is substrate, not the public abstraction,
  and is reachable only through the zero-consumer AsyncQueue surface.
  Authority: NOT NAMED. Formal: RESEARCH/DEFER — exact open boundary
  verbatim in the boundary section above (stage-6 card; old THEOREM B
  retired on `tracesEnc_shadow_false`). Owner: OWNER_NO. Disposition:
  RESEARCH (capability DEFER × any owner).
* **select** — code: zero callers of the public surface; depends on Event
  and the Scheduler timer; the Scheduler core retains dormant select
  machinery (admission/commit/finalize helpers, `waiting_select_count_`,
  `select_timer_pool_`) reachable only through that surface, which any
  future implementation work must untangle first.
  Authority: NOT NAMED. Formal: RESEARCH/DEFER — exact open boundary
  verbatim in the boundary section above (stage-7 card §6; old THEOREM B
  retired on the same refutation). Owner: OWNER_NO. Disposition: RESEARCH.
* **Scheduler::run / run_until_idle** — code: run's only caller is
  run_until_idle's inline body; run_until_idle has zero callers; the
  retained driver consumers use run_live through ApplicationRuntime and
  Group. Authority: §7.1 separation — ADR-0002 §8.1 frames Scheduler/Fiber
  as machinery that callers must not be forced through; no anchor names
  these two public entry points as product. Formal: RESEARCH/DEFER over the
  bespoke single-worker drain LTS (stage-8 card §6; old THEOREM B retired).
  Owner: OWNER_NO for these entry points specifically; driver-mechanism
  ownership is real and stays with the retained run_live/ApplicationRuntime
  path. Disposition: RESEARCH (capability DEFER × any owner).
* **multi-worker run** — code: production reality is
  `RuntimeBuilder::workers(n)` → `ApplicationRuntime` →
  `run_live(worker_count)`. Authority: execution policy per ADR-0001 §7,
  not a Scheduler public-API contract. Formal: explicitly outside the
  Stage-8 single-worker core; RESEARCH (deferred) per the campaign verdict.
  Disposition: RESEARCH.

## Adversarial decision attack (record)

Every row was attacked with the frozen A–L questions before write-back;
no row changed class during the attack. The decisive outcomes:

* **A (zero-consumer removal)** — every OWNER_NO survives with the
  zero-consumer fact removed: the load-bearing evidence is normative
  silence (O1), substrate ownership of the underlying boundaries (O3),
  and domain fit (O5). Zero-consumer appears only as corroborating O6.
* **B (capability labels hidden)** — every owner verdict follows from
  mission/ADR/architecture structure alone; no owner verdict cites
  THEOREM B or its absence as its reason.
* **C (implementation existence ≠ ownership)** — the scheduler_*.cpp
  implementation TUs and the retained QueuePort/wait substrate are
  mechanism facts (separation 2); they were not counted as ownership of
  any public abstraction except lock_guard's recorded mechanism-owner-YES,
  which is exactly what O4 separates from public ownership.
* **D (BASE(P) misuse)** — no row implements or proposes
  `BASE(P)`-shaped replacement; the BASE matrix is cited only as frozen
  capability language.
* **E (public API vs internal mechanism)** — applied explicitly to
  Scheduler::run/run_until_idle (mechanism retained via
  run_live/ApplicationRuntime; entry points not owned) and to lock_guard
  (mechanism owner YES; public owner NO).
* **F (hypothetical consumers)** — none relied on anywhere.
* **G (OWNER_NO completeness)** — the retained architecture that remains
  complete without every public abstraction above is named and real: apps
  + canonical File + blocking/await operations + RuntimeTaskContext /
  AsyncIoContext + the Scheduler park/wake/wait/timer substrate + backends;
  the census shows it references none of the candidate public surfaces.
* **H (OWNER_YES anchor)** — no row carries OWNER_YES for public
  ownership; no normative/product anchor exists for any of them on current
  authority.
* **I (KEEP honesty)** — no row is KEEP, so no row can be KEEP-because-
  THEOREM-B.
* **J (DELETE honesty)** — no row is DELETE; in particular the driver rows
  would have OWNER_NO but DELETE is unreachable because their capability
  is RESEARCH/DEFER, not proved reducible — the mapping was applied
  mechanically, not by taste.
* **K (INTERNALIZE reality)** — no INTERNALIZE disposition is issued in
  this closure; the two recorded internalization-shaped facts
  (lock_guard mechanism dependency; select→Event dependency) are kept as
  reconsideration evidence inside their RESEARCH rows, not converted into
  implementation authorizations.
* **L (RESEARCH boundary naming)** — every RESEARCH row names its exact
  open question in the follow-up boundary below.

Upgrades considered and rejected during the attack: AsyncMutex OWNER_YES
via "AsyncCondition depends on it" — rejected (the dependency chain never
reaches retained product; attack C/F); lock_guard INTERNALIZE now —
rejected (capability OPEN blocks a non-RESEARCH class; §7.4); Scheduler::
run DELETE despite OWNER_NO — rejected (DELETE requires proved
reducibility; the mapping forbids it).

## Implementation follow-up boundary

Nothing in this adjudication authorizes production changes. KEEP, CONVERGE,
INTERNALIZE and DELETE rows: none — consequently no narrow follow-up
implementation issues are created by #375; per the closure rule the
RESEARCH boundaries live in this document instead of speculative research
issues. A future implementation task must re-freeze the census and re-apply
this owner test on then-current authority before moving any surface.

Per-row RESEARCH boundary and the evidence that would allow
reconsideration:

* **Event** — capability: per-encoding-class conditionals only; the
  THEOREM-A question for the `extCap(set)=true` encoding class is open
  (stage-1 card §6; carried through the V2.3 replay in §11).
  Reconsideration: a settled capability result for
  that class, or a normative/product requirement naming a public event
  primitive, re-opens the owner mapping.
* **Semaphore** — capability: the THEOREM-B question is open in both
  directions (stage-2 card §5). Reconsideration: a reduction or a
  separating witness under Stage-0 V2.3, or a normative requirement for a
  public permit bound.
* **AsyncMutex** — capability closed (THEOREM B); the open question is
  product ownership: whether Sluice Core should own a public async
  exclusion primitive. Reconsideration: a retained internal consumer or an
  accepted product contract that names it.
* **lock_guard** — capability: the nonempty-`BASE` re-export test against
  the synchronous `Mutex` (GuardV2) was never delivered; the V1 THEOREM A
  is not inherited. Reconsideration: a delivered re-export proof (or
  equivalent settled capability result), after which the recorded
  owner split (mechanism YES / public NO) maps directly to an
  INTERNALIZE implementation task resolving the scheduler.hpp include
  surface.
* **AsyncCondition** — capability closed (THEOREM B); open question is
  product ownership, same shape as AsyncMutex.
* **AsyncRwLock** — capability closed (THEOREM B); open question is
  product ownership; any future disposition implementation must first
  resolve the `AsyncRwLock::ExpireCtx` timer-expiry coupling.
* **AsyncQueue** — capability: does there exist an Encoding QueueBase QSig
  observationally equivalent to `queuePrim` under Stage-0 V2.3 (stage-6
  card)? Reconsideration: that reduction, a universal mismatch proof, or a
  retained consumer that needs the public abstraction.
* **select** — capability: the equivalent SelSig question (stage-7 card
  §6). Reconsideration: same evidence classes as AsyncQueue.
* **Scheduler::run / run_until_idle** — capability: the equivalent RunSig
  question over `TracesRunS` (stage-8 card §6). Reconsideration: same
  evidence classes; separately, these two public entry points can be
  revisited by any future driver-surface implementation task if the
  retained driver consumers change.
* **multi-worker run** — capability: outside the Stage-8 single-worker
  core; modeling requires a multi-worker observation discipline or a
  serialized reduction, neither of which exists (campaign verdict,
  multi-worker section). Reconsideration: a constructed discipline of
  either kind.

## Gates at this closure

* Diff scope: docs/authority only — no changes under `src/`, `include/`,
  `tests/`, build files, or any `formal/*.lean` / `formal/tla` semantic
  artifact relative to `515d1d13`.
* `scripts/verify_formal.sh`: PASS — regression evidence that the authority
  write-back did not alter the merged formal campaign; the diff touches no
  formal semantic artifact.
* `scripts/verify_tla.sh`: PASS — regression evidence: 116 TLC
  configurations, unchanged from the campaign close.
* Fresh-context architecture review, round 1: REQUEST_CHANGES — BLOCKING 0,
  MAJOR 1 (this gates section pre-recorded an unperformed review verdict
  as a completed gate — the defect this revision fixes), MINOR 3 (the
  OWNER_NO rule wording; the select census omitting the retained
  scheduler-side select machinery; the Event open-boundary citation
  pointing at the wrong stage-1 section) — all fixed in this revision.
  The round-2 fresh-review verdict is recorded in the authority PR body
  and the #375 closure comment.
