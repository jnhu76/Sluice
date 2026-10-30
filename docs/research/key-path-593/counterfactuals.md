# Counterfactuals — paired hypotheses, arms, falsifiers

Every lane had at least two competing root-cause explanations and one arm that
distinguishes them. Arms are test-only and live in
`bench/research/key-path-593/patches/`. No result here is a product claim.

## 0. Arm taxonomy actually built

| Arm class | K1 | K2 | K3 | K4 |
|---|---|---|---|---|
| BASELINE | frozen baseline binary (SHA-verified) | same | same | same |
| OBSERVE | `SLUICE_OBSERVE` counters + per-pass trace; used for every lane | same | same | same |
| SHADOW | not built (the K2-A two-verdict shadow was designed, not implemented: <1 ns/op) | — | — | — |
| COUNTERFACTUAL | mechanism-frequency sanity before A/B | micro enter probe (TSC/core; corrected in [k2](k2-submit-dispatch.md) §9.4) | cadence guard (poll/waits per op) | `strace`/`submit_flushes_` double-口径 enters |
| ELISION ARM | K1-a + K1-b merged | E18 ungated deferral | three same-lock fusions | depth-gated deferral, then topology+depth-gated |
| COMBINATION | — | — | — | K4-b + K1(a+b) in one worktree |

The `BASE GREEN → MUTANT RED → RESTORED GREEN` gate holds dedicated mutant
receipts for the K2 and K4 elision arms (5 K2-a + 6 K4-b formal receipts) and
one guarded-mutant receipt for K3, whose unguarded draft was killed by two
oracles with no mutant build and no restored-green log. K1 has no dedicated
mutant binary: its only kill is an over-elided sink build caught by two
oracles, after which the corrected build passes the suite with no formal
restored-green log, and lost publication / `publish_batch_` double count /
K1-b false negative are unmutated. K1's A/B was accepted on the
frozen-baseline identity check, the per-run G0 and 64 MiB byte oracles, the
session A/A controls and the W-02 `published == reaped` trace invariant —
correctness evidence, not a mutation receipt — and "no K1 mutant" is recorded
among the reasons the arm is not a candidate
([correctness-review.md](correctness-review.md) §3,
[measurement-review.md](measurement-review.md) §10).

## 1. K1 — completion/publication/observer

| | H1 lock instructions | H2 coherence traffic | H3 zero-sum | H4 observer dead weight |
|---|---|---|---|---|
| Claim | uncontended lock + resolve constants dominate the segment | cache-line transfer dominates; visible only cross-thread | the segment is a minority; any elision stays in noise | no-observer claim/retire is pure removable work |
| Falsifier | arm change < session A/A | W-03 gains, W-02 null | both forms in band | a K1-b-only arm would be measurable |
| Outcome | partially true per acquisition (~20 cycles uncontended) but the total is 370 cycles/op | **supported**: 22.8 ns/acquisition in W-03 (3.8× uncontended) | **supported** | structurally true, but folded into the merged arm; no single-factor arm |

Arm: the merged K1-a+K1-b patch. Distinguishing measurement: W-03 d16 (E-A/H2
predicts an effect) vs W-02 d16 (H3 predicts null). Result: W-03 −2.29 %
(15/15), W-02 −0.43 % (11/15). **H2's mechanism holds at the per-acquisition
level, but H3's magnitude wins**: the measured 22.8 ns/acquisition is a
restatement of the same effect, not an independent cap; the independent bound is
the all-mutex share (3.4 % of cycles ≈ 370 cycles/op), which is below the 4.79 %
band.

Falsifier for the attribution itself: measure futex/context-switch counts in
both arms. They are unchanged (0.27/op both; 313 vs 302 switches), so the
removed acquisitions were uncontended — no blocking was eliminated and no cost
was pushed to another thread.

Answers to the brief's eight questions:

1. Work removed: 2.000 core acquisitions (claim/retire), 0.889 pop locks,
   0.209 pass-boundary folds.
