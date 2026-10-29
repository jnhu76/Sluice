# R8 — Statistical and Causal Analysis

Reduction: frozen `scripts/reduce_575.py` (self-tested), paired per-rep
ratios, median ratio, per-pair direction. **Post-Review-B revision:** the
first published record used a Monte Carlo bootstrap CI that is
systematically one order statistic too tight at n = 15 (~36–38 % of
endpoints); all intervals below are the exact percentile-bootstrap
intervals (order statistics v4/v12 for n = 15) recomputed from the same
raw rows. No repetition was excluded; every recorded row is in `raw/ab/`.
Preregistered reading rule (R5): a contrast is an effect only when its CI
separates from 1.0 **and** from the corresponding A/A calibration band.
Statistics-language note (closeout): "exact" qualifies the *computation*
of the percentile-bootstrap interval — deterministic order statistics of
the 15 observed ratios, no resampling error, verified against the frozen
self-tested reducer. It is **not** a claim of exact confidence-interval
*coverage*: at n = 15 the intervals remain small-sample uncertainty
estimates, and the many configuration-by-contrast cells in this record
are exploratory comparisons read against the preregistered separation
rule, not a family-wise guarantee.

## Effect inventory (all decision-bearing contrasts, corrected CIs)

### ThreadPool (stats off; stats on reproduces the same signs)

| Contrast | d1-w1 | d4-w4 | d16-w4 | d16-w8 | 64k | W-03 |
|---|---|---|---|---|---|---|
| A BASE→OP-ONLY wall | **−3.4 %** ⚠ | +1.0 % | +0.4 % | +0.8 % ⚠ | −0.3 % | −0.2 % |
| B OP-ONLY→FULL wall | **+3.8 %** ⚠ | −0.2 % | ≈0 | −0.5 % | +0.1 % | +0.4 % |
| C BASE→FULL wall | +0.6 % (knife-edge) | +0.8 % | **+1.2 %** | +0.2 % | +0.1 % | −1.0 % |
| A instructions | **−0.022 %** ✓ | +0.7 % | +0.1 % | +0.5 % | −0.1 % | −0.7 % |

