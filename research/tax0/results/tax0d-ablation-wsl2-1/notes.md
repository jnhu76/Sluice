# TAX-0D ablation session notes — tax0d-ablation-wsl2-1

Causal A/B session for F01 and F02-A (one mechanism, one A/B each).
Same protocol as the canonical ladder session (double-difference
R7/R14, `:u` counters, runner-side write verification); binary is
`tax0_ablation_bench` (the SAME harness linked against
`sluice_async_internal_testing`) with R1 modes installed via CLI:
`--f01-r1` gates `backend_->outstanding()` on stats presence;
`--f02-r1` skips the ordinary reap-sequence stamp. Variants: `r0`
(same binary, seams at default = production behavior) vs exactly one
R1 mode per run.

## Results (instructions/op, z2 = AsyncIoContext driver, z3w1 = runtime)

F01 (stats disabled, R1 skips the outstanding() evaluation):

| cell | arm | R0 | R1 | delta |
| --- | --- | --- | --- | --- |
| 4K d1 read | z2 | 3992 | 3917 | −75 |
| 4K d32 read | z2 | 3245 | 3170 | −75 |
| 4K d64 read | z2 | 3343 | 3268 | −75 |
| 4K d32 read | z3w1 | 4126 | 4049 | −77 |
| 4K d1 write | z2 | 4295 | 4220 | −75 |
| 4K d32 write | z2 | 2989 | 2915 | −74 |
| 64K d8 read | z2 | 16688 | 16613 | −75 |

Consistent −74..−77 instr/op across every measured cell and both arms
(≈2.3–2.5% of the z2 per-op cost). Session-to-session dispersion of the
production binary at read 4K d32 z2 was 3112/3112/3121 across formal2/
formal3/formal4 re-runs, so the effect is far outside variance.
Wall/op is neutral within noise (read 4K d32: 1222→1195 ns; write
4K d32: 2262→2410 ns) — a CPU/control-plane improvement, NOT a
throughput claim. VERDICT F01: PROVEN TAX (magnitude ≈ 75 instr/op).

F02-A (ordinary publications skip the process-global reap-seq stamp):

Deltas −4..−2 instr/op on read cells; write cells straddle zero
(4K d32 z2: R0 2989 vs R1 3012, +23). VERDICT F02-A: NEGLIGIBLE in
these cells. F02-B (seq_cst vs relaxed memory-order) was NOT run: per
the preregistered gate, F02-B is only worth testing if F02-A proves the
atomic materially hot, which it does not. The frozen comment/code
observation (comment says relaxed sufficient, `++` is seq_cst) remains
a documentation-level fact, unmeasured for contention under multi-worker
reap.

## Caveats

- The ablation binary is the internal_testing variant; its R0 numbers
  read ~4% above the production binary at the same cells (e.g. 3245 vs
  3112 at read 4K d32) — a build-composition codegen shift, which is why
  every A/B is internal-R0 vs internal-R1 (same build), never
  internal-R1 vs production.
- The F02-R1 seam build is not Batch-safe by design (research-only);
  the harness never uses Batch.
- z3w4 was excluded from ablation cells (unstable under perf wrapping;
  see formal4 notes) — F01/F02 effects under multi-worker reap are
  UNKNOWN.
