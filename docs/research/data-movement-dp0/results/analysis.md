# DP-0 R4 results analysis

Raw records: `raw/matrix-20261023T053456.jsonl` (main family, 870 runs),
`raw/fixedbt-20261023T054006.jsonl` (fixed-bench-TU family, 450 runs),
`raw/dp0_combo_054548.jsonl` (combo family, 210 runs; per-config values
preserved in `raw/combo-stdout.log`, no separate summary JSONL). Summaries
for the first two families are stored beside their raw files.
Zero runs were excluded by the validity gates in any matrix.

## Instruments, in order of trustworthiness established by the controls

1. **Assembly/GIMPLE store accounting** (exact, library-only): the primary
   quantitative instrument for "did physical data movement shrink".
   Receipts: `receipts/assembly-excerpts.md`.
2. **zop microbench case** (deterministic single-poll uring D1 loop):
   run-to-run instruction spread of an identical binary is ~1e-6 relative or
   below in the preserved matrix runs (e.g. 30 base w01ctl runs within
   28 130 921…28 131 009; see the calibration receipt). This case resolves
   single-instruction-level deltas and is used to validate the
   per-candidate predictions.
3. **w01ctl negative control** (blocking read loop, identical executed library
   code in all arms): exact zero for dp1/dp2/dp3a; exactly +1 instruction per
   iteration for dp2b, fully accounted by a bench-TU register-allocation
   artifact (`receipts/assembly-excerpts.md` §E).
4. **Spinning uring/threadpool cases** (cons4k, thr64, write4k, finfo,
   constp): their `while(!ready) poll()` loops iterate a kernel-timing-dependent
   number of times, so per-op user instruction/cycle counts carry a variance
   floor **demonstrated by the identical-binary self-pair control**
   (`cons4k × base`): median −5.4 % with 6/15 pair balance in the main matrix
   and −0.7 % with 6/9 balance in the fixedbt matrix. Deltas at or below this
   floor are not interpretable; deltas above it with strong pair balance are
   interpretable as (mostly second-order) poll-iteration reductions.

## zop deterministic validation (per-op user instructions, base − arm)

| arm     | measured median | pairs arm<base | assembly-predicted | matches |
|---|---|---|---|---|
| dp1     | 27.5 instr (0.860 %, 15/15) | 15/15 | ~27 (12-move payload copy block + caller 120 B init + arg setup) | yes |
| dp2     | −12.1 instr (−0.379 %, **0/15**) | 0/15 | ≥0, likely slightly negative (retains 72 B init + flag/cmov overhead, saves only the 3-instruction metadata copy) | yes (direction) |
| dp2b    | 8.8 instr (0.275 %, 14/15) | 14/15 | ~9–12 (72 B `RequestResultData` local init + metadata copies minus split-call overhead) | yes |
| dp3a    | 16.5 instr (0.516 %, 15/15) | 15/15 | ~13–16 (14 stores → 1 in reserve) | yes |
| dp1dp2b | 36.30 instr (15/15) | 15/15 | 27.5 + 8.8 = 36.3 (additive, exact) | yes |

The combo measurement (36.30) equals the sum of the individual arms (36.3)
exactly — the two mechanisms are independent and additive on
the measured path.

## Spinning-case instruction deltas (interpret with the variance floor)

Fixed-bench-TU family (bench TU compiled once against base headers, so the
bench-TU artifact class of §E is eliminated for dp1/dp3a):

| case | dp1 median | pairs | dp3a median | pairs | self-pair floor |
|---|---|---|---|---|---|
| cons4k | +6.18 % | 12/3 | +1.09 % | 8/7 | −0.73 % (6/9) |
| thr64 | +6.78 % | 11/4 | +0.38 % | 9/6 | — |
| write4k | +6.18 % | 10/5 | +6.48 % | 9/6 | — |
| finfo | +4.40 % | 11/4 | +1.14 % | 11/4 | — |
| constp | +3.22 % | 13/2 | +1.34 % | 9/6 | — |
| zop | +0.860 % | 15/0 | +0.516 % | 15/0 | exact |
| w01ctl | 0.000 % | 8/6 | −0.000 % | 2/13 | exact control |

dp1 is consistently positive on every spinning case (fixedbt balances
10–13/15; main matrix: thr64 +7.38 % 12/15, write4k +11.90 % 11/15,
constp +3.94 % 11/15, finfo +4.10 % 8/15, cons4k +0.92 % 9/15). This is the
second-order effect: removing ~27 instructions of publication work shifts
completion visibility earlier relative to the consumer's poll loop, so the
consumer often exits the poll loop one iteration sooner (one poll iteration is
worth hundreds–thousands of instructions, hence percentages far larger than
the direct 0.3–0.9 % direct saving).

dp3a on spinning cases is weak-positive with mixed balances (8/7…11/4) — at
or slightly above the self-pair floor; only the zop measurement resolves it
cleanly.

Combo (main-family binaries, artifact applies): zop +36.3 (15/15, exact);
finfo +8432 instr/op (11/15) and constp +9312 instr/op (11/15) strongly
positive; cons4k/thr64 positive at 8/15; write4k −2308 (6/15) — within the
floor, not interpretable. w01ctl −1.1 instr/op 0/15 — the same +1
instruction/iteration artifact class as dp2b alone, bounded and understood.

## Frozen-rule mechanical application, and its demonstrated miscalibration

Applying the frozen thresholds mechanically to the main matrix would mark,
e.g., thr64×dp1 REDUCED_CONFIRMED but would also mark the `cons4k × base`
SELF-PAIR (identical binary) as "REGRESSED" (−5.4 % median). The rule's noise
assumption is therefore invalidated for poll-spinning cases by its own
control, and no verdict in this study rests on spinning-case instruction
deltas alone. The verdicts in `../decision.md` rest on the assembly store
accounting plus the deterministic zop instrument, with spinning-case data
recorded as second-order indicatives.

## Wall-time and cycles

Per-op cycles and wall medians are reported in the raw summaries; on this host
their noise (scheduler, frequency, kernel completion timing) exceeds the
direct per-op deltas, and they are not used as verdict evidence. No
config shows a wall regression exceeding its own noise floor for the accepted
candidates; dp2's zop wall delta (−4.8 ns median, i.e. arm slightly faster)
is within noise and does not rescue its instruction regression.

## Correctness oracles (all arms)

- 76/76 conformance+unit tests pass on every arm and the combo, before and
  after every mutation/restore cycle.
- W-01 negative control binary `bench_w01_direct` byte-identical
  (sha256 b3c403c0…) across base, dp1, dp2, dp2b, dp3a, dp1dp2b.
- Mutation oracles: wrong metadata-skip is caught at runtime in the dp2 shape
  (4 assertions × 2 conformance binaries fail;
  `receipts/m1-dp2-mutation-failures.txt`) and at compile time in the dp2b
  shape (`receipts/m1-dp2b-mutation-summary.txt`).
