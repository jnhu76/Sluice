# Decision

Base: `bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57`. Host: E5 native. This is a
research decision; it authorizes no product change.

This revision incorporates the independent review. Every finding is
dispositioned in [measurement-review.md](measurement-review.md) §10. The three
substantive changes are: K4-b's supported depth range is **D=4/16/64** (D=2/D=3
unmeasured), K2's verdict is **`ELISION_CANDIDATE_FOUND`** (one mechanism,
supported verdict counted once under K4), and the 4.79 % band's
"pre-registered" label is **retracted** (provenance disclosed).

## 1. Decision summary

| Lane | Verdict (brief §13) | Paired class | One-line reason |
|---|---|---|---|
| K1 | `NO_CHANGE_JUSTIFIED` under the campaign 4.79 % band; `INCONCLUSIVE` if the band is rejected | — | 3.098 acquisitions/op removed, −2.29 % W-03 CPU (15/15); below the band, above the session A/A band |
| K2 | `ELISION_CANDIDATE_FOUND` | `END_TO_END_GAIN` | −32.20 %/−37.66 %/−26.31 % same-thread CPU, but the arm is ungated: D=1 +14.12 %, W-03 p99 +15.70 %, 64 KiB p99 +6.77 % |
| K3 | `NO_CHANGE_JUSTIFIED` | — | Dormant or 1/D-amortized; best arm −1.00 % (9/15, median in band, range wider) |
| K4 | `ELISION_PERF_SUPPORTED` (form-gated, measured **D=4/16/64**) | `END_TO_END_GAIN` | −28.07 %/−35.53 %/−13.06 % CPU with topology+depth gate; 64 KiB p99 +7.46 % and D=2/D=3 remain open |
| Combo | `MECHANISM_ONLY_GAIN` | `MECHANISM_GAIN` | W-02 gain belongs to K4-b (+0.31 % increment, no addition); W-03 −2.97 % belongs to K1; not a candidate |

`ELISION_PERF_SUPPORTED` does **not** mean "approved for merge" (brief §13).
No product issue is proposed by this PR. K2 and K4 are the **same mechanism**;
the campaign records one supported elision, under K4.

## 2. Lane decisions and the evidence that forces them

### K1 — no change under the campaign band, threshold-dependent

The lane found genuine redundancy and removed it legally: the completion segment
drops from 3.209 to 0.111 mutex acquisitions/op, W-02 `published == reaped` on
70,061 traced passes, the armed-observer path and sink delivery preserved, and
the over-elided build was caught by two oracles. The measured effect is −2.29 %
CPU and −2.31 % wall in W-03 d16 (15/15, reproducible), and null in W-02 (−0.43 %
CPU, p95 +0.82 %, p99 +3.07 %). The independent upper bound is the all-mutex
share (3.4 % of cycles ≈ 370 cycles/op), not a circular per-acquisition product.

The 4.79 % band's provenance is a measured baseline A/A drift whose designation
as a decision threshold before the arm is **not archived** (see
[measurement-review.md](measurement-review.md) §4). Under it, the answer is
`NO_CHANGE_JUSTIFIED`; under the W-03-d16 session band (max 1.88 %) the effect
is material. The verdict is stated as band-dependent rather than unqualified.

The arm also has an unexercised O(`request_capacity`) owed-scan lock hold and a
publish/owed reorder, and no K1 mutant exists. Those are additional reasons it
is not a candidate.

### K2 — mechanism supported, production form not found

The lane proved explanation A (fixed enter cost is real and amortizable) and
refuted explanation B (per-SQE cost + backfill) within the limits of a 0/1
`perf record`: enters fall, wall falls, and the io-wq worker's absolute cycles
fall 33 %. It also refuted its own optimistic predictions: D=1 is a pure
regression (+14.12 %, p50 ×14.1), W-03 tails regress (+15.70 % p99,
notifications +51.6 %), and 64 KiB p99 regresses (+6.77 %). The arm is ungated;
the only implemented gate is K4-b's. Its verdict is therefore
`ELISION_CANDIDATE_FOUND`, not `ELISION_PERF_SUPPORTED` per arm; the supported
mechanism is recorded once, under K4.

### K3 — no change

