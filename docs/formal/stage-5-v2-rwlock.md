# Stage 5V2.3 — AsyncRwLock core, replayed on the amended calculus

Campaign: FCB1-POST-V23-STACK-379-385 (Issue #375, §32). Calculus:
Stage-0-V2.3 as amended through Stage 4 (`CalcV2.lean`, unchanged by this
stage). Judgment layer: `JudgeV2.lean`. Chain: Stage-0 V2.3 → Event →
Semaphore (#378) → Mutex (#379) → Condition (#380) → this rwlock replay
(#381).

## 1. Scope and subject

AsyncRwLock: `include/sluice/async/async_rwlock.hpp`,
`src/async/scheduler_rwlock.cpp`. Core surface modeled: `read_lock`
(void), `write_lock` (void), `unlock_read` (void), `unlock_write`
(void), `try_read` (bool), `try_write` (bool), and `cancel` (bool,
external-capable). The timed extensions (`read_lock_until`,
`write_lock_until`, `expire`) are outside the core surface, exactly as
at Stages 2–4.

Two structural facts shape the model:

* **Reader shares are a count, not a registry.**
  `active_readers_` is an integer (`scheduler_rwlock.cpp:70`,
  `unlock_read` decrements it without an identity check — the
  count-only contract at :349); only the writer side carries an owner
  (`writer_owner_`, checked at `unlock_write`, :372-377). The model's
  `RwState` mirrors exactly that: `readers : Nat`, `wowner : Option
  FiberId`.
* **One mixed FIFO waiter queue, mode-tagged.** `WaitQueue` holds
  reader and writer nodes in arrival order (`RwWaitCtx`'s mode tag);
  the grant pass (`rwlock_grant_from_head_locked`, :38-133) decides
  from the head's mode. The model keeps `waitq : List (FiberId ×
  RMode)`.

## 2. Call-domain census (`extCap`/`extRun`)

| entry          | code                       | worker read | domain             | model                                   |
|----------------|----------------------------|-------------|--------------------|-----------------------------------------|
| `try_read`     | scheduler_rwlock.cpp:135-144 | none (mutex only) | external-capable | `extCap = true`, `extRun = rwExtRun` |
| `read_lock`    | scheduler_rwlock.cpp:258-288 | the prepare/finish worker pair | fiber-bound | `extCap = false` |
| `try_write`    | scheduler_rwlock.cpp:290-299 | none (mutex only) | fiber-bound (the result feeds the caller's section) | fiber machine only |
| `write_lock`   | scheduler_rwlock.cpp:315-344 | the worker pair | fiber-bound      | `extCap = false`                        |
| `unlock_read`  | scheduler_rwlock.cpp:346-356 | none (mutex only) | external-capable | `extCap = true`, `extRun = rwExtRun`    |
| `unlock_write` | scheduler_rwlock.cpp:358-366 | the worker pair (`assert(owner == me)`) | fiber-bound, owner-gated | `extCap = false` |
| `cancel`       | scheduler_rwlock.cpp:384-400 | none (mutex + queue mutex) | external-capable | `extCap = true`, `extRun = rwExtRun`    |
| `*_lock_until`, `expire` | timed extensions | `g_worker`  | fiber-bound        | extensions, outside the core surface    |

Disclosures carried into the model: the external-capable section is
non-executable in the worker (it runs on the caller's thread in the
C++), so `extRun` performs the section atomically and the issue is
silently refusable at dispatch; `try_write`'s result is consumed by the
caller's next step, so it is fiber-bound even though the check itself
takes no worker.

## 3. The grant pass and the two ledgers

The whole rwlock discipline lives in one function:
`rwlock_grant_from_head_locked` (:38-133). Its shape:

* a queued **writer** is served only when the lock is fully free
  (`readers == 0 && !writer_active_`, :60-69) — it claims the slot and
  becomes the owner;
* a queued **reader** drains the *maximal leading run* of readers,
  stopping at the first queued writer (:71-111) — each drained node
  publishes.

Everything else follows from re-running this pass at the right moments:
inline grants at admission (`read_lock` :165-172 resolves inline when
the fresh node lands at the head of an empty queue with the writer bit
clear; `write_lock` :223-230 additionally needs zero readers), after
`unlock_read` pays its share (the pass fires only at zero, :352-355),
after `unlock_write` clears the slot (:372-377), and after a cancel
removes its node (:384-400).

The safety split for this stage:

* **Mode exclusion** (`rwExclP`, Lean; `InvExclusion`, TLA): the
  writer bit excludes reader shares (`writing → readers = 0 ∧ wowner ≠
  none`) and its absence implies no owner (`¬writing → wowner = none`).
  Proved in Lean by runs-induction over all twelve `PrimStep2`
  constructors with per-transition preservation lemmas
  (`rwGrantHead_preserves`, `rwRun_preserves`, `rwPark_preserves`,
  `rwFinish_preserves`, `rwExtRun_preserves`) — the grant pass's
  writer branch is the only transition that sets the writer bit, and it
  requires the empty state; every reader-granting transition requires
  the bit clear.
* **The reader ledger** (`InvLedger`): `readers = grantR - unlockR`
  holds *exactly* — no window terms, unlike the condition's wake
  credit. Every effect (inline grant, batch drain, decrement, writer
  claim) is atomic inside its section action in both the Lean step and
  the TLA mirror, so there is no inter-step window to account for.

## 4. Direction B: batch completeness

The grant pass's reader branch is an under-production seam: a mutant
that grants only the head while publishing `batch_ready = 1` is
invisible to every safety invariant (the lock stays consistent; waiters
just starve). The stage-4 pattern applies: the drain facets state
exactly what the batch does — `rwGrantHead_batch_drains` (the granted
list *is* the maximal leading reader run), `rwGrantHead_batch_stops_at_writer`
(it stops at the first writer), `rwGrantHead_writer_blocked_by_readers`
(a writer claim requires zero readers) — and the battery-B run shows
the readied waiters actually surface. A mutant keeping the facets but
readying only the head is killed by witness separation: the batch
endpoint (`batch_ready ≥ 2 ∧ a resumed waiter completes`) is reachable
in the correct model (`RwCoreCovBatch`) and unreachable under
`MutBatchOne` (`RwCoreMutBatchOne` completes cleanly with
`NotBatchWitness` asserted, full safety intact).

## 5. What is proved (`formal/Sluice/Formal/RwLockV2.lean`)

* `rw_irreducible` — THEOREM B: the owner-gated *admission* of
  `unlock_write` (`assert(owner == me)`, :372-377) means the primitive
  never issues an `unlock_write` from a non-owner or over an inactive
  writer (`rwAdmit` refuses the dispatch), while every encoding's fiber
  machine dispatches any submitted call unconditionally. Witness:
  `wunlockIssueTrace` — `[issueObs RwSig (Caller.fiber 0)
  RwCall.wunlock]`, produced by every encoding (`rw_over_produces`) and
  by no run of the primitive (`rw_not_wunlockIssue`).
* `rw_exclusion` / `rw_exclusion_holds` — mode exclusion from
  `primInit` (§3).
* Grant-pass facets (§4's equations, plus
  `rwGrantHead_claims_head_writer`).
* Ledger facets: `rwUnlockRead_pays` (the decrement),
  `rwUnlockRead_zero_gate` (the pass fires only at zero),
  `rwConsume_mem`/`rwConsume_len` and the finish disciplines
  (`rwFinish_consumed`/`rwFinishW_consumed` — every waiter finish
  consumed a published record), and the admission identities
  (`rwAdmit_unchanged`, `rwAdmit_wunlock_owner_gated`).
* Possession batteries (explicit `PrimRuns2` step chains):
  - `rw_battery_inline_read` — a fresh fiber's `read_lock` resolves
    inline (empty queue, writer bit clear); `readers = 1` at the
    endpoint.
  - `rw_battery_writer_handoff` — from a held reader share with a
    queued writer, the `unlock_read` decrement reaches zero and the
    grant pass claims the writer; the endpoint owns the write.
  - `rw_battery_batch` — from a held reader share with two queued
    readers, the release drains both; `readers = 2` at the endpoint.
  - `rw_battery_cancel` — the external `cancel` of a queued waiter
    removes it, re-runs the grant pass, and completes `rbool true`.
* Mutant battery (four independent fault classes):
  - M1 `rwGrantHeadM1` (the writer claim ignores the reader count):
    killed by the exclusion discipline — `rwM1_exclusion_break` exhibits
    `writing` with readers held. Safety-kill shape.
  - M2 `rwRunM2` (`unlock_read` skips the decrement): killed by the
    ledger — `rwM2_unpaid` (shares minted without payment) and
    `rwM2_handoff_refuted` (the zero-gate never opens, the queued
    writer starves). Safety-kill shape.
  - M3 `rwGrantHeadM3` (the batch grants only the head):
    under-production — no safety invariant can catch it; killed by the
    drain facet refutation (`rwM3_undergrants`: the granted list is a
    proper prefix of the batch). Theorem-refutation shape.
  - M4 `rwAdmitM4` (`unlock_write`'s owner gate dropped at admission):
    killed by `rwM4_nonowner_admitted` — the primitive admits what the
    reference refuses, refuting the admission identity.
    Theorem-refutation shape.

Axiom audit (`scripts/verify_formal.sh`): all `rw*` theorems — the
irreducibility, exclusion, facets, batteries, and mutant kills —
depend on exactly `[propext, Quot.sound]` (or fewer).

## 6. Model correction recorded by this replay

The TLA mirror's first full run caught an invented invariant: the
mirror's queue-ownership discipline asserted `wowner ≠ waitq[i].f` (a
queued fiber is never the write owner). The counterexample is a
write-holder queueing as a reader — reachable in both the C++ and the
Lean model (`read_lock` has no owner gate; readers are a count, so the
library *cannot* check), safe under the grant pass (the queued reader
is not served until the writer releases), and a caller-side deadlock
protocol error at worst. The conjunct was removed: the mirror now
asserts exactly what the reference implementation enforces. This is the
stage's standing lesson against strengthening a mirror past its code.

## 7. The TLA mirror (`formal/tla/RwCore.tla`)

One action per `PrimStep2` constructor, the census of §2 as
comments, `Boot` selecting five initial states (`"prim"` — the
primInit-faithful start; `"rh1"`/`"rq2"`/`"rh2w"`/`"wq1"` — the
instrumented battery starts, relation-reachable but not
primInit-reachable for the same reason as Stage 4's: acquiring the
first share is the caller side's mutex traffic). Safety matrix:
`TypeOK`, `InvLedger`, `InvExclusion`, `InvWriterOwned`,
`InvFinishBacked`, `InvQueueNoDup`, `InvQueueOwnership`,
`InvCompDiscipline`.

Configurations and outcomes (distinct states at gate exhaustion): safety
`RwCore` (prim, h4, 1,603,930), `RwCoreRq2` (rq2, h5, 382,000),
`RwCoreWq1` (wq1, h5, 600,847) — all clean; coverage
`RwCoreCov{InlineRead,InlineWrite,WriterClaim,Batch,Cancel,TryFail,
CancelMiss}` — each violated on its negated witness (the witness is
reachable) under the mix-narrowing `WitConstraint`/`CancelConstraint`
(shrink-only); kills `RwCoreMutGrantWrite` → `InvExclusion`,
`RwCoreMutNoPay` → `InvLedger`, `RwCoreMutOwnerSkip` → `InvWriterOwned`;
separation `RwCoreMutBatchOne` — clean with `NotBatchWitness` asserted
(955,573 distinct states).

TLC lessons recorded by this mirror (each cost a red run before it was
understood; they are mirror-implementation facts, not model-vs-C++
disputes):

* **TLC evaluates `\/` and IF-conditions eagerly.** A `Head(q)` guarded
  only by an earlier `q = << >>` *disjunct* crashes on the empty queue
  even though the disjunction is true. Guards live behind nested
  IF-THEN-ELSE branches or `/\` conjuncts (`BatchLen`, `WriterHead`).
* **A function constructor over a computed empty domain fails `Seq`
  membership** (`[i \in (k + 1)..Len(q) |-> q[i]]` with `k = Len(q)`
  yields a value `Seq(S)` rejects). Queue slices go through `DropN`,
  which assigns the literal `<< >>` for the empty case.
* **`=` binds tighter than `/\`.** `wrel_owned' = wrel_owned /\
  (wowner = cur.fiber)` parses as *(the variable is unchanged) ∧ (the
  caller owns)* — a guard that silently disabled the ownerless release
  effects the M4 kill needed. The assignment is written
  `wrel_owned' = (wrel_owned /\ (wowner = cur.fiber))`.
* The decrement-then-grant order: `unlock_read` landing on zero with a
  reader batch lands at `readers = BatchLen(waitq)` (pay one, grant
  the run), not at zero — the first full run caught the mirror
  releasing the batch it had just granted.

## 8. Modeling disclosures

* **Count-only readers.** A reader share has no identity: the model
  cannot (and the code cannot) prevent a write-holder from queueing as
  a reader, nor attribute a share to a fiber. §6 records what that
  implies.
* **`unlock_read` from a non-holder is a dispatch refusal.** The :349
  held-share contract (`assert(active_readers_ > 0)`) is an admission
  gate (`rwAdmit`: `readers > 0`); an issued `runlock` over zero
  readers never dispatches, and the external section is non-executable
  past its effect (§2).
* **Void results.** `read_lock`/`write_lock`/`unlock_read`/
  `unlock_write` complete `runit` — no result semantics to bind, unlike
  Stage 2's `release(bool)`.
* **Boot waiters and boot holders are pre-Init.** The instrumented
  boots start with shares held and waiters parked past their issues;
  their first in-trace completion has no in-trace issue, the same
  prefix-scoped exception `InvCompDiscipline` documents at Stage 4.
* **Plain lock traffic is the mutex primitive's business.** The rwlock
  embeds no mutex; contention between fibers surfaces as queueing, and
  the worker-pair sections (`read_lock`/`write_lock`/
  `unlock_write`/`try_write`) are fiber-bound exactly as the census
  records.
