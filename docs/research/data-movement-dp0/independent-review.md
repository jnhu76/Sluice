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

## Second review round (erratum pass), 2026-10-23

An external reviewer re-derived the pairing statistics directly from the
three raw JSONL families (same case/config/rep pairing; no document summary
used as input) and confirmed: dp1 −27.50015 (15/15), dp2 +12.10004 (0/15),
dp2b −8.80001 (14/15), dp3a −16.50006 (15/15), dp1+dp2b −36.30015 (15/15);
the fixed-bench-TU family reproduced dp1 −27.5 and dp3a −16.5. The three
measured arms stand.

Three findings accepted as errata (all corrected in this revision):

1. **DP-3C source-model error (blocking for the research narrative, not for
   the measured arms).** `dp3-slot-layout.md` and
   `field-liveness-matrix.md` claimed `offer_terminal()` writes 40 B of
   `FileInfo` into the slot on every ordinary read/write. Baseline source
   refutes this: `request_core.cpp` gates the store on
   `candidate.has_metadata` (default `false`); `uring_completion.cpp`'s
   byte-op success branch never sets it; `threadpool_backend.cpp` sets it
   only for `file_info || size`; zero-ops are made terminal at `accept()`
   and bypass `offer_terminal` entirely. The sketched
   `descriptor.op == file_info` gate would additionally have broken
   `FileSize` (size ops require `metadata.size`). DP-3C is redefined as a
   per-site audit of `Slot::metadata` initialization/write sites vs
   `TerminalCandidate::metadata`. RESEARCH ONLY; no measured result
   depended on the false premise.
2. **52.8 instr/op relabeled as a projected sum.** The only measured
   combined arm is DP-1+DP-2B (−36.3). Now recorded as
   `THREE_ARM_PROJECTED_SUM = 52.8 instr/op`,
   `THREE_ARM_COMBINED_MEASURED = NOT_RUN`. The ≈500 B figure is relabeled
   a static-analysis logical load/store elimination estimate, not a
   memory-bus/DRAM-traffic measurement.
3. **DP-3B cache-line argument shrunk.** 168 B slots are not 64 B aligned;
   consecutive slot starts drift across the arena (168 mod 64 = 40), so
   slot offset 0x80 does not map to a fixed third physical cache line.
   With no cache/TLB PMU data and no implemented DP-3B/DP-3C arms, the
   cross-workload domination claim is withdrawn; at most, under the
   analyzed capacities and traffic model there is no demonstrated reason
   to implement the hot/cold split.

### Stage disposition

- DP-1, DP-2B, DP-3A: PRODUCTIZATION CANDIDATE — separate PRs in the order
  dp1 → dp2b → dp3a, each independently passing v2 conformance, the
  lifetime/concurrency gates, and real performance validation at final
  HEAD; dp3a additionally owes first-reserve, rollback,
  generation-exhaustion and slot-reuse initialization-safety evidence.
- DP-3B, DP-3C: RESEARCH ONLY.
- PR #570: REVISE DOCS / KEEP DRAFT; no merge.
- Next stage: E5 native-Linux pre-productization validation — real release
  build, GCC 16, W-02/W-03, ThreadPool, non-spinning result consumption,
  and the DP-1+DP-2B+DP-3A three-arm combo; the claimed second-order
  benefit ("completion visible earlier, one fewer poll") must be evidenced
  by recorded `poll_calls/op`, completion visibility and CPU/wall at equal
  request counts, not inferred from total-instruction deltas alone.

## E5 validation round (native Linux), 2026-10-23

The E5 stage demanded by the disposition above was executed on the same
bare-metal server as M2 (Fedora 44, kernel 7.2.9, GCC 16.2.1, release
build, `--liburing=y`; five trees at the branch commit with the arm
patches applied, each 76/76). Full record: e5-native-validation.md; raw
evidence committed under results/raw/e5/ and results/receipts/e5/.

Outcomes against the demanded checks:

- **Three-arm combo measured**: 62.7 instr/op removed on the zop
  instrument (15/15 interleaved pairs; negative control 0/5).
  `THREE_ARM_COMBINED_MEASURED` is no longer NOT_RUN.
- **W-02/W-03 + ThreadPool + non-spinning consumption**: driven; W-02
  pinned, W-03 unpinned (recorded deviation, see below). W02-uring-4k-d1
  regresses for the three-arm tree (+2.9 %, CI wholly > 1).
- **Attribution of that regression**: dp1 alone improves (0.9821), dp2b
  alone is neutral (0.9919) — the regression is a dp1×dp2b interaction;
  dp3a adds ≈ +0.5 % (noise). The dp2b PR's final-HEAD-on-dp1 gate is
  now evidence-backed with W02-uring-4k-d1 as the arbitrating shape.
- **poll_calls/op + completion visibility**: recorded via a stats-wired
  harness (the campaign also found and recorded that both W-02/W-03
  harnesses never wired their `AsyncStats` into the context factory, so
  their `stats_delta` was identically zero). The claimed second-order
  poll benefit is absent on W-02 shapes and real only on
  W03-uring-4k-d4 (−9.5 % poll passes); empty wakes are zero everywhere.
- **W-03 pinned-run stall**: root-caused to a stop-protocol race in the
  W-03 bench harness (it set `shared.stop` after `request_stop`, whose
  unconditional progress wake could be consumed and re-parked on by the
  driver before the flag became visible; single-CPU affinity opens the
  window ~1/3 of phases). fdinfo at hang proved all 20000 ops completed,
  were reaped and published (SqHead=SqTail=CqHead=CqTail=20001,
  eventfd-count 0); a Sluice-free ring repro hung 0/30; the reordered
  harness hung 0/15 vs 8/15 — causal. The library's notification paths
  were audited line by line and no missing-wake path exists. The bench
  is fixed in-tree by this branch; W-03 figures are unpinned (recorded
  deviation).

Stage disposition unchanged: DP-1/DP-2B/DP-3A remain productization
candidates as separate PRs (dp1 → dp2b → dp3a) with the full gates; PR
#570 remains draft with docs + bench fix + evidence only.
