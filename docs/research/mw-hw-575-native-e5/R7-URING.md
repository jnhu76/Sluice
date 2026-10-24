# R7 — io_uring Native A/B

Same protocol as R6: frozen runner v3, 15 paired interleaved repetitions,
per-rep identity, built-in oracle green on all 225 + 135 measured
invocations. Raw: `raw/ab/ab-ur/ab.jsonl`, `raw/ab/ab-ur-stats/ab.jsonl`,
attribution `raw/ab/ab-ur-attrib/ab.jsonl`, D8 supplement
`raw/ab/ab-ur-d8/d8.jsonl`.

**Statistics note (post-Review-B revision):** all CIs are exact
percentile-bootstrap intervals (order statistics v4/v12 at n=15); see R6's
note. One first-draft claim is **retracted** under this correction: the
attribution leg "BASE→ur-ctrl improves d1 wall by 1.3 % [0.985, 0.994]"
was an artifact of the too-tight Monte Carlo CI — the exact interval is
[0.978, 1.009], which spans 1.0 (direction split 11/15, sign-test
p = 0.118). The corrected attribution statement is below.

Note on the issue's "D8": the frozen product-bench case list has uring
d1/d4/d16/d64 but no d8. Primary evidence therefore uses d1/d4/d16 (d4 and
d16 bracket d8 on both sides), and a dedicated supplementary run drives
exact depth-8 pipelines with the M2 harness (`scripts/run_575_d8.sh`,
warmup 1 — a disclosed protocol difference from the main runner's 2),
varying capacity independently (8 and 1024).

## W-02 matrix (stats off) — the three contrasts (wall_s)

| Config | A: BASE→OP-ONLY | B: OP-ONLY→FULL | C: BASE→FULL |
|---|---|---|---|
| 4k d1 | 0.997 [0.977, 1.003] | 1.004 [0.989, 1.021] | 0.998 [0.983, 1.012] |
| 4k d4 | ≈1.000 (see raw) | ≈1.000 | ≈1.000 |
| 4k d16 | 1.003 [0.980, 1.019] | 1.001 [0.981, 1.027] | 1.007 [0.999, 1.018] |
| 64k d16 (neg. control) | 0.998 [0.994, 1.006] | 0.999 [0.995, 1.008] | 0.998 [0.996, 1.008] |
| W-03 d16 | 0.993 [0.979, 1.001] | 1.000 [0.978, 1.016] | 0.9925 [0.985, 0.998], 12/15 faster |

Cycles: C at d1 +0.8 % [1.000, 1.022] and at d16 +0.5 % [1.001, 1.010]
(slower; both lower bounds barely above 1.0, reported knife-edge); W-03 C
−0.8 % [0.985, 1.000]. Instructions: all ≤ ±0.4 %.

Stats-on runs (d1/d4/d16): same null picture (A d1 0.997, d16 1.011, d4
0.998; C d1 1.010, d4 0.995, d16 0.998 — every exact CI spans 1.0).

## D8 supplement (M2 harness, depth 8, 15 paired reps)

| Config | A wall | C wall |
|---|---|---|
| capacity 8, depth 8 | 1.013 (2/15 faster — slightly **slower**) | 1.002 |
| capacity 1024, depth 8 | 0.984 (11/15 faster) | 0.988 (10/15) |

The two capacity regimes point in opposite directions; both magnitudes are
inside the code-shape/A-A variance band established by the attribution
campaign. No consistent D8 effect.

## Attribution (corrected): kernel census unchanged; lock cost unconfirmed

The checklist items (SQE submission counts, `io_uring_enter` frequency,
ledger accounting, retry behavior, CQE delivery, cancellation control
processing):

- **Context-switch census is identical**: d1 A/B pairs differ by ≤ 7 of
  360 028 switches (ratio CI [0.99999, 1.00000]); `time_sys_s` ratio 1.00.
  No syscall-side reduction is claimed — none exists.
- The plain-increment control arm (`ur-ctrl`,
  `patches/ur-ctrl-plain-increment.diff`) probes the lock semantics:
  - d1 wall: BASE→ur-ctrl median 0.987, exact CI **[0.978, 1.009] — not
    separated from 1.0** (11/15, sign-test p = 0.118). *No claim.*
  - W-03 cycles: BASE→ur-ctrl 0.9916 [0.9888, 0.9974], 14/15, separates
    from 1.0 — but **overlaps the A/A cycles band** (cal5-aa-ur d16
    cycles CI [0.9875, 1.0084], applied by proxy since no W-03 A/A was
    run), so per the campaign's reading rule this is a **suggestive,
    unconfirmed trend**, not an established lock cost.
  - Deleting the increment outright (ur-ctrl→OP-ONLY) **adds** instructions
    at d1 (+0.43 % [1.0022, 1.0070], 14/15 — kernel-path/code-shape
    artifact of the changed submit-path binary).
- The W-03 −0.75 % (C) improvement (12/15, exact CI [0.985, 0.998])
  separates from 1.0 but lies **inside the W-02 UR A/A wall band**
  ([0.985, 1.008], by proxy), and is accompanied by +0.5–0.8 % cycles
  point estimates at W-02 d1/d16 in the same arm — it does not amount to
  a material, configuration-robust product win.
- Control-arm caveats mirror R6's (the ctrl patch is itself a code-shape
  perturbation; all UR counter writers hold `dispatch_mtx_`, so the ur-ctrl
  field access is race-free, unlike the TP control).

## Verdict for the io_uring candidate

- Mechanism elimination: real (~1 instruction per flush attempt per the
  static evidence; ≤ −0.02 %-class at the instruction metric).
- Runtime cost of the locked RMW: **unconfirmed** — one suggestive
  W-03-cycles trend that overlaps the A/A band; no separated end-to-end
  leg in either direction.
- End-to-end product effect: null across d1/d4/d16/d8(×2 capacities)/64k,
  stats off and on; the single W-03 point estimate that separates from 1.0
  sits inside the A/A band and is offset by W-02 cycles point estimates in
  the same arm. No material, reproducible benefit.
