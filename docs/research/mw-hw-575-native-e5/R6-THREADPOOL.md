# R6 — ThreadPool Native A/B

All runs: E5 native, frozen runner v3 (`scripts/run_575_ab.sh`), 15 paired
interleaved repetitions per decision-bearing configuration, per-rep binary
SHA recorded, built-in byte oracle green on every one of the 270 + 135
measured invocations. Raw rows: `raw/ab/ab-tp/ab.jsonl` (stats off),
`raw/ab/ab-tp-stats/ab.jsonl` (stats on), attribution campaign
`raw/ab/ab-tp-attrib/ab.jsonl`. A/A noise floors: R5.

## W-02 matrix (stats off) — the three contrasts

Median ratio [bootstrap CI95] / direction, wall_s:

| Config | A: BASE→OP-ONLY | B: OP-ONLY→FULL | C: BASE→FULL |
|---|---|---|---|
| 4k d1-w1 | **0.966 [0.958, 0.971], 15/15 faster** | **1.038 [1.029, 1.047], 15/15 slower** | 1.006 [1.000, 1.011], 12/15 slower |
| 4k d4-w4 | 1.010 [0.993, 1.010] | 0.998 [0.981, 1.013] | 1.008 [0.990, 1.009] |
| 4k d16-w4 | 1.004 [0.995, 1.011] | ≈1.000 | ≈1.000 |
| 4k d16-w8 | 1.008 [1.004, 1.013], 14/15 slower | 0.995 [0.993, 1.001] | 1.002 [1.000, 1.014] |
| 64k d16-w4 (neg. control) | 0.997 [0.993, 1.022] | ≈1.000 | ≈1.000 |
| W-03 d16-w4 | 0.998 [0.978, 1.012] | ≈1.000 | ≈1.000 |

Instructions (the mechanism metric): d1-w1 A 0.99978 [0.99968, 0.99985]
(a statistically solid **−0.022 %**, ≈ one instruction eliminated per
physical syscall, exactly the expected code elimination); all other configs
within ±0.7 % multi-worker noise. User/system CPU: no contrast separates
from the A/A band anywhere except the d1-w1 A/B wall swings analyzed below.

Stats-on headline runs (d1-w1, d4-w4, d16-w8) reproduce the same pattern
(A d1-w1 wall 0.972 [0.959, 0.977]; C 1.000 [0.999, 1.002]) — stats wiring
does not change the picture.

## The d1-w1 ±3–4 % swings are code-shape artifacts, not RMW cost

The A/B contrasts at d1-w1 show a −3.4 % "win" for OP-ONLY and a +3.8 %
"loss" for FULL — magnitudes impossible for a single uncontended relaxed
RMW per syscall (physical estimate ≈ 20–40 cycles ≈ 0.03 % of the ~9 600
cycles/op pipeline). The dedicated attribution campaign
(`patches/tp-ctrl-plain-increment.diff`, arm `tp-ctrl`: the
`fetch_add` replaced by a non-atomic load+add+store on the **same field**,
same memory traffic, no `lock` prefix, near-identical code size) decides
the question:

| Contrast (d1-w1, wall) | Median [CI95] | Reading |
|---|---|---|
| BASE → tp-ctrl (lock semantics removed, memory kept) | 1.009 [0.998, 1.017] — **no gain** | the locked RMW costs nothing measurable |
| tp-ctrl → OP-ONLY (increment code deleted) | 0.968 [0.951, 0.972], 14/15 | the −3.4 % appears only when the instructions are deleted |

**Conclusion:** the −3.4 % at d1-w1 is an artifact of deleting ~7 code bytes
from `worker_loop` (code-size/alignment/branch-layout luck of these specific
binaries), not of eliminating synchronization. Symmetrically the +3.8 % for
FULL (B) is the layout luck swinging the other way (class-size change shifts
allocation/alignment), and C — the actual product candidate — lands at
+0.6 % slower [1.000, 1.011]: **neutral to slightly negative**. At d16-w8
the deletion artifact points the other way (+0.8 % slower, 14/15), and
tp-ctrl confirms the lock itself is again ≈ free (wall 1.000, cycles 1.010
n.s.).

## W-03 external loop

Notification parity, no lost wakeups, correct progress-owner behavior and
drain/shutdown order are covered by the deterministic suite (78/78 on every
arm, R4) and the byte oracle inside every W-03 benchmark repetition. No W-03
wall/cycles/instruction contrast separates from the A/A band for either
candidate arm (A 0.998, C ≈ 1.000; sys-time and context-switch counts
unchanged).

## Verdict for the ThreadPool candidate

- Mechanism (instruction) elimination: real, ≈ 1 instruction per physical
  syscall (−0.02 % at the highest relative-frequency config).
- Runtime (lock/coherence) cost of the RMW: **below measurement resolution**
  at every config (tp-ctrl indistinguishable from BASE).
- End-to-end product effect of the actual patches: none; the measured swings
  are binary-specific code-shape luck (−3.4 % … +3.8 % at one config,
  cancelling to C ≈ +0.6 %), and no supported W-02/W-03 configuration
  improves materially.
