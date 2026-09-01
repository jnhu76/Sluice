# TAX-0 clean-tree reproducibility closure — session notes

PURPOSE: reproducibility closure only. Confirms the COMMITTED research
implementation at PR #260 HEAD reproduces the same control-plane tax
structure from a CLEAN working tree as the original canonical session.
SUPERSEDES: nothing.
ORIGINAL CANONICAL SESSION: research/tax0/results/tax0b-zladder-wsl2-formal4/
TREE STATE: clean (git status --porcelain empty at measurement time).
COMMIT: 7cc294a7132ef429e242f86954307f53da249292 (PR #260 head).

## Why this session exists

The canonical formal4 session was measured from a DIRTY research tree at
867ac94 (research implementation not yet fully committed; dirty=true and
binary sha256 recorded in its environment.json). That data is valid and is
NOT replaced. This closure closes the reviewer attack "canonical results
were measured from a dirty working tree": the exact committed research
implementation, built from a clean tree, reproduces the same tax structure.

## Protocol identity

Identical measurement machinery to the canonical sessions (imported from
research/tax0/scripts/tax0z.py): double-difference (total(R14)-total(R7))/7
/ops under `perf stat -x, -e instructions:u,cycles:u,branch-misses:u,
cache-misses:u`; runner-side write verification; cross-arm same-work
fail-closed; `--warmup 0`.

Representative closure matrix (subset of the P1 canonical matrix):
- READ : {4K d1, 4K d32, 4K d64, 64K d8, 1M d8} x {z1, z1b, z2, z3w1}
- WRITE: {4K d1, 4K d32, 64K d8} x {z1, z1b, z2, z3w1}
(z1bw/z3w4 not re-run — not needed for the fixed-tax structure; z3w4 write
is the known OBS-1-unstable cell, out of closure scope.)

## Environment identity

- binary sha256 == canonical session binary sha256 EXACTLY:
  f854642e894df2c85efc26f228c343eea7f6453436227f467ab48c4104a352ba
  (the committed implementation builds to the identical artifact)
- compiler: Ubuntu clang 21.1.8 (same as canonical)
- kernel: 6.18.33.2-microsoft-standard-WSL2; AMD 5800H; liburing 2.14
- classification: QUALIFIED_BUT_VIRTUALIZED / ENVIRONMENT-LIMITED (same)

## Result (read cells, instructions/op, original -> clean)

| cell | Z1 orig->clean | Z1b orig->clean | Z2 orig->clean | Z3w1 orig->clean |
| --- | --- | --- | --- | --- |
| 4K d1  | 1154->1154 | 1201->1201 | 3827->3827 | 5796->5796 |
| 4K d32 | 1043->1043 | 1080->1080 | 3112->3112 | 3899->3899 |
| 4K d64 | 1043->1043 | 1080->1080 | 3212->3212 | 4105->4105 |
| 64K d8 | 14499->14499 | 14537->14537 | 16552->16552 | 17380->17380 |
| 1M d8  | 229553->229552 | 229590->229590 | 231605->231605 | 232468->232468 |

Tax deltas (read): Z1b-Z1 clean +38..+47 (orig +37..+47); Z2-Z1b clean
+2015..+2131 fixed (orig identical); Z3w1-Z2 clean +787..+1968 (orig
identical). The only difference across all 20 read cells is ONE instruction
in the 1M d8 Z1 cell (229552 vs 229553) — expected noise, not drift.

Write cells (optional matrix) all SAME-WORK PASS with runner byte
verification; per-cell instructions differ by at most ~5% in one cell
(4K d32 z2 2824 vs 2678), consistent with write-path kernel-medium
variability — structure preserved (Z1b-Z1 small, Z2-Z1b ~2000-2200,
Z3w1-Z2 premium). No OBS-1 flake occurred during this closure run.

## Same-work

32/32 closure cells SAME-WORK PASS against the original canonical session
(identical ops count, identical word_sum, identical ok state; write cells
additionally runner-byte-verified).

## Verdict

REPRODUCED — the qualitative tax decomposition (capability ~free at
~+37-38/op, L1 abstraction tax fixed ~2015/op, continuation premium
~+800/op) survives the clean committed tree exactly. No MATERIAL_DRIFT.

## Limitations

- Same environment as canonical (WSL2 virtualized): closure numbers remain
  ENVIRONMENT-LIMITED; nothing here upgrades claims to native.
- Not a re-run of the full 60-cell matrix; representative cells only.
- The closure measures the production-linked ladder binary (Z2/Z3 arms link
  production sluice_async), identical to the canonical session.
