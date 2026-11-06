# #595 R5 Phase A — Tail (p99) Analysis

Deliverable: the consolidated evidence picture (R4 archive + R5 fresh
re-analyses + the executed phase-decomposition diagnostic), the corrected and
refined literature mechanism matrix, the not-yet-identified p99 sources, and
the R4 failure classification. Written 2026-11-07 by the Phase A synthesis
writer from `r5/analysis/tail-evidence-review.md`,
`r5/analysis/literature-matrix.md`, `r5/analysis/diagnostic-protocol.md`,
`r5/analysis/diagnostic-results.md`, `r5/analysis/findings-c1-c4.md`,
`r5/analysis/findings-c5-c7.md` and the frozen R4 archive; the correction
adjudications in §2 were re-verified against `r5/r4-archive/TAIL-LATENCY.md`
and `r4/RESULTS.md` during synthesis.

## SUMMARY

1. **Where p99 localization stands.** R4 possessed no timestamp between
   ReadOp build and `take_result` on the 4 KiB shapes, so Hkernel/Hreap/
   Hobserve predicted identically at rep level. The R5 diagnostic (executed
   2026-11-06, frozen protocol) obtained complete 7-event traces (480,000/
   480,000 ops joined) but **failed its pre-registered on/off perturbation
   window** (+3.39pp/+3.96pp > ±3pp), so per its own frozen rule the tail gap
   is **not formally attributed**. Direction-level lead: R's d16 tail is
   **localized, at lead level, to the submission side of restart refills**
   (tail-op means: prep +20.2 µs, submit +3.2 µs, offset by observe −14.5 µs,
   kernel −3.9 µs, reap −0.2 µs); intra-batch completion straggle is refuted
   at the T3 level; the d4 sign flip reproduces on-mode (−9.5%/−9.7% vs R4's
   −12.92%).
2. **Evidence still missing for attribution.** An off-mode reference with
   more than one pair (the current one is a single noisy A0/R pair), or a
   lighter instrument whose differential cost is within the SHADOW−R
   precedent (+1.22% p99); plus the inherently unavailable kernel instants
   (SQE consumption, CQE write) and the absent PMU/IRQ attribution.
3. **R4 failure classification (unchanged in kind, sharpened in mechanism).**
   S/SNI/Q2: indiscriminate signal + stale snapshot + action not on target.
   SNP: action at the wrong moment. R/Q8: "action does not act on the target
   object" with a genuine tradeoff not excluded but not established; the
   diagnostic lead now localizes R's tail cost to back-op queueing inside
   restart refill groups (T0 stamped at group build), not to kernel
   straggle, wake latency, or reap/publication deferral — all measured
   against at interval level.

---

## 1. Existing evidence (what exists, at what observational level)

### 1.1 R4 archive inventory (all opened by the evidence reviewer)

| Artifact | Contents | Observational level |
|---|---|---|
| `r4/raw/matrix/MAIN-W03-D16/block-*.json` (24 blocks × 8 arms) | per invocation: metrics (p50/p95/p99_ns, cpu_ns_per_op, ops_per_sec, r3_*/r4_* decision counters), per-rep `reps[]` (n=20000: lat percentiles, notifications, passes, empty_wakes), env pre/post, `irq_delta`, `ru_*` incl. nvcsw/nivcsw, CPU tick buckets (10 ms) | invocation + rep summaries; **no per-request records, no phase timing** |
| CORE-W03-{D4,D64,D2SERIAL}, CORE-W02-*, MAIN others | same schema | same |
| `r4/raw/matrix/TRAJECTORY/` | 0/24 valid; SHADOW/T1 tripped the frozen `shadow_outstanding_sum==0` oracle (structural zero on d2serial — C7.3) | decision records only |
| `r4/raw/diagnostics/*.diag` | per-decision lines (S and SHADOW only): ready/eligible/present/quantum/row/active/pressure/pass_completed/caller_window/accepted/stop/cause | decision snapshots, **no timestamps** |
| `r4/raw/extension/DIAG-64K-STRACE/*.strace` | µs-timestamped `io_uring_enter` traces, 64 KiB shape only | syscall timing, **wrong shape** (64k is batch-ineligible → immediate path, `uring_backend.cpp:437-439`) |
| `r4/analysis/tail-latency-p*.csv`, MAIN_SUMMARY.json, EXTENSION_SUMMARY.json | frozen aggregates | derived summaries |
| `r4/RESULTS.md`, `r5/r4-archive/TAIL-LATENCY.md` | frozen gates, arm tables, CPU buckets, hypothesis list | conclusions |
| A/A cells | p99 half-width 2.10 pp on W03-D16 (`r4/RESULTS.md:329`) | noise floor |

