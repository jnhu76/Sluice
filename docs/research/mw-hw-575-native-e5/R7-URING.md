# R7 — io_uring Native A/B

Same protocol as R6: frozen runner v3, 15 paired interleaved repetitions,
per-rep identity, built-in oracle green on all 225 + 135 measured
invocations. Raw: `raw/ab/ab-ur/ab.jsonl`, `raw/ab/ab-ur-stats/ab.jsonl`,
attribution `raw/ab/ab-tp-…`-analogous `raw/ab/ab-ur-attrib/ab.jsonl`,
D8 supplement `raw/ab/ab-ur-d8/d8.jsonl`.

Note on the issue's "D8": the frozen product-bench case list has uring
d1/d4/d16/d64 but no d8. Primary evidence therefore uses d1/d4/d16 (d4 and
d16 bracket d8 on both sides), and a dedicated supplementary run drives
exact depth-8 pipelines with the M2 harness (`scripts/run_575_d8.sh`),
varying capacity independently (8 and 1024).

## W-02 matrix (stats off) — the three contrasts (wall_s)

| Config | A: BASE→OP-ONLY | B: OP-ONLY→FULL | C: BASE→FULL |
|---|---|---|---|
| 4k d1 | 0.997 [0.977, 1.003] | 1.004 [0.989, 1.021] | 0.998 [0.983, 1.012] |
| 4k d4 | ≈1.000 (see raw) | ≈1.000 | ≈1.000 |
| 4k d16 | 1.003 [0.980, 1.008] | 1.001 [0.981, 1.027] | 1.007 [0.999, 1.018] |
| 64k d16 (neg. control) | 0.998 [0.994, 1.006] | 0.999 [0.995, 1.008] | 0.998 [0.996, 1.008] |
| W-03 d16 | 0.993 [0.979, 1.001] | 1.000 [0.986, 1.013] | **0.9925 [0.986, 0.993], 12/15 faster** |

Cycles: C at d1 +0.8 % [1.000, 1.020] and at d16 +0.5 % [1.001, 1.008]
(slower, CI excluding 1.0); W-03 C −0.8 %. Instructions: all ≤ ±0.4 %.

Stats-on runs (d1/d4/d16): same null picture (A d1 0.997, d16 1.011, d4
0.998; C d1 1.010, d4 0.995, d16 0.998 — every CI spans 1.0).

## D8 supplement (M2 harness, depth 8, 15 paired reps)

| Config | A wall | C wall |
|---|---|---|
| capacity 8, depth 8 | 1.013 (2/15 faster — slightly **slower**) | 1.002 |
| capacity 1024, depth 8 | 0.984 (11/15 faster) | 0.988 (10/15) |

The two capacity regimes point in opposite directions; both magnitudes are
inside the code-shape/A-A variance band established by the attribution
campaign. No consistent D8 effect.

## Attribution: kernel census unchanged, code shape dominates

The checklist items (SQE submission counts, `io_uring_enter` frequency,
ledger accounting, retry behavior, CQE delivery, cancellation control
processing):

- **Context-switch census is identical**: d1 A/B pairs differ by ≤ 7 of
  360 028 switches (ratio CI [0.99999, 1.00000]); `time_sys_s` ratio 1.00.
  No syscall-side reduction is claimed — none exists.
- The plain-increment control arm (`ur-ctrl`,
  `patches/ur-ctrl-plain-increment.diff`) isolates the lock semantics:
  BASE→ur-ctrl (lock removed, memory traffic kept) improves d1 wall by
  1.3 % [0.985, 0.994] and W-03 cycles by 0.8 % [0.990, 0.997] — a small
  but real execution cost of the locked RMW on the submit/poll critical
  section. However, deleting the increment outright (ur-ctrl→OP-ONLY)
  **adds** instructions at d1 (+0.43 %, 14/15 — kernel-path/code-shape
  artifact of the changed submit-path binary), which cancels the lock
  saving in the net A contrast (d1 0.997 n.s.).
- The W-03 −0.75 % (C) improvement (12/15, CI excluding 1.0) is the one
  end-to-end point estimate that separates from noise; it is accompanied by
  −1 % context switches and −2.3 % sys time for the A arm — consistent with
  a marginally shorter `dispatch_mtx_` critical section. It is, however,
  (a) at the edge of the ±2 % wall noise floor, (b) not reproduced by the
  stats-on W-03 runs, and (c) offset by the +0.5–0.8 % cycles regressions
  at W-02 d1/d16 in the same arm — so it does not amount to a material,
  configuration-robust product win.

## Verdict for the io_uring candidate

- Mechanism elimination: real (~1 instruction per flush attempt; −0.02 %
  class at the instruction metric).
- Runtime cost of the locked RMW: small and real per the control arm
  (~0.4–1 % on submit-path-dominated regimes) but **not realized
  end-to-end** by the actual deletion patches, whose code-shape side
  effects are of the same magnitude and opposite sign.
- End-to-end product effect: null across d1/d4/d16/d8(×2 capacities)/64k,
  stats off and on; one borderline W-03 improvement offset by W-02
  regressions in the same arm. No material, reproducible benefit.
