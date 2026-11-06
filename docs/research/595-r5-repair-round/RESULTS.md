# #595 R5-repair — RESULTS (three-arm measurement round)

Round `r5-repair-measure`, executed 2026-11-06/07 (UTC) on the native E5
host under `PREREGISTERED.md` (sha256 `7a412133d9c72a497da6f3689f114daebc
946d7df6ee9207ef1aef0a88251fb7`, frozen before any formal timing).
712/712 planned invocations valid (0 invalid, 0 retries needed); every
timing segment under the dual measurement lock with receipts
(`logs/LOCK_RECEIPT.md`).

## Answers to the three questions

**Q1 — can #630's diagnostic and analysis methods be repaired to
usable/reviewable?** YES for the instrument, NO for formal attribution.
The v1 races are confirmed, fixed (v2 per-writer instrument,
`research-595-r5rm-diag` @ `61bf73d6` incl. off-mode fast path) and
validated (TSan-clean on both a 5-thread synthetic harness — v1 reports
races under the same harness — and the real io_uring bench; full
20000/20000 join on every event kind). The analysis-method errors (F2
percentile-contribution rule, F4 semantics wording, F6 archive gaps) are
corrected. But the repaired instrument STILL fails the frozen ±3pp
on/off perturbation window (interactions −4.9/+5.4pp on d16, +5.1/+9.2pp
on d4; CIs span up to ±23pp) — so diagnostic numbers stay lead-level
and `TRACE_READY` holds for the instrument while **formal tail
attribution remains NOT_ESTABLISHED**.

**Q2 — does C1/C2 cleanup actually reduce CPU work and improve p99 vs
R_original?** Mixed and small. On the main shape the raw C−R p99 effect
is −1.77% with 95% upper bound −0.55% < 0 ("established" by the frozen
criterion), BUT the A0-original/A0-cleanup binary calibration shows the
same-order binary-layout effect (p99 −1.47%, cpu −1.45% on MAIN), so the
p99 gain is **within the cross-binary calibration band and not
attributable to the cleanup logic**. C−R CPU on MAIN is −0.73%
[−2.02,+0.50] — not established. On W03-d4 and d2serial, calibrated CPU
residuals are ≈ −2.4%/−2.1% (raw −3.86%/−4.42%, calibration −1.49%/−2.28%)
— small real savings where the removed scan ran most often relative to
work. On W02 cells C−R is positive (+3.2% cpu) but the calibration is
+2.1/+3.4% there too — residual ≈ +0.9/−0.1%: no cleanup harm, no gain.
Nowhere is C significantly worse than R after calibration.

**Q3 — does R_cleanup meet the full gate vs A0?** NO — same single
failure as R4, robustly: MAIN-W03-D16 CPU/op −22.09% [−22.75,−21.41]
(PASS), throughput +28.49% (one-sided lower +27.53% ≥ −3%, PASS), p95
+1.15% (upper +3.66% ≤ +5%, PASS), **p99 +17.97% (upper +20.34% > +5%,
FAIL; Holm-adjusted p = 1.0)**. Protections: W03-d4 PASSES every
protection bound (C is better than A0 on all four metrics there:
cpu −16.8%, tput +20.3%, p95 −22.4%, p99 −19.1% — the d4 sign flip
reproduces); **W03-d2serial, W02-d1 and W02-d2 FAIL** (cpu upper +5.1
to +6.4 > +5%, tput lower −5.7 to −6.0 < −3%, p95/p99 uppers above +5%
on d2serial and W02-d2; W02-d1 p99 precision additionally
NOT_ESTABLISHED, A/A half-width 3.39pp). The shallow-queue adapter tax
persists under cleanup, as expected from proven equivalence.

## Full numbers

### MAIN-W03-D16 (24 matched blocks × 3 arms)

| contrast | cpu/op | throughput | p95 | p99 |
|---|---|---|---|---|
| C−A0 | **−22.09%** [−22.75,−21.41] | **+28.49%** [+27.35,+29.59] | +1.15% [−1.78,+4.10] | **+17.97%** [+15.18,+20.82] |
| R−A0 | −21.47% [−22.25,−20.65] | +27.51% [+26.19,+28.78] | +3.46% [+1.21,+5.73] | +20.14% [+17.50,+22.88] |
| C−R | −0.73% [−2.02,+0.50] | +0.83% [−0.45,+2.20] | −2.10% [−4.86,+0.87] | −1.77% [−3.16,−0.33] |

Absolute means: A0 3196 cpu-ns/op, 51.6µs p99, 311k ops/s; R 2509, 62.0µs,
400k; C 2491, 62.3µs (see `analysis/round-analysis.json`, figure F1).

Gate p-values (bootstrap, Holm across the 4 main hypotheses):
CPU p_holm=0.0004 PASS; throughput p_holm=0.0004 PASS; p95 p_holm=0.0082
PASS; p99 p=1.0000 **FAIL**.

### Protections (C−A0, 24 blocks each)

| cell | cpu | tput | p95 | p99 | verdict |
|---|---|---|---|---|---|
| W03-d4 | −16.81% [−17.55,−16.07] | +20.31% | −22.44% | −19.14% [−22.61,−15.26] | PASS |
| W03-d2serial | +3.90% [+2.69,+5.12] | −3.67% [−4.79,−2.54] | +9.17% | +8.47% [+2.27,+15.01] | FAIL (p99 precision NOT_ESTABLISHED: A/A hw 8.24pp) |
| W02-d1 | +5.37% [+4.72,+6.02] | −5.09% | +4.72% [+3.59,+5.67] | +3.94% [+1.44,+6.12] | FAIL (p99 precision NOT_ESTABLISHED: A/A hw 3.39pp) |
| W02-d2 | +5.78% [+5.21,+6.36] | −5.46% | +4.90% [+3.99,+6.04] | +5.67% [+2.03,+9.88] | FAIL |

