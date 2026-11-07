# #595 R5 Phase A — Next-Experiment Design (Candidates A/B/C + Phase-B contract)

Deliverable: field-by-field designs for the three candidate directions, the
recommendation, and the Phase-B execution contract. **Design only — nothing
in this document was executed in Phase A.** Written 2026-11-07 by the Phase A
synthesis writer from the verifier/tracer/diagnostic materials cited below.

Context recap (one paragraph): the policy's only real scheduling control is
the presentation loop's prefix length and re-entry rhythm
(`bench/r3_ready_batch.hpp:442-576`); chunk boundaries are not yields (C5);
the R4 p99-only regression at W03-d16 has a direction-level lead placing the
added tail time in the submission-side serialization of restart refills
(tail-op prep/submit means +20.2/+3.2 µs; tail ops 98.6% at `caller_window=0`,
in the second `input_exhausted` presentation; perturbed mode —
`r5/analysis/diagnostic-results.md`); the literature matrix adopts
Shenango-style persistence eligibility and Shinjuku's beneficiary
precondition, and rejects shrink-as-preemption (`r5/analysis/literature-matrix.md`).

---

## Candidate A — lighter R/S execution chain (policy action unchanged)

| Field | Design |
|---|---|
| **Target wait** | The per-presentation fixed cost that every back op of a restart-refill group waits behind (the L1 lead): the remaining duplicated preparation around each presentation — (a) the eligibility triple check (adapter mirror scan `r3:189-196` [now bounded by C1], seam per-op precheck `test_access.hpp:55-67`, backend per-member recheck `uring_backend.cpp:437-439,537`); (b) per-chunk re-copies of per-group data (facts copied into every chunk record, `r3:449-452`); (c) construction of the ~30-field `DecisionRecord` on the timed path when no trace sink is attached (`r3:446-537`). |
| **Input & authority** | Eligibility authority stays the backend predicate `batch_eligible_read` (`uring_backend.cpp:437-439`); the adapter's bounded scan stays a derived policy input (C1). `RequestCore::outstanding_` remains the sole authority for its fact — **accepted-and-unpublished** (a drain detector; not kernel inflight, and ==0 does not imply no observable/cleanup work remains — wording corrected 2026-11-07, F4). A removes only *derived duplicates*; it adds no state (mechanism-justification gate: classification "derived summary, non-authoritative", replacing existing duplicated derivations — replacement condition: oracle-equivalence proof below). |
| **Decision point** | Unchanged: the existing presentation-loop iteration on the cleanup base (`research-595-r5` @ `b191bb89`). No new decision is introduced. |
| **Action & executor** | Submitter thread, same loop: (a) classify the group's shape once per `Adapter::submit` call (all-eligible-4KiB vs mixed) and reuse it for policy input instead of re-deriving per re-entry — justified by call-invariance (see semantics); the backend re-check is NOT removed (it stays authoritative); the seam precheck is NOT narrowed this round (it guards the public-path mirror and its removal is not provably transport-identical for ineligible spans); (b) hoist the facts copy and the per-group record fields out of the per-chunk loop; (c) fill trace-only record fields only when `trace_ != nullptr`, keeping every `r4_*` counter increment byte-for-byte (frozen R4 join keys). |
| **Actual yield / service opportunity** | **None created.** A shortens work adjacent to the presentation critical sections; lock-free windows enlarge only in proportion to removed work. Stated plainly: A is a cost reduction, not a scheduling change. |
| **New work** | None (removals only; no new runtime state, flag, counter or token). |
| **Semantic proof obligations** | (i) Group-immutability invariant: within one `Adapter::submit` call the ready span is never mutated (harness builds it before the call, `bench_w03:399-413`, and does not touch it during), so per-member eligibility (`len==4096 && dst!=nullptr`, which reads neither `file` nor mutable state) is call-invariant; a once-per-call classification equals the per-chunk re-derivation. (ii) Transport-event equivalence: the R4 18-scenario deterministic ready-reuse oracle must pass 18/18 with byte-identical output (same instrument as C1/C2). (iii) Counter invariance: every `r4_*` counter unchanged on deterministic shapes (`r4_arm_smoke` 57/57 byte-identical; per-arm W03 smoke counters informational only, scheduling-nondeterministic per the A/B repeat study). (iv) No eligibility caching across calls or `NativeFileRef`/borrow lifetimes: the classification lives and dies inside one submit call. |
| **Expected direction (not a percentage)** | Downward pressure on the prep/submit tail intervals (less work per presentation the back ops queue behind) and on CPU; no expected effect on kernel/wake/reap/observe intervals; p99 gap should not widen. Throughput neutral-to-slightly-positive. |
| **Minimal counterfactual** | **Candidate vs R_cleanup** in the same matched blocks isolates exactly A's additional removals (per the contract's comparison semantics below: candidate−cleanup answers the candidate's change). R_cleanup vs R_original is the separate code-question comparison and does **not** isolate A; A0 anchors the formal gate. |
| **Rejection conditions** | Any oracle scenario diverges; any `r4_*` counter changes on deterministic shapes; W03-d16 CPU worse than R_original beyond noise; formal gate fail vs A0 on the **complete** target set — CPU point > −10% or one-sided-95 upper > −5%, throughput one-sided lower < −3%, or p95/p99 one-sided upper > +5% (A is judged against the full R4 caliber, not only the p95/p99 members); OR R_cleanup ≈ R_original within noise everywhere — then record "the duplicated preparation has no measurable d16 cost" (the code question is answered, proceed to Candidate B), which is a result, not a failure. |

