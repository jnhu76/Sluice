# #575 — Native E5 Atomic RMW Elimination Research

Research campaign for [issue #575](https://github.com/jnhu76/Sluice/issues/575)
(parent #563): whether the two test-only atomic read-modify-write counters
`ThreadPoolBackend::syscall_count_` and `UringAsyncBackend::submit_flushes_`
impose unnecessary cost on production I/O hot paths, whether they can be
removed from Release without losing semantics, and whether removal produces
material end-to-end benefit.

- Baseline: `3df10dc44d84799cccb18d29f20149d2983137ec` (v0.0.3 master)
- Execution: E5 native bare metal, GCC 16.2.1, xmake 3.1.2, liburing 2.13
  (R0); research worktrees under `/tmp/sluice-575/`
- Verdict: see `VERDICT.md` (both backends `NO_CHANGE_JUSTIFIED` on the
  performance axis; semantic findings recorded for the owner)

## Document index

| File | Content |
|---|---|
| `R0-IDENTITY.md` | Environment verification, baseline freeze, boundaries |
| `R1-AUTHORITY-AND-DAG.md` | Full reference inventory, call DAGs, semantic classification, ODR analysis |
| `R2-BASELINE-CODEGEN.md` | GIMPLE/object/linked-binary proof that both RMWs survive -O3 |
| `R3-ARMS.md` | Three-arm design (BASE/OP-ONLY/FULL per backend), patches, layout deltas, assembly oracle |
| `R4-CORRECTNESS.md` | Deterministic suite matrix, oracle coverage mapping, stats parity |
| `R4-MUTATION-RECEIPTS.md` | TP-M1/UR-M1 kill receipts; the UR oracle-gap finding |
| `R5-CALIBRATION.md` | A/A calibration, protocol evolution, noise floors, the path-name bias finding |
| `R6-THREADPOOL.md` | TP A/B results and attribution (code-shape artifact proof) |
| `R7-URING.md` | UR A/B results, D8 supplement, kernel-census parity |
| `R8-CAUSAL-ANALYSIS.md` | Effect inventory, causal model, materiality ladder |
| `R9-REVIEWS.md` | Independent Review A (semantics) and Review B (methodology) |
| `VERDICT.md` | Final per-backend and issue verdicts |
| `manifest.json` | File inventory with SHA-256 checksums |

## Key findings (short form)

1. Both counters are observation-only derived summaries; the sole consumer
   is the internal test seam. No production reader, no ordering role, no
   public-ABI exposure (R1).
2. Neither RMW is compiler-eliminated: `lock addq` survives GCC 16.2.1 -O3
   in the linked Release executables (R2).
3. The macro-guard patches are semantically safe: all-green deterministic suite (78/78 at first run, 79/79 with the new oracle)
   on every arm, stats on/off parity, mutants killed by their intended
   counter assertions (R3/R4). One real oracle gap was found and closed
   (`tests/uring_flush_count_oracle_test.cpp`).
4. End-to-end, removal produces **no material benefit** (R6/R7/R8): the
   genuine locked-RMW cost is ~0 (TP) or ~0.4–1 % on submit-path regimes
   (UR), but deleting the instructions re-shapes the binary and swings
   results by ±0.7–3.8 % in both directions — reproducibly per binary
   pair, but not transferable, and netting to ≈ 0 or slightly negative.
5. Methodological findings: a one-character difference in the benchmark's
   tmpfs path name biased identical binaries by ~1 % (R5); the campaign's
   control-arm protocol (plain-increment controls) separates lock-semantics
   cost from code-shape artifacts (R6/R7).

## Reproduction

```sh
# arms (five worktrees at 3df10dc4 with the patches in patches/)
cd /tmp/sluice-575/<arm> && xmake f -m release --liburing=y && xmake build -g bench -y
# campaigns (15 paired interleaved reps each)
scripts/run_575_ab.sh <out-dir> 15 tp 0        # TP, stats off
scripts/run_575_ab.sh <out-dir> 15 ur 0        # UR, stats off
scripts/run_575_d8.sh <out-dir> 15             # D8 supplement
# reduction (self-tested)
scripts/reduce_575.py --selftest
scripts/reduce_575.py <out-dir>/ab.jsonl base,tp-oponly,A-RMW-removed --metric=wall_s
```

Raw evidence: `raw/` (calibration + all campaigns, ~1.2 MB total, one JSONL
row per measured invocation with per-rep binary SHA, wall/instructions/
cycles/user/sys/context switches/frequency). Assembly evidence: `assembly/`.
Patches: `patches/` (arms, controls, mutants).