### A/A and calibration

| cell | A/A cpu/tput/p95/p99 half-widths (pp) | A/A drift | A0c−A0o cpu | A0c−A0o p99 |
|---|---|---|---|---|
| MAIN | 0.62 / 0.67 / 2.17 / 2.81 all PASS | −2.05/+2.11/−1.90/−2.26 % | −1.45% [−2.30,−0.58] | −1.47% [−3.93,+0.98] |
| W03-d4 | 1.51/1.47/1.51/2.57 PASS | ≤1.0% | −1.49% | −5.35% [−7.71,−3.42] |
| W03-d2serial | 0.79/0.78/2.33/**8.24 FAIL** | +7.30% p99 drift | −2.28% | +8.98% [−5.80,+23.76] |
| W02-d1 | 0.83/0.81/1.44/**3.39 FAIL** | ≤0.6% | +2.08% [+1.05,+3.22] | +3.61% [+1.10,+6.71] |
| W02-d2 | 0.48/0.48/0.57/0.83 PASS | ≤1.2% | +3.38% [+1.88,+4.96] | +5.41% [+3.55,+7.33] |

Cross-binary calibration interpretation: A0 runs the identical product
path in both binaries (the adapter is not even constructed for `a0` on
W03), so A0c−A0o measures pure binary-layout effects — ±1.5% on W03 and
+2 to +3.4% on W02. All C−R effects (|·| ≤ 4.7%) are within or near
these bands; the only C−R effects that survive calibration are the
d4/d2serial CPU savings (≈−2.4/−2.1% residual).

### Resources

- Thread counts identical across arms per shape (W03: max_user_threads=3
  including the driver; W02: 2). No new threads/timers/queues.
- io-wq kernel-worker ticks: R/C ≤ A0 everywhere (batching reduces
  io-wq entries); no increase.
- RSS (dedicated supplement session, 3×3 invocations, gate +2%):
  A0 19,135 KB; R 19,199 KB (+0.33%); C 19,252 KB (**+0.61% ≤ +2% PASS**;
  absolute values reported; the formal-matrix records lacked
  `ru_maxrss_kb` — recorded as a capture omission and closed with this
  supplement under the same lock/gates).
- CPU accounting: primary = process rusage (all joined threads);
  per-thread buckets recorded per invocation; io-wq kernel threads are
  outside process rusage and reported separately — a stated coverage gap,
  never added, never called whole-system CPU. PMU: UNAVAILABLE
  (perf_event_paranoid=2); no PMU number reported.

### Diagnostic round (fixed instrument, on/off, 12 blocks × 3 arms × 2 modes × 2 shapes)

Perturbation interactions (X−A0)_on − (X−A0)_off on p99:
d16: R −4.89pp [−12.18,+2.28], C +5.42pp [−8.09,+23.29];
d4: R +5.10pp [−4.64,+12.71], C +9.18pp [−4.19,+23.11].
**All four windows FAIL the ±3pp equivalence window → PERTURBED; the
diagnostic numbers below are lead-level only (no formal attribution),
figure F5 is marked PERTURBED.**

Lead-level (R, d16, measured phases, 240k joined ops):
conditional tail(>55µs) risk by real in-window position at build —
position 0: 5.2% (n=225,079, median 42.7µs, p99 74.2µs); position 8
(back half of a 16-op refill): **26.3%** (n=14,904, median 36.5µs,
p99 63.7µs) — a ~5× conditional tail-risk ratio concentrated on
back-of-refill ops under `input_exhausted` presentations (10,263 of
15,593 tail ops at pos 0 are input_exhausted; 3,923 of 3,923 pos-8 tails
are input_exhausted). This reproduces the R5 Phase-A lead
(back-op queueing inside restart refills) under the repaired instrument,
with the position risk now conditional rather than tail-share. Kernel
completion instants remain unobservable; kernel-side tails are NOT
excluded.

## Outcome states

- MAIN gate: **FAIL** (p99 only; CPU/throughput/p95 PASS robustly).
- Protections: W03-d4 PASS; W03-d2serial/W02-d1/W02-d2 FAIL.
- Conditional branches (d64, btrfs): **NOT_RUN_CONDITIONAL** (main gate
  did not pass; per prereg §6).
- S/SNI/SNP/Q2/Q8 matrix: not in this round's scope.
- A/A precision: MAIN/d4/W02-d2 ESTABLISHED; d2serial p99 and W02-d1 p99
  NOT_ESTABLISHED (their gate FAILs stand on point estimates and bounds
  but precision qualification is recorded honestly).
- EXECUTION_STATUS: MAIN_COMPLETE + TRACE instrument fixed/validated,
  formal tail attribution TRACE_NOT_ESTABLISHED (perturbation).

## Single next action (one recommendation)

**Stop pursuing cleanup-class changes on this line.** The equivalence-
preserving cleanup is complete and safe to keep (never harmful after
calibration, small real CPU savings at d4/d2serial), but the p99 gap to
A0 (+18%) and the shallow-queue tax are policy properties of the
batching itself, not dead work. The next single experiment, if the line
continues, is the pre-designed **Candidate B (residue-aware continuous
submission budget)** from NEXT_EXPERIMENT.md — the only designed
candidate whose action targets the now-confirmed back-of-refill
queueing — run on top of R_cleanup under the same gate caliber, with
the corrected attribution protocol. If the line does not continue, the
R-family conclusion stands: batching trades ~+18% p99 (d16) and a
shallow-queue tax for −22% CPU/+28% throughput.
