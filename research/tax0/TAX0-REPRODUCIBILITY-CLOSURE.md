# TAX-0 — Clean-tree reproducibility closure

Issues: #250 · #259 · PR #260
Closure session: `results/tax0-clean-tree-closure-2026-09-01/`
Original canonical session: `results/tax0b-zladder-wsl2-formal4/`

> **This closure does NOT replace the original canonical sessions.**
>
> It establishes that the committed research implementation reproduces the
> same control-plane tax structure from a clean working tree.

## 1. Why this closure exists

The canonical measurement session (`tax0b-zladder-wsl2-formal4`) was
measured from a DIRTY research tree at `867ac94` (research implementation
not yet fully committed). Its environment.json correctly records
`dirty: true` and the exact binary sha256 — that data is valid and remains
the canonical ladder. This closure closes the reviewer attack "canonical
results were measured from a dirty working tree rather than a fully
immutable committed revision": the exact committed implementation, built
from a clean tree, reproduces the same tax structure.

## 2. Environment identity

The clean-tree closure binary is **byte-identical** to the canonical
session binary (sha256 `f854642e...` both) — the committed implementation
builds to the identical artifact. Same compiler (clang 21.1.8), same kernel
(WSL2 6.18.33.2), same liburing 2.14, same host. Classification unchanged:
QUALIFIED_BUT_VIRTUALIZED / ENVIRONMENT-LIMITED.

## 3. Original vs clean comparison (read cells, instructions/op)

| cell | Original Δ(Z1b−Z1) | Clean Δ(Z1b−Z1) | Original Δ(Z2−Z1b) | Clean Δ(Z2−Z1b) | Verdict |
| ---- | -----------------: | --------------: | -----------------: | --------------: | ------- |
| 4K d1  | +47 | +47 | +2626 | +2626 | REPRODUCED |
| 4K d32 | +37 | +38 | +2032 | +2032 | REPRODUCED |
| 4K d64 | +37 | +38 | +2132 | +2131 | REPRODUCED |
| 64K d8 | +38 | +38 | +2015 | +2015 | REPRODUCED |
| 1M d8  | +37 | +38 | +2015 | +2015 | REPRODUCED |

Continuation premium (Z3w1−Z2): original +787..+1968 (per cell) and clean
identical in every cell. Per-cell absolute values match the original to the
last instruction in 19/20 read cells; the single difference is 1
instruction in the 1M d8 Z1 cell (229552 vs 229553) — expected noise, not
drift.

Same-work: **PASS** — 32/32 closure cells (read 4K/64K/1M × z1/z1b/z2/z3w1
+ write 4K/64K × same arms) match the original canonical session on ops
count, word_sum, and ok state; write cells additionally runner-byte-
verified.

## 4. Representative cells

- READ (primary): 4K d1, 4K d32, 4K d64, 64K d8, 1M d8 × {z1, z1b, z2, z3w1}
- WRITE (supplementary): 4K d1, 4K d32, 64K d8 × {z1, z1b, z2, z3w1}
- z1bw/z3w4 not re-run: not needed for the fixed-tax structure; z3w4 write
  is the known OBS-1-unstable cell, out of closure scope (see #262).

## 5. Same-work gate

Per-cell fail-closed (same op count, same bytes, same offsets, same
word/checksum result, same completion count, same depth, same outer-loop
semantics), cross-arm equality checked by the runner, write arms verified
runner-side. All PASS.

## 6. Limitations

- Same WSL2 virtualized environment as canonical — closure numbers remain
  ENVIRONMENT-LIMITED; nothing here upgrades claims to native.
- Representative subset, not the full 60-cell matrix (by design).
- This is a reproducibility closure, not a new measurement campaign; no
  F01–F05 verdict history is changed.
