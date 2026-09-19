# Stage 6V2.3 — AsyncQueue core, replayed and re-adjudicated on the amended calculus

Campaign: FCB1-POST-V23-STACK-379-385 (Issue #375, §33). Calculus:
Stage-0-V2.3 as amended through Stage 5 (`CalcV2.lean`, unchanged by this
stage). Judgment layer: `JudgeV2.lean`. Chain: Stage-0 V2.3 → Event →
Semaphore (#378) → Mutex (#379) → Condition (#380) → RwLock (#381) → this
queue replay (#382). **RE-ADJUDICATED ON POST-#378 V2.3 AUTHORITY**: the
pre-V2.3 stage-6 verdict is retired (§6); this stage's capability verdict
is RESEARCH/DEFER.

`BASE(AsyncQueue) = {Semaphore}` — the Stage-0 §5 stage default, unchanged by
this stage. No enlargement, no shrinking, no alternative base.

## 1. Scope and subject

AsyncQueue: `include/sluice/async/async_queue.hpp`,
`src/async/scheduler_queue.cpp`, `src/async/queue_port.cpp`. Core surface
modeled: `push` (result, fiber-bound), `pop` (item, fiber-bound),
`try_push` (bool-ish result, external-capable), `try_pop` (item or
refusal, external-capable), `close` (void, external-capable). Outside
the core surface, exactly as at Stages 2–5: the timed paths
(`push_until`/`pop_until` and the timed admission legs,
scheduler_queue.cpp:283-365, :37-99, :108-172), the teardown session
(queue_port.cpp:311-361), the lease location machine, the snapshot
reads, the re-entrancy audits, and `Scheduler::queue_cancel`
(scheduler_queue.cpp:367-379 — it takes the caller's own `WaitNode&`
and is reached only from `scheduler_test_access.hpp`; no public
`AsyncQueue` path issues it).

Two structural facts shape the model:

* **A ring, a bit, and two role FIFOs.** Buffered items live in a
  head-first FIFO ring (`ring_head_`/`ring_count_`); the closed bit is
  terminal; suspended producers and consumers sit in two separate
  FIFOs (`waiters_[0]`/`waiters_[1]`), and a suspended producer carries
  its item on the queue (the lease rides the node,
  `QueueWaitCtx::prod_lease`). The model's `QState` mirrors exactly
  that: `ring : List QItem`, `closed : Bool`, `waitqP : List (FiberId ×
  QItem)`, `waitqC : List FiberId`.
* **Two cross-grants and a drain.** `queue_grant_consumer_locked`
  (:381-405) hands the ring head to the head suspended consumer while
  items remain, else resolves `closed`; `queue_grant_producer_locked`
  (:407-432) commits the head suspended producer's lease into the ring
  iff the queue stands open and not full (:417-423), else resolves
  `closed`. `close` (:200-218) sets the bit (:214), drains consumers
  then producers (:216-217) through those two grant functions, and
  publishes every drained waiter runnable.

## 2. Call-domain census (`extCap`/`extRun`)

| entry        | code                        | worker read | domain             | model                                   |
|--------------|-----------------------------|-------------|--------------------|-----------------------------------------|
| `push v`     | scheduler_queue.cpp:56-99, :197-239 | the worker pair (`queue_push_admit` asserts a running Fiber, :199-200) | fiber-bound | `extCap = false` |
| `pop`        | scheduler_queue.cpp:108-172, :241-281 | the worker pair (`queue_pop_admit`, :243-244) | fiber-bound | `extCap = false` |
| `try_push v` | queue_port.cpp:125-164      | none (mutex only) | external-capable; **barges** past suspended producers | `extCap = true`, `extRun = qExtRun` |
| `try_pop`    | queue_port.cpp:166-198      | none (mutex only) | external-capable | `extCap = true`, `extRun = qExtRun`     |
| `close`      | queue_port.cpp:200-218      | none (mutex only) | external-capable | `extCap = true`, `extRun = qExtRun`     |
| `push_until`/`pop_until`, `expire` | scheduler_queue.cpp:283-365 | `g_worker` | timed extensions | outside the core surface |
| `queue_cancel` | scheduler_queue.cpp:367-379 | test access only | outside the core surface | not modeled |

Disclosures carried into the model: an inline grant additionally
requires the fresh node to land at the role queue's head
(`node.prev_ == nullptr`, :56/:128 — in the model, the role queue being
empty); `try_push` never registers, so it commits past suspended
producers; `admit ≡ some` — registrations cannot reject (a fresh
detached node), and closed/full/empty are *results*, not caller
precondition aborts.

## 3. The commit/delivery ledger

The whole queue discipline compresses into one ghost state equation
(the Lean `qSafe`, the TLA `InvLog`):

  **clog = dlog ○ ring**

`clog` records the ring-insertion order of every committed item; `dlog`
the delivery order of every handed-off item. The equation states item
conservation (every committed item is buffered or delivered) *and* the
external FIFO (completed successful deliveries respect committed item
order) at once. With it stand:

* **Capacity** (`InvCap`): `Len(ring) ≤ 2` — the frozen core capacity
  (`QueuePort` rejects capacity 0, queue_port.cpp:61-63; capacity is
  fixed per instance).
* **Drained consumers** (`InvDrained`): a suspended consumer only ever
  coexists with an empty ring — promoted from disclosure to proved
  invariant.
* **No commit after close** (`InvNoCommitClosed`): from the closed bit,
  the commit log is frozen and the ring never grows (items may still
  drain to consumers or read `closed` — the drain semantics).
* **Result agreement** (`InvResultAgree`): the Semaphore lesson — a
  wrong public result preserves every state invariant, so the
  agreement with the outcome authority (`qSpecRun`/`qSpecFin`, written
  grant-free) is its own check, per facet (`qRun_result_agrees`,
  `qExtRun_result_agrees`, `qFinish_result_agrees`).

Proved in Lean by runs-induction over all twelve `PrimStep2`
constructors with per-transition preservation lemmas
(`qRun_preserves`, `qPark_preserves`, `qFinish_preserves`,
`qExtRun_preserves`, and the grant lemmas `qGrantConsumer_preserves`,
`qGrantProducer_preserves`), plus the close-discipline trio
(`qRun_closed_inert`, `qExtRun_closed_inert`, `q_closed_no_commit` —
the commit log frozen and the ring non-growing from a closed state).

## 4. Possession batteries (six, one per behavior class)

Explicit `PrimRuns2` step chains (the Stage-5 idiom; batteries B–F
start from instrumented intermediate configurations — each a
constructed state proved safe by `qSafe`, whose shape the TLA mirror
explores from `primInit` via the `full2`/`emptyc` boots and the
park-coverage witnesses):

* `q_battery_inline` — from `primInit`: `push a` commits inline and
  returns `committed`; `pop` then delivers `a` in commit order
  (4 observations).
* `q_battery_handoff_pc` — ring full with a producer suspended on its
  lease: an external `try_pop` delivers the ring head, the cross-grant
  commits the suspended producer's item, and the resumed producer
  reports `committed` (the c→p capacity handoff: the slot the consumer
  freed is handed to the producer).
* `q_battery_handoff_cp` — a consumer suspended on an empty ring: an
  inline `push` commits and the cross-grant hands the committed item to
  the suspended consumer (the p→c handoff: the item flows producer to
  consumer).
* `q_battery_close_buffered` — `close` over a buffered item: the bit
  stands, the item stays poppable (the first `pop` still delivers it),
  a further `pop` reads `closed` (6 observations).
* `q_battery_close_consumer` — `close` with a suspended consumer: the
  drain resolves its outcome to `closed`, the resumed consumer reports
  it.
* `q_battery_close_producer` — `close` with a suspended producer and
  buffered items: the drain resolves the producer's outcome to `closed`
  (the bit refuses the lease), the buffered items stay poppable, and
  the retired producer pops one (5 observations).

## 5. Mutant battery (five independent fault classes)

* M1 `qRunM1` — the `closed` gate dropped from `push` (SAFETY):
  a push on a closed queue commits and grows the commit log;
  `qM1_breaks` exhibits the mutant committing where the model reads
  `closed`. TLA `MutCloseCommit` killed by `InvNoCommitClosed`.
* M2 `qRunM2` — `pop` delivers the second ring item (SAFETY):
  `qM2_fifo_break` — the ledger equation breaks
  (`clog ≠ dlog ○ ring` at the mutant endpoint while holding at the
  start). TLA `MutFifo` killed by `InvLog`.
* M3 `qRunM3` — delivery skips the producer cross-grant
  (TRACE-REMOVAL): the state stays safe; `qM3_handoff_refuted` exhibits
  the divergent endpoints (the parked producer is never granted). TLA
  `MutNoProducerGrant` completes cleanly with `NotHandoffPC` — the
  handoff witness is unreachable there, while `QueueCoreCovHandoffPC`
  certifies it reachable in the correct model.
* M4 `qRunM4` — `try_pop` on an empty closed queue returns `item a`
  (RESULT-SEMANTICS): the state does not move and stays safe;
  `qM4_wrong_result` exhibits the contradiction with the outcome
  authority. TLA `MutWrongResult` killed by `InvResultAgree`.
* M5 `qRunM5` — `push` commits without the consumer cross-grant
  (TRACE-REMOVAL): `qM5_handoff_refuted`; TLA `MutNoConsumerGrant`
  clean with `NotHandoffCP`.

## 6. Capability adjudication

* **OLD VERDICT — THEOREM B** (pre-V2.3 stage 6): AsyncQueue is
  irreducible to `BASE(AsyncQueue) = {Semaphore}`; every encoding over
  the semaphore base mismatches the queue primitive.
* **THE OLD PROOF IS REFUTED.** The proof rested on the
  completion-shadow projection: the claim that an encoding machine's
  completion outcomes project onto the primitive's runs through a
  stateless per-call mapping. `tracesEnc_shadow_false` (CalcV2.lean)
  refutes the shadow lemma itself under the V2.3 per-caller
  discipline: the projection does not exist, so no THEOREM-B-shaped
  conclusion can rest on it.
* **INVALID UNDER STAGE-0 V2.3.** With the shadow gone, the old
  universal mismatch proof has no premises to stand on; the verdict it
  carried is retired, not appealed. The retirement preserves the old
  result as historical evidence (the disposition record; the old
  branch head remains referenced from the campaign ledger).
* **NEW SEARCH RESULTS.** What the frozen method does support are two
  conditional, per-encoding-class statements (`q_capability_defer`):
  - Over-production for the class that declares the fiber-bound `push`
    externally callable: the primitive never emits an external `push`
    issue (`qExtCap (push _) = false`), while every such encoding does
    — at the encoding machine's entry step, before any program runs
    (`q_over_produces_of_extCap`, separator `extPushTrace`,
    non-vacuity instance `encExtPush_over`).
  - Under-production for the class that declares `try_push` not
    externally callable: the primitive emits the external `try_push`
    issue plus its committed completion (`q_possesses_extTryPush`),
    while no such encoding can emit that issue at all
    (`q_under_produces_of_no_extCap`, non-vacuity instance
    `encNoTry_under`).
  Neither statement is named THEOREM B and neither closes the universal
  question. The encoding class is inhabited (`encQ`,
  `q_encoding_class_inhabited` — the natural shapes bottom out with no
  completed observation: one semaphore cannot carry an item through
  the two-slot FIFO discipline).
* **VERDICT — RESEARCH/DEFER.** RESEARCH/DEFER does NOT mean
  reducible. RESEARCH/DEFER does NOT mean probably irreducible. The
  exact open boundary:

  > Does there exist an Encoding QueueBase QSig whose disciplined
  > trace language is observationally equivalent to `queuePrim` under
  > Stage-0 V2.3? No such reduction is presently constructed. No
  > universal mismatch proof is presently known. The previous universal
  > mismatch proof is invalid because its completion-shadow lemma is
  > false under V2.3.

* **The bounded probe (non-blocking).** The disposition asked whether
  an encoding can use persistent substrate/base state to distinguish
  "pop after push(a)" from "pop on an initially empty queue". Answer:
  YES — the semaphore base is *stateful* (`SemState` carries permits
  across calls), so base-side state can persist between an encoding's
  own operations. Recorded as counter-evidence against the old
  stateless-completion frame: the shadow projection's statelessness
  assumption was never a property of the base class. Persistence alone
  does not construct a reduction — the capacity-2 ceiling, the two
  role FIFOs with carried leases, and the barge/drain discipline are
  not carried by one semaphore's two-call language — but it removes
  "the base is stateless" from the list of candidate separators, and
  with it the last premise the old proof relied on.

## 7. Model corrections recorded by this replay

* **The close section publishes its drained waiters.** The first
  battery run exposed the model's `close` returning an empty wake list
  while the production drain resolves every suspended waiter and
  publishes it runnable (consumers before producers, in FIFO order).
  The model now returns the drained fibers from the close section
  (Lean `qRun`/`qExtRun`; TLA `ExtClose` builds the publications
  explicitly), and the close batteries exercise the resumed waiters'
  completions. Proof-neutral for the state invariants (the ledger
  never moved); VERIFY_FORMAL re-run green.
* **`PopDeliverGrantClosed`.** The TLA mirror's first full run
  dead-ended where the code delivers: a `pop` over a *closed* queue
  with a suspended producer must deliver the item AND resolve the
  producer's outcome to `closed` (the :417-423 gate on the bit), not
  skip the grant. The Lean model already had the shape (the grant is
  total); the mirror gained `PopDeliverGrantClosed` and the ext
  closed-grant branch.

## 8. The TLA mirror (`formal/tla/QueueCore.tla`)

One action per `PrimStep2` constructor, the census of §2 as comments,
`Boot` selecting three initial states (`"prim"` — the primInit-faithful
start; `"full2"` — ring `<<A,B>>` with f0 parked pushing A; `"emptyc"`
— f0 parked popping on an empty open ring; the boots are
relation-reachable but not primInit-reachable in-window, the documented
battery disclosure). Safety matrix: `TypeOK`, `InvCap`, `InvLog`,
`InvDrained`, `InvNoCommitClosed`, `InvResultAgree`, `InvQueueNoDup`,
`InvQueueOwnership`, `InvCompDiscipline`.

Configurations and outcomes: safety `QueueCore` (prim), `QueueCoreFull2`,
`QueueCoreEmptyC` — all clean; coverage `QueueCoreCov{InlinePush,
InlinePop,PushPark,PopPark,HandoffPC,HandoffCP,CloseBuffered,
ClosePushPark,ClosePopPark,FifoMulti}` — each violated on its negated
witness (the witness is reachable) under the mix-narrowing
`WitConstraint` (shrink-only); kills `QueueCoreMutCloseCommit` →
`InvNoCommitClosed`, `QueueCoreMutFifo` → `InvLog`,
`QueueCoreMutWrongResult` → `InvResultAgree`; separations
`QueueCoreMutNoConsumerGrant` / `QueueCoreMutNoProducerGrant` — clean
with `NotHandoffCP` / `NotHandoffPC` asserted (full safety intact).

TLC lessons recorded by this mirror (mirror-implementation facts, not
model-vs-C++ disputes; the first two are the Stage-5 lessons biting
again in new clothing):

* **`=` binds tighter than `/\` and `\/`.** `f' = a \/ b` parses as
  `(f' = a) \/ b` — the assignment silently narrows to `f' = a` and `b`
  becomes a standalone guard; TLC then reports "successor state not
  completely specified" (or, worse, accepts a wrong ledger). Every
  primed boolean assignment is parenthesized: `f' = (a /\ b)`.
* **Every primed variable must be constrained per action.** A guard
  (`waitqP = << >>`) is not an `UNCHANGED`; TLC rejects the action the
  first time the guarded variable's value matters.
* **Resumed waiters complete with their consumed outcome.** The finish
  carries the record's outcome (`c.out`) into the completion
  observation — the resumed slot's own `result` field is still `none`
  (the V2.3 split: the waker fixed the outcome, the finish reports it).

## 9. Modeling disclosures

* **Two role FIFOs, not one mixed queue.** The C++ splits waiters by
  role; the model keeps `waitqP`/`waitqC` and never crosses them (the
  drain order consumers-then-producers is the only cross-role
  ordering).
* **Suspended producers carry their items.** The lease rides the node;
  the grant commits the carried item, not a re-read.
* **The closed queue stays readable.** Buffered items remain poppable
  after `close` (the drain semantics); the bit refuses only new
  commits and leases.
* **Fiber-origin close is outside the modeled fiber call domain.**
  `close`'s core-relevant caller is the external one; the TLA fiber
  call domain carries only push/pop (the RwLock `cancel` precedent),
  keeping the state space at two fibers. The Lean `qRun` retains the
  fiber path (the sections are caller-agnostic).
* **Boot `phase` is loose off the queue**, and boot waiters are
  pre-Init — the same prefix-scoped `InvCompDiscipline` exception the
  Stage-4/5 cards document.
* **Barge vs queue fairness.** `try_push` barges past suspended
  producers by construction (it never registers); the model encodes
  the mechanism, and makes no fairness claim about which suspended
  producer starves.
* **The external `try_pop` delivery guard is conservative.** The
  model's delivery branch additionally requires `waitqC = []`
  (Lean `qRun`, TLA `ExtTryPop`); the C++ fast path has no such
  check. It fires only on states the model itself proves unreachable
  (the drained conjunct of `qSafe`: a nonempty consumer queue implies
  an empty ring), so no reachable behavior is added or removed.

## 10. Gates and verdict

* `scripts/verify_formal.sh` — **PASS** (lake build; no sorry/admit;
  every exported queue theorem depends only on
  `[propext, Quot.sound]` or fewer).
* `scripts/verify_tla.sh` — **PASS** (stages 1–5 re-run green; Stage 6:
  3 safety boots clean, 10 coverage witnesses violated, 3 safety
  mutants killed on their exact intended invariants, 2 trace-removal
  separations clean).
* `CalcV2.lean` / `JudgeV2.lean` — unchanged.
* Fresh-context adversarial review: **READY** (2026-09-19). All ten
  protocol questions OK — census/base/production fidelity (no
  model-vs-code fact conflict), safety validity, battery non-vacuity,
  mutant discipline, old-verdict retirement, adjudication,
  mandated wording, gates. Five MINOR wording/hygiene findings, all
  fixed in this revision: the §4 handoff arrow glosses were swapped
  (`handoff_pc` is the c→p *capacity* handoff, `handoff_cp` the p→c
  handoff — the naming tracks the parked/unblocked role); the
  battery-header "reachable" claim softened to the constructed-shapes
  claim; the TLA census comment's false justification replaced by the
  disclosed scope narrowing; two census line ranges tightened
  (:197-239, :241-281); the conservative external `try_pop` delivery
  guard added to §9 (plus the §1 `push` result-type gloss). Both
  gates re-run green after the fixes. A proposed-but-unproved
  separator candidate (wake-order through a single substrate FIFO) is
  recorded as a proposal only — per protocol it does not move the
  verdict.
* Verdict: **STAGE6_SEMANTICS_PASS / CAPABILITY_RESEARCH_DEFER /
  READY_FOR_STACK_CONTINUATION.**
