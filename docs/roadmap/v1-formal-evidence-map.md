# Sluice v1 Formal Evidence Map

> **Non-normative.** This document does not define Sluice semantics,
> architecture, or public behavior. The sole normative authority is
> [`docs/explicit-io-v1-final-decision.md`](../explicit-io-v1-final-decision.md)
> (revision v1-r3). This map assigns evidence obligations to the adopted
> v1-r3 requirements, records what the existing formal corpus does and does
> not establish, and defines the closure format future proof records must
> satisfy. It cannot relax any root requirement and cannot upgrade any
> conformance-ledger status by itself.

| Field | Value |
|---|---|
| Role | Active tracking / evidence governance; non-normative |
| Established by | Issue #391 (V1-F0 formal evidence reset) |
| Baseline | master `145b952b15d1bb5dd15d326cf69281420ef844dc` (asset counts and audits below are facts about this commit) |
| Governing principle | Architecture selects proof obligations; proofs protect architecture; proofs do not select architecture |
| Owners of future closures | #394, #395, #396, #397, #400, #401 (per section 9) |

A mismatch between this map and the root is a defect in this map. Fix the
map; never reinterpret the root to match a proof or an asset inventory.

## Contents

1. [Formal asset inventory](#1-formal-asset-inventory)
2. [Disposition policy](#2-disposition-policy)
3. [Lean disposition table](#3-lean-disposition-table)
4. [TLA+ disposition table](#4-tla-disposition-table)
5. [Historical campaign reports](#5-historical-campaign-reports)
6. [#388 gap remap](#6-388-gap-remap)
7. [Evidence classes](#7-evidence-classes)
8. [Requirement family → evidence map](#8-requirement-family--evidence-map)
9. [Required conditional-liveness ownership](#9-required-conditional-liveness-ownership)
10. [Fairness and environment discipline](#10-fairness-and-environment-discipline)
11. [C++ memory-model separation](#11-c-memory-model-separation)
12. [Mutation and counterexample discipline](#12-mutation-and-counterexample-discipline)
13. [Source correspondence standard](#13-source-correspondence-standard)
14. [Lean's role under v1-r3](#14-leans-role-under-v1-r3)
15. [Closure record template](#15-closure-record-template)
16. [Tooling reproducibility status](#16-tooling-reproducibility-status)
17. [Standing review checklist](#17-standing-review-checklist)
18. [Non-goals of this map](#18-non-goals-of-this-map)

## 1. Formal asset inventory

Mechanical enumeration at the baseline commit (`find formal -type f | sort`;
theorem counts are `grep -c` of `^[ \t]*theorem `; the axiom-audit count is
the number of `#print axioms` entries in `scripts/verify_formal.sh`).

| Kind | Count | Notes |
|---|---:|---|
| Lean modules (`formal/**/*.lean`) | 12 | 11 logical modules + `Sluice.lean` (import hub, no content) |
| Lean lines | 16,851 | largest: `QueueV2` (2,368), `SelectV2` (2,031), `ConditionV2` (1,927) |
| Lean theorem declarations | 633 | includes step/facet lemmas; 240 exported theorems are axiom-audited by `verify_formal.sh` |
| TLA+ modules (`formal/tla/*.tla`) | 8 | 5,680 lines; largest: `RwCore` (1,024), `QueueCore` (908) |
| TLC configurations (`formal/tla/*.cfg`) | 116 | each defines `SPECIFICATION Spec` + `INVARIANT` entries (safety matrices, must-violate coverage certificates, must-die mutants); 0 contain `PROPERTY`/`TEMPORAL` |
| Verification scripts | 2 | `scripts/verify_formal.sh` (Lean), `scripts/verify_tla.sh` (TLC) |
| Toolchain/config files | 3 | `formal/lean-toolchain` (leanprover/lean4:v4.33.1), `lakefile.toml`, `lake-manifest.json` |

Two different things share the word "formal" and must not be conflated
([docs/README.md](../README.md)):

- `formal/**` — formal **source assets** (audited here).
- `docs/archive/formal/fcb1/**` — historical **campaign reports** (section 5).

No other formal source exists in the repository (no Lean/TLA+ under `docs/`
or `research/`).

What the corpus covers: the retired async **synchronization-primitive family**
(Event, Semaphore, Mutex, Condition, RwLock, Queue, select) and the
**single-worker drain driver** (`Scheduler::run` / `run_until_idle`). What it
does not cover, confirmed by reading every module: **no file I/O semantics,
no request lifecycle, no publication/visibility, and no liveness or
temporality of any kind** (no `WF_`/`SF_` conjunct, no `PROPERTY`, no
eventuality theorem). The corpus is entirely safety-class over bounded
prefix-closed traces.

## 2. Disposition policy

Every existing logical formal module carries exactly one disposition.

| Disposition | Meaning |
|---|---|
| `REUSE` | Proven abstraction, state vocabulary, authority owner, and observable semantics still correspond precisely to a retained v1-r3 mechanism, with root requirement IDs and source correspondence. Requires positive correspondence evidence — an elegant theorem is not enough. |
| `ADAPT` | The underlying mathematical/protocol idea remains valuable, but the module's state vocabulary, ownership, or environment assumptions no longer match v1-r3. The surviving core, the vocabulary that must be dropped, and the future owner ticket are named. |
| `HISTORICAL` | The proof is true and has research value, but proves a retired, research-only, or non-v1 surface. Keep the source; do not count it toward v1 conformance. |
| `RETIRE_FROM_V1_EVIDENCE` | The result must not be cited in support of any current v1 closure (wrong architecture owner, obsolete abstraction, safety-only result claimed as liveness, no source correspondence, or unbuilt product surface). This is not deletion: no formal source is removed by this reset. |

Decision rule: zero-consumer status alone decides nothing. The question is
whether the proof establishes a reusable mathematical invariant for a
**retained** v1 mechanism. A primitive proof may be `HISTORICAL` while one of
its lemmas (a bounded-queue invariant, a conservation ledger, a wake-credit
account) is recorded as an `ADAPT` core with a named future owner. Per
GOV-02/MIG-03, no old proof becomes v1 evidence by relabeling: adoption
requires re-derivation against the v1-r3 vocabulary and the source
correspondence standard of section 13.

## 3. Lean disposition table

The corpus is the FCB1/V2 campaign (stage reports in
`docs/archive/formal/fcb1/`). Theorem classes referenced below:
(a) safety invariants over arbitrary runs; (b) reachability/`Possesses`
witnesses; (c) refutations and mutant kills; (d) pure math lemmas;
(e) capability/irreducibility-calculus results. **No module contains any
temporal or eventuality claim** (class f is empty corpus-wide).

| Module | Lines | Thms | What it proves (representative theorems) | Old owner (C++ surface) | v1-r3 correspondence | Disposition | Surviving core / future owner |
|---|---:|---:|---|---|---|---|---|
| `Sluice.lean` | 11 | 0 | import hub only | — | none | n/a (index) | — |
| `CalcV2` | 1,756 | 39 | Stage-0 base calculus; trace alternation (`SeqOK`), effect-vs-return slot split (`FSlot`), `Wakes` order; refutation `tracesEnc_shadow_false`; run-splitting | scheduler substrate (`WaitQueue`, `worker_loop`, `global_mtx_`) | none — substrate retired; Scheduler demoted to optional host adapter (HOST-01) | HISTORICAL | `Obs`/`SeqOK` alternation, `FSlot` effect/return split, `Wakes`, run-splitting lemmas → re-derivable for any future Lean publication/wake ledger (#394/#397, optional) |
| `JudgeV2` | 196 | 11 | Vocabulary-free judgment layer: `Guarantees`, `Possesses`, `Refines`, `Reduction`, `ReducibleTo`/`IrreducibleTo`, `SeparatedBy` (`obligations_preserved_of_reduction`, `irreducible_iff_not_reducible`) | none (method layer) | none directly; parameterized machinery only | ADAPT | The judgment definitions are already vocabulary-free (parameterized over `ApiSig`/`PrimLTS2`/`Encoding`); the `PrimLTS2`/`Encoding` instantiations are retired. Owner: whichever future ticket first introduces a Lean pure-invariant artifact, under section 14's rules; no closure depends on it |
| `VacuityV2` | 544 | 16 | Non-vacuity controls: echo-primitive reduction (`echoRed`, `sim_echo_fwd/bwd`); lying-encoding refutation (`encLie_overProduces`) | none (synthetic probes) | none | HISTORICAL | Simulation-template method (shape predicates + config maps + run induction) noted for future model-vs-implementation equivalence arguments |
| `EventV2` | 1,378 | 44 | `eventNoWaitBeforeSet` guarantee (witness-in-prefix invariant `EventInv`); drain/ext batteries; mutant kill `eventMutant_not_guarantees` | `Event` primitive (`event.hpp`, `scheduler_event.cpp`) | none — public dormant primitive, outside v1 (MIG-02) | HISTORICAL | witness-in-prefix invariant pattern is generic; no named v1 obligation |
| `SemV2` | 1,643 | 83 | Permit conservation `semBalance_mirror`; `semPermitsHonored` guarantee (completed takes ≤ issued releases + initial); effect/return window refutations; mutant kill | `Semaphore` primitive (`semaphore.hpp`, `scheduler_semaphore.cpp`) | none — primitive retired | ADAPT | Conservation/take-bound ledger (in-flight credit terms) → template for progress-token/epoch and budget accounting (`V24`-progress, BOUND-01); owner #397 (+#394 for slot/budget accounting) when its model is built |
| `MutexV2` | 1,903 | 69 | `mutexExclHonored` guarantee; two-sided conservation `mutex_mirror`; THEOREM B `mutex_irreducible` over `BASE={Semaphore}` | `AsyncMutex` (`async_mutex.hpp`, `scheduler_mutex.cpp`) | none — primitive retired; core-internal locking is a THREAD-02 implementation detail, not a protocol | HISTORICAL | two-sided ledger + grant-credit lemmas noted |
| `ConditionV2` | 1,927 | 37 | Wake-backedness ledger (`condWakeBacked`, `cond_mirror`: completed waits ≤ wake credit published by resolvers — no-spurious/no-lost-wake accounting); THEOREM B `cond_irreducible`; 6 mutant kills | `AsyncCondition` (`condition.hpp`, `scheduler_condition.cpp`) | none — primitive retired; Mesa wait itself is not a v1 surface | ADAPT | Wake-credit/wake-backedness accounting → safety-half template for PROG-02 no-lost-wake evidence; owner #397 |
| `RwLockV2` | 1,450 | 36 | `rw_exclusion` invariant over arbitrary runs; THEOREM B `rw_irreducible`; batch-grant list lemmas; 4 mutant classes | `AsyncRwLock` (`async_rwlock.hpp`, `scheduler_rwlock.cpp`) | none — primitive retired | HISTORICAL | serve-maximal-prefix/stop-at-barrier list lemmas noted |
| `QueueV2` | 2,368 | 96 | `q_safe` (capacity bound; commit/delivery ledger `clog = dlog ++ ring`; drained-consumer gate); FIFO `q_fifo_holds`; result agreement; 5 mutants | `AsyncQueue` (`async_queue.hpp`, `scheduler_queue.cpp`, `queue_port.cpp`) | none — primitive retired | ADAPT | Bounded-ring + commit/delivery FIFO ledger → template for request-queue conservation and delivery-order invariants; owner #394 (order facet also feeds #396 delivery) |
| `SelectV2` | 2,031 | 110 | `selSafe` one-shot group discipline (at most one resolution record; committed winner only; `sel_battery_noSpurious`); 10 batteries; 5 mutants | `select` (`select.hpp`, `select.cpp`, `select_event.cpp`, `select_timer.cpp`) | none — primitive retired | ADAPT | One-shot attach/deliver/retire window + single-winner arbitration → template for OBS-02 at-most-once delivery; owner #396 |
| `DriverV2` | 1,644 | 92 | `runSafe` task conservation (`minted = queued + pending + running + retired`) + terminate-quiescence gate; 6 batteries; 5 mutants | `Scheduler::run` / `run_until_idle` / `spawn` (`scheduler.cpp`) | none — and the model's driver concept (drain-until-idle, single worker) contradicts PROG-01's fixed progress owner; the v1 obligations it would be miscited for are exactly the ones it cannot express | RETIRE_FROM_V1_EVIDENCE | Conservation/quiescence accounting is re-derivable for a progress-owner model; barred from any PROG/L2 citation. Research value retained |

Counts: **ADAPT 5, HISTORICAL 5, RETIRE_FROM_V1_EVIDENCE 1, REUSE 0**
(11 logical modules).

No module is `REUSE`: no old proof's abstraction, state vocabulary, authority
owner, and observable semantics correspond to a retained v1-r3 mechanism. The
primitive family is outside the v1 canonical surface (MIG-02) and was already
adjudicated zero-consumer/RESEARCH by the campaign's own owner test; the
driver model formalizes a surface whose v1-r3 successor (fixed progress owner
over `IoContext`, PROG-01/02) is architecturally different, not a refinement
of it.

## 4. TLA+ disposition table

Mechanical audit at the baseline commit, re-derived from source (not copied
from #388):

- All 8 modules define `Spec == Init /\ [][Next]_vars`.
- `grep -E 'WF_|SF_' formal/tla/*.tla` → **0 matches**. No fairness conjunct
  exists anywhere.
- `grep -E 'PROPERTY|TEMPORAL' formal/tla/*.cfg` → **0 matches** across all
  116 configs. Only `SPECIFICATION Spec` + `INVARIANT` entries exist.
- Every history/trace action is fuel-gated (per-cfg `MaxHistory`, range 3–11),
  and `[][Next]_vars` permits infinite stuttering; behaviors that stall forever
  satisfy every `Spec`. **Therefore every checked property is safety-only and
  no module, cfg, or past TLC PASS may be described as a liveness result.**
  Liveness-shaped names (`InvTermQuiet`, `InvWakeBacked`, `NoWaitBeforeSet`)
  are prefix-quantified state/trace invariants; a waiter that is never woken
  violates none of them.

| Module | Lines | Safety properties (semantic, beyond `TypeOK`) | Liveness? | Fairness? | Old C++ correspondence | Disposition | Surviving core / future owner |
|---|---:|---|---|---|---|---|---|
| `EventCore` | 302 | `NoWaitBeforeSet` (wait completion presupposes earlier set issue) | none | none | `Event` + scheduler wait queue | HISTORICAL | witness-in-prefix pattern; no named v1 obligation |
| `SemCore` | 823 | `InvCapacity`, `InvPermitPool` (exact conservation), `InvTakeBound` (completed takes ≤ releases + initial), FIFO/queue-ownership, `InvReleaseResult` | none | none | `Semaphore` | ADAPT | `InvTakeBound`/`InvPermitPool` ledger → `V24`-progress token/epoch accounting and BOUND-01 budget evidence; owner #397 (+#394) |
| `MutexCore` | 706 | `InvGrantBound` (counting-form exclusion), `InvBalance`, `InvRecordOwner`, `InvReleaseBacked` | none | none | `AsyncMutex` | HISTORICAL | — |
| `CondCore` | 786 | `InvWakeBacked` (completed waits covered by published wake credit), `InvFinishBacked`, `InvFinishOwned`, queue disciplines | none | none | `AsyncCondition` Mesa wait | ADAPT | wake-credit accounting → safety half of PROG-02 no-lost-wake evidence (the model itself serializes the park/wake race and has no epochs, so only the ledger shape transfers); owner #397 |
| `RwCore` | 1,024 | `InvExclusion`, `InvLedger` (`readers = grantR − unlockR`), `InvWriterOwned`, `InvFinishBacked` | none | none | `AsyncRwLock` | HISTORICAL | batch-grant lemmas noted |
| `QueueCore` | 908 | `InvCap`, `InvLog` (`clog = dlog ∘ ring` — conservation + delivery order), `InvDrained`, `InvNoCommitClosed`, `InvResultAgree` | none | none | `AsyncQueue` / `QueuePort` | ADAPT | bounded-ring + commit/delivery-order ledger → request-queue conservation and delivery-order invariants; owner #394 (order facet feeds #396) |
| `SelectCore` | 666 | `InvQuietArmed`, `InvWinnerDone`, `InvWindow` (≤1 undelivered resolution, exactly the committed winner), `InvGroupOwnership` | none | none | `select` group slot | ADAPT | one-shot attach/deliver/retire window → OBS-02 at-most-once delivery shape; owner #396 |
| `DriverCore` | 465 | `InvConservation` (no lost/duplicated task), `InvOccupied`, `InvIdleBacklog`, `InvTermQuiet` (`term` ⇒ quiescent state — the converse of "drain returns", i.e. still safety), `InvExtShape` | none | none | `Scheduler::run` / `run_until_idle` | RETIRE_FROM_V1_EVIDENCE | wrong architecture owner for v1 driver obligations (no progress owner, no persistent readiness, no wait protocol); barred from PROG/L2/L4 citations; conservation ledger re-derivable |

Counts: **ADAPT 4, HISTORICAL 3, RETIRE_FROM_V1_EVIDENCE 1, REUSE 0** (8 modules).

What the models lack relative to v1-r3's load-bearing protocols (basis for the
dispositions above): no versioned identity surviving accept→publish→reclaim
(no generations/epochs anywhere); no observer set (single tracked caller;
`SelectCore`'s `ParkUntracked` explicitly models extra waiters as never
woken); no shutdown state machine (one boolean in `DriverCore`, a close bit in
`QueueCore`); and no memory/visibility modeling below atomic-section
granularity (all state mutation is one atomic transition by design). The
reusable value is methodological — the `Mut*` mutation switches, `InvCov*`
negated-conjunction coverage certificates, and `Not*` witness separations
(sections 12) — plus the four ADAPT ledger cores.

## 5. Historical campaign reports

`docs/archive/formal/fcb1/**` (campaign verdict + stage 0–8 reports) are
**historical evidence only**. They document the FCB1/V2 campaign against the
superseded primitive architecture: PASS verdicts for Mutex/Condition/RwLock
irreducibility, RESEARCH/DEFER for Event/Semaphore/Queue/select/driver, the
`tracesEnc_shadow_false` refutation that retired the V1 shadow projection,
and the architecture-owner adjudication that found every campaign surface
zero-consumer. Per GOV-03 and the archive banner, FCB1 PASS verdicts do not
transfer to v1 and must not be cited as v1 evidence. This reset does not
modify those reports and does not reactivate their verdicts; the current
dispositions are sections 3–4, derived from source.

## 6. #388 gap remap

#388 is historical risk evidence (GOV-03). Its findings were re-derived
against current master where mechanical, and remapped to v1-r3 IDs and current
owners. Old North-Star/ADR/Scheduler vocabulary is not carried forward.

| #388 finding | Current validity | v1-r3 requirement | Owner | Action |
|---|---|---|---|---|
| Formal coverage concentrated on the primitive family; the production I/O spine had near-zero formal coverage | Still valid (re-confirmed: zero file-I/O/request/publication content in any module) | All load-bearing families (REQ/HANDLE/LIFE, OBS, PROG, CANCEL/BACKEND, SHUT) | #394/#395, #396, #397, #400, #401 | This map defines the obligation map; models are built with their owning tickets (no premature models — section 8) |
| No TLA+ model has any fairness conjunct; all 116 cfgs safety-only; the decisive liveness obligation is not statable | Still valid (mechanically re-confirmed at baseline: 0 `WF_`/`SF_`, 0 `PROPERTY`) | VERIFY-03 (L1–L5), PROG-02, SHUT-03 | #394/#395, #396, #397, #401 | Fairness/environment schema is mandatory for every future liveness closure (sections 9–10); old models remain safety-only forever |
| `scripts/verify_tla.sh` hard-fails: `tla2tools.jar` absent and gitignored | Valid at baseline; **fixed by this reset** (pinned, checksummed bootstrap; run record lands in section 16 with this PR) | VERIFY-01 evidence rules; GOV-04 (evidence references must name a commit/configuration) | #391 (fixed) | Option A implemented: version-pinned download + SHA-256 verification + offline override; no binary committed |
| No single source for the old §5.5 validation precedence (four independent implementations + divergent legacy fifth) | Partially superseded: the old ADR vocabulary is gone; SEM-03 is now the single normative source, and #392 delivered the shared semantic oracle (`detail/file_semantics.hpp`) used by direct/ThreadPool/io_uring paths | SEM-03, ERR-01 | #392 (done; status IMPLEMENTED_UNVERIFIED in the ledger), remainder #393/#394/#400 | Remaining divergence is tracked as ledger GAP rows (request-side precedence, legacy surfaces), not as a formal-model gap |
| DC-13 authority conflict: capacity reservation ordered before validation under saturation | Superseded as stated (old vocabulary); **closed on the request paths**: B1-B (ThreadPool) and B1-C (io_uring) put validation before the core reservation, and the A1 precedence rows record `#394 closed for this row` with zero recorded divergences | SEM-03 precedence steps 4 vs 5–7 | closed for #394's B1 scope | The arena `submit_transaction` keeps the old ordering as zero-consumer compatibility code; its retirement belongs to #395/#402 |
| MC-1 park/wake liveness (lost wake = hang; nothing can state the property) | Still valid, remapped | PROG-02, VERIFY-03 L2 | #397 | No-lost-wake safety + explicit-fairness liveness model + real poll/eventfd integration (section 9) |
| MC-2 RequestArena/Completion slot FSM unmodeled (terminal arbitration, deferred publication, exactly-once) | Still valid, remapped (surface becomes context-owned RequestCore) | REQ-01–REQ-05, HANDLE-01/02, VERIFY-03 L1/L5 | #394 (+#395 public binding) | Lifecycle TLA+ model + deterministic C++ interleavings + mutation configs (section 9) |
| MC-3 wait routing: stale drops/double delivery unproved | Still valid, remapped | OBS-01–OBS-03 | #396 | Observer attach/cancel/deliver/retire model + race tests (section 9) |
| MC-4 teardown lifecycle: six barriers unproved; destruction-order failures | Still valid, remapped | SHUT-01–SHUT-04, S8 | #401 | Shutdown state-space model distinguishing external vs internal refs + teardown fault injection (section 9) |
| MC-5 io_uring cancellation: exactly-once terminal under racing CQE unproved (and #388's F-11 noted a smoke test cited as cancel evidence that contains no cancel symbol) | Still valid, remapped | CANCEL-01, BACKEND-03, ERR-02 | #400 | Abstract arbitration model + fault injection + real-kernel evidence (section 9); the F-11 documentation defect is absorbed by the A1 ledger record |
| "The gap larger than all five": canonical I/O core (File semantics, precedence, backend refinement) has no model card | Partially addressed: SEM/ERR decision-table evidence now exists (#392); backend refinement remains open | SEM-01–SEM-07, ERR-01/02, BACKEND-01–03 | #393, #400 | Differential cross-backend evidence against the shared oracle; real-kernel io_uring configuration; request-side capability matrix (section 8) |
| FCB1 architecture-owner adjudication: all campaign surfaces zero-consumer RESEARCH | Still valid as historical evidence | MIG-02 (public dormant primitives outside v1) | #402 | Legacy retirement follows the consumer/migration audit; this reset does not delete formal sources regardless |

## 7. Evidence classes

Every evidence obligation belongs to exactly one primary class. "FORMAL /
TEST" is not a class. A single closure may combine classes, but each must be
individually recorded.

| Class | Covers | Typical instruments |
|---|---|---|
| `PURE_SEMANTIC_PROPERTY` | Decision tables, validation precedence, error mapping, checked arithmetic, finite semantic relations (SEM, ERR) | Exhaustive table tests, reference/property tests against the semantic oracle, Lean for stable pure math where it earns its place |
| `PROTOCOL_SAFETY` | State-machine invariants over all interleavings (acceptance, terminal arbitration, generation, reclaim predicate, observer transitions, no-lost-wake safety half) | TLA+ safety model with mutation configs; deterministic C++ interleavings; executable models |
| `CONDITIONAL_LIVENESS` | VERIFY-03 L1–L5 (shutdown convergence is L4; destruction additionally requires the HANDLE-03 binding-release precondition) — never standalone, always with explicit fairness/environment assumptions | TLA+ liveness properties (`PROPERTY` + `WF_`/`SF_` conjuncts), mutation configs that break the enabling mechanism |
| `CPP_MEMORY_MODEL` | Publication/happens-before, delivery visibility, handoff ordering (REQ-04, OBS-02, PROG handoff) | release/acquire chain review, mutex-synchronization argument, atomics-order documentation, litmus/deterministic concurrency tests, TSan where meaningful |
| `BACKEND_KERNEL_REFINEMENT` | Shared contract across ThreadPool/io_uring (BACKEND-01/02) | differential property tests against the semantic oracle, capability/setup-failure matrix |
| `RESOURCE_LIFETIME` | Slot/handle/binding lifetimes, reference retirement, reclaim predicate (REQ-05, HANDLE, LIFE, SHUT obligations) | lifetime/teardown tests, leak/retirement accounting, ASan/UBSan |
| `FAULT_INJECTION` | Behavior under dispatch failure, partial submit, poison, registration failure, allocation failure | deterministic fault-injection harnesses, negative tests, mutant models |
| `REAL_KERNEL_INTEGRATION` | Actual io_uring/eventfd/poll behavior on Linux | real-kernel io_uring conformance runs, event-loop integration examples (W-03), errno/negative-submit evidence |
| `HISTORICAL_RESEARCH_ONLY` | The retired corpus and campaign reports | none — excluded from v1 closure by definition |

Class discipline: a `PROTOCOL_SAFETY` result never closes a
`CONDITIONAL_LIVENESS` obligation; a `CONDITIONAL_LIVENESS` result never
closes a `CPP_MEMORY_MODEL` obligation; an `HISTORICAL_RESEARCH_ONLY` result
closes nothing.

## 8. Requirement family → evidence map

Preferred evidence per family, with explicit non-goals. Formal obligations
are introduced by the owning ticket together with the C++ protocol they
protect (MIG-01); this map pre-commits the *shape* of the evidence, not the
models. **No `RequestCore.tla`, `Observer.tla`, `ProgressSource.tla`, or
`Shutdown.tla` may be created from the root text alone** — each model grows in
its owning ticket with source correspondence (section 13).

| Family | Preferred evidence | Formal role | Non-goal | Owner |
|---|---|---|---|---|
| SEM / ERR | Decision tables + pure properties + reference/property tests against the shared oracle; real File/kernel evidence where drivable | Lean only for stable pure mathematics (checked arithmetic, finite relations); TLA+ is not a SEM/ERR default | Re-formalizing enums/tables in Lean "to have Lean"; a second semantic authority | #392 done (ledger: IMPLEMENTED_UNVERIFIED where production closure is incomplete); #393, #400 |
| REQ / HANDLE / LIFE | Protocol-safety TLA+ lifecycle model + deterministic C++ interleavings + publication happens-before (CPP_MEMORY_MODEL class) + fault/mutation tests | Future model state vocabulary must cover: `Reserved / Accepted / Executing / Chosen / Published / Reclaimable / Free`; public binding; backend/control pins; observer pins; generation; stale identity. V24 is **split**: `V24-request-id` (context/slot/generation exhaustion, stale alias — #394) is a distinct obligation from `V24-progress-token` (#397) | Deriving memory visibility from TLA+; assuming "the request eventually finishes"; one merged generation proof for both V24 halves | #394, #395 |
| OBS | Protocol-safety + conditional-liveness TLA+ observer model + deterministic attach×terminal and cancel×delivery race tests + reference-retirement tests | Safety floor: terminal-before-attach and attach-before-terminal each yield a unique delivery; at-most-once per registration generation; registration failure preserves the Request; observer cancel ≠ operation cancel ≠ borrow end; lifetime pin until delivery retirement. Liveness: section 9 (L3) | A general callback framework; letting observer cancellation settle the operation; Scheduler identity in the protocol | #396 |
| PROG | No-lost-wake safety model + real Linux pollable-notification integration + fairness-explicit progress liveness | Safety floor must cover: arm; acknowledge stale readiness; snapshot/token; bounded poll; final recheck; park/wait; wake race; epoch/token exhaustion (`V24-progress-token`). Liveness assumptions recorded explicitly (section 9): fixed progress-owner scheduling, backend eventual signal, persistent readiness, OS poll/eventfd behavior, control wake | "Thread eventually wakes" without a named fairness assumption; periodic busy polling masking a protocol defect; a condition-variable-only wait source satisfying W-03 | #397 |
| CANCEL / BACKEND | Abstract protocol-safety model (cancel intent, terminal arbitration, control-record retirement, confirmed-progress preservation, bounded/coalesced control state) + fault injection + real-kernel io_uring evidence + ThreadPool evidence | TLA+ may prove only the abstract arbitration/boundedness protocol | TLA+ standing in for io_uring CQE semantics, kernel cancel behavior, partial submit, eventfd, errno, or physical reference retirement | #400 |
| SHUT | Protocol-safety TLA+ state-space model + teardown fault injection + real teardown/lifetime tests | Future model state vocabulary: `Open / AdmissionClosed / Settling / ExecutionClosed / Destroyable` with `ExecutionClosed ≠ Destroyable`; variables must separate `external_binding_count`, `internal_backend_refs`, `internal_control_refs`, observer/delivery refs, public result binding, execution resources. Two distinct predicates, never merged: ready-but-unconsumed Request ⇒ external-precondition violation on destruction; only-delayed-internal-control-pin ⇒ destructor/internal retirement responsibility | Proving "all physical refs vanish" from "shutdown requested"; collapsing caller preconditions into internal retirement or vice versa | #401 |

For every future model, whatever its family, the safety-model action
vocabulary must map to implementation transitions per root VERIFY-02, with
separate variables for public binding, borrow-touching refs, control refs,
observer state, and progress readiness — collapsing them requires a proved
refinement, not similar enum names.

## 9. Required conditional-liveness ownership

Root VERIFY-03's conditional liveness is **required, not an optional
enhancement**. The word "required" below is binding: a ticket does not close
while its liveness rows are unproved or under-assumed, and no liveness row may
be written as "if the implementation declares eventual …". Every closure
record follows section 15 and the fairness schema of section 10. L2 (the
progress owner eventually observes an actionable event) is owned by #397 and
is the shared premise of L3/L5/shutdown liveness.

| Obligation | Property (conceptual) | Owner | Required evidence |
|---|---|---|---|
| L1 acceptance → publication | accepted request ~> eventual public publication | #394 core; #395 public-`Request<T>` correspondence | TLA+ liveness with declared fairness + C++ correspondence + mutation (below) |
| L2 owner observes actionable event | actionable work/control exists ~> owner observes it | #397 | no-lost-wake safety + fairness on owner scheduling and signal source |
| L3 observer delivery | armed + terminal published + driver continues ~> eventual delivery | #396 | TLA+ liveness + observer integration tests |
| L3 observer retirement | canceled/in-progress registration ~> eventual retirement | #396 | TLA+ liveness + reference-retirement tests |
| L5 core reclaim | released terminal slot + all pins retired ~> eventual reclaim | #394 | TLA+ safety of the reclaim predicate + liveness of reclaim progression |
| L5 progress-driven reclaim | released slot with only a delayed control/reclaim obligation ~> reclaim without unrelated new I/O | #397 (with #394) | ProgressSource model + integration evidence |
| no-lost-wake (safety) | a completed poll/arm/wait cycle cannot sleep past an unadvertised obligation (S9) | #397 | TLA+ safety + real wait-source evidence |
| progress liveness | persistent readiness + scheduled owner ~> progress observed | #397 | fairness/environment made explicit |
| L4 execution shutdown convergence | external preconditions met (per SHUT-03: finite accepted set, service, retirement assumptions) ~> `ExecutionClosed`; `Destroyable` additionally requires release of every public/host binding (HANDLE-03) | #401 | TLA+ liveness + real teardown evidence |
| publication visibility | backend writes happen-before consumer reads (REQ-04) | #394/#395 | CPP_MEMORY_MODEL class — never TLA+-only |
| backend refinement | both backends refine the same allowed result/effect set | #400 | abstract model + fault injection + real-kernel evidence |

**L1 closure schema** (the template for every liveness obligation): the
record must contain the actual checked temporal formula; the fairness and
environment assumptions (driver continues service; each backend operation
eventually completes or reaches a documented retirement; internal delivery
hooks terminate; required control progress occurs); the model (module, cfg,
bounds, state count, run metadata); at least one mutation/counterexample where
L1 fails; the C++ correspondence (acceptance linearization source, terminal
selection source, publication source, progress path); and the assumptions
outside Sluice authority (kernel/filesystem eventual completion, OS
scheduling). Decomposition rule: L1 must be proved through the protocol's own
enabling steps (dispatch → execution → terminal selection → publication),
each carrying its fairness conjunct. Assuming `EventualCompletion` to prove
`EventualPublication` and calling the result an L1 proof is prohibited.

**L5 ownership detail** (#394/#397): the model must express that a released
Request is not immediately reclaimable — backend refs, control refs, observer
refs, the public binding, result destruction, and any progress obligation each
gate reclaim. The named mutation: with a delayed control reference outstanding
and no unrelated new I/O arriving, the system must still converge to reclaim;
a model or implementation whose reclaim requires some future unrelated
submission to wake the driver fails this obligation.

**L3 alignment rule** (#396): L3's fairness/environment assumptions must be
stated in the same vocabulary as #397's progress assumptions (same owner
service premise, same hook-termination premise) so the two closures compose.

## 10. Fairness and environment discipline

A liveness claim without a fairness discipline is void. For every future
closure:

1. **Checked formula.** Quote the actual temporal property as checked (the
   `PROPERTY` line / TLA formula), not a prose paraphrase.
2. **Fairness conjuncts.** Enumerate each `WF_`/`SF_` conjunct on a named
   action. Minimum vocabulary:
   - driver/progress-owner service: the owner's progress step is fair
     (`WF` on the owner's dispatch/poll transitions);
   - backend operation: eventually completes **or** reaches a documented
     retirement (poison/fault terminal), never silently neither;
   - internal hooks: delivery/control hooks terminate (`WF`/`SF` on
     hook-return transitions);
   - control progress: required control transitions (cancel processing,
     reclaim servicing) are fair.
3. **Environment assumptions outside Sluice authority**, named separately and
   never presented as proved: kernel/filesystem eventual completion where the
   workload depends on it; OS scheduling of the runnable owner; eventfd/poll
   semantics; hardware/storage failure behavior. These bound the claim: a
   stalled filesystem invalidates the premise, and the closure says so.
4. **Prohibitions.**
   - `ASSUME EventualCompletion` ⟹ `PROVE EventualPublication` is not an L1
     proof.
   - "Thread eventually wakes" without a fairness label is not an assumption.
   - A safety-only model (no fairness, no `PROPERTY`) may never be described
     as proving liveness — this applies retroactively to all 8 existing
     modules (section 4).
   - Fairness must not be added to a retired model solely to rebrand it as a
     v1 liveness proof (section 14).
5. **Bounds.** cfg constants, explored state count, and run metadata are part
   of the record; finite TLC establishes only the explored instances
   (VERIFY-01).

## 11. C++ memory-model separation

Hard rule: **TLA+ is not a C++ memory-model proof.** `Published = TRUE` in a
model does not establish that backend writes happen-before Request consumer
reads. The `CPP_MEMORY_MODEL` evidence class is required independently for:

- REQ-04 publication (ready/terminal acquisition, backend→core handoff,
  producer → core-publisher → consumer chain) — owner #394/#395;
- observer delivery visibility (publication happens-before the hook observes
  the terminal; lock-outside-hook does not lose the lifetime pin) — #396;
- progress/core handoff where relevant (token/epoch publication vs state
  writes) — #397.

Acceptable evidence: the release/acquire chain (each link named with its
source location), mutex synchronization argument, atomic order documentation,
litmus or deterministic concurrency tests for any disputed ordering, and TSan
runs where meaningful. A memory-order review is recorded even when the
mechanism is "one mutex"; the review, not the mutex's existence, is the
evidence.

## 12. Mutation and counterexample discipline

Every load-bearing protocol closure must include at least one
mutation/negative model/counterexample demonstrating that if the invariant or
progress mechanism is broken, the evidence fails. Happy-path TLC passes are
not closure evidence. The existing corpus already models this discipline
mechanically (`Mut*` cfg switches killed by named invariants, `InvCov*`
negated-conjunction reachability certificates, `Not*` witness separations);
future models keep the same standard, with the exact violated invariant
checked, never a bare exit code.

Required mutation archetypes (to be instantiated by the owning ticket):

| Obligation | Mutation that must be killed |
|---|---|
| L1 | disable the dispatch/progress transition → liveness property fails |
| L3 delivery | drop the observer delivery transition → eventual delivery fails |
| L3 retirement | drop the retirement transition → registration pins forever, retirement liveness fails |
| PROG safety | remove the final recheck → lost-wake counterexample reachable |
| L5 | require an unrelated new I/O arrival for reclaim → reclaim liveness fails |
| SHUT | collapse internal and external reference classes → an invalid destroyable state becomes reachable |
| generation (V24-request-id) | reuse a slot without generation advance → stale-alias counterexample |
| token/epoch (V24-progress-token) | wrap the wait token without quiescent reset → lost-wake/alias counterexample |
| CANCEL | fabricate a zero-effect canceled result without dispatch-proof or erase a raced success count → arbitration/ERR-02 invariant fails |

## 13. Source correspondence standard

Correspondence is not "TLA state `Ready` ≈ C++ `maybe ready_`". A closure
record's correspondence section must state, for each model variable/action it
claims maps to code:

```text
MODEL VARIABLE / ACTION:
C++ AUTHORITY:            the owner of the state (root ARCH-02 table)
WRITE SITES:              file:line of every write
READ/OBSERVATION SITES:   file:line where the value is observed
LIFETIME:                 creation/destruction/retirement of the storage
SYNCHRONIZATION:          what serializes or orders the accesses
```

If such a table cannot be established for a claimed mapping, the record must
say `NO CURRENT CORRESPONDENCE`, and the relevant asset's disposition is
`HISTORICAL` or `ADAPT` — not `REUSE`. This standard is applied retroactively
in sections 3–4: every old module fails it against v1-r3 surfaces, which is
precisely why none is `REUSE`.

## 14. Lean's role under v1-r3

Lean is appropriate for: pure parameterized invariants; finite algebra;
checked arithmetic; generation arithmetic if it stabilizes; semantic
relations; small reusable mathematical lemmas (the `JudgeV2`-style judgment
layer is the retained template — see section 3).

Lean is not appropriate for: kernel behavior, thread scheduling, C++
destructor behavior, eventfd lifecycle, or io_uring refinement. Those live in
the `CPP_MEMORY_MODEL`, `FAULT_INJECTION`, and `REAL_KERNEL_INTEGRATION`
classes.

Constraints: no public primitive abstraction may be retained *because* its
old Lean corpus exists (MIG-02/PROD-03 decide retention); no retired model
may be given fairness or re-touched solely to rebrand it as v1 evidence; any
new Lean artifact must clear the section 13 correspondence standard for
whatever it claims to bind to, and otherwise stands as pure mathematics with
no v1 citation weight.

## 15. Closure record template

Every future formal closure (including every liveness obligation) appends a
record of exactly this shape to the owning PR description and to
`docs/roadmap/v1-conformance.md`. "TLC PASS" alone is never a closure.

```text
REQUIREMENT_IDS:            <root IDs, e.g. REQ-04, VERIFY-03 L1>
OWNER_TICKET:               <#NNN>
PROPERTY_CLASS:             <PURE_SEMANTIC_PROPERTY | PROTOCOL_SAFETY |
                             CONDITIONAL_LIVENESS | CPP_MEMORY_MODEL |
                             BACKEND_KERNEL_REFINEMENT | RESOURCE_LIFETIME |
                             FAULT_INJECTION | REAL_KERNEL_INTEGRATION>
FORMAL_MODEL:               <module path, revision/commit>
ACTUAL_PROPERTY:            <exact invariant or temporal formula as checked>
FAIRNESS_ENVIRONMENT_ASSUMPTIONS:
                            <each WF_/SF_ conjunct on a named action; each
                             outside-Sluice premise listed separately>
CONFIGURATION:              <cfg, bounds, state count, run metadata>
MUTATIONS_COUNTEREXAMPLES:  <variant where the property fails; expected
                             violated invariant; observed violation>
CPP_CORRESPONDENCE:         <per section 13 table; or NO CURRENT
                             CORRESPONDENCE, with the consequence stated>
CPP_MEMORY_EVIDENCE:        <for CPP_MEMORY_MODEL claims: chains, tests,
                             instrumentation>
KERNEL_BACKEND_EVIDENCE:    <backend/kernel runs, faults injected, configs>
ASSUMPTIONS_OUTSIDE_SLUICE_AUTHORITY:
                            <kernel/fs/OS scheduling premises>
RESULT:                     <PASS/FAIL/NOT RUN per command, with version>
LEDGER_STATUS:              <the v1-conformance.md row this record moves,
                             and to what>
```

## 16. Tooling reproducibility status

Baseline defect (#388, re-confirmed at this reset's baseline):
`scripts/verify_tla.sh` hard-failed because `formal/tla/tla2tools.jar` was
absent and gitignored, so no TLC evidence was reproducible from a clean clone.
This reset implements **Option A — fix reproducibility**: the script now
bootstraps a version-pinned `tla2tools` release and verifies its SHA-256
before running anything. No binary is committed (the jar remains gitignored);
`SLUICE_TLA2TOOLS_JAR` allows a pre-provisioned jar (CI cache / offline
mirror); a checksum mismatch refuses to run. A missing dependency still exits
nonzero with a distinct message — `NOT RUN` is never reported as `PASS`.

```text
LEAN_TOOLCHAIN:  leanprover/lean4:v4.33.1 (pinned in formal/lean-toolchain;
                 installed via elan 4.2.4)
LEAN_COMMAND:    scripts/verify_formal.sh          (lake build + sorry/admit
                 scan + 240-theorem axiom audit; allowed axioms:
                 propext, Classical.choice, Quot.sound)
LEAN_RESULT:     PASS (2026-09-21; elan 4.2.4, toolchain leanprover/lean4:v4.33.1):
                 lake build clean; sorry/admit scan clean; axiom audit over the
                 240 exported theorems — 224 depend only on
                 [propext, Classical.choice, Quot.sound] and 16 are fully
                 axiom-free ("does not depend on any axioms"); 0 sorryAx.

TLC_VERSION:     tla2tools v1.7.4 (latest stable release; pinned in
                 scripts/verify_tla.sh)
TLA_TOOLS_SHA256: 936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88
TLA_COMMAND:     scripts/verify_tla.sh             (full 8-module gate: safety
                 matrices, must-violate coverage certificates, must-die
                 mutants, exact-invariant kill checks; -deadlock is
                 legitimate: fuel bounds create terminal parked states)
TLA_RESULT:      PASS (2026-09-22, full gate; tla2tools v1.7.4, OpenJDK
                 25.0.4.1, Linux x86_64): all 117 TLC invocations completed
                 with their exact expected reason — 29 must-complete-cleanly
                 runs (safety matrices and witness-separation mutants) and 88
                 must-violate runs (coverage certificates and mutant kills,
                 each checked against its named invariant, never a bare exit
                 code); 0 FAIL. Operational note: this is a campaign-scale
                 gate, not a per-commit CI job — wall time ≈ 2h20m on a
                 dedicated 20-core host, and full-exploration clean-mutant
                 configurations (e.g. `CondCore MutParkHolds`, ≈35 min on
                 the reference run) dominate the cost.

REPRODUCIBLE_FROM_CLEAN_CLONE: YES
    prerequisites: java (>= 8; verified with OpenJDK 25.0.4), curl, network
    for first-run bootstrap; elan for Lean. Offline use: pre-provision
    SLUICE_TLA2TOOLS_JAR and an elan toolchain cache.
EXTERNAL_DEPENDENCIES:
    - tla2tools v1.7.4 from github.com/tlaplus/tlaplus releases (pinned,
      checksummed)
    - Lean toolchain v4.33.1 from the leanprover release channel (pinned by
      formal/lean-toolchain via elan)
    - bash, coreutils (sha256sum), java, curl
```

The run records above are facts about the #391 PR's own verification runs
(2026-09-21/22). This section is maintained by whichever PR next changes
tooling, keeping PASS/FAIL/NOT RUN distinguishable forever.

## 17. Standing review checklist

Re-verify these on every change that touches `formal/`, the root's VERIFY
sections, or the ledger:

- **A** — Does any old proof grant a retired public abstraction v1 standing?
  Must be **NO** (dispositions in sections 3–4; retention decisions belong to
  MIG-02/#402, never to proof existence).
- **B** — Is any fairness-free model described as a liveness proof? Must be
  **NO** (all 8 existing modules are safety-only; future liveness requires
  section 10).
- **C** — Are L1/L3/L5/progress/shutdown all REQUIRED? Must be **YES**
  (section 9); they are closure conditions of their owning tickets, not
  future enhancements.
- **D** — Is TLA+ ever treated as a substitute for the C++ memory model? Must
  be **NO** (section 11).
- **E** — Do FCB1 PASS verdicts enter the v1 ledger as VERIFIED? Must be
  **NO** (section 5; GOV-03).
- **F** — Does every future liveness closure require mutation/counterexample
  evidence? Must be **YES** (section 12).
- **G** — Is formal tooling reproducible from a clean clone? Must be **YES**
  (section 16) or carry an explicit `FORMAL_TOOLING_BLOCKED` record.
- **H** — If every old primitive proof were deleted tomorrow, would v1-r3
  architecture change? Must be **NO** (proofs protect architecture; they do
  not select it).

## 18. Non-goals of this map

This map does not: define or amend any requirement (root only); upgrade any
ledger status (only recorded evidence in the owning PR can); create the
future protocol models (owned by #394–#401 with source correspondence);
delete or rewrite retired formal sources; or turn the historical corpus into
v1 evidence. New formal gaps discovered later map onto the existing owner
tickets (#394/#395/#396/#397/#400/#401) rather than new issues, unless the
root itself must change first (GOV-04).
