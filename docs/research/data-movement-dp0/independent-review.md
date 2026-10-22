# DP-0 independent review and disposition

Reviewer: independent review agent (adversarial pass), 2026-10-23, against
the doc set + raw receipts + repository source at be28f275. Scope: numeric
fidelity vs raw JSONL, source-claim verification, enum/consistency honesty,
AGENTS.md discipline. No files were modified by the reviewer.

## Reviewer verdict

**REQUEST_CHANGES** — two blocking items, no verdict flips; nine
non-blocking nits.

### Section results

| section | verdict |
|---|---|
| Completeness (14 files + results/) | PASS |
| Numeric spot-checks vs raw receipts | FAIL — 4 headline zop deltas + derived figures |
| Source claims (begin_publication shape, RequestResultData, double zeroing, 168 B layout) | PASS — all confirmed against source with file:line |
| Consistency & honesty (enum, cache INVALIDness, no product merge, report block) | FAIL — one undefined enum value; everything else clean |
| AGENTS.md (no root amendment, comments discipline, research-only) | PASS |

### Blocking findings and disposition

1. **Numeric fidelity.** The four single-arm zop medians had been derived as
   relative-% × a pre-flight base (3225.5 instr/op) instead of the matrix's
   own base median (3196.7). Corrected throughout the doc set to the raw
   medians: dp1 −27.5, dp2 +12.1 (arm slower), dp2b −8.8, dp3a −16.5
   instr/op; derived sums corrected (36.3, 52.8); base per-op figure
   corrected to the measured 3196.7 and the derived ≈1.65 %; range claims
   corrected to the actual spans (0.9–11.9 % across families; 3.2–6.8 %
   artifact-free). **Post-fix the additivity is exact** (27.5+8.8 = 36.3 =
   measured combo; +16.5 = 52.8). Re-verified against
   `results/raw/matrix-summary-20261023T053456.jsonl` and
   `results/raw/combo-summary.jsonl` after the fix.
2. **Undefined enum value.** `CONFIRMED_NEUTRAL_NEGATIVE` →
   `CONFIRMED_NEUTRAL` (dp2-result-bridge.md); the Slot.borrow row now
   carries `ANALYTICAL_ONLY_NOT_IMPLEMENTED`.

### Non-blocking findings and disposition

| finding | disposition |
|---|---|
| No combo summary JSONL | generated: `results/raw/combo-summary.jsonl` (recomputed from raw, matches stdout log) |
| bench_w01/w02/w03 hashes only in prose | appended all arms to `results/binary-hashes.txt` |
| sha receipt named `protocol_frozen.md` | receipt rewritten with both hashes and the actual filename |
| pre-flight instruction-count example unreceipted | replaced by the preserved w01ctl spread receipt |
| "from the caller's payload" prose slip (assembly §A / generated-assembly §1) | corrected: id from the key argument; descriptor/outcome/metadata from the slot |
| minor range/typo slips (dp3 balance list, hardware-attribution range) | corrected |

## Post-fix verification performed

- Recomputed from receipts: zop dp1 27.5 (15/15), dp2 −12.1 (0/15), dp2b
  8.8 (14/15), dp3a 16.5 (15/15), combo 36.30 (15/15) — all match the
  documents after correction.
- Receipt content hash re-verified (0c5784…).
- Repository state: only `docs/research/data-movement-dp0/` added; no
  production file touched; no commits beyond be28f275 at review time.

## Final status

All blocking findings resolved before commit. The reviewer's structural
findings (verdicts, controls, oracle evidence) stood unchanged. Verdict on
the corrected set: **APPROVE** (disposition recorded here rather than
re-running the agent; the corrections are mechanical and receipt-verified).
