# T0-U-ROUTER — SHOOTOUT RE-FREEZE A2 (Layer A instrument correction only)

Campaign: TAX-0 / T0-U-ROUTER router fix-candidate shootout (#255),
branch `research/tax0-router-fix-shootout`, PR #256.
Trigger: corrective review of PR #256 (review ID 5063072823).
This document re-freezes ONLY the Layer A instrument and the evidence
tooling. The original freeze (d45f620, recorded in §5 of
`TAX0-ROUTER-FIX-SELECTION.md`) remains closed and unedited.

## R.1 Defects corrected (all found by the corrective review; none change
any candidate, backend, or production semantics)

| # | defect | fix |
| --- | --- | --- |
| D1 | Layer A microbench allocated per measured window (`std::vector order` constructed per window; P1 permutation returned a fresh vector; P0/P2 `resize`), while the artifact hardcoded `steady_allocations_per_op: 0` | completion-order buffer hoisted out of the window loop; `window_permutation` fills a caller-owned buffer in place; every replaceable global allocation function is counted (full `operator new`/`delete` surface incl. aligned + nothrow, per [replacement.functions]); the trace runs between two counter observations; nonzero count fails the run (exit 3). The artifact now records the OBSERVED count. Gate proven live: a canary build with one deliberate in-trace allocation fails with `steady-state allocation inside the measured trace`. |
| D2 | Layer A `--seed` parser stopped at the `x` of `0x52545253`, so the effective P1 permutation seed was 0, not the frozen campaign seed | parser accepts an optional `0x` prefix and fail-fasts on any non-hex suffix. History: in ALL prior Layer A runs (incl. the superseded first run and the aa8dff4 official run) every candidate consumed an identical seed-0-derived permutation — fairness, determinism and candidate-comparability are intact (the seed is shared by definition); only the recorded-vs-effective seed mismatch is a defect. Layer B is unaffected: its bench takes no `--seed` and its trace does not consume one. |
| D3 | Validator sealing hole: cell sets / candidates / sessions were checked for internal consistency only; an artifact could delete an ugly cell, candidate, or whole session and resync rows + derived | validator now embeds the externally frozen matrix (`FROZEN_MICRO`, `FROZEN_SHOOTOUT`, `FROZEN_SESSIONS`) and requires each artifact's declared cell set to EQUAL it (missing AND extra cells fail), pins the row-side cell count, and seals the exact four fs×op sessions (dropped / duplicated / unknown sessions fail). New mutations in `--self-test`: `drop-cell-synced`, `drop-candidate-synced` (both layers), `drop-session`, `unknown-session`, `micro-tamper-alloc`. |
| D4 | Micro validator recomputed instruction medians/normalized/GM only; cycles derived were recorded but never cross-checked | micro medians, normalized ratios and GMs are now recomputed and cross-checked for BOTH axes; new mutations `micro-tamper-gm-cycles`, `micro-tamper-normalized-cycles`. |
| D5 | §25 selector bug: `cycles_ok = best.gm_cycles >= best_cycles.gm_cycles - TIE` is satisfied by almost anything (direction inverted) | fixed to `best.gm_cycles <= best_cycles.gm_cycles + TIE` (instruction winner may not lose more than the tie band to the cycles winner). Tail symmetry now checked on BOTH axes in the outright-winner path AND the practical-tie set (worst-cell cycles, not just worst-cell instructions). The aa8dff4 verdict is unaffected: nobody led the GM by ≥2%, so selection went through the practical-tie branch; the corrected recompute (this round) confirms it. |
| D6 | Report wording overreach + provenance gap | §20/§21 of `TAX0-ROUTER-FIX-SELECTION.md` re-worded (guardrail phrasing instead of "noise"; "same direction reproduces across the tested sessions" instead of op-/filesystem-independence) and an explicit provenance statement added (candidate binary unchanged, measurement parameters unchanged, Layer B raw artifacts unchanged through tooling corrections). |

## R.2 Frozen parameters — UNCHANGED from the original freeze

Layer A (`tax0routermicro`): candidates `r0,r1,r2,r3`; patterns `P0,P1,P2`
(P2 only D==C cells); geometries `8:8, 8:32, 8:128, 8:512, 32:32, 32:128,
32:512, 128:128, 128:512` (84 cells); windows 20000; reps 9; seed
`0x52545253`; taskset `0,2,4,6`; perf events instructions:u, cycles:u,
branches:u, branch-misses:u, cache-misses:u; blocked randomized rounds;
validator recomputes the order.

Layer B (`tax0routershootout`): NOT re-frozen, NOT re-measured. The four
official sessions (`read|write × tmpfs|btrfs`, 7 geometries, 4 KiB
requests, 128 MiB, reps 9, warmup 2, Q==D) and their raw artifacts are
untouched; the strengthened validator re-validates them as-is.

## R.3 Files in this re-freeze

- `bench/tax0router_micro_bench.cpp` — D1, D2 (bench_version 1 → 2)
- `scripts/bench/perf-attribution.py` — D1 (measured allocation fields,
  row-side gate, derived propagation)
- `scripts/bench/tax0router-validate.py` — D3, D4, D5
- `scripts/bench/perf-evidence-validate.py` — structural row check for the
  new micro field
- `research/tax0/TAX0-ROUTER-REFREEZE-A2.md` — this document

Correctness gates at re-freeze: `uring_router_fix_equivalence_test` PASS,
`uring_router_fix_death_test` PASS (local run, this commit). Validator
`--self-test`: 18/18 mutations rejected + aggregate / multi-session /
duplicate-session / drop-session / unknown-session checks OK.

## R.4 Consequences

- Layer A official artifact `tax0router-fix-micro.json` is re-measured
  under this re-freeze and SUPERSEDES the aa8dff4 artifact (parameters
  identical; instrument corrected). The supersede chain is recorded in
  the artifact note and the selection report.
- Layer B verdict authority is unchanged; the campaign verdict is
  recomputed from (new Layer A) + (unchanged Layer B) by the sealed
  validator and quoted in the selection report.