## Candidate B — residue-aware continuous-submission budget with a genuine return to the progress/observer path

> **[WARNING — added 2026-11-07, r5-repair review R5: this design is NOT
> approved for execution as written.]** B splits a refill so back ops are
> rebuilt (and T0-stamped) at their later submission time; the measured
> latency then excludes the wait that already happened before that later
> build. That is a latency-origin change of exactly the kind F3 forbids:
> comparing the new origin against A0's per-item build origin is not a
> same-origin comparison, and equal gate thresholds do not repair a moved
> time origin. Any future run of B must keep A0's original T0 semantics for
> the comparison (stamp at first build attempt, or preregister an explicit
> arrival-origin metric applied to BOTH arms). Additionally, the r5-repair
> round's corrected diagnostic (CORRECTIONS R1) supports B's motivating
> lead — the quantum-8 chunk step and occupancy-8 refills — only at
> PERTURBED lead level, so B's premise is not yet confirmed evidence. The
> r5-repair round's verdict: Candidate B is not green-lit; no next
> intervention selected.

Naming note: "Candidate B" (this candidate) is unrelated to the R4
**arm B** (R3_FIXED_PREFIX, the suffix-singles baseline arm — defined in
CALL_PATTERNS.md §1.0 and CODE_FINDINGS_AND_FIXES.md C7.1); this candidate's
budget variable is always written `B_ops` to avoid the triple collision.

