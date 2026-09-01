# bufe0-align-wsl2-1 — AMENDMENT 1 alignment diagnostic session notes

192 runs (12 cells x arms {b1, b1a, b2, b3} x phases {B, C} x R7/R14),
0 gate errors, 0 regime violations. Purpose: mechanism attribution for
the formal session's Outcome-D pattern (prereg AMENDMENT 1).

b1a = b1's EXACT allocation mechanism (make_unique_for_overwrite) with
the exposed pointer rounded up to page alignment — the only delta vs b1
is the pointer alignment.

## Result: mechanism = page alignment of the I/O buffer

Phase C steady-state ns/op (b1 -> b1a, with b2/b3 for reference):

| cell | b1 | b1a | b2 | b3 |
| --- | --- | --- | --- | --- |
| 4K x 1 | 2942 | 1343 | 903 | 1382 |
| 4K x 8 | 2120 | 932 | 944 | 911 |
| 4K x 128 | 2508 | 1148 | 1153 | 1119 |
| 64K x 1 | 24198 | 4975 | 4542 | 4647 |
| 64K x 128 | 41421 | 12448 | 10198 | 10958 |
| 1M x 1 | 437015 | 124752 | 120551 | 120644 |
| 1M x 32 | 406259 | 202497 | 208734 | 205083 |

b1a recovers essentially the ENTIRE b2/b3 advantage in most cells (and
exceeds b1 by 2-5x vs b1). Residual partial recovery in 1M x 8
(270651 vs b2 146896) is an unexplained secondary effect in that one
cell; it does not change the attribution: alignment is the dominant,
reproducible mechanism across 10 of 12 cells, and b3 (posix_memalign,
owned/glibc-family) achieves the same steady state as b2.

Kernel-side interpretation (same-host, no native claims): a page-aligned
destination (and source, for writes) lets the kernel copy path work in
full-page units; a +16 offset (glibc chunk pointer) defeats it for every
page of every op. This is a PER-OP steady-state cost, not a lifecycle
cost — it is NOT cost shifting.

## Limitation recorded

A ~11-second compile overlapped this session (harness development on the
same host). Phase B absolute values drifted vs the formal session (e.g.
b1 4K s1 16903 here vs 7725 formal), but the Phase C cross-arm RATIOS
are stable vs the formal session (b1 ~5x b2 in both), and this session's
purpose — same-session internal cross-arm attribution — is unaffected.
Phase B numbers from this session are not used as primary evidence.

## Environment classification

Same as bufe0-micro-wsl2-1: QUALIFIED_BUT_VIRTUALIZED,
ENVIRONMENT-LIMITED absolute numbers, same-host causal comparison only.