Explicitly absent (verified): per-request latency vectors (discarded after
`compute_stats`, `bench_w03:787`); enter→CQE or reap/publish timing on 4 KiB
(R4 §10.5 NOT_RUN, `r4/RESULTS.md:342`); PMU/IRQ attribution (paranoid=2);
ns entry→enter (stripped binaries) — `r4/RESULTS.md:439`.

### 1.2 Fresh re-analyses of frozen R4 raw data (run by the evidence reviewer)

1. Driver cadence per arm (mean passes/20k ops): A0 1650, R 1290, Q8 1292,
   SHADOW 1293, S 1380 — R wakes 21.8% less often than A0 (1650→1290);
   S wakes 16.4% less often than A0 (1650→1380) but 7.0% more often than
   R (1290→1380).
2. Distribution shape (rep means, ns): A0 p50/p95/p99/max =
   36864/45118/51942/98773; R = 38613/46315/63196/**209153**; Q8 max
   **416760**; SHADOW max **576852**; SNP max 314790; Q2/S/SNI max
   111609–132899. The 8-batch arms stretch specifically above p95
   (p99−p95: 6.8 µs → 16.9–19.1 µs) **and** carry rare 210–577 µs outliers
   the 2-batch arms and A0 never show; the 2-batch arms shift the whole body
   right instead.
3. Per-block paired correlations with d_p99: corr(d_p99, d_passes) −0.057 (R)
   … +0.219 (S) — block-level p99 deltas are not explained by wake counts.
4. `ru_nivcsw`: A0 57476 vs R 26589 (R has **fewer** involuntary switches);
   irq_delta on cpu2 ≈ 1159–1387 for all arms (no arm signal).
5. SHADOW decision trace (acts ≡ R): `outstanding()==0` at 55.8% of
   decisions; **all 3,425 `stop=cap` truncations except one occur with
   `outstanding()==0`, `caller_window==0`, `ready=16`** — the cap fires
   exactly on full-window restart bursts after a drain. Corpus note: these
   figures are computed over the 7,504 decision lines of the single
   diagnostic invocation archived at
   `r4/raw/diagnostics/W03-D16__SHADOW__S.diag`, which pools the harness's
   three subphases (measured + oracle + instrumented headers present in the
   file; ≈2,501 presentations each, 3 × 2,501 ≈ 7,504); 55.8% = 4,188/7,504.
   They are a different artifact from the per-invocation counters C1 quotes
   from `r4/raw/diagnostics/logs/b00-p0-SHADOW-S-a1.log` (presentations=2501,
   cap_reentries=1221 for that invocation) — same arm and shape, separate
   runs, so the counts need no reconciliation.

### 1.3 R5 phase-decomposition diagnostic (EXECUTED — status and results)

Protocol frozen before collection (`r5/analysis/diagnostic-protocol.md`);
executed 2026-11-06T15:54Z–16:04Z under flock with LOCK_RECEIPTs
(`r5/raw-diag/LOCK_RECEIPT.md`); diag branch `research-595-r5-diag` @
`21374a40`, binary sha256 `78a228f2…`, 26 invocations all exit 0 (24 on-mode
measured phases = 2 shapes × 2 arms × 3 blocks × 2 orderings — **6 pairs per
shape, i.e. 6 runs per arm**, the block/ordering grid being 3×2 — plus the
off-mode pair), arms A0/R only, W03-d16 primary + W03-d4 control.

- **Coverage: complete** — 480,000/480,000 records fully joined, zero join
  failures, zero arena overflow; identity triple (slot, generation, cookie)
  on every record; negative `Tw−T2` differences kept (inline completions:
  A0 15.6%, R 3.6% at d16).
- **On/off perturbation check: FAILED** — off-mode R−A0 p99 delta +15.18% vs
  on-mode +18.57% (ratio of means) / +19.14% (mean of pair deltas); gap
  +3.39pp/+3.96pp against a ±3pp window (sign condition held). Per the
  frozen rule: **all on-mode numbers are perturbed leads; the protocol does
  not attribute.** The instrument's differential inflation, computed as
  (on-mode run-level mean p99 ÷ the single off-mode run's p99) − 1 per arm:
  R 64,516.5/60,786 = **+6.1%**, A0 54,412.5/52,775 = **+3.1%** — i.e. a
  ~3pp differential. Caveat on this derivation: the off-mode reference is
  one run per arm (single pair), the on-mode side is the mean of 6 runs per
  arm, so each arm's percentage carries single-run noise; the sign and rough
  magnitude of the differential are the point, not the third decimal.
- **Direction-level lead (joint, exactly additive tail-band means, d16; all
  five intervals listed so the sum is exact):** R tail ops carry
  **+20.2 µs prep (T1−T0)** (+20,246.8 ns), **+3.2 µs submit (T2−T1)**
  (+3,179.9), offset by **−14.5 µs observe (T5−T4)** (−14,468.3),
  −3.9 µs kernel (T3−T2) (−3,873.1), and −0.2 µs reap→publish (T4−T3)
  (−190.5); net **+4.9 µs** (+4,894.8 ns — the four rounded figures alone
  would read +5.0; the −0.2 reap term closes the rounding gap). A0's own
  tail is observe-dominated (24.9 µs mean: published-behind-front latch).
- **Tail-op phase membership:** 98.6% of R d16 tail ops submitted at
  `caller_window=0`; only 7.7% in `stop=cap` presentations (body 47.5%) —
  tail ops sit in the **second (`input_exhausted`) presentation of restart
  refills**, whose T0 was stamped at refill-build time before the first
  presentation was admitted/entered.
- **Hkernel-a refuted at T3:** intra-presentation completion spread within
  R's 8-op presentations p50 1.03 µs / p99 1.34 µs (max 50.2 µs on ≤0.1%).
- **d4 control:** on-mode R−A0 p99 −9.5%/−9.7% reproduces R4's sign flip
  (−12.92%); tail-band decomposition, all five intervals (from
  `raw-diag/aggregate-summary.json` tail-band means, A0→R):
  prep +0.58 µs (209.9→787.3), submit +10.71 µs (4,999.7→15,708.7),
  kernel −7.85 µs (16,963.5→9,112.5), reap +0.20 µs (579.1→783.1),
  observe −5.93 µs (12,863.9→6,930.5); net **−2.29 µs**
  (35,616.1→33,322.1, exactly additive).

Interpretation constraints (explicit): single session, single host, 6 pairs
per shape (6 runs per arm), single-rep phases; off-mode reference is one pair;
kernel-side instants unavailable by construction; the instrument's T3/T4
stamps are taken under `dispatch_mtx_`, so their cost is part of the measured
perturbation.

### 1.4 What existing evidence already answers

- The regression is p99-concentrated for 8-batch arms, body-wide for 2-batch
  arms; not the cost of reading state (SHADOW−R p99 +1.22%, CPU +1.02% —
  with the C3 caveat: that bound covers ~2 decisions/group, not S's ~8); not
  block-level wake-count variation (§1.2.3); not gross noise (A/A 2.10 pp ≪
  +21.85%).
- Restart/drain phases are structurally special (§1.2.5, §1.3 membership);
  SNP's wrong-moment small batches reach S-level p99; batching helps the tail
  when completions are slow (btrfs p99 −30.43%) or shallow (d4 p99 −12.92%).
- CPU attribution is 10 ms-bucket only: submitter 38–46%, io-wq 32–36%,
  driver ≤0.5%, residual 22–26% — tick buckets cannot see tail events.

---

## 2. Literature mechanism matrix (merged, refined, corrected)

Source: `r5/analysis/literature-matrix.md` (p99 literature-mechanism analyst;
paper provenance and retrieval honesty recorded there, §4 — five papers:
IX OSDI'14, Shinjuku NSDI'19, Shenango NSDI'19, Perséphone SOSP'21
submission #344, ZygOS SOSP'17 camera-ready text via course-site mirror,
Linux 6.12 NAPI docs). This section merges that matrix with the verifier
findings and the executed diagnostic. **Corrections applied to the
literature analyst's Sluice-side facts** (each verified this session):

- **COR-1 (orientation mixing, fixed):** the original row 6 cited
  "R−B: re-entry cheaper than suffix singles (T:107-109)" next to RESULTS
  numbers in the opposite orientation without noting the flip. Corrected
  statement: **arm B** is the R4 "B supplement" baseline arm R3_FIXED_PREFIX
  (one full-span presentation, no cap re-entry, remainder via per-op suffix
  singles; `arm_policy` `bench/r3_ready_batch.hpp:176-181`; glossary in
  CALL_PATTERNS.md §1.0 — distinct from NEXT_EXPERIMENT.md's Candidate B and
  its budget variable `B_ops`). TAIL-LATENCY §5 reports mean-over-blocks of
  per-block `100·(R−B)/B` with negative = R better (p50 −7.77%, p95 −24.06%,
  p99 −12.44%, CPU/op −8.18%, tput +9.07%); RESULTS.md:188/394 report the
  gate pipeline's `paired_diff_B_minus_R_pct` container (verified in
  `r4/raw/bsupp/B-SUPP-W03-D16/DESCRIPTIVE.json`: cpu mean 9.0363, p95 mean
  32.1672, p99 mean 14.5017, tput_drop mean 8.2123), i.e. mean-over-blocks of
  per-block `100·(B−R)/R` for cost and `100·(R−B)/R` for throughput.
  **These two columns are not algebraically interconvertible**: converting
  denominators per block is impossible after averaging (and the naive pooled
  conversion of TAIL's p99, −12.44% ⇒ ≈+14.20%, indeed does not reproduce
  the gate's +14.50%, nor p95's ≈+31.68 vs +32.17, CPU's ≈+8.91 vs +9.04,
  tput's ≈+8.32 vs +8.21). Both columns are correct under their own frozen
  convention and each is cited to its own source; never mix them in one row
  label, and do not "reconcile" them by arithmetic (C7.1 rule).
- **COR-2 (SHADOW−R caliber, added):** row 6 used SHADOW−R (+1.02% CPU) as
  "the read cost" without the decision-frequency caveat. C3 established S
  reads `outstanding()` ≈8×/group at d16 vs SHADOW ≈2×/group (R4 counters:
  S 49,962 pressure reads vs SHADOW 12,524 per invocation, T:47), so the
  bound covers ~2 decisions/group only.
- **COR-3 (stale citations, fixed):** `outstanding()` is read at
  `r3:469` only (not 469-470,503; line ~503 is the active-zero counter);
  kTable rows live at `r3:222-245`; `cap_reuse` condition at `r3:561-563`.
- **COR-4 (superseded predictions, updated):** rows 2 and 10 predated the
  diagnostic; their predictions are now partially resolved — see the
  "R5 status" column below.

| # | Paper mechanism | Sluice counterpart | Action under round constraints | R5 status after the diagnostic | Adopt/defer |
|---|---|---|---|---|---|
| 1 | IX adaptive bounded batching — never wait to batch; cap bounds the batch (OSDI'14 §5) | R/Q8 presentation = cap 8 over the already-ready span (`r3:497`); R4 counters confirm no forced waits (R presents ~8 of ~8 ready) | Keep R's admission rule as the reference frame | Unchanged; R4 counters had already refuted "waiting to batch" for R/Q8 | **Adopt** (reference frame) |
| 2 | IX batch = run-to-completion execution unit (§5; ZygOS §4.2) — batch boundary = execution boundary | In Sluice the group is a *submission* unit only; completions are individual CQEs reaped by the owner pass | Per-position completion timing inside presented groups (test-only) | **Executed** (T3 spread): intra-batch straggle ≈1 µs — hypothesis 1 (last-member completion wait) refuted at T3 in perturbed mode; the wait that exists is *before* the enter (prep/submit), not inside the kernel batch | **Adopted and executed**; result folded into G5 E10 |
| 3 | ZygOS work-conserving stealing (SOSP'17 §4.2,§4.4,§4.5) | W03/W02 have one presentation queue, one submitter, one reaper thread; no second executor or stealable queue | None legal (would need new threads/queues) | n/a | **Defer** (precondition absent) |
| 4 | ZygOS §3.3/§6.2: tiny-task batching tension — disabling IX batching improved tail; winner flips with SLO axis | tmpfs 4 KiB = tiny-homogeneous regime; R4 reproduces both faces (CPU −19.6%/tput +24.5% AND p99 +21.85% at d16; p99 −12.9 at d4; −30.4 btrfs) | Use the depth/speed sweep as the tradeoff map | Diagnostic: the d16-vs-d4 flip is reproduced on-mode and decomposes into submit-side cost vs kernel+observe savings — consistent with completion-path speed governing the flip | **Adopt** (interpretation frame) |
| 5 | Shinjuku: preempt only when a beneficiary exists (NSDI'19 §3.4) | No userspace preemption point over submitted SQEs; quantum slicing shrinks *future* submissions only; SNP shrinks at `outstanding()==0` (no beneficiary by definition) | Predicate discipline only: never shrink at zero-outstanding | Diagnostic sharpens it: zero-outstanding = restart moments where the tail is created; shrinking there (SNP) hits exactly the tail requests | **Adopt discipline; reject shrink-as-preemption** |
| 6 | Shinjuku/Perséphone preemption floor cost | Sluice's presentation cap is nearly free (SHADOW−R +1.02% CPU at ~2 decisions/group — COR-2) but buys no interruption; S paid +8.51% CPU and +10.52% p99 vs R (`r4/RESULTS.md:187`, S−R row) | Reject S-family as tail fixes | Unchanged | **Adopt** (rejection rationale) |
| 7 | Shenango: congestion = same item still queued across two consecutive samples (NSDI'19 §4.1.1) | `owner_pass_activity` is flow-positivity ("previous drain pass published >0"), fires 99.92% — not persistence | Same-item persistence / wait age derived at the existing decision point (analysis-stage) | Carried into NEXT_EXPERIMENT Candidate B as the eligibility predicate class | **Adopt** (replacement eligibility hypothesis) |
| 8 | Shenango dedicated sampling core (§3.1) | Forbidden (no dedicated core/new thread/timer) | None legal | n/a | **Defer** (constraint-forbidden; only the signal design transfers) |
| 9 | Staleness: a signal consumed at decision time must describe decision time | DecisionFacts describe the owner's previous terminal pass, unbounded age; `last_pass_immediate_work` structurally false when published | Snapshot-age counter (test-only) | Not executed this round; C2 renamed the signal and documented the lag | **Adopt** (measurement first, still open) |
| 10 | NAPI budget-exhaustion continuation (kernel 6.12 docs) | `stop=cap` + immediate suffix re-entry = continuation paid inline (extra presentation per refill) | Keep R's re-entry (it beat arm B — R3_FIXED_PREFIX suffix-singles baseline, defined at COR-1 below and CALL_PATTERNS §1.0 — on every percentile, both orientations, COR-1) | Diagnostic: tail ops are *not* in cap presentations (7.7%) but in the following `input_exhausted` presentation — the restart-refill serialization, not the cap boundary itself, is where the tail lands | **Adopt** (interpretation, sharpened) |
| 11 | NAPI drain-phase policy: at empty, repoll boundedly then return to events; never shrink the restart batch | SNP shrank restart batches at `outstanding()==0`; the NAPI-analogous action is full-prefix presentation or park | SNP falsifier: replace clamp2-at-zero with full-prefix presentation | Not executed; folded into Candidate B's "budget exhausted → genuinely return to the existing progress/observer path" | **Adopt** (as falsifier design) |

Explicitly not adopted (no support under resource constraints; §3.4 of the
original matrix): hedging/duplicate I/O, busy polling, SQPOLL, thread-count
increases, deadline-oriented I/O APIs. Row-level verdicts in the original
matrix: 9 adopt (including adopt-as-interpretation/rejection-rationale), 2
defer; §3.1 condensed them into 4 design commitments, §3.2 defers 4 mechanism
families (stealing, sampler core, preemption machinery, Perséphone-style core
reservation — the last retained only as a cost datum: one reserved worker of
16 ≈ 5% throughput, ~11,200 cycles per reservation update). The input
digest's "adopt 5 / defer 5" is a coarser grouping (§3.1's commitments plus
keep-R; §3.2's families plus Perséphone); the row- and section-level counts
above are the authoritative ones from the matrix file.

---

## 3. p99 sources: identified, refuted, not yet identified

**Supported (lead level, perturbed mode):**

- L1 — **Restart-refill submission serialization**: T0 is stamped for all k
  ops at refill build (`bench_w03:404-413`); back ops wait in `prep`/`submit`
  while earlier presentations of the same `submit()` call are
  admitted/prepared/entered. Tail-band means +20.2/+3.2 µs; tail-op
  membership 98.6% at `caller_window=0`. (G5 E4/E5/E18.)
- L2 — **Observe-latch asymmetry**: the published-behind-front latch
  (front-only drain) penalizes A0 more than R in this shape (A0 observe
  tail-mean 24.9 µs vs R 10.4 µs) — R currently *reduces* the observe
  component. Any candidate that slows publication aggregation must not
  surrender this. (G5 E16.)

**Measured against (lead level):**

- Intra-batch completion straggle (Hkernel-a): T3 spread ≈1 µs.
- Driver-wake latency: wake-part p99 R 32.9 µs < A0 35.4 µs.
- Reap→publish deferral: Δp99 +59 ns.

**Not yet identified / not yet measurable (the honest open set):**

- U1 — **io-wq / inline execution scheduling delay** of late SQEs: kernel
  instants unavailable; io-wq CPU share (31.8–36.1%) is bucket accounting
  only. The rare ~50 µs T3-spread events (≤0.1% of presentations) are the
  only kernel-side tail candidate visible and are unexplained.
- U2 — **Thread placement / SMT / IRQ interaction** (submitter+driver+io-wq
  on cpu2, sibling cpu12): no PMU (paranoid=2), no IRQ attribution.
- U3 — **Lock-held-time magnitude** of the per-CQE `dispatch_mtx_` vs
  whole-batch submitter critical section: structure proven, cost unmeasured.
- U4 — **The extreme 210–577 µs outliers** in R4's 8-batch-arm reps (max
  latencies): not reproduced in the on-mode diagnostic pools (max ~96.7 µs);
  their source (kernel, scheduler, or instrumentation) is unidentified.
- U5 — **The instrument's differential perturbation** (+3–4pp on R's p99):
  itself unexplained; blocks formal attribution.
- U6 — **Phase-3 vs phase-1 dynamics**: latency statistics come from the
  harness's separate instrumented subphase (`bench_w03:779-787`); same
  conditions both arms, but phase divergence remains a residual, unexcluded
  risk (Hmeasure residual).

REQUIRED_INFORMATION_GAP (formal): the decomposition
`T5−T0 = (T1−T0)+(T2−T1)+(T3−T2)+(T4−T3)+(T5−T4)` is measured but not
attributable — the on/off acceptance failed and the off-mode reference is a
single pair. Minimum to close: (i) ≥3 off-mode pairs (or a lighter
instrument with differential cost within the SHADOW−R +1.22% p99 precedent),
re-run under the same lock discipline; (ii) then re-apply the frozen ≥50%
attribution rule. Kernel-side instants stay unavailable by construction and
must be reported as such.

---

## 4. R4 failure classification (mechanism-level verdicts)

| Arm(s) | Classification | Mechanism evidence |
|---|---|---|
| S, SNI, Q2 | **indiscriminate signal + stale snapshot + action not on target** (signal defect primary) | `owner_pass_activity` is flow-positivity firing 99.92% under saturation (`r4/RESULTS.md:199`); the fact is one owner-pass old with unbounded age and its `immediate_work` half is structurally false when published; quantum-2 slicing cannot interrupt submitted SQEs — it throttles pipeline refill, matching S's whole-distribution right shift (p50 +13.03, `r5/r4-archive/TAIL-LATENCY.md:24`; S−R CPU +8.51% / p99 +10.52%, `r4/RESULTS.md:187`) rather than targeted tail relief; S ≈ Q2 to +0.83% (`r4/RESULTS.md:186`, S−Q2 CPU) — the measured signature of a no-op condition |
| SNP | **action not on target (wrong moment)** | its live `outstanding()==0` trigger fires exactly when no beneficiary exists — at drain/restart moments, which the diagnostic shows are precisely where R's tail ops live; 35% small presentations yet p99 +34.48 ≈ S level vs uniform Q8 +23.33 |
| R, Q8 (p99-only regression) | **action does not act on the target object**; genuine tradeoff NOT excluded but NOT established | R/Q8 never delay pending work (presentation = min(cap, ready)) yet regress p99 24/24 blocks; own sign flips (d4 −12.9%, btrfs −30.4%) point at completion-side/speed interactions the batching action does not touch; the diagnostic lead localizes the added tail time to back-op queueing inside restart refills (submission side), not to kernel straggle/wake/reap-publish (all measured against); R's own action is cheap (SHADOW−R bound, COR-2) |
| literature alternatives (preemption, sampler, reservation) | **execution too expensive / structurally unavailable** — attaches to the alternatives, not to R | preemption needs a point that does not exist over submitted SQEs; Shenango's sampler needs a dedicated core; Perséphone's DARC reserves cores and needs a payload classifier uniform-4KiB ops do not expose; work stealing has no legal target |
| unavoidable tradeoff | **only R/Q8 unexcluded, retained as a candidate — not a conclusion** | ZygOS §3.3 proves the tension exists in this regime; IX §5 and R4's own d4/btrfs sign flips show it is conditional, not a law, at d16 |

---

## 5. What this analysis recommends (pointer)

The evidence chain (L1 lead + C1/C3 overhead structure + the control-point
finding) motivates the candidate ordering in NEXT_EXPERIMENT.md: Candidate A
(lighter R execution chain on top of the verified cleanup) first, Candidate B
(residue-aware budget with a genuine service opportunity) as the structural
follow-up, Candidate C deferred. No Phase-B execution happened in this round.
