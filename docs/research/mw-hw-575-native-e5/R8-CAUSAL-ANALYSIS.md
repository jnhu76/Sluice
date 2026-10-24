# R8 — Statistical and Causal Analysis

Reduction: frozen `scripts/reduce_575.py` (self-tested), paired per-rep
ratios, median ratio, bootstrap CI95 (10 000 resamples), per-pair direction.
No repetition was excluded; every recorded row is in `raw/ab/`. The
preregistered reading rule (R5): a contrast is an effect only when its CI
separates from 1.0 **and** from the corresponding A/A calibration band.

## Effect inventory (all decision-bearing contrasts)

### ThreadPool (stats off; stats on reproduces the same signs)

| Contrast | d1-w1 | d4-w4 | d16-w4 | d16-w8 | 64k | W-03 |
|---|---|---|---|---|---|---|
| A BASE→OP-ONLY wall | **−3.4 %** ⚠ | +1.0 % | +0.4 % | +0.8 % | −0.3 % | −0.2 % |
| B OP-ONLY→FULL wall | **+3.8 %** ⚠ | −0.2 % | ≈0 | −0.5 % | ≈0 | ≈0 |
| C BASE→FULL wall | +0.6 % | +0.8 % | ≈0 | +0.2 % | ≈0 | ≈0 |
| A instructions | **−0.022 %** ✓ | +0.7 % | +0.1 % | +0.5 % | −0.1 % | −0.7 % |

⚠ = statistically separated but attributed to code shape (below).
✓ = the true mechanism elimination.

### io_uring (stats off; stats on null)

| Contrast | d1 | d4 | d16 | 64k | W-03 | d8 cap8 | d8 cap1024 |
|---|---|---|---|---|---|---|---|
| A wall | −0.3 % | ≈0 | +0.3 % | −0.2 % | −0.7 % | +1.3 % | −1.6 % |
| B wall | +0.4 % | ≈0 | +0.1 % | −0.1 % | ≈0 | — | — |
| C wall | −0.2 % | −0.5 % | +0.7 % | −0.2 % | **−0.75 %** | +0.2 % | −1.2 % |
| C cycles | **+0.8 %** | ≈0 | **+0.5 %** | +0.2 % | −0.8 % | +0.7 % | −0.6 % |

### Attribution campaign (plain-increment control arms, 15 reps)

| Contrast | TP d1-w1 wall | TP W-03 wall | UR d1 wall | UR W-03 cycles |
|---|---|---|---|---|
| BASE→CTRL (lock off, memory kept) | +0.9 % (no gain) | −0.9 % (n.s.) | **−1.3 %** | **−0.8 %** |
| CTRL→OP-ONLY (increment deleted) | **−3.2 %** | +0.1 % | +0.3 % (n.s.; instr +0.43 %) | +0.4 % |

## Causal model consistent with all observations

1. **Instruction elimination (mechanism, level A): real and bounded.**
   Exactly ~1 instruction per counted event disappears: −0.022 % at the TP
   highest-frequency config (d1-w1), statistically solid. This is the
   entire directly-attributable static saving.
2. **Locked-RMW execution cost (level B): near zero for TP, ~0.4–1 % for
   UR submit-path regimes.** TP: the control arm is indistinguishable from
   BASE everywhere — with a single writer per op and no production reader
   of the line, the locked add is a local L1 RMW, ~0.03 % of the pipeline.
   UR: the control shows a small real cost (d1 wall −1.3 %, W-03 cycles
   −0.8 %), consistent with the flush RMW sitting inside the
   `dispatch_mtx_` critical section on the submit/poll path.
3. **Code-shape effects dominate the naive contrasts.** Deleting or adding
   a few instructions in `worker_loop` / `submit_transport_locked` shifts
   binary code layout; the resulting swings (TP d1-w1: −3.4 % for OP-ONLY
   and +3.8 % for FULL, cancelling to C=+0.6 %; UR d1: +0.43 %
   instructions for OP-ONLY) are reproducible per binary pair but are
   luck-of-alignment, not semantics: they appear when the *code bytes*
   change (CTRL→OP-ONLY) and not when only the *lock prefix* is removed
   (BASE→CTRL). They would not transfer to a different compiler, flag set
   or surrounding code, so they cannot be claimed as the cost of the
   counter.
4. **Kernel side is untouched.** Context-switch census identical (±7 of
   360 028 at UR d1), system-CPU ratios ≈ 1.0, W-01 negative control
   (byte-identical binaries across arms) centered at 1.000–1.013 with CI
   spanning 1.0. No cache/TLB/branch PMU attribution is claimed beyond
   this: the Haswell PMU events available under the owner's paranoid=1
   override were not needed for the verdict, and no unsupported
   attribution is manufactured.

## Materiality ladder (per the campaign brief)

- **A. Mechanism improvement — YES (both backends).** Target RMW removed
  from Release assembly (proven); instruction count falls by the expected
  ~1/event.
- **B. Runtime improvement — NO as an end-to-end claim.** A small genuine
  locked-RMW cost exists only for UR (~0.4–1 % on submit-path regimes) and
  is not realized by the deletion patches because their code-shape side
  effects are of the same magnitude and mixed sign.
- **C. Product improvement — NO (both backends).** No supported W-02/W-03
  configuration shows a material, reproducible end-to-end improvement for
  either candidate arm; TP's C at its best-frequency config is slightly
  *negative* (+0.6 % [1.000, 1.011] slower), UR's best point (W-03
  −0.75 %) is offset by +0.5–0.8 % cycles regressions in the same arm and
  does not reproduce under stats-on.

## Preregistered exclusions

None. All 15 repetitions of every campaign are included. The only
protocol-level deviations, both documented before their respective
measurements, are: (1) the W-03 threadpool case id fix (a runner bug that
aborted the first TP campaign before any measurement was used), and (2) the
runner v1→v2→v3 evolution, which occurred entirely inside the calibration
phase (A/A on identical binaries) and before any candidate A/B measurement
was adopted; the final v3 protocol was re-calibrated (cal5) and then frozen
for every A/B campaign reported in R6/R7.

## Verdict input

Only level C authorizes a performance-driven `PROPOSE_UPSTREAM`. Level C is
not reached for either backend. Both candidates therefore fall to
`NO_CHANGE_JUSTIFIED` on the performance axis, with the semantic/minimal-
work axis reported separately (R1: both counters are observation-only,
test-only state; removal is *semantically* safe and the internal-testing
build preserves all counting behavior — a product owner may still weigh
that dead-state cleanup on its own merits, but this campaign's evidence
does not establish a performance benefit, and the code-shape sensitivity
measured here says a removal PR must not cite performance as its
justification).
