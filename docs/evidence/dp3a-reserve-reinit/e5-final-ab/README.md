# DP-3A final-candidate E5 validation — direct incremental A/B and full gate

Answers the one open question from the three-arm campaign: does DP-3A cause a
regression **on top of the shipped DP-1 product**, i.e. ARM = master + DP-3A
vs BASE = master, rather than the old three-arm comparison against a pre-DP-1
baseline. All raw JSONL, perf-stat output, analyzer scripts, runner scripts,
summaries and logs are in this directory; every number below is recomputable
from them with `scripts/analyze_e5_w23.py <dir>` (pair-unit bootstrap, fixed
seed 0xE5E5).

## Identity

| field | value |
|---|---|
| host | bare-metal E5, Fedora 44, kernel 7.2.9-201.fc44.x86_64, Xeon E5-2666 v3 |
| compiler | GCC 16.2.1 20260819 (Red Hat 16.2.1-2), liburing 2.13 |
| BASE tree | master `4221c7e3` (includes DP-1 via #572 and #571 bench fixes) |
| ARM tree | PR #573 rebased head `da602ccc` (only the 7 DP-3A files vs BASE) |
| build | clean per-target release builds, `--liburing=y`, no sanitizers |
| binaries | `bench_w02_pipeline` base `913ce2ee…`, arm `e07a1bd2…` (sha256 in each campaign log; rebuilt byte-identical after the gate) |
| protocol | interleaved AB/BA pairs, `taskset -c 2` unless stated, 1 warmup + 3 measured reps per invocation, `SLUICE_BENCH_STATS=0`, perf stat around every invocation |
| metric | per-invocation median of measured wall_s/ops (and cpu_s/ops), paired arm/base ratios, median + bootstrap 95% CI over 15 pairs |

## Preregistered acceptance (fixed before the first measurement)

Primary = wall ns/op paired-ratio median; secondary = cpu ns/op.

- A (pass): median CI includes or is below 1.0 AND deterministic instructions/op not increased.
- B (`ACCEPT_WITH_MEASURED_SMALL_REGRESSION`): median ≤ 1.03 and ≤ 10/15 pairs above 1.03 (standing H2b engineering bound).
- C (`NO_GO`): median > 1.03 or > 10/15 pairs above 1.03.
- D (`INCONCLUSIVE`): control noise of the same magnitude as the signal, or conflicting batches.

## Stage 1 — W02-uring-4k-d1 arbitration (`stage1-d1/`)

| comparison | n | wall median [CI95] | cpu median [CI95] |
|---|---|---|---|
| m1 ARM vs BASE | 15 | **1.00142 [0.99349, 1.00539]** | 1.00109 [0.99380, 1.00571] |
| m2 BASE vs BASE control | 15 | 1.00030 [0.98254, 1.01128] | 1.00064 [0.99271, 1.00945] |
| m3 zop instrument, instr/op delta | 15 | **−20.9 (15/15 pairs arm removes)** | cyc/op −3.5 |
| m3 w01ctl negative control | 15 | −0.00002 instr/op (5/15 positive) | — |

Verdict: **Case A.** The +0.7 % (CI entirely above 1) residual from the old
three-arm campaign does not exist against the true incremental baseline: the
direct incremental effect is +0.14 % with the CI spanning 1, an order of
magnitude below this round's base-vs-base noise floor. 0/15 pairs exceed the
1.03 engineering bound. Deterministic work strictly decreases (zop −20.9
instr/op; whole-process perf on m1 pair 0: −192 instr/op, −1128 cyc/op).

## Stage 2 — remaining W-02/W-03 shapes, stats, battery, mutants (`gate/`, `w03tp-confirmation/`, `w01-control/`)

W-02 (pinned, stats off) and W-03 (unpinned — the fixed stop-order bench is
the arbiter; affinity recorded as unpinned for the perf numbers):

| shape | n | wall median [CI95] |
|---|---|---|
| W02-uring-4k-d16 | 15 | 0.98560 [0.97963, 1.01762] |
| W02-uring-64k-d16 | 15 | 0.99977 [0.99303, 1.00752] |
| W02-threadpool-4k-d16-w4 | 15 | 0.99936 [0.98714, 1.02309] |
| W03-uring-4k-d4 | 15 | 1.00193 [0.92342, 1.05961] |
| W03-uring-4k-d16 | 15 | 1.00124 [0.98037, 1.02484] |
| W03-threadpool-4k-d16-w4, first sample | 15 | **1.06310 [1.01568, 1.11903]** |
| W03-threadpool-4k-d16-w4, confirmation unpinned | 15 | **0.94166 [0.90591, 0.99043]** |
| W03-threadpool-4k-d16-w4, confirmation pinned | 15 | 0.99757 [0.98982, 1.00110] |
| W03-threadpool-4k-d16-w4, base vs base unpinned | 15 | 0.99860 [0.98134, 1.02370] |
| W01 negative control (arm binaries byte-identical) | 8 | 1.00076 [0.96768, 1.09862] |

The single first-sample outlier (+6.3 % unpinned W03-threadpool) was treated
as a signal, not noise: the confirmation round re-ran it three ways and it
not only failed to repeat but flipped sign (−5.8 %, CI below 1, unpinned;
exactly neutral pinned; shape noise floor ±2 %). A mechanism-specific
regression cannot alternate +6 %/−6 % between interleaved batches while every
other shape — including the most reserve-sensitive d1 — stays neutral. This
matches the old campaign, where the same shape also produced a non-repeating
+6 % first sample. W03-threadpool is arm-neutral; both outlier samples are
disclosed here as measured.

Stats-on pollmetrics (5 pairs, `SLUICE_BENCH_STATS=1`): poll/op 1.5007 vs
1.4848 (d1) and 0.1593 vs 0.1598 (d16) — timing-dependent coalescing counts,
arm-independent within noise; 20000 requests fully accounted
(submits/waits/completions) in every record.

Regression battery on the ARM tree (E5, GCC 16): release+liburing **78/78**,
debug+liburing **78/78** (includes the initialization-closure oracle).

Mutants on GCC 16: all six named DP-3A mutants killed with signatures
identical to WSL2/GCC 15 (`mutant-signatures/`, failure counts 28/1/1/3/9/8,
exit 255; rollback and exhausted-reuse mutants end in the teardown fail-fast
by design).

Machine code (`../reserve.e5.objdump.txt`): in the measured release archive
(GCC 16.2.1), ARM `RequestCore::reserve()`'s pop path keeps exactly one slot
store — `movb $0x1,(%rax)` (phase = reserved) — and zero zeroing stores;
BASE retains the full zero-store sequence (`pxor` + `movups`/scalar), with
the saturated-generation branch at 0xa0 (ARM) vs 0x120 (BASE) bytes into the
function. Binary identity across stage-1 measurement, stage-2 gate and the
post-gate rebuild is byte-identical (same sha256), so the disassembled code
is the measured code.

## Disclosures

- The first W-01 attempt used an exact-match case filter (`=W01`) that matches
  no case id; it produced empty runs and is not committed. Re-run with the
  substring filter (`W01`) as `w01-control/` (`scripts/e5_w01_control.zsh`).
- Stage-2 helper defects: the s6 mutant step and the s7 objdump step ran
  outside the worktree / against a stripped binary and produced empty output;
  both were re-executed correctly (mutants above; `../reserve.e5.objdump.txt`).
  The s5 battery and all measurements used correct paths and are unaffected.
- W-03 gate numbers are unpinned by design (W-02/W-01 and both negative
  controls are CPU2-pinned); the pinned confirmation row isolates affinity
  sensitivity for the one outlier shape.
