# R6 — ThreadPool Native A/B

All runs: E5 native, frozen runner v3 (`scripts/run_575_ab.sh`), 15 paired
interleaved repetitions per decision-bearing configuration, per-rep binary
SHA recorded, built-in byte oracle green on every one of the 270 + 135
measured invocations. Raw rows: `raw/ab/ab-tp/ab.jsonl` (stats off),
`raw/ab/ab-tp-stats/ab.jsonl` (stats on), attribution campaign
`raw/ab/ab-tp-attrib/ab.jsonl`. A/A noise floors: R5.

**Statistics note (post-Review-B revision):** all confidence intervals in
this record are *exact* percentile-bootstrap intervals for the median
(order statistics v4/v12 at n=15). The first published record used a Monte
Carlo bootstrap whose endpoints landed one order statistic too tight in
~36–38 % of cases at n=15; every CI below has been recomputed with the
exact form (see `scripts/reduce_575.py`, self-tested). No TP conclusion
changes under the correction.

## W-02 matrix (stats off) — the three contrasts

Median ratio [exact CI95] / direction, wall_s:

| Config | A: BASE→OP-ONLY | B: OP-ONLY→FULL | C: BASE→FULL |
|---|---|---|---|
| 4k d1-w1 | **0.966 [0.958, 0.974], 15/15 faster** | **1.038 [1.029, 1.047], 15/15 slower** | 1.006 [1.000, 1.011], 12/15 slower |
| 4k d4-w4 | 1.010 [0.989, 1.029] | 0.998 [0.981, 1.013] | 1.008 [0.990, 1.009] |
| 4k d16-w4 | 1.004 [0.995, 1.011] | ≈1.000 [0.98, 1.02] | **1.012** [0.993, 1.021] |
| 4k d16-w8 | 1.008 [1.004, 1.015], 14/15 slower | 0.995 [0.985, 1.001] | 1.002 [0.999, 1.014] |
| 64k d16-w4 (neg. control) | 0.997 [0.993, 1.022] | ≈1.000 | 1.001 [0.991, 1.024] |
| W-03 d16-w4 | 0.998 [0.978, 1.012] | 1.004 [0.975, 1.008] | 0.990 [0.978, 1.015] |

No cell except the two d1-w1 swing cells and the d16-w8 A cell separates
from 1.0 (d1-w1 C's lower bound is 1.0004 — separated by less than one
order statistic, reported as knife-edge). C at d16-w4 is a +1.2 % slower
point estimate (CI spans 1.0), not "zero": printed as measured.

Instructions (the mechanism metric): d1-w1 A 0.99978 [0.99968, 0.99985]
(a statistically solid **−0.022 %**; the absolute median elimination is
1.18 M instructions per invocation, ≈ 59 per request over the ~60 000
ops of warmup+measured+fixture work — the exact per-event decomposition
(≈ 1 eliminated instruction per physical syscall plus kernel-side
variance) is established by the static R2/R3 evidence, not derivable from
this A/B dataset alone; the separation clears the A/A instruction band
lower bound (0.99987) by 0.00002 and is reported as barely separated).
All other configs within ±0.7 % multi-worker instruction noise.
User/system CPU: no contrast separates from the A/A band anywhere except
the d1-w1 A/B wall swings analyzed below.

Stats-on headline runs (d1-w1, d4-w4, d16-w8) reproduce the same pattern
(A d1-w1 wall 0.972 [0.959, 0.977]; C 1.000 [0.999, 1.002]) — stats wiring
does not change the picture.

## The d1-w1 ±3–4 % swings are code-shape artifacts, not RMW cost

The A/B contrasts at d1-w1 show a −3.4 % "win" for OP-ONLY and a +3.8 %
"loss" for FULL — magnitudes impossible for a single uncontended relaxed
RMW per syscall (physical estimate ≈ 20–40 cycles ≈ 0.03 % of the ~9 600
cycles/op pipeline). The dedicated attribution campaign
(`patches/tp-ctrl-plain-increment.diff`, arm `tp-ctrl`: the
`fetch_add` replaced by a non-atomic load+add+store on the **same field**)
decides the question:

| Contrast (d1-w1, wall) | Median [exact CI95] | Reading |
|---|---|---|
| BASE → tp-ctrl (lock-prefixed RMW replaced by plain load/add/store) | 1.009 [0.998, 1.017] — no gain | no evidence of a lock-semantics cost |
| tp-ctrl → OP-ONLY (increment code deleted) | 0.968 [0.951, 0.972], 14/15 | the −3.4 % appears only when the instructions are deleted |

