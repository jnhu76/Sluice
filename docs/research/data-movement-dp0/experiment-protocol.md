# DP-0 experiment protocol (R4)

## Frozen protocol

The protocol was written and sha256-frozen at 2026-10-23T05:23:09+08:00,
**before** the first matrix run. The frozen text, including the single
amendment recorded before the successful run, is preserved verbatim at
`results/receipts/protocol-frozen.md`:

- pre-amendment freeze sha256: `57dff165f1052f44f47d6a0cd9129a82473623e93e8537dba68ef0d28900a751`
- post-AMENDMENT-1 sha256: `0c578410025d8d8fbec886d662dd96fbe9688ec66b14c3134057fda0c018b3af`

Its substance:

- **Baseline**: master `be28f275` (post PR #569 merge). Arms `base`, `dp1`,
  `dp2`, `dp2b`, `dp3a` — single research patch each, all 76/76 tests green.
- **Matrix**: 7 cases (zop 200k, cons4k 20k, thr64 10k, write4k 20k, finfo
  20k, constp 10k after amendment, w01ctl 200k iterations) × candidate arms,
  15 interleaved AB/BA pairs per (case, arm), one process per run wrapped in
  `perf stat -x, -e instructions:u -e cycles:u -e task-clock
  -e context-switches -e cpu-migrations`.
- **Controls**: w01ctl (identical executed library code across arms) and a
  `cons4k × base` identical-binary self-pair config in every matrix.
- **Frozen decision thresholds**: REDUCED_CONFIRMED ≥ 12/15 pairs and ≥ 0.5 %
  median instruction reduction; REDUCED_HINT / NO_EFFECT / REGRESSED bands at
  0.2 %/0.5 %. Counter-validity gates (perf rc 0, instructions > 0,
  0 < IPC < 6) — zero runs were excluded in any matrix.
- **PMU honesty**: only `instructions:u` and `cycles:u` were collected
  (calibrated; see hardware-attribution.md). L1-dcache/dTLB events do not
  exist on this host; cache-references/cache-misses are host-inconsistent —
  neither was collected, and no cache claim in this study rests on a PMU
  counter.
- **AMENDMENT-1** (recorded 05:41+08:00 before the successful matrix; no
  statistics from the aborted first launch were produced or consumed): the
  constp case's wait loop had no `ctx.poll()` call, and threadpool-backend
  completion delivery requires the caller to drive the context — the first
  launch hung on its first config. Fix applied uniformly to all arms
  (harness defect, not an arm difference); constp iterations 20k → 10k for
  wall budget. Details in the frozen text.

## Matrices actually run (after the freeze)

| family | binaries | configs | runs | raw |
|---|---|---|---|---|
| main | per-arm bench TUs (`dp0b_*`) | 29 (7 cases × 4 arms + cons4k×base self-pair) | 870 | `results/raw/matrix-20261023T053456.jsonl` |
| fixed-bench-TU | one bench TU (base headers) linked against base/dp1/dp3a archives (`dp0f_*`) | 15 | 450 | `results/raw/fixedbt-20261023T054006.jsonl` |
| combo | dp1+dp2b patches applied together (`dp0b_dp1dp2b`) | 7 | 210 | `results/raw/dp0_combo_054548.jsonl` |

The fixed-bench-TU family was **added after the main matrix**, motivated by a
finding of the main matrix itself: recompiling the bench TU against a modified
header lets GCC's register allocator reshuffle unrelated loops, producing a
deterministic ±1-instruction-per-iteration artifact (fully accounted in
`results/receipts/assembly-excerpts.md` §E). dp1/dp3a change only library
`.cpp` bodies, so a bench TU compiled once against base headers links against
their archives unchanged and eliminates that artifact class for them. dp2/dp2b
change header-inlined consumer code, so their effect **includes** the
header-inline shape — the artifact cannot be split from the mechanism there,
and their instruction claims rest on the deterministic zop case plus assembly
accounting instead.

The combo family was added to satisfy the pre-registered DP1+DP2 combination
requirement; the two patches touch disjoint files (backends vs
request_core.cpp/request.hpp), `dp1dp2b` passes 76/76, and its
`bench_w01_direct` remains byte-identical to every other arm
(`b3c403c0…`).

## Binary hashes

Microbench binaries (harness source `results/receipts/bench_dp0_movement.cpp`,
same for every arm; per-arm archive link):
see `results/binary-hashes.txt`. Key library receipts:
- `bench_w01_direct` byte-identical across all six arms:
  `b3c403c036e33f912788897fe847fa53a5f46cd74ea758b72bab07fb93c04300`
- dp2b `bench_w02_pipeline` restored byte-identical after the mutation cycle:
  `4c5e08ea110ba9d09e5e908281bb596aa02a67abce72bb767bd9e318b6a41dd4`

## Interpretation and its limits

`results/analysis.md` ranks the instruments by the trust their controls
earned: assembly store accounting and the deterministic zop case carry the
verdicts; spinning-case instruction deltas are interpretable only above the
self-pair variance floor; wall/cycle medians are context, never verdicts. The
frozen thresholds are applied mechanically in `results/analysis.md` and their
demonstrated miscalibration for poll-spinning cases (self-pair control) is
recorded there rather than silently repaired.