⚠ = statistically separated but attributed to code shape (below).
✓ = the true mechanism elimination (barely clears the A/A instruction band;
see R6's quantification caveat).

### io_uring (stats off; stats on null)

| Contrast | d1 | d4 | d16 | 64k | W-03 | d8 cap8 | d8 cap1024 |
|---|---|---|---|---|---|---|---|
| A wall | −0.3 % | ≈0 | +0.3 % | −0.2 % | −0.7 % | +1.3 % | −1.6 % |
| B wall | +0.4 % | ≈0 | +0.1 % | −0.1 % | ≈0 | — | — |
| C wall | −0.2 % | −0.5 % | +0.7 % | −0.2 % | **−0.75 %** § | +0.2 % | −1.2 % |
| C cycles | **+0.8 %** (knife-edge) | ≈0 | **+0.5 %** (knife-edge) | +0.2 % | −0.8 % | +0.7 % | −0.6 % |

§ = separates from 1.0 (exact CI [0.985, 0.998]) but lies inside the
W-02 A/A wall band [0.985, 1.008] applied by proxy (no W-03 A/A was run —
recorded limitation); not read as an effect.

### Attribution campaign (plain-increment control arms, 15 reps, corrected)

| Contrast | TP d1-w1 wall | TP W-03 wall † | UR d1 wall | UR W-03 cycles |
|---|---|---|---|---|
| BASE→CTRL (RMW → plain load/add/store) | +0.9 % [0.998, 1.017] — no gain | −0.9 % (n.s.) | 0.987 [0.978, 1.009] — **no separation** | 0.9916 [0.9888, 0.9974] — separates from 1.0, **overlaps A/A band** |
| CTRL→OP-ONLY (increment deleted) | **−3.2 %** | +0.1 % | +0.3 % (n.s.; instr +0.43 % [1.002, 1.007]) | +0.4 % |

† = multi-worker plain-counter control — C++ data race (undefined
behavior); the rows are retained verbatim as raw observations but are
`INVALID_FOR_CAUSAL_INFERENCE` and contribute to no causal statement in
this record (closeout revision; the TP d16-w8 attribution rows are invalid
for the same reason, and the UR control is race-free — all counter writers
hold `dispatch_mtx_`). The single-worker TP d1-w1 legs remain the valid
TP attribution evidence.

## Causal model consistent with all observations (revised per Review B)

1. **Instruction elimination (mechanism, level A): real and bounded.**
   Exactly ~1 instruction per counted event disappears: −0.022 % at the TP
   highest-frequency config (d1-w1), statistically solid (barely). This is
   the entire directly-attributable static saving; its per-event
   decomposition rests on the static R2/R3 evidence, not on this dataset.
2. **Locked-RMW execution cost (level B): no established cost for either
   backend.** TP: at the single-worker attribution config the control arm
   is indistinguishable from BASE (the multi-worker ctrl rows are
   `INVALID_FOR_CAUSAL_INFERENCE` — data race, see the attribution note)
   — consistent with the ~0.03 % physical estimate (single writer per op,
   no production reader of the line). UR: one suggestive W-03-cycles trend
   (−0.8 %, 14/15) that overlaps the A/A band, and a d1 wall trend that
   does not survive the exact CI. The honest statement is *neither
   established nor excluded at the ~0.5–1 % level for UR*; the first-draft
   claim of a real ~0.4–1 % UR lock cost is retracted as
   statistically unsupported.
3. **Code-shape effects dominate the naive contrasts
   (`CODE_SHAPE_CONFOUNDING_SUPPORTED`, not
   `CODE_LAYOUT_CAUSALITY_PROVEN`).** Deleting or adding
   a few instructions in `worker_loop` / `submit_transport_locked` shifts
   binary code layout; the resulting swings (TP d1-w1: −3.4 % for OP-ONLY
   and +3.8 % for FULL, cancelling to C ≈ +0.6 %; UR d1: +0.43 %
   instructions for OP-ONLY) are reproducible per binary pair but are
   luck-of-alignment, not semantics: they appear when the *code bytes*
   change (CTRL→OP-ONLY) and not when only the *lock prefix* is removed
   (BASE→CTRL). "Code shape" is used as a best-explanation exclusion label
   backed by the control arms and the physical bound — no specific
   instruction-alignment, branch-layout or I-cache-set mechanism is
   claimed or proven. These effects would not transfer to a
   different compiler, flag set or surrounding code, so they cannot be
   claimed as the cost of the counter.
4. **Kernel side is untouched.** Context-switch census identical (±7 of
   360 028 at UR d1), system-CPU ratios ≈ 1.0, W-01 negative control
   (byte-identical binaries across arms) centered at 1.000–1.013 with CIs
   spanning 1.0. No cache/TLB/branch PMU attribution is claimed; the
   available Haswell PMU events were not needed for the verdict and no
   unsupported attribution is manufactured.

## Materiality ladder (per the campaign brief, revised)

- **A. Mechanism improvement — YES (both backends).** Target RMW removed
  from Release assembly (proven); instruction count falls by the expected
  ~1/event.
- **B. Runtime improvement — NOT ESTABLISHED.** No separated, A/A-band-
  clearing locked-RMW cost exists for either backend under the corrected
  statistics.
- **C. Product improvement — NO (both backends).** No supported W-02/W-03
  configuration shows a material, reproducible end-to-end improvement for
  either candidate arm; TP's C at its best-frequency config is slightly
  *negative* (+0.6 % [1.000, 1.011] knife-edge slower; +1.2 % point at
  d16-w4), UR's best point (W-03 −0.75 %) sits inside the A/A band and is
  offset by +0.5–0.8 % cycles point estimates in the same arm.

## Preregistered exclusions and limitations

- **Exclusions: none.** All 15 repetitions of every campaign are included.
  Protocol deviations, all documented before or at their measurement: (1)
  the W-03 threadpool case-id fix (runner bug; the aborted first TP
  campaign contributed no measurement), (2) the runner v1→v2→v3 evolution,
  entirely inside the A/A calibration phase and before any candidate A/B
  measurement (verified from git history and provenance timestamps by
  Review B), (3) the D8 supplement's warmup=1 (disclosed in its script).
- **Limitations recorded for the owner:** no W-03 A/A calibration exists —
  W-03 contrasts are read against W-02 A/A bands by proxy (a proxy
  calibration, not a same-workload W-03 calibration), so the current data
  is **insufficient to exclude every sub-percent W-03 effect**; this is a
  limitation of the evidence, not a reason to continue optimizing or to
  productize, and the evidence still does not support `PROPOSE_UPSTREAM`
  ("no evidence of a benefit" is not a mathematical proof that the benefit
  is exactly zero). DVFS sag inside
  measured invocations is real (7–58 of the per-campaign records ended
  below 3 GHz) and is a plausible contributor to the ±2 % wall floor;
  1-minute load reached 1.9–3.0 during campaigns on a 20-CPU host; the
  control arms are themselves small code-shape perturbations; the TP
  control's plain field access is a data race (undefined behavior) at
  multi-worker configs — research-only arm, never run under the
  deterministic suite, and its multi-worker rows are
  `INVALID_FOR_CAUSAL_INFERENCE` (closeout revision: previously described
  as "benign for timing", which understated undefined behavior).

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
