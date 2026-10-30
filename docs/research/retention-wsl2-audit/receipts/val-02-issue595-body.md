# Shape-Aware Submission Policy — amortize `io_uring_enter` without sacrificing progress or tail latency

> **Goal:** convert the independently measured #593 K2/K4 submission gap into a **minimal, safe internal io_uring submission policy**. Preserve v2 semantics, retain a correct immediate-submit fallback, and measure the *complete* request lifecycle on native Linux. This is an optimization/implementation issue, **not another broad key-path discovery campaign**.
>
> **Parent:** #563 (Minimal Necessary Work × Hardware-Aware Design). **Evidence:** #593 / research PR #594. **Benchmark authority:** #591 and [performance methodology](https://github.com/jnhu76/Sluice/blob/master/docs/performance/methodology.md). **Downstream workload validation:** #578. **Transport-flush oracle:** #585 (CLOSED).
>
> **Execution model:** continuous workflow; no mandatory owner-approval pause between research phases. Any production PR stays **OPEN / not merged** until explicit owner decision. Correctness and measurement validity remain hard requirements.

## 1. Confirmed opportunity and known falsifiers

Research archive: [#594](https://github.com/jnhu76/Sluice/pull/594), especially [K4](https://github.com/jnhu76/Sluice/blob/research/593-key-path/docs/research/key-path-593/k4-sqe-cqe-batching.md), [decision](https://github.com/jnhu76/Sluice/blob/research/593-key-path/docs/research/key-path-593/decision.md), and [correctness review](https://github.com/jnhu76/Sluice/blob/research/593-key-path/docs/research/key-path-593/correctness-review.md).

Frozen #593 study: `bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57`; native E5 Xeon E5-2666 v3 / warm tmpfs / GCC 16.2.1 / liburing 2.13. **Re-freeze current master for this new issue; do not treat the old SHA as the new product baseline.**

- Baseline `dispatch_after_accept` calls `submit_transport_locked()` inline: roughly **1.00007 `io_uring_enter` per logical operation**; SQE submit side does not batch while CQE reaping already batches.
- **K4-b**, deferring to the existing progress-pass submission with a derived topology + `outstanding_summary()>1` predicate, measured **W-02 4 KiB D=4/16/64**: CPU/op **−13.06% / −28.07% / −35.53%**; W-02 d16 wall **−28.08%**; enters/op **0.50017 / 0.12577 / 0.03447**.
- **K2-a ungated deferral** exposes additional headroom (`~1/D` enters vs K4-b `~2/D`) but **regresses D=1 CPU +14.12% and p50 ×14.1, W-03 d16 p99 +15.70%, 64 KiB p99 +6.77%**. It is a mechanism bound, **not a deployable policy**.
- K4-b itself **regresses 64 KiB d16 p99 +7.46%** (and p95 +7.50%). **D=2/D=3 unmeasured**; W-03 d4 +2.54% CPU is unattributed and needs same-session A/A. Do not claim a valid general `D≥2` optimization.
- K2-a and K4-b are **the same optimization mechanism**, not additive improvements. K1 contributes a possible smaller independent W-03 improvement, but is **out of scope for the first implementation**. K3's separate pass-tail fusion is a negative finding; do not reopen without new trigger.

**Research question:** Who should decide *when* an accepted operation's prepared SQE becomes kernel-visible, and based on which already-authoritative state, while ensuring that no admitted work can be stranded or unacceptably delayed?

## 2. Explicitly compare three internal policies

All three obey the **same public v2 API and request/result/lifetime semantics**:

| Policy | Mechanism | Benefit | Principal failure mode |
|---|---|---|---|
| `IMMEDIATE` | Flush at/after each accepted nonzero dispatch (current baseline) | Low first-operation latency, reliable fallback | One enter per op; poor amortization |
| `PROGRESS_DRIVEN` | Defer selected SQEs to an already-required, legally driven progress pass | Amortize kernel boundary and io-wq/notification cost | No/late progress pass, split owner, unexpected mixed-driver transitions |
| `BOUNDED_BATCH` | Flush on demonstrated batch/urgency/progress conditions, with a **real finite submission bound** when no pass is forthcoming | Potentially reduce K4-b's first-window flush while bounding latency | Timer/scheduling overhead, batching-induced tail regression, unproved deadline |

Do **not** automatically create a timer thread, public batching knob, virtual scheduling layer, or global policy manager. `BOUNDED_BATCH` is an experiment until a concrete, enforceable bound and its cost are demonstrated. An existing progress boundary can be preferred over added infrastructure. A logical batch threshold without a producer/driver wake guarantee is **not** a time bound.

A **conservative internal choice** (operation/payload, achieved outstanding, submitter/driver topology, pending kernel/ledger/control obligations, and existing progress guarantees) is a candidate, **not a policy assumption**. K4-b's "two most recent driver thread IDs" is an approximate predictor, **not proof of same-thread ownership**. Reusing this summary requires explicit mixed-driver fallback analysis.

## 3. Concrete optimization hypotheses (investigate all; adopt only winners)

### P1 — Safe, shape-aware submission selection (primary)

Compare K4-b to a narrower, robust internal predicate:
- Preserve `IMMEDIATE` for isolated/no progress driver, W-03/split-owner or unknown topology, cancellation/control urgency, unsupported payloads, and unverified depth shapes.
- Experiment first with supported **4 KiB / D=4,16,64**. Distinguish configured D from **actual outstanding count**. Test **D=1,2,3** explicitly; a predicate that incurs a Core lock but cannot reduce enters at D=2 is not a win.
- Explore payload crossover including 4/8/16/32/64 KiB, and confirm whether an operation-kind/payload gate can prevent the known 64 KiB p95/p99 regression without merely overfitting one threshold.
- Test W-03 d4 with matched A/A, W-03 d16/d64, and submitter/driver migration or mixed progress ownership.
- Make fallback correctness explicit: an unverified hint may select a *slower* valid mechanism, but must never make progress/liveness depend on that hint.

### P2 — First-flush elimination / batch formation (higher-risk research)

Explain why K4-b pays roughly `2/D` enters versus K2-a's `1/D`:
- Is the first inline flush necessary to guarantee prompt progress? Under exactly which public or internal driving promises could an already-owed pass absorb it?
- Compare first-submit-inline versus **provably prompt** progress-boundary submission; explicitly price deadline/timer/scheduling mechanisms if introduced.
- **Do not** remove first flush merely because a benchmark's caller is known to refill later; API callers may submit one request and then wait.
- Stop with `FIRST_FLUSH_NECESSARY` if a safe, cheap scheduling witness is unavailable.

### P3 — Residue advertisement / notification coalescing

K4-b deferral adds `signal_ready_progress()` for newly unsubmitted ledger residue. Can the **transition from no actionable residue to actionable residue** be signaled once rather than per accepted op?
- Map token/epoch/wait handshake, `eventfd` writes, acknowledgment, concurrent producers, cancellation and a parked external driver.
- Count extra progress locks, notifications, eventfd writes/reads, passes/op; count work transferred to a different thread.
- Coalescing must retain an exact "new work cannot be missed" obligation; no sleep-based oracle or speculative lost-wake suppression.

### P4 — Avoid the added Core-lock decision cost

K4-b adds `core_->outstanding_summary()` (locking `RequestCore`) per submission. Can an **already validated** reserve/accept count, existing state summary, or lifetime-valid snapshot support the decision?
- No second outstanding authority; no stale snapshot used to waive a correctness obligation.
- Compare net added reads/writes, atomics/cache coherence, fallback checks and CPU/op.
- Reject if the complexity exceeds measured savings. Treat `submit_flushes_` from #585 as a **submit-attempt counter**, not a count of SQEs or a requirement for 1 flush/op.

## 4. Correctness obligations: deferral changes *time*, not semantics

Read and map relevant code at the execution SHA: `AsyncIoContext`, `UringAsyncBackend::submit_request/dispatch_after_accept/poll`, `dispatch_one_locked`, `submit_transport_locked`, `TransportLedger`, `RequestCore`, `ProgressSource`, `reap_cqes`, cancellation and shutdown.

Every candidate must preserve:
- Accept/reserve/rollback/capacity/rejection and public request identity/context/generation.
- Exact cookie/router correspondence, SQE-position/ledger accounting, `rc<pending`, `EAGAIN/EINTR/EBUSY`, partial consumption and sticky poison recovery.
- A finite, justified route for **all accepted but not yet kernel-submitted operations** to be submitted or safely settled (including when no second submit arrives); no unbounded silent buffering.
- Terminal exactly once; buffer borrow not released before physical touch is retired; late/stale CQEs, cancel/control refs and slot reuse.
- ProgressSource snapshot/park/wake handshake, dispatch retry, control interruption, bounded pass fairness, CQ overflow flush, W-03 external-loop expectations, close admission and shutdown/settlement.
- Zero-length operations and metadata/sync semantics; no assumed reads-only public behavior.

**Specific missing oracles from #593 that must be repaired, not merely re-disclosed:**
1. `k4_residue_wake_test` uses 100 ms sleep and any progress wake: replace with a **deterministic prepark handoff** and an oracle that distinguishes the intended residue wake from unrelated wake sources.
2. `k4_retry_residue_wake_test` covers inline-flush EAGAIN, not **deferred multi-entry pass-start partial/retry**. Force `ledger.size()>1`, explicit `rc<pending` and retry; verify exact accounting and eventual settlement.
3. Add deferred-shape cancel/control storms, cancel-before-first-flush, late/duplicate CQE, early slot reuse, CQ overflow, poison, shutdown and mixed-driver concurrency tests; at least one deterministic mutation for each newly relied-upon obligation.
4. Reconcile existing #585 positive flush-count assertions with *actual submit attempts* under each mode. Fix the fixture's now-invalid **per-op** expectation only under a new, narrow behavioral test; do **not** redefine the counter as "number of requests" or silently weaken the old oracle. `#585` itself stays CLOSED.

Require `BASE GREEN → NAMED MUTANT RED → RESTORED GREEN`. Timeout/crash not attributable to the targeted assertion does not count as a mutation kill. No `sleep` used as synchronization proof.

## 5. Continuous implementation workflow (no ceremonial phase gates)

**W0 — Freeze and reproduce:** inspect #594's research patches and reports; freeze current master SHA, machine identity and full W-02/W-03 byte/lifecycle oracles. Work in isolated worktrees. **Do not touch or relocate** `/home/jnhu/sluice-593-research/evidence/` (owner retains existing raw evidence there; no archival or backup project is authorized here).

**W1 — Policy anatomy and hypotheses:** source-backed DAG and decision table for `IMMEDIATE`, `PROGRESS_DRIVEN`, experimental `BOUNDED_BATCH`; list exact state owner, validity window, promptness witness and fallback conditions. Derive competing explanations for 64 KiB tails, first-flush necessity and W-03 cost migration.

**W2 — Research arms:** adapt the verified K4-b patch without blindly importing its heuristics; build separate minimal variants for P1/P2/P3/P4 (prefer single-factor). Preserve clean baseline and full identity. For any infeasible variant record a concrete falsifier/complexity reason rather than fabricating one.

**W3 — Deterministic safety:** repair wake/retry oracles, add deferred-ledger/partial submit/cancel/overflow/teardown witnesses and mutants. Run available debug/release/liburing-off/sanitizer lanes where applicable. Stop unsafe arms, not the entire campaign.

**W4 — E5 native experiments:** when E5 is available, execute a **targeted** matrix, not the old K1–K4 full campaign. Explore depth/payload/topology with counterfactuals, then use same-host A/A + ≥15 interleaved AB/BA valid pairs for promoted claims. Keep instrumented path attribution separate from full uninstrumented timing. Include full CPU/op, wall/op, throughput, p50/p95/p99 (adequate samples), actual outstanding depth, enters/SQE, progress/notify/park, kernel/system vs user cost, negative controls and regressions. Document storage (warm tmpfs vs filesystem/device); no cross-machine absolute speed ratios.

**W5 — Product candidate and review:** choose the **simplest** policy that meets the v2 obligations, has a measured net win in admitted W-02 shapes, and avoids material p95/p99 regressions in fallback shapes. A real `BOUNDED_BATCH` must demonstrate a finite max submission delay; otherwise exclude it. Integrate into a **separate scoped production PR** only after the code's behavioral and performance claims are supported. Run independent semantic/concurrency and performance/statistics reviews. No automatic merge.

**W6 — Scope and conclusion:** publish one compact synthesis comparing baseline / K4-b / new variants / fallback. If every added policy is too costly or cannot be proved safe, record `KEEP_IMMEDIATE` or `KEEP_K4_RESEARCH_ONLY`; a negative outcome is valid.

The workflow may proceed across W0–W6 without mandatory Owner checkpoints; unsafe/unverified arms must not be promoted as product candidates. Use fresh evidence for all claimed new improvements.

## 6. Experimental decision criteria

- **Primary objective:** reduce **full-lifecycle** `io_uring_enter` cost and E5 CPU/op/wall/op on supported W-02 4 KiB D=4/16/64, compared with both frozen current-master and K4-b; do not demand improvement over K4-b if a simpler policy wins the complexity/safety trade-off.
- **Regression control:** D=1/2/3, W-03 d4/16/64, 64 KiB d16, cancellation/control, mixed-driver, saturation and shutdown must be measured or explicitly designated safety-only. Fallback cells must remain in-band under *matched* A/A. In particular, **no unexplained 64 KiB p95/p99 regression accepted**.
- **Accounting:** count prepared/accepted/kernel-consumed SQEs, submit attempts vs enter syscalls, unsubmitted ledger residue, actual max/mean submit delay, progress/notification and kernel error recovery; explicitly quantify any new scheduler/timer/atomic/lock costs.
- **Model choice:** state whether the policy is construction-time/internal specialization or runtime selection, what information actually justifies its decision, and who is responsible for triggering the next flush. No API changes, new semantic authority or unlimited pending state.
- **Independent review:** one reviewer tries to break progress, cancellation, borrow and ledger invariants; another challenges measurement causality, workload parity, noise and tails. Disclose reviewer-channel limitations honestly.
- **Transferability:** E5 evidence is sufficient for **E5-scoped research conclusions**, not a library-wide claim. Second native host and #578 real application evidence are needed before claiming broad default-policy portability; do not add them as a ceremonial gate to completing the E5 study.

## 7. Deliverables and closeout

- `docs/research/uring-submit-policy/decision.md`: exact evidence, policy selection/fallback, accepted/rejected hypotheses, latency trade-offs, limits, decision.
- `docs/research/uring-submit-policy/causal-dag.md` and `policy-table.md`: source authority and promptness/flush witness.
- `docs/research/uring-submit-policy/counterfactuals.md`, `correctness-review.md`, `measurement-review.md`: named mutations, A/A, paired A/B and negatives.
- Minimal reproducible `bench/research/uring-submit-policy/` scripts/patches with frozen source/build IDs; external raw data under a **new** workspace, not #593's evidence tree.
- Research PR and, only if earned, a **separate minimal production PR** (OPEN; no Owner merge authorization assumed). Update this issue with links and next decision.

**Closeout vocabulary:** `POLICY_PERF_SUPPORTED`, `PRODUCT_CANDIDATE_READY`, `KEEP_IMMEDIATE`, `KEEP_K4_RESEARCH_ONLY`, `BLOCKED_CORRECTNESS`, `INCONCLUSIVE`. `PRODUCT_CANDIDATE_READY` is not an instruction to merge.

```text
BEGIN_SLUICE_URING_SUBMISSION_POLICY
BASE_SHA:
E5_HOST_IDENTITY:
POLICIES_COMPARED:
P1_SHAPE_GATE:
P2_FIRST_FLUSH:
P3_NOTIFICATION_COALESCING:
P4_GATE_COST:
DEFERRED_LEDGER_ORACLE:
WAKE_RETRY_ORACLE:
CANCEL_OVERFLOW_SHUTDOWN:
D1_D2_D3:
PAYLOAD_TAILS:
W02_W03_MIXED_DRIVER:
ENTERS_PER_OP:
CPU_WALL_LATENCY_AB:
REGRESSION_VERDICT:
PRODUCT_API_CHANGE: NONE
RESEARCH_PR:
PRODUCT_PR:
MERGED: NO
DECISION:
END_SLUICE_URING_SUBMISSION_POLICY
```

**North Star:** *An accepted operation must always have a justified route to a timely kernel submission or lawful settlement. Remove redundant kernel entrances only where the full execution and tail-latency trade-off is proven.*