2. Per op: 3.098 acquisitions (W-03), 3.124 (W-02).
3. Work added: one preallocated publication batch queue, one preallocated owed
   vector (`request_capacity` each), an `OwedDelivery` record, a persistent
   `DeliveryRecord::observer_armed` bit, and an overload of
   `complete_publication`; plus an O(`request_capacity`) owed scan **under the
   same lock** when `owed_count_ != 0`. The last item was never exercised in the
   measured loads.
4. Cost moved: none measurable in the measured loads (futex/context switches
   unchanged); the owed-scan lock hold is a potential unmeasured progress
   blocker.
5. Fallback paths: armed observers take the unchanged path; `routing_sink_`
   forces the unchanged path; owed sweep uses the same gate.
6. Full execution improved: yes, −2.29 % W-03; null W-02 (with p99 +3.07 %,
   p95 +0.82 %).
7. Regression conditions: W-02 p99 point estimate is positive; 64 KiB,
   saturate and cancel not measured; the owed-scan branch not measured.
8. Attribution falsifier: the W-03 session A/A band (median 0.18 %, max 1.88 %)
   is smaller than the effect and futex is unchanged, so the effect is neither
   drift nor a contention artefact. The verdict is nevertheless
   threshold-dependent: 2.29 % is below the campaign 4.79 % band and above the
   session band.

## 2. K2 — submit/admission/dispatch

| | A fixed enter cost amortizable | B per-SQE cost + same-CPU backfill |
|---|---|---|
| Claim | one enter/op is real, fixed, and removable by batching | the cost is per-SQE; the copy worker fills saved cycles; wall is unchanged |
| Prediction | enters fall, CPU **and** wall fall | enters fall, CPU/wall flat or worse |
| Outcome | **wins** on same-thread: −32.20 % CPU and −32.24 % wall at d16 | refuted: worker absolute cycles −33.0 % |

Arm: E18 (`k2-e18-arm.patch`) removes the inline `submit_transport_locked` from
`dispatch_after_accept`. Third explanation (mutex micro-work) was excluded in
advance by the 3.4 % symbol share.

Pre-arm calibration: the micro probe is TSC/core-cycle and has a recorded
defect — the `batched run1` line has no cycles event, so the 1,845 value is not
a cycle count. The two valid core-cycle rounds give 1,723 and 1,862
(median 1,792.5); the run-3 TSC differential is 1,814; the separate N=64 TSC
run gives f = 0.910, while the core-cycle fixed fraction is ≈0.37. The
calibration is order-of-magnitude only. See
[k2-submit-dispatch.md](k2-submit-dispatch.md) §9.4.

Required gate tests and what they falsified:

| Falsifier | Prediction if the arm is invalid | Observed |
|---|---|---|
| D=1 | no overlap, so "+1 pass boundary" cannot be a constant | **+14.12 % CPU, p50 ×14.1** — the deferral is the whole latency at D=1 |
| W-03 tail | if the driver absorbs the flush cheaply, tails stay flat | p50 +10.55 %, p99 +15.70 %, notifications +51.6 % — **cost migration** |
| 64 KiB | fixed enter is a small share of a 12 µs op | CPU −3.05 % but p99 +6.77 % — payload-dependent |
| A/A | no effect | −0.86 % CPU (inside band) |

The eight questions:

1. Work removed: one `io_uring_enter` per op and its fixed cost.
2. Per op: 1.00007 → 0.06257 (d16); 0.01572 (d64); 0.25007 (d4). D=2/D=3
   unmeasured; the ungated 1/D pattern predicts 0.5 at D=2.
3. Added: SQEs accumulate in the ring between passes; residue signal fires on
   every deferred submit (existing mechanism, condition now always true).
4. Cost moved: in W-03, into the driver's pass + eventfd (measured); at D=1,
   into the completion latency.
5. Fallback: SQ-full recovery flush, pass-start flush, retry advertisement,
   poison recovery. There is no cancel-specific flush: cancel SQEs are flushed
   by the same pass-start/SQ-full paths.