The progress machinery is dormant in the measured domain (W-02 zero parks in
540,000 ops, zero user signals, W-03 zero empty wakes) and amortized by 1/D.
The largest-signal fusion measured −1.00 % (9/15); its median is inside the A/A
median/range but its own range extends below it. A safe implementation would
require a new `dispatch_state_version_` mechanism. Mechanism argument and
measurement close together.

### K4 — mechanism supported only under a form gate

K4-b is the campaign's only deployable-in-principle mechanism: −28.07 % CPU and
−28.08 % wall at d16, −35.53 % at d64, −13.06 % at d4, p50/p95/p99 improving,
with D=1 and all W-03 forms kept inline. The kernel-side mechanism explanation
is **inherited** from the K2 ungated 0/1 `perf record`; K4-b's own evidence is
the process-CPU/`cycles:u` divergence plus the context-switch collapse.

The supported claim is **D=4/16/64**. The observed ≈2/D enters pattern gives 1.0
at D=2, i.e. no enter gain, while the gate already defers there; D=2/D=3 were
not measured. The 64 KiB p99 +7.46 % is material and needs a payload gate. The
W-03 d4 +2.54 % is **below** the campaign band and inside the only measured
d4 A/A [−6.14,+6.51]: unattributed, not a material regression. The d1 −2.50 % is
not explainable by the gate (the discrimination round's d1 is +1.88 %); it is
treated as clock/layout noise, not a benefit.

### Combo — measured, non-additive, not a candidate

The composition answers the interaction question and nothing more. The W-02
effect is K4-b's; the W-03 effect is K1's; the increment over K4-b is +0.31 %
on W-02 (no gain) and −2.97 % on W-03. The two percentages are not added, and
the combination is classified `MECHANISM_ONLY_GAIN`. The combo has 210/210
valid runs, of which 60 are the increment comparison.

## 3. Global findings

1. **The critical path is the kernel boundary.** One enter, the io-wq dispatch,
   and the copy on a worker sharing the application's logical CPU. The worker
   share is 32.5–40.4 % of all cycles across two 0/1 records. User-space
   coordination is 23–25 % of cycles, and all mutexes together are 3.4 %.
2. **The only material avoidable work is the per-op `io_uring_enter`.** Every
   other provable redundancy is real and sub-threshold.
3. **K2-a and K4-b are one mechanism with two gates.** They cannot be combined
   or added; the supported verdict is counted once.
4. **The cost-migration channel is the progress-owner topology.** Same thread:
   the flush is absorbed for free. Split owner: it becomes an extra pass plus an
   eventfd round trip.
5. **Form dependence is the systemic gap.** The same mechanism is −28 %…−36 %
   in one form, neutral in another, and a tail regression in a third. A product
   form needs a three-condition gate (same-thread, depth, payload); K4-b
   implements two, and its depth gate is only validated at D≥4.

## 4. Productization preconditions (all currently unsatisfied)

For the deferred-flush mechanism:

1. **Payload gate** or a measured explanation of the 64 KiB p99 +7.46 %.
2. **Depth validation at D=2/D=3** (or tighten the gate, since 2/D predicts no
   enter gain at D=2).
3. **Variant selection**: K4-b (topology gate, tails neutral) vs K2-a (better
   W-03 d16 CPU, worse tails) is not settled.
4. **Combination decision**: K1 adds ~2.6 % on W-03 and nothing on W-02; decide
   whether the detail-header change is worth it.
5. **Cancel/control load, mixed-driver load, and deferred partial
   submit/overflow/shutdown**, none measured or receipted.
6. **#585 flush-accounting semantics** must be redefined for a deferred form
   under Owner review.
7. **Mechanism justification gate** for the derived topology summary, the
   per-submit `outstanding_summary()` read, and the per-deferred-submit
   `signal_ready_progress()`.
8. **Re-validation** on a real filesystem/device and a second host.

K1 revival additionally requires a new baseline (after a K4 product change), the
brief's Step-4 protocol, and a pre-run threshold registration; the current
verdict may not be carried over. K1's owed-scan lock hold must be redesigned
before it can be a candidate at all.

## 5. What this PR does not authorize

- No production source, header, test, build or specification change.
- No public API change.
- No merge, no auto-merge, no product issue.
- No extrapolation of tmpfs results to device I/O, NVMe or #578 applications.
- No claim of independent approval: the revision response is by the author, and
  the independent review was an input, not a sign-off (see
  [correctness-review.md](correctness-review.md) §6).

## 6. Next round priorities

1. E5 depth: payload-gated variant; D=2/D=3; W-03 d4 attribution with a
   same-session A/A; cancel/control storm; mixed-driver load; deferred
   partial-submit/overflow/shutdown; saturate latency instrumentation; #585
   product semantics.
2. Second native host (multi-socket/NUMA).
3. #578 real application: submit cadence, depth distribution, payload mix and
   cancel rate may fall outside the measured gates.

## 7. Final output block

```text
BEGIN_SLUICE_593_E5_KEY_PATH_RESEARCH

BASE_SHA: bdfd0d8d4595fd82d1cd4b099af8ed37851e6d57
HOST: E5_NATIVE
ENVIRONMENT_VERIFIED: YES — Xeon E5-2666 v3 (1 socket/10 cores/SMT2/1 NUMA), kernel 7.2.11-200.fc44.x86_64, gcc 16.2.1, glibc 2.43, liburing 2.13; taskset -c 2 (SMT sibling cpu12 not isolated); schedutil 1200-3500 MHz, governor/turbo/drop_caches/sysctl unchanged; warm tmpfs only (no device I/O); baseline binary SHA-256 verified before every arm (w02 e07a1bd2…bf484, w03 80992f4b…a3ab); user-space PMU available, kernel PMU/tracepoints only via logged transient sudo.

K1_COMPLETION_PUBLICATION:
  CRITICAL_PATH: completion→publication→observer is hot on every op but not on the measured critical path; user-space is 2,572 user-cycles/op vs 8,293 kernel-cycles/op (d16) and all mutexes together are 3.4% of cycles (~370 cycles/op). W-03 cross-thread acquisitions cost 22.8 ns each (3.8x uncontended) but that number restates the measured effect; the independent bound is the all-mutex share.
  AVOIDABLE_MECHANISMS: per-event publication pop lock (0.889 acquisitions/op); no-observer claim_observer_delivery+retire_observer_delivery (2.000 core acquisitions/op); pass-boundary C/E fold (0.209/op). Total 3.098/op in W-03, 3.124/op in W-02. publication_pending_ itself is NOT removable. The arm's O(request_capacity) owed scan under one lock and its owed-before-publish reorder were never exercised (all measured loads owed_count_==0).
  COUNTERFACTUAL: merged test-only arm K1-a (single-lock entry-snapshot publication drain + owed collection in the same scope) + K1-b (complete_publication returns the already-computed queue_armed_observer; deliver transitions run only when armed; sink on_ready NOT elided). New state: publish_batch_, owed_batch_, OwedDelivery, DeliveryRecord::observer_armed; detail-header overload. 15 interleaved AB/BA pairs; W-03 d16 primary, W-02 d16 control.
  PERF_RESULT: W-03 d16 CPU -2.29% (15/15, range -4.86..-0.99), wall -2.31%, p50/p95/p99 -1.95/-1.54/-2.23%. W-02 d16 CPU -0.43% (null), p95 +0.82%, p99 +3.07%. A/A W-03 +0.18% [−1.17,+1.88], W-02 -0.64%. Mechanism counts 3.209->0.111 acquisitions/op; W-02 published==reaped on 70,061 traced passes, zero violations (W-03 aggregate parity only); futex 0.27/op and context switches unchanged. No K1 mutant binary; lost-publication and K1-b false-negative are unmutated.
  VERDICT: NO_CHANGE_JUSTIFIED (campaign 4.79% band; INCONCLUSIVE if the band is rejected — the effect is 15/15 and above the W-03-d16 session band)

K2_SUBMIT_DISPATCH:
  CRITICAL_PATH: one io_uring_enter per logical op (1.00007 enters/op, exactly 1 SQE/enter at every D) caused by the inline submit_transport_locked in dispatch_after_accept; enter path ~15.8% of total cycles. Removing the inline flush changes end-to-end CPU/wall by -32% (d16) to -38% (d64).
  AVOIDABLE_MECHANISMS: the per-op independent kernel submission (syscall trap plus kernel-side per-item cost: io-wq scheduling, spinlock, eventfd flush; worker absolute cycles -33.0% on a 0/1 K2 record). Not avoidable: reserve/accept/claim_execution, delivery-init before accept, ledger append/pop/reconcile, SQ-full recovery, poison recovery, and the pass-start flush that submits cancel SQEs. closed/access double check is real redundancy but <1 ns/op.
  COUNTERFACTUAL: E18 arm deletes the inline submit_transport_locked; SQEs flush at the existing pass-start submit_transport_locked; residue signal = existing unsubmitted_after_dispatch. Micro probe: batched run1 has no cycles event; valid core-cycle differentials are 1,723/1,862 (median 1,792.5), run-3 TSC differential 1,814; N=64 TSC run gives f=0.910 but the core-cycle fraction is ~0.37; the earlier 1,815/f~0.91 pairing is retracted. Five mutation receipts (missed wake, false eligibility/defer-past-poison, lost residue, batch-invisible, masked SQ cardinality) BASE GREEN→MUTANT RED→RESTORED GREEN; 13 suite targets GREEN at arm start, 7 re-run at end; 390/390 runs valid.
  PERF_RESULT: paired class END_TO_END_GAIN for the same-thread forms. W-02 d16 CPU -32.20% [-32.63,-31.62] (bootstrap CI), d64 -37.66%, d4 -26.31%, saturate -47.48%, p50/p95/p99 -35.91/-46.66/-45.14% (d16); but D=1 +14.12% CPU with p50 x14.1, W-03 d16 p99 +15.70% with notifications +51.6%, W-03 d4 p50 +26.42%, 64 KiB p99 +6.77%. Ungated arm is not deployable. D=2/D=3 unmeasured.
  VERDICT: ELISION_CANDIDATE_FOUND (paired END_TO_END_GAIN; same mechanism as K4-b — the supported verdict is counted once, under K4)

K3_PROGRESS_WAKEUP:
  CRITICAL_PATH: not on the measured critical path. W-02 makes 0 parks in 540,000 ops (24 eventfd reads, 0 poll(2)); W-03 empty_wakes=0.0000, 0.084 notifications/op (~12 ops/wake); passes/op 1.34→0.030 (D=1→64); empty pass = 8 non-contended locks + 0 syscalls.
  AVOIDABLE_MECHANISMS: three repeated same-lock acquisitions per pass (backend summary, core health+outstanding, progress health+exhausted); 4.2 acquisitions/op at D=1. Not removable: immediate/accepted/retry residue facts (PROG-01), eventfd drain read (PROG-02), 10 ms bounded retry, owed_count_ and pending_control_count_ gates. The census "productive pass" counter is completed>0 (a return-value criterion), not an obligation-based definition.
  COUNTERFACTUAL: K3-a fuses the three same-lock reads; the unguarded version was killed by a_zero_op_after_sweep_is_immediate_residue and zero_op_from_another_thread_during_pass_is_served (no mutant build / restored-green log), so the safe version adds dispatch_state_version_ with 8 release bumps plus an acquire re-check and locked fallback; mutation ..._TAIL_STALE_IGNORES_VERSION killed by two oracles. Cadence guard shows poll/waits-per-op unchanged.
  PERF_RESULT: W-02 d1 (largest signal) CPU -1.00% [-3.78,+1.89] (9/15), wall -0.98%, p50/p95/p99 -1.97/-1.50/-1.43%; d4 +0.54%; same-session A/A d1 +0.40% [-2.43,+2.49]. The median is inside the A/A median/range; the effect's own range extends below the A/A lower bound. Per-fusion lock round-trip ≈55 cycles. 11 suite targets GREEN with zero test modifications.
  VERDICT: NO_CHANGE_JUSTIFIED

K4_SQE_CQE_BATCH:
  CRITICAL_PATH: the kernel boundary is on the critical path. CQE side is already batched (11.76 CQEs/peek at d16, 28.88 at d64); SQE side never batches (1.00 SQE/enter, sq_ready=1.00 at every D). K4-b removes 87.4%/96.6%/50.0% of enters (d16/d64/d4) and 28.07%/35.53%/13.06% of CPU/wall, measured at D=4/16/64.
  AVOIDABLE_MECHANISMS: one independent enter per op (material). Sub-threshold and not pursued: per-CQE dispatch_mtx_ lock pair (K4-a, no arm; all-mutex upper bound 3.4% of cycles). The cqe_seen khead store is NOT a mutex and is not covered by that bound. Obligations kept: TransportLedger append/pop/reconcile, partial-submit accounting, submit_flushes_ test counter (recorded, not removed).
  COUNTERFACTUAL: three arms in sequence — uniform deferral (no gate), depth gate (outstanding>1), then topology+depth gate (submitter_drives_progress_locked_() && outstanding_summary()>1). Each arm's failure falsified the previous gate: uniform failed D=1/W-03 tails, depth gate failed W-03 d4 (+34.46% CPU). The 2/D enter pattern gives 1.0 enters/op at D=2, so the gate's supported range is D>=4; D=2/D=3 were not measured. There is no cancel-specific flush: cancel SQEs are flushed by the pass-start flush or the SQ-full branch. Six mutation receipts RED→GREEN; 14 targets BASE GREEN + 7 key oracles; 390/390 runs valid; patch round-trip verified byte-identical.
  PERF_RESULT: paired class END_TO_END_GAIN for the gated W-02 4 KiB D=4/16/64 claim. d16 CPU -28.07% [-28.69,-27.34] / wall -28.08% / p50/p95/p99 -34.08/-39.27/-39.97%; d64 -35.53%; d4 -13.06%; saturate -43.06%; D=1 and all W-03 inline (enters 1.00007), W-03 d16 +0.30% (CI crosses 0). Open: 64 KiB p99 +7.46% (0/15 improved, material); W-03 d4 +2.54%/+2.57% (below the 4.79% band, inside the only measured W-03-d4 A/A [-6.14,+6.51], unattributed); D=1 -2.50% unexplained (discrimination d1 +1.88%). Kernel-side attribution is inherited from the K2 ungated 0/1 record, not measured on K4-b. K4-a withdrawn: NO_CHANGE_JUSTIFIED (partial bound only).
  VERDICT: ELISION_PERF_SUPPORTED (form-gated, measured D=4/16/64; productization preconditions open)

CROSS_PATH_INTERACTIONS: four channels are one coupling (who flushes, who pays the pass). K2-a and K4-b are the same statement (src/async/uring_transport.cpp:87) under two gates; W-03 enters/op: K2-a 0.06257/0.25015/0.01572 (d16/d4/d64) vs K4-b 1.00007 (topology gate). K4 deferral changes K1's per-pass denominator and adds one core-lock read while K1 removes two. K1's publication strategy is wake-neutral (futex/context switches unchanged); K4's deferral is the wake-cost migration source, sign set by topology. Measured composition (K4-b+K1 a+b, one merge worktree, 210/210 valid runs of which 60 are the increment comparison): vs baseline W-02 d16 -27.87%, d64 -36.13%, W-03 d16 -2.57%; vs K4-b increment W-02 +0.31% (no gain) and W-03 -2.97%. Paired class MECHANISM_GAIN = §13 MECHANISM_ONLY_GAIN; not additive, not a candidate.
KEY_PATH_ATLAS: CRITICAL = kernel submit/completion boundary and, at D=1, submit→wait alternation; HOT = completion chain, submit chain, router, core mutex transitions; WAIT = park/epoll (dormant on warm tmpfs); AVOIDABLE = one enter/op (material), 3.1 K1 acquisitions, 3 pass-tail locks, per-CQE reap lock pair (mutex part only); UNAVOIDABLE = exactly-once terminal, borrow retirement, armed observer OBS-02, publication begin/complete, delivery-init before accept, admission/rollback, ledger reconcile, router identity, cancel/control/poison, bounded pass, no-lost-wake, shutdown quiescence; OVERLAPPED = io-wq copy chain (32.5-40.4% of all cycles across two 0/1 records, same logical CPU); MOVED = K2-a/early-K4 W-03 (driver pass+eventfd), K4-b 64 KiB (copy/tail).
ELISION_OPPORTUNITIES: (1) per-op io_uring_enter — material, form-gated, D=4/16/64 measured; (2) no-observer claim/retire — real, 2.0 acquisitions/op, sub-threshold; (3) per-entry publication pop lock — real, 0.889/op, sub-threshold; (4) pass-tail same-lock repeats — real, 3/pass, sub-threshold; (5) per-CQE reap lock pair + cqe_seen — no arm; the store part is unbounded by measurement. No other elision candidate survived.
TOP_CANDIDATES: K4-b (only form-gated end-to-end mechanism; preconditions open); K2-a (upper bound and gate-price reference, ELISION_CANDIDATE_FOUND); K1(a+b) (threshold-dependent no-change; revival only after a K4 product change and a redesign of the owed scan); combo (interaction evidence only).
NEGATIVE_FINDINGS: K1 NO_CHANGE_JUSTIFIED under the campaign band (INCONCLUSIVE if rejected); K3 NO_CHANGE_JUSTIFIED (dormant/amortized, best arm -1.00%); K4-a NO_CHANGE_JUSTIFIED (no arm, partial bound); K3-b NO_CHANGE_JUSTIFIED (premise refuted); K2-A/K2-B/K2-C closed; #580 router not reopened. New systemic gap: form dependence (topology x depth x payload); payload gate and D=2/D=3 are missing; cancel/partial-submit/early-reuse deferral load is unmeasured.

CORRECTNESS_REVIEW: all arms preserved terminal/lifetime/admission/publication/observer obligations; 11 formal mutation receipts BASE GREEN→MUTANT RED→RESTORED GREEN (5 K2-a + 6 K4-b, sharing 5 classes); K3 adds one guarded-mutant receipt plus two kills of an unguarded draft with no mutant/restored log; K1 has no dedicated mutant binary — its only kill is an over-elided sink build caught by two tests, and lost publication / publish_batch_ double count / K1-b false negative are unmutated. Combo base suite 16 targets GREEN and all six K4 mutants still red after the merge. Oracle limitations: k4_residue_wake_test is racy (100 ms sleep, no prepark proof, accepts any progress wake); k4_retry_residue_wake_test covers inline-flush EAGAIN, not a deferred multi-entry pass-start retry. Open: cancel/control load, deferred partial submit/overflow/shutdown, early reuse in the unflushed window, #585 semantics, K1 detail-header shape, K1 owed-scan lock hold.
MEASUREMENT_REVIEW: 15 interleaved AB/BA pairs, 2 warmup + 7 measured, fresh process, taskset -c 2, G0 checker + 64 MiB byte oracle per run; full binary is cost authority, observe binary only for frequency with +3.31..+15.35% overhead disclosed. CI method differs per artifact: K2-a/K4-b/combo case-summary CIs are bootstrap 95% (10,000 iterations, seed 593); K1/K3/K4 discrimination results are min-max pair ranges. p50/p95/p99 are reported (p95 reduced from pair-results.json / raw JSONL). The 4.79% band is NOT defined in the brief (grep count 0); its value is the max baseline A/A drift for W03-d4 (baseline-summary.md:126), the first text calling it pre-registered is after the K1 arm, and the label is retracted; K1/combo verdicts are band-dependent. Worker share is 32.5-40.4% across two 0/1 perf records (66.29/32.52 vs 58.43/40.35); rep_movs 12.0-14.2%. Saturate latency is not instrumented (lat_n=0) and the archived TSV's +0.00% is a reducer artifact. Warm tmpfs only; kernel attribution inherited from transient-sudo 0/1 records.
REPRODUCIBILITY: patches rebuildable from BASE_SHA; K4-b round-trip verified, combo patch regenerated from the merge tree; harness archived under bench/research/key-path-593/harness (K2-a/K4-b/combo reducer shared; K1/K3 reducers copied; K4 reducer MISSING; hard-coded workspace paths; one-shot provenance); environment, protocol, reducers and binary hashes recorded; raw evidence (1.4 GB / 6,870 files) persists only at /home/jnhu/sluice-593-research/evidence on the E5 host — single copy, not replicated, not in CI artifacts, and the per-rep JSONL/perf data are NOT hash-anchored; only summaries/patches/identities are. EVIDENCE_PRESERVATION: PARTIAL.
PRODUCT_CHANGE: NONE

RESEARCH_PR: https://github.com/jnhu76/Sluice/pull/594 (branch research/593-key-path at the frozen base; opened and remains OPEN, unmerged per brief §14).
PR_STATUS: OPEN_NOT_MERGED
OWNER_DECISION: PENDING

END_SLUICE_593_E5_KEY_PATH_RESEARCH
```
