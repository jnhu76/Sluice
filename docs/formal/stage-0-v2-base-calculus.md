# Stage 0-V2 — Frozen Base Calculus and Observation Model

Campaign: `FCB1-METHOD-CORRECTIVE-1` (repair of the FORMAL-CAPABILITY-BOUNDARY-1
stack, PRs #376–#385; charter issue #375)
Status: **FROZEN (V2.2)** — the V1 freeze was invalidated by the
method-corrective review; V2 was amended to V2.2 by BRAKE-1 (execution
domains, §7.1); this document is the single, complete Stage-0 freeze.
Toolchain: Lean 4.33.1, pinned in `formal/lean-toolchain`; no mathlib.
Verification gate: `scripts/verify_formal.sh` — `lake build` + no `sorry`/`admit`
+ axiom audit.
Current gate result: PASS (see §10).

## 0. Why V1 was invalidated

The method-corrective review found four MAJOR defects in the V1 stack:

* **MAJOR A — completion-shadow semantic asymmetry.** V1's encoding-side
  `complete` step emitted an observation and changed nothing, so any subset of
  completion observations could be erased from any encoding trace
  (`tracesEnc_shadow`).  The primitive side fused state mutation with the
  completion.  Every V1 THEOREM-B verdict rested on that asymmetric shadow.
* **MAJOR B — reduction was not parameterized by `BASE(P)`.** V1 could only ask
  "reducible to the bare substrate"; encodings had no access to earlier
  primitives, so `BASE(AsyncMutex) = {Semaphore}` was unexpressible.
* **MAJOR C — fiber identity was erased.** V1 minted a fresh actor per call,
  making mutex/condition/rwlock ownership contracts unmodelable.
* **MAJOR D — the freeze was not real.** Stages 1, 2 and 4 amended Stage 0
  retroactively (issue observations, serialized-completion declaration,
  admission effects) while keeping earlier verdicts.

V2 addresses all four at the calculus level, once, before any primitive stage.

## 1. Artifacts of the freeze

| Artifact | Content |
| --- | --- |
| `formal/Sluice/Formal/CalcV2.lean` | substrate, observation model, base-capability signatures, both call-execution machines, discipline lemmas, the anti-shadow certificate |
| `formal/Sluice/Formal/JudgeV2.lean` | judgments over the disciplined trace languages, base-parameterized reduction and irreducibility |
| `formal/Sluice/Formal/VacuityV2.lean` | method-level negative tests (§9) |
| this document | the frozen semantics and governance |

No primitive-specific verdict appears in any of these.  BRAKE-1 governs every
change.

## 2. Code-level identities

| Calculus | C++ counterpart |
| --- | --- |
| `FiberId` | `Fiber*` — **persistent** across the calls a fiber issues (`ws->current`, `WaitResume::fiber(me)`) |
| `Tick` | `deadline_tick_t` (`Scheduler::deadline_t`) |
| `QueueId` | a distinct `WaitQueue&` |
| `WaiterId` | a distinct `WaitNode&` registration token |
| `Outcome` | `WaitOutcome` (`wait_node.hpp`); `unresolved` is pre-terminal, not an outcome |
| `Registration` | a `WaitNode` linked into a `WaitQueue` (`waiter`, `owner`, `deadline`) |
| `SubState.queue` | `WaitQueue::head_/tail_` FIFO; `register_wait_locked` appends at the tail |
| `SubState.resolved` | one-shot `WaitNode::resolve_` results awaiting delivery |
| `SubOp.attach` | `WaitQueue::register_wait_locked` via `Scheduler::await_wait*` |
| `SubOp.suspend` | the fiber switch inside `await_wait` (a suspended fiber makes no progress) |
| `SubOp.wakeOne` | `Scheduler::wake_wait_one_locked` (resolves the queue head with `woken`) |
| `SubOp.cancelTok` | `Scheduler::cancel_wait` (resolves with `cancelled`, reports liveness) |
| `SubOp.nowTick` | `Scheduler::monotonic_now` |
| `EnvOp.expire` | `Scheduler::expire_wait` domain (timed registrations resolve `expired`) |

Substrate honesty (one-shot resolution, token freshness) is unchanged from V1
and remains proved from `nextWaiter` freshness plus per-queue sortedness.

## 3. Observation model (frozen)

The caller-observable events are API observations `Obs(caller, call,
result)` (V2.2):

* an **issue** (`result = none`) — emitted when the call enters execution:
  a fiber call at its fresh dispatch, an external call at its entry;
* a **completion** (`result = some r`) — emitted at the call's **physical
  return** to its caller.

The **caller** is a `Caller`: a scheduler `Fiber` (`Caller.fiber f`) or an
external OS thread (`Caller.ext x`).  External threads are never faked as
fibers — an external call has no legitimate `FiberId`, so the observation
record carries the caller, not a fiber.

Internal events (registrations, wakes, resolutions, dispatches) are not
observable.  This mirrors the public headers: callers see call entry and return
values, never scheduler internals.

**Invocation identity (MAJOR C).**  One caller has at most one call in
flight, and a caller's calls are strictly sequential: its k-th issue is
followed by exactly its k-th completion.  Invocation identity is therefore
per-caller alternation; no invocation counter is surfaced.  The same fiber
issues later calls after its earlier call returned — modeled by the
`retired` record on both machines and by the `SeqOK` trace discipline
below.  For an external caller the same alternation is its own thread's
program order (the `exts` in-flight record enforces one call per external
caller on both machines).

**Sequential-call discipline (`SeqOK`).**  Both trace languages are restricted
by the same predicate: per fiber, issues and completions strictly alternate
starting with an issue.  This is the C++ blocking-call contract
(`await_*` + `context_switch`).  It is symmetric by construction and checkable
on the trace alone.

## 4. The call-execution discipline (frozen, symmetric — MAJOR A)

Both sides — the encoding machine (`SysStep`) and the primitive machine
(`PrimStep2`) — run under the identical discipline, anchored in the single
worker `worker_loop` (`scheduler.cpp`):

1. **submit** — the environment makes a caller fiber runnable (silent; queued
   at the tail of the runnable queue).  Only a freshly minted fiber
   (`f = nextFiber`) or a retired fiber (`f ∈ retired`) may submit; a retired
   fiber leaves the retired list for the duration of the call.  C++:
   `pending_spawn_`, `route_runnable_locked` pushing onto `local_runnable`.
2. **dispatch (FIFO)** — the single worker pops the runnable-queue head.  A
   fresh dispatch emits the issue observation; a resumed dispatch (woken parked
   fiber) is silent.  C++: `worker_loop` popping `local_runnable` front; the
   issue observation is the fiber entering the API.
3. **run-to-block / run-to-return** — the dispatched fiber runs without
   interleaving until it suspends or physically returns.  No other fiber can
   act in between (the worker is busy).  On the encoding side its program
   (`ExtProg`) executes non-blocking substrate/base operations stepwise; on the
   primitive side the LTS's `run`/`park` facets decide, atomically.  C++:
   the call's critical sections run under `global_mtx_` inside the caller.
4. **physical return = completion** — the only completion-emitting step is the
   running call's return.  There is no step that emits a completion without the
   running fiber returning, and no step that delays a return arbitrarily: the
   V1 completion-shadow projection is **not** valid under V2, and
   `CalcV2.lean`'s `tracesEnc_shadow_false` proves its failure formally.
5. **wake publication** — a resolution whose registrant is a parked fiber
   suspended on the resolved token publishes that fiber runnable immediately,
   appending it at the tail of the runnable queue (FIFO).  A resolution with
   no such registrant records the outcome for the token's `WaitNode` and
   publishes nothing.  C++: `publish_wait_winner_locked` →
   `route_runnable_locked` (push_back under `inbox_mtx`) for the published
   case; the one-shot `WaitNode::resolve_` semantics for the recorded case,
   with the already-set branch of `event_wait_admit_locked` (resolve the
   caller's own node inline, `resolved_inline`) as the running-fiber instance.
6. **park** — a suspension moves the fiber out of the running slot (worker
   idle) and applies the suspension state effect.  A resumed call may park
   again (Mesa reacquire, `AsyncCondition::wait`).
7. **environment** — time advances and deadlines expire only while no fiber is
   dispatched: the single worker services the timer wheel between fibers
   (`worker_loop` idle path, `pump_deadlines_locked`, `scheduler_timer.cpp`).
   The clock visible to calls is sampled at these points.
8. **dispatch order** — among simultaneously runnable fibers the code is FIFO
   (`local_runnable` push_back/pop_front); the machines model exactly that
   FIFO.  No global cross-fiber completion order beyond the FIFO dispatch order
   is contractual.
9. **external calls (V2.2)** — an external-capable call is executed by its
   calling thread, not by the worker: it never enters the runnable FIFO and
   never waits for a fiber dispatch.  Both machines model the code's
   three-phase shape (function entry → critical section → return) as three
   independent steps:
   * **entry** — the issue observation is emitted and an in-flight record
     is registered (`extApply` primitive-side, gated by the primitive's
     per-call domain declaration `PrimLTS2.extCap`; `extStart`
     encoding-side, gated by `Encoding.extCap`).  No state effect here:
     entry precedes `global_mtx_` acquisition in the code, so entry order
     may differ from critical-section order (two threads may enter
     `Event::set` in one order and run their sections in the other).
   * **critical section** — the state effect, silent: the fused `extRun`
     facet fixes the result and readies woken parked fibers on the
     primitive side (`extEffect`); the encoding side executes its program
     stepwise (`extSubOpStep`/`extSubOpWake`) using only `extOpAllowed`
     substrate operations.
   * **return** — the completion observation (`extDone`/`extComplete`),
     unordered with respect to other steps (rule 10).
   External steps interleave with fiber steps wherever `global_mtx_` is
   free — including while a fiber is between its critical sections
   (`cur` does not gate external steps) — and an external call may begin
   while fibers are queued or running.  C++: `event_set_broadcast`,
   `event_reset`, `sem_release`, `sem_cancel` take `global_mtx_` only and
   read no `g_worker`.
10. **external completion (V2.2)** — an external call's physical return is a
    separate step (`extDone` / `extComplete`), deliberately unordered with
    respect to the other steps: holding `global_mtx_` serializes *state
    effects*, not physical returns.  Another caller's critical section may
    serialize between this call's effect and its return.  Confusing the two
    is exactly the V1 completion-shadow defect, so the calculus forbids the
    fused form by construction.

### 4.1 Primitive-side LTS shape (`PrimLTS2`)

A primitive under judgment provides: `State`, `init`, `admit` (entry critical
section, atomic with the issue observation), `run` (the fused inline paths —
`some (r, s', woken)` completes at physical return with state effect and woken
fibers in order; `none` goes to park), `park` (suspension state effect),
`finish` (a resumed parked call's completion at its dispatch), `extCap`
(V2.2 — the per-call external-domain declaration: `extCap c = true` iff `c`
can be issued by an external thread; it gates `extApply` and must agree
with `extRun`), `extRun` (V2.2 — the external execution of a call: its
single fused critical section run off the scheduler; `none` marks the call
fiber-only), `onTick` (clock mirror at idle points), `expire` (environment
expiry of one parked deadline).
These facets are the code's decision points; each stage card maps them to
`include/`+`src/` line anchors.

### 4.2 Encoding-side language (`ExtProg`)

A finite program per call over the substrate operations plus base operations
(§5), with pure control flow and a pure decoder.  No recursion, no cross-fiber
shared state, no external persistent state.  Substrate operations are frozen
(`SubOp`/`SubStep`); the V1 freeze clauses carry over.  V2.2 adds the
**call-domain declaration** `extCap`: the calls an external thread can issue
against this implementation.  An encoding whose `extCap` disagrees with the
primitive's own domains is refuted by the vacuity gate (`encLie_overProduces`);
the declaration is therefore part of the adjudicated surface.  Symmetry
holds by construction: both sides declare their external domains
(`PrimLTS2.extCap` / `Encoding.extCap`), run the same three-phase external
sequence, and face the same observation language and the same `SeqOK`
discipline.  One recorded granularity asymmetry: the encoding-side
external critical section executes *stepwise*, so fiber steps may interleave
between its substrate operations, while the primitive-side `extRun` is one
atomic step — the encoding side is therefore the more permissive of the
two, which is conservative for reductions (extra encoding freedom can only
make `bwd` harder, never vacuously easier).

## 5. `BASE(P)` composition (frozen — MAJOR B)

An encoding for stage N is an `Encoding O A` where `O : BaseOpsSig` is the
stage's frozen `BASE(P)` as a capability signature: one operation per base
call, the joint private state of the invoked base cores, and three execution
facets (`run` = fused admission + inline completion; `park`; `resume`) that are
exactly the adjudicated base core's own relations.

* `BASE(P) = {}` is `BaseOpsSig.none` — the bare substrate.
* A base operation executes as an **abstract semantic oracle**: its transition
  facets are the base stage's model relations, not its (nonexistent)
  implementation.  **Composition-faithfulness obligation (per stage, recorded
  in the stage card):** the oracle is exactly the adjudicated base core, whose
  model↔code mapping and verdict carry its fidelity; the reduction verdict for
  stage N is therefore relative to the base stage's verdict.
* Waking discipline for base operations (frozen one-level rule): a base call's
  waker step readies at most the parked base calls it announces (one level per
  machine step); base `resume` results may not chain wakes inside the same
  step.  Every base family used in this campaign (Semaphore, Mutex) satisfies
  this.
* `IrreducibleTo P O` quantifies over the closed class `Encoding O A` — the
  universal claim is kernel-checked.
* Enlarging `BASE(P)` after a proof gets hard is the BRAKE-2 stop condition;
  silently shrinking it because the framework cannot express it is BRAKE-7.

Stage defaults (subject to per-stage cards, unchanged from the V1 freeze):
`BASE(Event) = BASE(Semaphore) = {}`, `BASE(AsyncMutex) = {Semaphore}`,
`BASE(AsyncCondition) = {AsyncMutex}`, `BASE(AsyncRwLock) = {AsyncMutex}`,
`BASE(AsyncQueue) = {Semaphore}` (waking substrate only), `BASE(select) = {}`
(substrate + Event arm semantics), `BASE(Scheduler::run) = {}`.
`lock_guard`'s base is the **synchronous** `Mutex` (`lock_guard.hpp` binds
`Mutex&`, the `std::mutex` wrapper — not `AsyncMutex`).

## 6. Judgments (`JudgeV2.lean`)

* `Guarantees P Inv` — every disciplined primitive trace satisfies `Inv`.
* `Possesses P Q` — some disciplined primitive trace satisfies `Q` (vacuity
  guard).
* `Reduction P enc` — trace equivalence over the disciplined languages
  (`TracesPrimS` / `TracesEncS`).
* `ReducibleTo P O enc obls` / `Reducible P O obls` — `REDUCIBLE(P, BASE(P))`
  with obligation preservation (a corollary of `Reduction`).
* `SeparatedBy P O t` — one witness trace no encoding over `O` produces;
  kernel-checked universal, sufficient for non-reducibility.
* `UnderProduces` / `OverProduces` / `Mismatch` / `IrreducibleTo P O` — the
  complete criterion, exactly `¬Reducible P O obls`
  (`irreducible_iff_not_reducible`).

Allowed verdicts per primitive: **THEOREM A** (`Reducible P (BASE P) obls`),
**THEOREM B** (`IrreducibleTo P (BASE P)`), **RESEARCH / DEFER**.  RESEARCH is
preferable to a theorem whose proof target does not match the intended
architecture question.

## 7. Governance (MAJOR D)

1. This freeze is whole: `CalcV2.lean` + `JudgeV2.lean` + this document.  No
   later stage may modify them without **BRAKE-1**: STOP, record the amendment,
   invalidate all downstream verdicts, re-run affected stages.
2. No stage may amend the observation model, the machines, or the judgments
   "locally".  The V1 pattern (a stage silently gaining an `admit` field or a
   serialized-completion declaration) is prohibited.
3. Every stage card must record, before proving: the frozen `BASE(P)` as an
   explicit `BaseOpsSig` instantiation, the oracle-faithfulness obligation, and
   the model↔code anchors for every LTS facet.
4. Every THEOREM-B verdict must carry a possessed separating witness or a
   complete mismatch argument that does **not** rely on any completion-timing
   asymmetry (the shadow route is dead — see §8).
5. Every stage must keep: negative mutants per artifact, the gate scripts
   green, and a fresh-context adversarial review with corrective commit.

### 7.1 BRAKE-1 ledger

**v2.1 (during Stage 1V2 validation).** `subOpStep`'s original freshness
premise required a program step to resolve *nothing new*.  Stage 1V2 found
this unfaithful to the code and hostile to every stateful encoding: the code
routinely resolves registrations that no parked fiber is suspended on (the
already-set branch of `event_wait_admit_locked` resolves the caller's own
node inline and returns `resolved_inline` without suspending; sentinel and
marker idioms resolve retired registrants' nodes through
`WaitNode::resolve_`'s one-shot semantics).  Under the original premise the
encoding machine could not execute such steps at all, so an Event-stage
verdict would have reflected a machine artifact, not the architecture
question.  Amendment: `subOpStep` now requires only that no *newly resolved*
token has a parked registrant (those still go through `subOpWake`, which
publishes the registrant).  Code anchors: `event_wait_admit_locked`
(`resolved_inline`), `WaitNode::resolve_` one-shot semantics,
`scheduler_event.cpp:180-190`.  Downstream invalidation: none — no
downstream verdicts existed; the calc-internal certificates
(`tracesEnc_shadow_false`, `probeEnc_possesses`, `seqOK_not_shadow`) and the
vacuity reduction were re-verified by the gate after the amendment.

**v2.2 (external-call corrective, before any downstream merge).**  The V2
calculus treated every API call as a scheduler/fiber submission
(`submit` → FIFO → dispatch → run).  The production code contradicts this:
`Event::set`/`Event::reset` (`scheduler_event.cpp`) and Semaphore's
`release`/`try_acquire`/`cancel` (`scheduler_semaphore.cpp`) enter scheduler
state under `global_mtx_` with no `g_worker`/`ws->current` read, so an
external OS thread can issue them while scheduler fibers are queued or
running — an external `set` between two queued waits is legal production
behavior the V2 primitive LTS cannot express.  Amendment: `Caller`-
identified observations (`Obs(caller, ...)`, `SeqOK` per caller), the
per-call external-domain declarations on both sides (`PrimLTS2.extCap`
gating entry; `Encoding.extCap` symmetric), the `exts` in-flight records on
both configurations, and the external step rules on both machines as a
three-phase sequence — entry (issue observation, `extApply`/`extStart`),
critical section (silent state effect fixing the result,
`extEffect`/`extSubOpStep`+`extSubOpWake`), physical return (completion
observation, `extDone`/`extComplete`) — plus `extOpAllowed` (external
callers execute only the substrate operations that never register or
suspend: `attach`/`suspend` live in `await_wait*`, which requires
`g_worker`).  The issue is deliberately not fused with the state effect:
entry precedes mutex acquisition in the code, so entry order may differ
from critical-section order (an initial fused-`extApply` shape was refuted
in this stage's fresh-context adversarial review and corrected before
merge).  External invocation of base operations is deferred (no rule; see
§9.3b).  Downstream invalidation: **the Stage 1V2 Event verdict (PR #377)
is invalidated** — its countermodel relied on "set/reset must enter the
scheduler FIFO", which V2.2 removes; the Event stage is re-adjudicated on
V2.2 before any verdict is reused.

## 8. Method-level vacuity and negative tests (§9 of the corrective document)

| Test | Artifact | Requirement |
| --- | --- | --- |
| the framework is alive | `probeEnc_possesses` | an encoding produces a nonempty trace |
| the shadow is dead | `tracesEnc_shadow_false` | an encoding produces a trace whose completion-erased shadow it cannot produce — the V1 THEOREM-B engine is formally refuted |
| the identity discipline bites | `seqOK_not_shadow` | a trace violating per-fiber alternation is outside both languages |
| a reducible wrapper proves reducible | `VacuityV2.lean` (`echoPrim`) | `Reducible echoPrim {} []` — the THEOREM-A branch is exercisable |
| a base re-export must not become THEOREM B | stage 3 (lock_guard vs its declared base) | the first nonempty `BASE(P)` adjudication must certify the re-export THEOREM A |
| the external domain bypasses the FIFO (primitive side) | `CalcV2.lean` (`domPrim_possesses`) | two fibers queued ahead of an external caller; its issue and completion serialize between the first fiber's return and the second fiber's issue |
| the external domain is symmetric (encoding side) | `CalcV2.lean` (`domEnc_possesses`) | the same schedule produced by the encoding machine, same observation positions |
| a lying call-domain declaration over-produces | `encLie_overProduces` | an encoding claiming `extCap` for a fiber-only call is refuted against the primitive's `extRun = none` |

Correct models PASS, mutants FAIL; both branches of the verdict space are
exercised before any primitive verdict is trusted.

## 9. Open assumptions

1. Liveness claims are possibility properties in Lean; interleaving
   exploration and fairness live in TLA+ per primitive, with fairness named.
2. The encoding DSL excludes recursion and cross-fiber state.  A legitimate
   C++ behavior requiring either is a separating witness candidate — recorded,
   never absorbed silently.
3. Stage 8 (`Scheduler::run`) needs a declared substrate extension (driver
   state); adding it is a recorded extension, not a silent change.
3b. External invocation of `ExtProg.base` operations has no machine rule
   (V2.2 deferral, §7.1 v2.2), and an external program that steps outside
   `extOpAllowed` is likewise stuck.  A stage whose encoding needs either
   must first extend the machine by BRAKE-1 — both are checkable at the
   stage's PR boundary (a stuck program can only *shrink* the encoding's
   trace language, so under-production is the failure mode to audit).
3c. The `sluice_async_test::test_phase` hook inside `event_set_broadcast`
   is not in the retained tree (its header is gone); it is compiled out of
   production builds.  When test control is rebuilt, that hook must not
   read `g_worker` — it sits on an off-fiber path.
4. Multi-worker `run` remains outside the serialized single-worker discipline
   above; its adjudication status is a campaign-level question the FINAL
   VERDICT must address explicitly (RESEARCH is acceptable).

## 10. Gate

`scripts/verify_formal.sh` — **PASS (V2.2)**: build clean, no
`sorry`/`admit`, axiom audit within `{propext, Quot.sound}` — 13 audited
theorems including the three V2.2 execution-domain tests.