| Field | Design |
|---|---|
| **Target wait** | The restart-refill back-op queueing itself (L1): today a k-op refill is built and T0-stamped entirely at refill time, then presented in one `submit()` call; the back ops' measured latency contains the whole earlier-presentation serialization, and the driver rarely publishes mid-group. **[Corrected 2026-11-07, F4: the driver *can* publish mid-group — every presentation releases `access_mtx_`+`dispatch_mtx_`, and poll()'s publication loop runs outside the dispatch lock; mid-group publication is source-possible, merely rare because the submitter reacquires back-to-back. B's argument rests on created service opportunities, not on an impossibility.]** `B_ops` bounds the number of ops submitted consecutively, then **genuinely returns** to the caller's existing wait path. |
| **Input & authority** | **Primary signal (selected): the owner-current pending fact — live `ctx_.outstanding()`** (accepted∧unpublished, fresh, `async_io_context.cpp:837-840`; two locks per read), read **once per submit call, at `Adapter::submit` entry** — the same call site as the existing facts snapshot (`bench_w03:415-422`). Since B returns before the whole group is presented, one submit call == one budget epoch, so "once per submit call" *is* once per return boundary; there is no second read at the call tail. The dwell signal (the adapter's own untouched-suffix persistence across returns: "ops eligible at the previous presentation are still unserved now") is retained as the **pre-registered alternative** only — switched to if the primary's live read proves non-discriminating (it would fire almost never at saturation; Shenango row 7). Owner-pass facts (`owner_pass_activity`) are NOT used as the budget input (mislabeled and stale — C2); at most as a secondary, explicitly-stale diagnostic. **Default experimental budget (parameterized, one default): `B_ops = 8` when the entry read shows `outstanding()==0` (drain/restart moment — restart big, NAPI row 11) and `B_ops = 4` when `outstanding()>0` (owner still has pending accepted-unpublished work — return sooner, creating the service opportunity); variants {fixed 8, fixed 4, k = previous drain count} recorded per run as the ablation grid.** Authority: `RequestCore::outstanding_` remains the only inflight authority; the budget counter is a harness-layer derived summary. **Freshness**: live read at submit entry, never a cached group snapshot. **Dimension**: `B_ops` = max ops accepted without returning, per submit call. **Reset**: only on an actual return (the next submit call starts a new epoch); never inside the same submit call — otherwise B degenerates to chunking (C5's finding). **Ownership**: harness/adapter layer via a new research arm token; zero production-state change. **Invalidation/fallback**: on seam error, authority row, driver failure, or owner detach, fall back to the existing R behavior (full-span presentation with cap-reuse; the authority/rejected exits already exist, `r3:547-551`). |
| **Decision point** | Once per refill round, at `Adapter::submit` entry (the existing facts-snapshot site `bench_w03:415-424`), not per chunk; the budget value read there governs the whole epoch (call). |
| **Action & executor** | Submitter thread: present at most `B_ops` ops, then return from `submit()` with the unaccepted suffix **left in the caller's ready array** (harness-owned; see suffix lifecycle below). The caller proceeds to its existing cv wait (`bench_w03:522-539`); the driver (progress owner — the only thread that may poll, and it already does) publishes; the caller drains fronts; the next refill round builds the remaining ops fresh. The submitter never calls `poll_progress` (no cross-owner poll); no unconditional per-2-op polling of anything — one signal read per submit call. **Suffix lifecycle (explicit):** the stale ReadOps remaining in `ready` are never re-presented as-is; the harness's next refill round already begins with `ready.clear(); ready_ns.clear();` (`bench_w03:399-401`) and rebuilds from `base = submitted` — so the leftovers are simply discarded and overwritten, and no per-op state survives them (they were never admitted: no reservation, no slot binding). **Outer-loop change points (why the caller cannot immediately re-submit the residue before parking):** (a) adapter: a new `stop=bounded_budget` outcome returns control with `accepted < ready.size()` treated as *normal* (not the error/rejected path — no suffix-singles fallback for the un-presented remainder); (b) harness fill loop: on `accepted < want` with stop=bounded_budget, break out of the inner fill loop (`bench_w03:395-511` gains this break alongside the existing `max_refill` break at `:509-511`) so control falls through to the cv wait at `:522-539` — the existing wait/drain structure is what enforces the park before any next submit; (c) rebuild: unchanged `ready.clear()` + build-from-`submitted` as today. |
| **Actual yield / service opportunity** | **Yes — this is the mechanism.** After budget exhaustion the submitter reaches the existing `cv.wait_for` — a *potential* park (the predicate may already hold, so no block occurs; actual per-iteration park counts were never measured — wording corrected 2026-11-07, F4); the driver can acquire `access_mtx_`/`dispatch_mtx_` and publish the earlier ops; the caller drains them; the remaining ops are built later (T0 stamped then), so their measured latency no longer contains the earlier presentations' serialization. |
| **New work** | Suffix rebuild next round (re-derive ReadOps from `submitted` — same cost class as today's refill build, per round not per request); one `outstanding()` read per group (≤ SHADOW's per-presentation read frequency); a budget counter in the adapter. No new threads, timers, or per-request allocations. |
| **Semantic proof obligations** | (i) **Unaccepted-suffix ownership**: never-presented ops have no `RequestReservation`, no slot binding, no outstanding_ accounting (accept is the only increment site, `request_core.cpp:202`) — the library holds nothing; the suffix lives entirely in the caller's array and is rebuilt from `base = submitted` (only accepted ops advance `submitted`), preserving sequence monotonicity. (ii) **Window contiguity**: only accepted ops `push_back`; the window still holds consecutive sequences with size ≤ depth, so the C6 `sequence % depth` safety proof inputs are unchanged. (iii) **Application endpoint unchanged**: front-only in-order settle/verify/take (`bench_w03:541-572`) is untouched. (iv) **Finite progress**: the wait resolves only when `completions > watermark`; every accepted op has a pending kernel completion, so each wait is eventually satisfied in the no-fault case; fault cases use the existing 20 s timeout + failure path; budget ≥ 1 keeps acceptance advancing. (v) **Arm isolation**: implemented behind a new env token; R_original/R_cleanup code paths untouched (verified by build-flag diff). |
| **Expected direction (not a percentage)** | p99 at W03-d16 downward **if L1 is causal** (back ops no longer queue behind the whole group); CPU ambiguous (smaller serialized bursts vs added park/wake round-trips per budget exhaustion); throughput risk slightly negative if round-trips dominate; d4/btrfs must be rechecked — on shapes where R already improves the tail, extra waits could erode the win. |
| **Minimal counterfactual** | Candidate vs R_cleanup (same blocks) isolates the budget; if sensing value must be proven, add the fixed-policy ablation (same arm, budget fixed to a constant instead of sensed) — isolates signal value from action value. |
| **Rejection conditions** | Any protection cell violates (W03 d4/d2serial, W02 d1/d2: CPU upper > +5%, tput lower < −3%, p95/p99 upper > +5%); W03-d16 formal targets missed while prep/submit tail interval (if re-instrumented) did not move — refuting L1's causal role; livelock/timeout in smoke; any correctness suite fails; d64/btrfs recheck shows a new regression. |

## Candidate C — reduce preparation/observation coupling (two separate alternatives, not mixed)

**C-variant 1 — streaming preparation and submission.** Build/present the
refill in bounded pieces so back ops' ReadOps (and their T0 stamps) are
created closer to their actual submission. Honest assessment: without a
yield, the ops are still submitted in the same order at the same times —
only the latency origin moves. That changes **what the bench measures**, not
what the pipeline does (ReadOp construction itself is trivial); as a product
mechanism it is weak, and as a benchmark change it risks redefining the
metric rather than fixing the tail. Not recommended unless paired with an
explicit preregistered statement of what the latency origin is *supposed* to
mean (arrival vs build).

**C-variant 2 — separate completion observation from in-order application
delivery.** Observe per-request readiness independently (already public API:
`Request::ready()`/`try_result()`, `request.hpp:130-144`) while keeping the
application's in-order completion endpoint via a caller-side reorder stage;
window slots release earlier; the published-behind-front latch (G4/G5 E16)
loosens. Costs that must be counted: reorder buffer entries (bounded ≤ depth,
per-entry bookkeeping = a counted resource), changed failure-surfacing order,
and the safety obligation from C6 (no deque-scan + old-slot-allocation; slot
release must remain consumption-gated or per-entry buffer ownership
introduced). Changing the application contract to out-of-order *delivery* is
NOT this candidate — that is a separate workload exploration. Honest
assessment of priority: the diagnostic lead says the observe latch currently
penalizes **A0 more than R** (A0 observe tail-mean 24.9 µs vs R 10.4 µs), so
C-2 targets A0's weakness, not R's regression; its value for the R-vs-A0 p99
gap is speculative.

**C verdict: deferred.** Revisit C-2 only if Candidate B shows the observe
interval growing (i.e., the tradeoff moves); revisit C-1 only with a
preregistered latency-origin definition.

---

## Recommendation — the single next experiment

**Run Candidate A (on top of R_cleanup) under the Phase-B contract below.**

Rationale: (1) A is required by the matrix anyway — the R_cleanup and
Candidate arms both sit on the verified cleanup, so A's removals are the
natural first "one change"; (2) the diagnostic lead points at submission-side
serialization, and A directly shortens exactly that serialization with zero
semantic risk profile (equivalence-provable, no new state); (3) the ask
permits stopping at a lighter R: if A meets the formal targets, S redesign
stops; (4) Candidate B remains fully specified above as the follow-up if A is
insufficient — it is the only candidate that creates a *real* service
opportunity, and its risks (throughput, protection cells) are strictly
larger; (5) C is deferred for the reasons stated. All candidates were
designed, none rejected as impossible; the ordering is risk-ascending.

If the round owner prefers evidence over mechanism first: the alternative
single experiment is an **off-mode diagnostic replication** (≥3 A0/R pairs,
same frozen protocol, top-level lock) to convert the L1 lead into an
attribution — but this does not subsume A, which the matrix needs regardless.

---

## Phase-B execution contract (DESIGN ONLY — not executed in Phase A)

**Arms (all in one matrix, same frozen case set):**

| Arm | Definition | Question it answers |
|---|---|---|
| A0 | IMMEDIATE product path (`ctx.submit_read` per op) | reference |
| R_original | frozen R4 arm semantics @ `f3b25329` (run from the frozen tree/binary) | R4 anchor |
| R_cleanup | `f3b25329` + `c55165a2` + `b191bb89` only (confirmed-equivalent fixes) | **code question**: does the verified cleanup shift any metric vs R_original? |
| Candidate | R_cleanup + exactly ONE change (first: Candidate A's removals; if later: Candidate B's budget behind its own env token) | **scheduling/cost change**: candidate vs R_cleanup |
| (only if sensing value must be proven) fixed-policy ablation | Candidate with the sensed input replaced by a constant (e.g. `B_ops` fixed at 8 regardless of the entry read) | isolates signal value from action value |

Reading of comparisons: **cleanup−original** answers the code question;
**candidate−cleanup** answers the scheduling-change question; **candidate−A0**
answers the actual benefit (the formal gate).

**Cases and protections:** primary W03-uring-4k-d16 (warm tmpfs). Protected:
W03 d4, W03 d2serial, W02 d1, W02 d2 (CPU upper ≤ +5%, tput one-sided lower
≥ −3%, p95/p99 one-sided uppers ≤ +5%, candidate vs A0). Scope recheck: W03
d64 and btrfs warm buffered (R4 sign-flip shapes; descriptive-plus-protective
re-run to confirm no new harm).

**Formal targets (R4 full caliber, candidate vs A0, W03-d16):** CPU point
estimate ≤ −10% and one-sided-95 upper ≤ −5%; throughput one-sided lower
bound ≥ −3%; p95 and p99 one-sided uppers ≤ +5%. Latency/CPU orientation per
the C7.1 rule: cost `(X−A0)/A0` (negative = better), throughput
`(A0−X)/A0`-family with the pair order in the label — no mixed-orientation
row labels.

**CPU caliber:** whole-system CPU per op — submitter + kernel + io-wq (+ any
SQPOLL poller; none used). All participating executors counted. **No new
threads, timers, or per-request allocations** (thread-count and RSS sub-gates
as in R4; static-audit/strace/valgrind remain NOT_RUN unless separately
scheduled — the resource gate stays NOT_ESTABLISHED per the C7.6 wording
rules; never phrase untested dimensions as zero cost).

**Statistics:** default 24 balanced paired blocks per cell, interleaved
AB/BA orderings; blocks (not requests) are the independent units — requests
are never treated as independent samples; A/A-derived half-widths reported
next to every p99 claim; p99 precision insufficient → record
**NOT_ESTABLISHED** (no rescue by re-running until a preferred outcome).

**Discipline:** preregister arm definitions, case list, targets and analysis
scripts before any timed run; diagnostic-only runs never mixed with clean
blocks; measurement lock per the R5 round constraint
(`/home/jnhu/sluice-595-e5/MEASUREMENT.lock`, LOCK_RECEIPT with UTC stamps,
`fuser -v`, second-fd exclusivity probe; the r3-lock cross-round question is
recorded open in `env-context.md` §8 A1); no parallel builds/compression
during timed sessions; environment invalidation per R4 PREREG §10.1.