6. Full execution improved: yes on same-thread D≥4; **no** at D=1; mixed on
   W-03/64 KiB.
7. Regression conditions: D=1, split owner (tail), 64 KiB (tail).
8. Attribution falsifier: worker absolute cycles fall (0/1 record), so the
   saving is not merely moved to the kernel thread; but W-03 counters show
   where it *is* moved when topology changes.

## 3. K3 — progress/wakeup/retry

| | E-A amortized/dormant | E-B low-depth pass round-trip |
|---|---|---|
| Claim | mechanisms are asleep or 1/D-amortized; effect <2–3 % | D=1 pays a serial pass round on the completion path; fusion crosses the threshold |
| Prediction at D=1 | in band | above band |
| Outcome | **wins** | refuted |

Arm: three same-lock fusions (`k3-fusion-arm.patch`). Required guard: an
unguarded first draft was killed by two payload oracles
(`a_zero_op_after_sweep_is_immediate_residue`,
`zero_op_from_another_thread_during_pass_is_served`), so the safe arm needs
`dispatch_state_version_` plus eight release bumps and an acquire re-check with
locked fallback. Mutation `..._TAIL_STALE_IGNORES_VERSION` is killed by
`uring_owed_residue_test` and `uring_external_loop_test`.

Result: −1.00 % CPU (9/15, range −3.78…+1.89) against an A/A median of +0.40 %.
Cadence guard proves the difference is not pass-cadence. Per-fusion lock
round-trip ≈55 cycles.

The eight questions:

1. Work removed: three acquisitions per pass (backend summary, core
   health+outstanding, progress health+exhausted).
2. Per op: 4.2 at D=1 (1.405 passes/op); ≈0.3 at d16.
3. Added: a new version atomic, eight bump points, and a fallback recompute path
   — the mechanism-justification cost is larger than the measured benefit.
4. Cost moved: none measured.
5. Fallback: version mismatch → locked recompute; owed sweep path always
   recomputes.
6. Full execution improved: no (inside band).
7. Regression conditions: none observed, but the arm's safety depends on the
   new guard.
8. Attribution falsifier: cadence guard (poll/wait per op unchanged) and the
   A/A comparison.

## 4. K4 — batching and the kernel boundary

| | E1 real recoverable fixed cost | E2 unrecoverable / migrated |
|---|---|---|
| Claim | enter path ≈15 % of cycles and ≈1/passes/op amortization headroom | wall is set by the copy chain; deferral moves cost and shrinks overlap |
| Prediction | enters fall AND wall/CPU fall | enters fall, CPU/wall flat or tails worse |
| Outcome | **wins on same-thread** | **wins on split-owner** |

The lane produced three arms in sequence, each falsifying the previous gate
design:

| Arm | Gate | Result | Why it was replaced |
|---|---|---|---|
| Uniform deferral (= K2-a) | none | W-02 huge gain; D=1 +14 %, W-03 tail +15.7 % | no gate |
| Depth gate | `outstanding > 1` | d1 in band; W-02 gains; **W-03 d4 CPU +34.46 %, p50 +34.97 %**, d16 p99 +48.96 % | gate does not consider topology |
| Topology + depth gate (K4-b) | submitter drives progress **and** `outstanding > 1` | W-02 d16 −28.07 %; d1/W-03 inline; W-03 d4 +2.54 % residual (sub-threshold) | accepted with disclosed residual |

enters/op double relative to K2-a (≈2/D instead of ≈1/D) because the first
submission of each window stays inline and each pass adds one flush. Measured
gate price: d4 −26.31 % → −13.06 %, d16 −32.20 % → −28.07 %, d64 −37.66 % →
−35.53 %. The ≈2/D pattern gives 1.0 enters/op at D=2, so **the measured
supported range is D=4/16/64, not D≥2**; D=2/D=3 were not run.

The eight questions:

1. Work removed: one independent enter per op (same as K2-a, minus the first
   submission of each window).