**Conclusion (`CODE_SHAPE_CONFOUNDING_SUPPORTED`, not
`CODE_LAYOUT_CAUSALITY_PROVEN`):** the −3.4 % at d1-w1 is a code-shape
artifact of changing the code bytes in `worker_loop` (the CTRL→OP-ONLY leg
deletes the ~7-byte increment encoding), not of eliminating
synchronization — it appears when the code bytes change and not when only
the lock prefix is removed (BASE→CTRL). No specific instruction-alignment,
branch-layout or I-cache mechanism is claimed or proven; "code shape" is
the best-explanation exclusion label backed by the control arms, the
instruction metric and the physical bound (R8 §3). Symmetrically the
+3.8 % for FULL (B) is the same binary-pair luck swinging the other way
(class-size change shifts allocation/alignment), and C — the actual
product candidate — lands at +0.6 % slower [1.000, 1.011]: **neutral to
slightly negative**. At d16-w8 the deletion artifact points the other way
(+0.8 % slower, 14/15), and tp-ctrl is again ≈ BASE (wall 1.000, cycles
1.010 n.s. — a multi-worker row, observation only,
`INVALID_FOR_CAUSAL_INFERENCE` per the caveat below).

Control-arm caveats (per Review B P2-4, revised at closeout): the ctrl
patch is itself a small code-shape perturbation (different instruction
sequence/encoding length than `lock add`), so BASE→CTRL is another
binary-pair comparison, not a pure lock-semantics isolation. At
multi-worker configs (d16-w8, W-03 d16-w4) the ctrl arm's plain
load/store on the shared field is a C++ **data race — undefined
behavior**, not a benign timing artifact: those rows are retained verbatim
as raw observations but are `INVALID_FOR_CAUSAL_INFERENCE` and are
excluded from every causal statement in this record (the ctrl arm is
research-only and never ran the deterministic suite; the UR control is
race-free — all its counter writers hold `dispatch_mtx_`, see R7). The
single-worker attribution legs (d1-w1) carry the causal reading. The ctrl
conclusion is therefore read as: *no evidence* of a lock cost at TP,
consistent with the 0.03 % physical prior — not as a proof of exactly
zero.

## W-03 external loop

Notification parity, no lost wakeups, correct progress-owner behavior and
drain/shutdown order are covered by the deterministic suite (all green on
every arm, R4) and the byte oracle inside every W-03 benchmark repetition.
No W-03 wall/cycles/instruction contrast separates from the A/A band for
either candidate arm (A 0.998, C 0.990 [0.978, 1.015]; sys-time and
context-switch counts unchanged). Note (Review B, restated at closeout):
no dedicated W-03 A/A calibration was run; the A/A bands applied to W-03
contrasts are the W-02 bands of the same backend **by proxy — a proxy
calibration, not a same-workload W-03 calibration** — recorded as a
limitation in R8. The current data is therefore **insufficient to exclude
every sub-percent W-03 effect**; this insufficiency is a limitation of the
evidence, not a reason to continue optimizing or to productize, and it
does not change that no W-03 contrast passed the campaign's effect rule —
the evidence still does not support `PROPOSE_UPSTREAM`. "No evidence of a
benefit" here and below means exactly that: not established by this data;
it is **not** a mathematical proof that the benefit is exactly zero.

## Verdict for the ThreadPool candidate

- Mechanism (instruction) elimination: real, ~1 instruction per physical
  syscall (−0.02 % at the highest relative-frequency config; per-event
  attribution per R2/R3 static evidence).
- Runtime (lock/coherence) cost of the RMW: **no evidence of a measurable
  cost** (tp-ctrl indistinguishable from BASE at the single-worker
  attribution config d1-w1; the multi-worker ctrl rows are
  `INVALID_FOR_CAUSAL_INFERENCE` per the data-race caveat above;
  consistent with the 0.03 % physical estimate).
- End-to-end product effect of the actual patches: none; the measured swings
  are binary-specific code-shape luck (−3.4 % … +3.8 % at one config,
  cancelling to C ≈ +0.6 % [1.000, 1.011] knife-edge slower), and no
  supported W-02/W-03 configuration improves materially.