2. Per op: 1.00007 → 0.12577 (d16), 0.03447 (d64), 0.50017 (d4), 0.13267 (64k).
3. Added: two derived topology fields (`thread::id` ×2 + bool), one
   `outstanding_summary()` core-lock read per submission whose topology
   predicate is true (including the inline D=1 branch), and one
   `signal_ready_progress()` per deferred submit (progress mutex, epoch
   increment, possible eventfd write).
4. Cost moved: in the primary shape nothing measurable (vctx/ictx fall,
   poll/wait fall, no extra parks; LLC misses +0.007/op). In W-03 the gate
   prevents the migration entirely.
5. Fallback: gate-false submissions use the baseline inline path; SQ-full,
   pass-start flush, retry and poison paths unchanged. **There is no
   cancel-specific flush**: a cancel SQE is flushed by the pass-start flush or
   the SQ-full recovery branch, so a deferred form adds up to one pass of
   cancel latency. Cancel/control load is unmeasured.
6. Full execution improved: yes, same-thread **D≥4**; no, D=1/W-03 (inline) and
   64 KiB tail.
7. Regression conditions: payload > 4 KiB (p99 +7.46 %), mixed-driver usage
   (unmeasured). W-03 d4 +2.5 % is below the campaign band and inside the only
   measured d4 A/A; unattributed but not material.
8. Attribution falsifier: K2-a is the same code line without the gate; the
   difference between the two arms is exactly the value and price of the gate.
   The 64 KiB and W-03 results falsify "the gain is intrinsic to the
   mechanism". The kernel-side attribution is inherited from the K2 0/1
   `perf record`, not measured on K4-b.

## 5. Combination — K4-b + K1(a+b)

Question: does K1's W-03 effect survive on a K4-b base, and does K4-b's extra
core-lock read eat K1's core-lock elision?

| Hypothesis | Prediction | Observed |
|---|---|---|
| Additive | combo ≈ K4-b + K1 | **rejected**: W-02 increment over K4-b is +0.31 % (no gain) |
| Interference (gate eats K1) | combo W-03 worse than K4-b | rejected: W-03 increment −2.97 % |
| Topology complementarity | each effect stays in its own form | **supported**: W-02 belongs to K4-b, W-03 to K1 |

Combo vs baseline: W-02 d16 −27.87 % CPU / −28.04 % wall; d64 −36.13 % /
−36.17 %; W-03 d16 −2.57 % / −2.64 %. Combo vs K4-b: +0.31 % W-02 d16,
−2.97 % W-03 d16. No percentage is summed. The combo has **210/210 valid
runs**, of which 60 are the increment comparison (105 valid pairs across 7
case sets).

Classification: `MECHANISM_GAIN` (campaign vocabulary) / `MECHANISM_ONLY_GAIN`
(§13). The W-03 effect is real, reproducible (15/15 CPU/wall) and outside the
session A/A band, but below the 4.79 % campaign materiality band — and that
band's pre-run designation is not archived (see
[measurement-review.md](measurement-review.md) §4). The combo verdict is
therefore threshold-dependent in the same way as K1's.

## 6. Falsifier registry (what would overturn each conclusion)

| Conclusion | Falsifier |
|---|---|
| K1 ceiling is the acquisition count | a form where per-acquisition cost is much higher without changing the count (multi-socket/NUMA) and the effect scales proportionally |
| K2/K4 gain is kernel-side amortization | a run where worker absolute cycles do not fall while enters fall, or where wall stays flat although the worker falls |
| K4 gain requires same-thread owner | a topology-gated arm with the flush owned by a driver thread but without extra passes/eventfd showing the same gain |
| K3 has no opportunity in this domain | a real-device arm where parks occur and the park fixed cost becomes a measurable per-op component |
| K4-a is below threshold | a measured arm showing the per-CQE lock pair alone exceeds 4.79 % (contradicts the 3.4 % all-mutex symbol share). **The `cqe_seen` khead store is not covered by this falsifier**; no bound exists for it |
| K2-a and K4-b are the same mechanism | any semantic or measurement difference other than the gate predicate |
